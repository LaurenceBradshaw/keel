#include "ir/simplify.h"
#include <unordered_set>

namespace keel
{
namespace
{
bool fold_constant_branches( Function& func, const Literal_pool& literals )
{
    bool folded = false;
    for( Block& block : func.blocks )
    {
        Terminator& terminator = block.terminator;
        if( terminator.kind != Terminator_kind::Branch || terminator.condition.kind != Operand_kind::Constant ||
            !terminator.condition.constant.is_valid() )
        {
            continue;
        }

        const bool taken = literals.integer( terminator.condition.constant ) != 0;

        terminator.kind       = Terminator_kind::Goto;
        terminator.targets[0] = terminator.targets[taken ? 0 : 1];

        // A Goto carries neither, and leaving them would hand a later pass an edge nothing takes.
        terminator.targets[1] = Block_id {};
        terminator.condition  = Operand {};

        folded = true;
    }

    return folded;
}

Block_id final_target( const Function& func, Block_id block_id )
{
    // Follows a chain while the block it lands on has statement_count == 0 and a Goto terminator.
    std::unordered_set<u32> visited {};
    while( block_id.is_valid() )
    {
        const Block& block = func.blocks[block_id.v];

        if( block.statement_count != 0 || block.terminator.kind != Terminator_kind::Goto )
        {
            return block_id;
        }

        if( !visited.insert( block.terminator.targets[0].v ).second )
        {
            return block_id;
        }

        block_id = block.terminator.targets[0];
    }

    return block_id;
}

void thread_gotos( Function& func )
{
    for( Block& block : func.blocks )
    {
        Terminator& terminator = block.terminator;

        // Both edges of a branch, not just a goto's: the empty block an `if` without an `else`
        // leaves behind is reached by the false edge and by nothing else.
        const u32 targets = terminator.kind == Terminator_kind::Branch ? 2 : terminator.kind == Terminator_kind::Goto ? 1 : 0;

        for( u32 i = 0; i < targets; ++i )
        {
            terminator.targets[i] = final_target( func, terminator.targets[i] );
        }
    }
}

// The same walk verify's check_reachability does, with a set instead of error strings. Deliberately
// a second copy: verify must not depend on the pass it exists to check.
std::vector<bool> reachable( const Function& func )
{
    std::vector<bool> seen( func.blocks.size(), false );

    if( func.blocks.empty() )
    {
        return seen;
    }

    std::vector<Block_id> pending { Block_id { 0 } };
    seen[0] = true;

    while( !pending.empty() )
    {
        const Terminator& terminator = func.blocks[pending.back().v].terminator;

        pending.pop_back();

        const u32 targets = terminator.kind == Terminator_kind::Branch ? 2 : terminator.kind == Terminator_kind::Goto ? 1 : 0;

        for( u32 i = 0; i < targets; ++i )
        {
            const Block_id target = terminator.targets[i];

            if( !target.is_valid() || target.v >= func.blocks.size() || seen[target.v] )
            {
                continue;
            }

            seen[target.v] = true;
            pending.push_back( target );
        }
    }

    return seen;
}

// Blocks are indices, so dropping one renumbers every block after it. Statements are rebuilt in the
// same walk rather than left with holes in them: two things scan the whole vector - whether a
// function assigns its return slot anywhere, and which locals need drop flags - and a dead block's
// statements would answer both with code that cannot run.
void prune( Function& func, const std::vector<bool>& live )
{
    std::vector<Block_id> renumbered( func.blocks.size(), Block_id {} );
    u32                   kept = 0;

    for( u32 i = 0; i < func.blocks.size(); ++i )
    {
        if( live[i] )
        {
            renumbered[i] = Block_id { kept++ };
        }
    }

    if( kept == func.blocks.size() )
    {
        return;
    }

    std::vector<Block>     blocks;
    std::vector<Statement> statements;

    blocks.reserve( kept );
    statements.reserve( func.statements.size() );

    for( u32 i = 0; i < func.blocks.size(); ++i )
    {
        if( !live[i] )
        {
            continue;
        }

        Block block = func.blocks[i];

        // Read the old range out before writing the new one, as drop elaboration's rebuild does:
        // the copy below indexes the original vector.
        const u32 first = block.first_statement;
        const u32 count = block.statement_count;

        block.first_statement = narrow_cast<u32>( statements.size() );
        statements.insert( statements.end(), func.statements.begin() + first, func.statements.begin() + first + count );

        const u32 targets = block.terminator.kind == Terminator_kind::Branch ? 2
                            : block.terminator.kind == Terminator_kind::Goto ? 1
                                                                             : 0;

        // Every target of a live block is live by construction, so the map always answers.
        for( u32 t = 0; t < targets; ++t )
        {
            block.terminator.targets[t] = renumbered[block.terminator.targets[t].v];
        }

        blocks.push_back( std::move( block ) );
    }

    func.blocks     = std::move( blocks );
    func.statements = std::move( statements );
}

} // namespace

// One sweep, not a fixpoint: folding is what turns a loop header into an empty goto, threading
// cannot turn a goto back into a branch, and pruning creates neither.
void simplify( Function& func, const Literal_pool& literals )
{
    fold_constant_branches( func, literals );
    thread_gotos( func );
    prune( func, reachable( func ) );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include <sstream>

#include "check/assign_check.h"
#include "common/diagnostics.h"
#include "common/source_manager.h"
#include "ir/lower.h"
#include "ir/verify.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{
namespace
{

// Real source rather than hand-built CFGs, for the same reason the check/ passes use it: what is
// worth testing here is a loop header, a join and a back edge, and those are the graphs that are
// worst to assemble by hand. The function under test is always the first one; `main` is there
// because a program needs one.
struct Simplified
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    Ast            ast;
    Resolution     resolution;
    Types          types;

    std::vector<Function> functions;

    explicit Simplified( std::string_view source )
    {
        const File_id file = sm.add_file( "t.kl", std::string( source ) );

        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
        resolution = resolve( ast, sm, interner, diags );
        types      = type_check( ast, resolution, literals, sm, interner, diags );

        if( diags.has_errors() )
        {
            return;
        }

        functions = lower( ast, resolution, types, literals, interner );

        for( Function& function : functions )
        {
            simplify( function, literals );
        }
    }

    bool clean() const
    {
        return !diags.has_errors();
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags.render( sm, out );
        return out.str();
    }

    const Function& first() const
    {
        return functions.front();
    }

    // Every fixture asks for it: a pass that renumbers blocks has to leave a graph behind, and
    // verify already knows what one looks like - every target in range, every block reachable.
    std::vector<std::string> problems() const
    {
        std::vector<std::string> all;

        for( const Function& function : functions )
        {
            for( std::string& problem : verify( function ) )
            {
                all.push_back( std::move( problem ) );
            }
        }

        return all;
    }

    std::size_t unassigned() const
    {
        std::size_t count = 0;

        for( const Function& function : functions )
        {
            count += check_assignment( function ).unassigned.size();
        }

        return count;
    }

    bool has_a_return() const
    {
        for( const Block& block : first().blocks )
        {
            if( block.terminator.kind == Terminator_kind::Return )
            {
                return true;
            }
        }

        return false;
    }
};

const std::string k_main = "\ni32 main() { return 0; }\n";

} // namespace

// The acceptance M6.5 states: three blocks carrying no statements become two. Nothing is folded
// here - `for( ; ; )` has no condition to fold - so this is threading alone.
TEST_CASE( "simplify_threads_a_chain_of_empty_blocks", "[ir][simplify]" )
{
    SECTION( "a loop with no condition keeps two blocks" )
    {
        const Simplified p( "i32 f() { for( ; ; ) { return 7; } }" + k_main );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.problems().empty() );
        REQUIRE( p.first().blocks.size() == 2 );
    }

    SECTION( "the empty false block of an `if` without an `else` goes" )
    {
        const Simplified p( "i32 f( i32 a ) { i32 b = 0; if( a > 0 ) { b = 1; } return b; }" + k_main );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.problems().empty() );

        // entry, the arm, the join. The false edge threads straight to the join.
        REQUIRE( p.first().blocks.size() == 3 );
    }

    SECTION( "a block reached by both edges of one branch is still reached by both" )
    {
        const Simplified p( "i32 f( i32 a ) { if( a > 0 ) { } else { } return a; }" + k_main );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.problems().empty() );
        REQUIRE( p.first().blocks.size() == 2 );

        const Terminator& entry = p.first().blocks[0].terminator;

        REQUIRE( entry.kind == Terminator_kind::Branch );
        REQUIRE( entry.targets[0] == entry.targets[1] );
    }
}

// The false positive this pass exists to remove: the exit block of a constantly-true loop is
// reachable in the graph and never taken by the program, so the implicit return in it was reported
// as a path that produces no value.
TEST_CASE( "simplify_folds_a_constant_branch", "[ir][simplify][returns]" )
{
    SECTION( "a constantly true loop owes no return" )
    {
        const Simplified p( "i32 f() { while( true ) { return 7; } }" + k_main );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.problems().empty() );
        REQUIRE( p.unassigned() == 0 );
        REQUIRE( p.first().blocks.size() == 2 );
    }

    SECTION( "a break still reaches the loop exit" )
    {
        const Simplified p( "i32 f( i32 a ) { while( true ) { if( a > 0 ) { break; } a = a + 1; } return a; }" + k_main );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.problems().empty() );
        REQUIRE( p.unassigned() == 0 );

        // The exit is reached by the break and by nothing else, so folding the header must not
        // orphan it - without it the function has no way out at all.
        REQUIRE( p.has_a_return() );
    }

    SECTION( "the arm a constant condition never takes is gone" )
    {
        const Simplified folded( "i32 f() { i32 b = 1; if( false ) { b = 2; } return b; }" + k_main );
        const Simplified plain( "i32 f() { i32 b = 1; return b; }" + k_main );

        INFO( folded.rendered() );
        REQUIRE( folded.clean() );
        REQUIRE( folded.problems().empty() );

        // Not merely unreachable: the arm's statements leave the function with it, which is what
        // stops a global scan over `statements` reading code that cannot run.
        REQUIRE( folded.first().statements.size() == plain.first().statements.size() );

        // One block more than the straight-line form, because the join survives: merging a block
        // into its only predecessor is a different optimisation, and this pass does not do it.
        REQUIRE( folded.first().blocks.size() == plain.first().blocks.size() + 1 );
    }

    SECTION( "so is the arm it always takes" )
    {
        const Simplified p( "i32 f() { i32 b = 1; if( true ) { b = 2; } else { b = 3; } return b; }" + k_main );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.problems().empty() );

        const bool assigns_three = std::any_of(
            p.first().statements.begin(),
            p.first().statements.end(),
            [&]( const Statement& statement )
            {
                return statement.value.kind == Rvalue_kind::Use && statement.value.a.kind == Operand_kind::Constant &&
                       statement.value.a.constant.is_valid() && p.literals.integer( statement.value.a.constant ) == 3;
            }
        );

        REQUIRE_FALSE( assigns_three );
    }
}

// A loop whose body is empty is a cycle of blocks that all thread to each other. Following it has
// to stop somewhere, and the block it stops at may legitimately be its own successor.
TEST_CASE( "simplify_leaves_a_loop_that_never_finishes", "[ir][simplify]" )
{
    SECTION( "with no condition" )
    {
        const Simplified p( "void f() { for( ; ; ) { } }" + k_main );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.problems().empty() );
    }

    SECTION( "with a constant one" )
    {
        const Simplified p( "void f() { while( true ) { } }" + k_main );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.problems().empty() );
    }
}

// A switch arm is the other place empty blocks breed, and `fallthrough` is the one that joins two
// arms rather than leaving one.
TEST_CASE( "simplify_keeps_a_switch_well_formed", "[ir][simplify][switch]" )
{
    const Simplified p(
        "i32 f( i32 a ) { switch( a ) { case 1: fallthrough; case 2: return 5; default: return 6; } }" + k_main
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.problems().empty() );
    REQUIRE( p.unassigned() == 0 );
}

// One sweep is the fixpoint: threading cannot turn a goto back into a branch, and pruning creates
// neither. A second run that changed anything would mean one of those is false.
TEST_CASE( "simplify_is_a_fixpoint_after_one_sweep", "[ir][simplify]" )
{
    Simplified p( "i32 f( i32 a ) { while( true ) { if( a > 0 ) { break; } a = a + 1; } if( a > 2 ) { } return a; }" + k_main );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::size_t blocks     = p.first().blocks.size();
    const std::size_t statements = p.first().statements.size();

    simplify( p.functions.front(), p.literals );

    REQUIRE( p.first().blocks.size() == blocks );
    REQUIRE( p.first().statements.size() == statements );
    REQUIRE( p.problems().empty() );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
