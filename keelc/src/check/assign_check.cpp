#include "check/assign_check.h"

namespace keel
{

namespace
{

// A *must* analysis, which is the whole reason this is not a question inside check_moves. There the
// bottom is "no information" and merging raises a local up the lattice; here the bottom is "assigned
// everywhere" and merging can only lower it, because a value is definitely assigned only if every
// path assigned it.
//
// `reached` is what keeps that honest. A block the worklist has not visited yet holds the identity -
// all assigned - and intersecting with it would be a lie, so it is skipped until something reaches
// it. `ever` is the same information joined the other way, and exists only so the diagnostic can
// say "on some paths" rather than "never".
struct Flow
{
    std::vector<u8> always; // assigned on every path into here
    std::vector<u8> ever;   // assigned on at least one
    bool            reached = false;
};

Flow bottom( std::size_t locals )
{
    return Flow { std::vector<u8>( locals, 1 ), std::vector<u8>( locals, 0 ), false };
}

// Merges `from` into `into`, returning whether anything changed - the worklist's only termination
// signal, exactly as in check_moves.
bool merge_into( Flow& into, const Flow& from )
{
    if( !into.reached )
    {
        into = from;
        return true;
    }

    bool changed = false;

    for( std::size_t i = 0; i < into.always.size(); ++i )
    {
        const u8 always = static_cast<u8>( into.always[i] && from.always[i] );
        const u8 ever   = static_cast<u8>( into.ever[i] || from.ever[i] );

        changed = changed || always != into.always[i] || ever != into.ever[i];

        into.always[i] = always;
        into.ever[i]   = ever;
    }

    return changed;
}

// 0, 1 or 2 of them, by terminator kind. Deliberately the same shape as check_moves', because it is
// the same question about the same graph.
void successors( const Terminator& terminator, std::vector<Block_id>& out )
{
    switch( terminator.kind )
    {
    case Terminator_kind::Goto:
        out.push_back( terminator.targets[0] );
        return;
    case Terminator_kind::Branch:
        out.push_back( terminator.targets[0] );
        out.push_back( terminator.targets[1] );
        return;
    case Terminator_kind::Unset:
    case Terminator_kind::Return:
    case Terminator_kind::Unreachable:
        return;
    }
}

// Only an Assign matters. A projected target counts: `(*_1).x = ...` is a step in building the
// referent, and an `out` parameter is written exactly that way when its type is a struct.
//
// Storage_dead clears, so a local reused after its scope ends does not carry the old answer. It
// cannot name an `out` parameter - those live for the whole function - but the pass is over locals
// rather than over parameters, and a rule that holds for one should hold for all of them.
void transfer_block( const Function& func, u32 block, Flow& flow )
{
    const Block& b = func.blocks[block];

    for( u32 i = 0; i < b.statement_count; ++i )
    {
        const Statement& statement = func.statements[b.first_statement + i];

        if( statement.place.is_global() )
        {
            continue;
        }

        const u32 local = statement.place.local.v;

        switch( statement.kind )
        {
        case Statement_kind::Assign:
            flow.always[local] = 1;
            flow.ever[local]   = 1;

            // Taking a place's address counts as assigning it. The analysis cannot see what happens
            // through a pointer, and this is the only thing that makes forwarding work:
            // `void forward( out i32 n ) { init( out n ); }` lowers to `&n`, and the callee's
            // promise to assign it is exactly what the marker at that call site says.
            //
            // It is the same shallowness place_root already has, and it errs in the safe direction
            // for a *must* analysis - a `const ref` argument is treated as assigning too, so this
            // can miss a real mistake but can never invent one.
            if( statement.value.kind == Rvalue_kind::Address_of && !statement.value.a.place.is_global() )
            {
                flow.always[statement.value.a.place.local.v] = 1;
                flow.ever[statement.value.a.place.local.v]   = 1;
            }

            break;
        case Statement_kind::Storage_dead:
            flow.always[local] = 0;
            flow.ever[local]   = 0;
            break;
        case Statement_kind::Storage_live:
        case Statement_kind::Drop:
            break;
        }
    }
}

Flow entry_flow( const Function& func )
{
    Flow flow { std::vector<u8>( func.locals.size(), 1 ), std::vector<u8>( func.locals.size(), 1 ), true };

    // The one thing this pass asserts about the entry: an `out` parameter's referent is empty until
    // the body writes it. Everything else is taken as assigned, because this pass is not general
    // definite assignment - it answers one question, and a local nobody made it look at must not
    // become an error by accident.
    for( const Local_id local : func.out_parameters )
    {
        flow.always[local.v] = 0;
        flow.ever[local.v]   = 0;
    }

    // The return slot is an `out` parameter of the function: the caller supplies the storage and the
    // body's obligation is to have written it before any way out. Seeded here rather than given a
    // pass of its own, because it is the same question on the same graph.
    if( func.returns_a_value )
    {
        flow.always[k_return_slot.v] = 0;
        flow.ever[k_return_slot.v]   = 0;
    }

    return flow;
}

} // namespace

std::vector<Unassigned_error> check_assignment( const Function& func )
{
    std::vector<Unassigned_error> errors;

    if( func.out_parameters.empty() && !func.returns_a_value )
    {
        return errors; // nothing to be unassigned, and the fixpoint would prove it slowly
    }

    std::vector<Flow> in( func.blocks.size(), bottom( func.locals.size() ) );
    in[0] = entry_flow( func );

    std::vector<Block_id> work { Block_id { 0 } };
    std::vector<Block_id> next;

    // Forward, pushing into successors rather than pulling from predecessors, so no predecessor map
    // is needed. Terminates because a local's `always` only ever falls and its `ever` only ever
    // rises, both are one bit, and there are finitely many blocks - so merge_into eventually
    // returns false everywhere and the worklist drains.
    while( !work.empty() )
    {
        const Block_id block = work.back();
        work.pop_back();

        Flow out = in[block.v];
        transfer_block( func, block.v, out );

        next.clear();
        successors( func.blocks[block.v].terminator, next );

        for( const Block_id target : next )
        {
            if( merge_into( in[target.v], out ) )
            {
                work.push_back( target );
            }
        }
    }

    // Every return is a way out, so every return has to have written them. Reported once per block
    // rather than inside the loop above, which would emit one error per visit for any block on a
    // back edge - the same reason check_moves reports in a second walk.
    for( u32 block = 0; block < func.blocks.size(); ++block )
    {
        const Block& b = func.blocks[block];

        if( b.terminator.kind != Terminator_kind::Return || !in[block].reached )
        {
            continue;
        }

        Flow flow = in[block];
        transfer_block( func, block, flow );

        // What this exit owes: every `out` parameter, and the return slot when the function has
        // one. The two are reported through one lambda so that the set checked here cannot drift
        // from the set entry_flow seeds - they have to name exactly the same locals.
        const auto owed = [&]( Local_id local )
        {
            if( flow.always[local.v] != 0 )
            {
                return;
            }

            errors.push_back( Unassigned_error { .local = local, .at = b.terminator.span, .maybe = flow.ever[local.v] != 0 } );
        };

        for( const Local_id local : func.out_parameters )
        {
            owed( local );
        }

        if( func.returns_a_value )
        {
            owed( k_return_slot );
        }
    }

    return errors;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "common/diagnostics.h"
#include "common/literals.h"
#include "common/source_manager.h"
#include "ir/lower.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{
namespace
{

// Real source rather than hand-built CFGs, for the same reason check_moves' tests use it: what is
// worth testing here is what happens at a branch, a join and a back edge, and those are the graphs
// that are worst to assemble by hand.
struct Checked
{
    Source_manager sm;
    Interner       interner;
    Literals       literals;
    Diagnostics    diags;
    Ast            ast;
    Resolution     resolution;
    Types          types;

    std::vector<Function> functions;

    explicit Checked( std::string_view source )
    {
        const File_id file = sm.add_file( "t.kl", std::string( source ) );

        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
        resolution = resolve( ast, sm, interner, diags );
        types      = type_check( ast, resolution, literals, sm, interner, diags );

        if( !diags.has_errors() )
        {
            functions = lower( ast, resolution, types, literals, interner );
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

    // Every function, the way the driver does it. Note a case must now keep its *other* functions
    // returning properly: the return slot is checked too, so a stray `i32 helper() { }` in a
    // fixture would add an error of its own.
    std::vector<Unassigned_error> errors() const
    {
        std::vector<Unassigned_error> all;

        for( const Function& function : functions )
        {
            for( const Unassigned_error& error : check_assignment( function ) )
            {
                all.push_back( error );
            }
        }

        return all;
    }

    // Which parameter an error is about. The whole point of carrying a Local_id rather than a
    // message is that the driver names it, so a case can check the naming too.
    std::string_view name_of( const Unassigned_error& error ) const
    {
        for( const Function& function : functions )
        {
            if( error.local.v < function.locals.size() && function.locals[error.local.v].name.is_valid() )
            {
                return interner.text( function.locals[error.local.v].name );
            }
        }

        return {};
    }
};

} // namespace

// The return slot is local 0, and `return x` lowers to an Assign into it - so "can a path reach a
// return without having produced a value" is this pass's own question asked of one more local.
// Before this, `i32 f() { }` compiled and returned whatever was in the slot.
TEST_CASE( "assign_check_requires_a_value_on_every_path_out", "[check][assign][returns]" )
{
    SECTION( "no return at all" )
    {
        const Checked c( "i32 f() { }\ni32 main() { return f(); }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 1 );
        REQUIRE( c.errors()[0].local == k_return_slot );
    }

    SECTION( "a return in an `if` with no `else`" )
    {
        const Checked c( "i32 f( i32 x ) { if( x > 0 ) { return 1; } }\ni32 main() { return f( 0 ); }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 1 );
        REQUIRE( c.errors()[0].local == k_return_slot );
    }

    SECTION( "one arm of a switch" )
    {
        const Checked c( "i32 f( i32 x ) { switch( x ) { case 1: return 1; default: i32 y = 2; } }\n"
                         "i32 main() { return f( 1 ); }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 1 );
    }

    SECTION( "only inside a loop" )
    {
        const Checked c( "i32 f() { while( false ) { return 1; } }\ni32 main() { return f(); }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 1 );
    }

    SECTION( "a non-empty case arm falling out of the switch" )
    {
        // D7 says this is an error in its own right and nothing enforces that yet - but the arm
        // jumps past the switch rather than into the next one, so it lands here.
        const Checked c( "enum E { A, B };\n"
                         "i32 f( E e ) { switch( e ) { case E::A: i32 y = 1; case E::B: return 2; } }\n"
                         "i32 main() { return f( E::A ); }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 1 );
    }

    SECTION( "main is not exempt" )
    {
        const Checked c( "i32 main() { }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 1 );
    }
}

// Each of these is a shape that could plausibly report and must not. The enum switch is the one
// worth having: it has no `default` and no block after it, so nothing falls through - and if the
// lowerer ever starts emitting one, this is what says so.
TEST_CASE( "assign_check_accepts_every_path_that_does_return", "[check][assign][returns]" )
{
    for( const char* source : {
             "i32 f( i32 x ) { if( x > 0 ) { return 1; } else { return 2; } }\ni32 main() { return f( 1 ); }",
             "enum E { A, B };\n"
             "i32 f( E e ) { switch( e ) { case E::A: return 1; case E::B: return 2; } }\n"
             "i32 main() { return f( E::A ); }",
             "i32 f( i32 x ) { switch( x ) { case 1: return 1; default: return 2; } }\ni32 main() { return f( 1 ); }",
             "enum E { A, B, C };\n"
             "i32 f( E e ) { switch( e ) { case E::A: case E::B: return 1; case E::C: return 2; } }\n"
             "i32 main() { return f( E::A ); }",
             "i32 f() { { { return 1; } } }\ni32 main() { return f(); }",
             "i32 f( i32 x ) { if( x > 0 ) { return 1; } else { if( x < 0 ) { return 2; } else { return 3; } } }\n"
             "i32 main() { return f( 0 ); }",
             "i32 f( out i32 a ) { a = 1; return 2; }\ni32 main() { i32 x; return f( out x ); }",
             "i32 f() { for( ; ; ) { return 1; } }\ni32 main() { return f(); }",
         } )
    {
        const Checked c( source );

        INFO( source << "\n" << c.rendered() );
        REQUIRE( c.errors().empty() );
    }
}

// A `void` return slot is not an obligation, so nothing that has one may report. Constructors and
// destructors are the cases that would be easy to miss - they have a return slot like any other
// function, and it is `void`.
TEST_CASE( "assign_check_leaves_void_functions_alone", "[check][assign][returns]" )
{
    for( const char* source : {
             "void g() { }\ni32 main() { g(); return 0; }",
             "void g() { return; }\ni32 main() { g(); return 0; }",
             "void g( i32 x ) { if( x > 0 ) { return; } }\ni32 main() { g( 1 ); return 0; }",
             "class C { i32 x; C( i32 v ) { x = v; } ~C() { } };\ni32 main() { C c = C( 1 ); return 0; }",
             "class C { i32 x; C( i32 v ) { x = v; } void set( i32 v ) { x = v; } };\n"
             "i32 main() { C c = C( 1 ); c.set( 2 ); return 0; }",
         } )
    {
        const Checked c( source );

        INFO( source << "\n" << c.rendered() );
        REQUIRE( c.errors().empty() );
    }
}

// The known false positive, pinned so that the day constant branches are folded this test says so
// rather than the behaviour changing quietly. `while( true )` leaves a loop-exit block that is
// reachable in the graph and never taken at run time; `for( ; ; )` has no condition, so there is no
// exit block at all - which makes the workaround the better code anyway.
TEST_CASE( "assign_check_reports_a_constantly_true_loop", "[check][assign][returns]" )
{
    const Checked flagged( "i32 f() { while( true ) { return 1; } }\ni32 main() { return f(); }" );

    INFO( flagged.rendered() );
    REQUIRE( flagged.errors().size() == 1 );

    const Checked clean( "i32 f() { for( ; ; ) { return 1; } }\ni32 main() { return f(); }" );

    INFO( clean.rendered() );
    REQUIRE( clean.errors().empty() );
}

TEST_CASE( "assign_check_accepts_an_out_parameter_assigned_on_every_path", "[check][assign]" )
{
    SECTION( "straight through" )
    {
        Checked p( "void init( out i32 n ) { n = 1; }\ni32 main() { i32 x = 0; init( out x ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }

    SECTION( "before a branch, so both arms inherit it" )
    {
        Checked p( "void init( out i32 n, i32 c ) { n = 1; if( c == 0 ) { n = 2; } }\n"
                   "i32 main() { i32 x = 0; init( out x, 1 ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors().empty() );
    }

    SECTION( "in both arms of a branch" )
    {
        Checked p( "void init( out i32 n, i32 c ) { if( c == 0 ) { n = 1; } else { n = 2; } }\n"
                   "i32 main() { i32 x = 0; init( out x, 1 ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors().empty() );
    }

    SECTION( "and before a loop, which may run zero times" )
    {
        Checked p( "void init( out i32 n, i32 c ) { n = 0; while( n < c ) { n = n + 1; } }\n"
                   "i32 main() { i32 x = 0; init( out x, 3 ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors().empty() );
    }
}

// The two failures read differently because they are different mistakes: one is a path the author
// missed, the other a parameter they never wrote to at all. That distinction is the only reason the
// pass tracks a second bit.
TEST_CASE( "assign_check_reports_an_out_parameter_that_can_go_unassigned", "[check][assign]" )
{
    SECTION( "never assigned" )
    {
        Checked p( "void init( out i32 n ) { }\ni32 main() { i32 x = 0; init( out x ); return x; }" );

        INFO( p.rendered() );

        const std::vector<Unassigned_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE_FALSE( errors[0].maybe );
        REQUIRE( p.name_of( errors[0] ) == "n" );
    }

    SECTION( "assigned in one arm only" )
    {
        Checked p( "void init( out i32 n, i32 c ) { if( c == 0 ) { n = 1; } }\n"
                   "i32 main() { i32 x = 0; init( out x, 1 ); return x; }" );

        INFO( p.rendered() );

        const std::vector<Unassigned_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].maybe );
    }

    // The case a *may* analysis gets wrong: a loop body assigns it, so "assigned somewhere" is
    // true, and the loop can still run zero times. This is why the pass exists separately from
    // check_moves rather than as a second question inside it.
    SECTION( "assigned only inside a loop body" )
    {
        Checked p( "void init( out i32 n, i32 c ) { while( c > 0 ) { n = 1; c = c - 1; } }\n"
                   "i32 main() { i32 x = 0; init( out x, 3 ); return x; }" );

        INFO( p.rendered() );

        const std::vector<Unassigned_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].maybe );
    }

    SECTION( "and on an early return that precedes the assignment" )
    {
        Checked p( "void init( out i32 n, i32 c ) { if( c == 0 ) { return; } n = 1; }\n"
                   "i32 main() { i32 x = 0; init( out x, 1 ); return x; }" );

        INFO( p.rendered() );

        const std::vector<Unassigned_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE_FALSE( errors[0].maybe ); // nothing assigned it on the path to that return
    }
}

// One error per return that can be reached unassigned, not one per visit. A back edge makes the
// worklist revisit a block, and reporting inside the fixpoint would multiply the message by however
// many times it converged - the same property check_moves pins for the same reason.
TEST_CASE( "assign_check_reports_once_across_a_back_edge", "[check][assign]" )
{
    Checked p( "void init( out i32 n, i32 c ) { while( c > 0 ) { c = c - 1; } }\n"
               "i32 main() { i32 x = 0; init( out x, 3 ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors().size() == 1 );
}

TEST_CASE( "assign_check_reports_each_out_parameter_separately", "[check][assign]" )
{
    Checked p( "void init( out i32 a, out i32 b ) { a = 1; }\n"
               "i32 main() { i32 x = 0; i32 y = 0; init( out x, out y ); return x + y; }" );

    INFO( p.rendered() );

    const std::vector<Unassigned_error> errors = p.errors();

    REQUIRE( errors.size() == 1 );
    REQUIRE( p.name_of( errors[0] ) == "b" );
}

// A function with no `out` parameters has nothing to prove, and says so before building any state -
// which is what keeps the pass free for the whole existing test suite.
TEST_CASE( "assign_check_says_nothing_about_a_function_with_no_out_parameters", "[check][assign]" )
{
    Checked p( "i32 add( i32 a, i32 b ) { return a + b; }\ni32 main() { return add( 1, 2 ); }" );

    INFO( p.rendered() );
    REQUIRE( p.errors().empty() );
}

// Forwarding: passing an `out` parameter straight on as an `out` argument satisfies it, because
// the callee's promise to assign it is what the marker at that call site means. It works because
// taking a place's address counts as assigning it - see transfer_block - which also means a
// `const ref` argument counts, so this errs towards missing a mistake rather than inventing one.
TEST_CASE( "assign_check_accepts_a_forwarded_out_parameter", "[check][assign]" )
{
    Checked p( "void init( out i32 n ) { n = 1; }\n"
               "void forward( out i32 n ) { init( out n ); }\n"
               "i32 main() { i32 x = 0; forward( out x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.errors().empty() );
}

// The cost of that rule, pinned so it is a known trade rather than a surprise: a `const ref`
// argument takes an address without assigning anything, and is treated as an assignment anyway.
// Sound in the direction that matters - it can only fail to report - and the fix is for KIR to
// record which arguments are `out`, which is worth doing when something needs it.
TEST_CASE( "assign_check_is_satisfied_by_any_address_taken_for_now", "[check][assign]" )
{
    Checked p( "i32 peek( const ref i32 n ) { return n; }\n"
               "void init( out i32 n ) { i32 ignored = peek( n ); }\n"
               "i32 main() { i32 x = 0; init( out x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors().empty() );
}

// The pass asks whether a place was written, never what was written into it. An `out` parameter of
// pointer type is the case where that distinction is visible: `nullptr` satisfies it as completely
// as a real address would, because "the callee assigned it" is the entire promise `out` makes.
TEST_CASE( "assign_check_asks_only_whether_a_place_was_written", "[check][assign]" )
{
    SECTION( "nullptr satisfies an out pointer" )
    {
        Checked p( "void to_null( out i32* p ) { p = nullptr; }\n"
                   "i32 main() { i32* a = nullptr; to_null( out a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }

    SECTION( "and so does an address that came from elsewhere" )
    {
        Checked p( "void pass_on( i32* q, out i32* p ) { p = q; }\n"
                   "i32 main() { i32 v = 7; i32* b = nullptr; pass_on( &v, out b ); return *b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }

    // And the consequence, pinned because it is deliberate rather than missed: a raw pointer is
    // outside §8's structural rule, so a callee may hand back the address of one of its own locals
    // and nothing objects. `ref` is the spelling that cannot dangle; `T*` is the one that can, and
    // D2 already says an address tells you nothing about who owns what is at the other end.
    SECTION( "including the address of a local, which dangles and is still accepted" )
    {
        Checked p( "void escapes( out i32* p ) { i32 local = 7; p = &local; }\n"
                   "i32 main() { i32* a = nullptr; escapes( out a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }
}

// A known limitation, pinned so that fixing it is a deliberate change rather than a surprise: the
// analysis is per local, so writing one field of a struct `out` parameter satisfies it. Partial
// initialisation is the same granularity problem that stops a field being moved on its own, and it
// wants the same answer - per-field state - whenever either is addressed.
TEST_CASE( "assign_check_accepts_a_partly_initialised_struct_for_now", "[check][assign]" )
{
    Checked p( "struct P { i32 x; i32 y; };\n"
               "void init( out P p ) { p.x = 1; }\n"
               "i32 main() { P v = P { 0, 0 }; init( out v ); return v.x; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors().empty() );
}

} // namespace keel
#endif
