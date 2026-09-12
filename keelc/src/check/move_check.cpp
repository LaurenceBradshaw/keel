#include "check/move_check.h"

namespace keel
{

namespace
{

// PLAN §8. Uninitialised is the bottom - "no information yet" - so joining it with anything gives
// the other side, which is what lets a block start empty and be filled by its predecessors.
enum class State : u8
{
    Uninitialised,
    Live,
    Moved,
    Maybe_moved
};

// Per-local state, plus where the move happened, so a diagnostic can point at both ends.
struct Flow
{
    std::vector<State> state;
    std::vector<Span>  moved_at;
};

State join( State a, State b )
{
    if( a == b )
        return a;
    if( a == State::Uninitialised )
        return b;
    if( b == State::Uninitialised )
        return a;

    return State::Maybe_moved; // Live vs Moved, or either with Maybe_moved
}

// The earlier of the two, so the message does not depend on the order blocks came off the worklist.
Span join_span( Span a, Span b )
{
    if( !a.file.is_valid() )
        return b;
    if( !b.file.is_valid() )
        return a;

    return a.start <= b.start ? a : b;
}

// Merges `from` into `into`, returning whether anything changed. That answer is the worklist's
// only termination signal.
bool merge_into( Flow& into, const Flow& from )
{
    bool changed = false;

    for( std::size_t i = 0; i < into.state.size(); ++i )
    {
        const State merged = join( into.state[i], from.state[i] );
        const Span  span   = join_span( into.moved_at[i], from.moved_at[i] );

        // The span changing counts: two paths can agree a local is Moved while disagreeing
        // about where, and the message names the place.
        changed = changed || merged != into.state[i] || span.start != into.moved_at[i].start;

        into.state[i]    = merged;
        into.moved_at[i] = span;
    }

    return changed;
}

// 0, 1 or 2 of them, by terminator kind.
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
    case Terminator_kind::Return:
    case Terminator_kind::Unreachable:
    case Terminator_kind::Unset:
        return;
    }
}

// Reads one operand: reports if it names a moved local, then applies the move if it is one.
// `errors` is null during the fixpoint and non-null on the reporting pass - which is what lets
// both use this rather than keeping two traversals in step by hand.
void read_operand( const Operand& operand, Span span, Flow& flow, std::vector<Move_error>* errors )
{
    // A constant names no local, and a global has no scope to be moved out of.
    if( operand.kind == Operand_kind::Constant || operand.place.is_global() )
    {
        return;
    }

    // Nothing can be moved but a whole named local, so a projection here is only ever a read.
    const u32   local = operand.place.local.v;
    const State state = flow.state[local];

    if( errors != nullptr && ( state == State::Moved || state == State::Maybe_moved ) )
    {
        errors->push_back( Move_error {
            .local = operand.place.local, .use = span, .moved = flow.moved_at[local], .maybe = state == State::Maybe_moved
        } );
    }

    // Moveing twice is itself a use-after-move, so this runs after the check rather than instead.
    if( operand.kind == Operand_kind::Move && operand.place.num_projections == 0 )
    {
        flow.state[local]    = State::Moved;
        flow.moved_at[local] = span;
    }
}

// value.a, value.b, and operands[first_argument .. + argument_count].
void read_rvalue( const Function& func, const Rvalue& value, Span span, Flow& flow, std::vector<Move_error>* errors )
{
    // a and b cover Use, Binary, Unary and Cast; the argument range cover Call. An Rvalue leaves
    // the operands its kind does not use at their Constant default, so reading all of them is safe
    // and needs no switch - which is the point of that default in kir.h
    read_operand( value.a, span, flow, errors );
    read_operand( value.b, span, flow, errors );

    for( u32 i = 0; i < value.argument_count; ++i )
    {
        read_operand( func.operands[value.first_argument + i], span, flow, errors );
    }

    // Address_of reads a place rather than an operand, and taking the address of a moved local is
    // not a use of its value - so there is deliberately nothing here for it.
}

// Every statement of a block, then its terminator's condition. Mutates `flow` in place.
void transfer_block( const Function& func, u32 block, Flow& flow, std::vector<Move_error>* errors )
{
    const Block& b = func.blocks[block];

    for( u32 i = 0; i < b.statement_count; ++i )
    {
        const Statement& statement = func.statements[b.first_statement + i];

        switch( statement.kind )
        {
        case Statement_kind::Assign:
            // Reads before the write, which is the order the statement happens in:
            // // `_1 = move _1 + 1` reads _1 and then re-initialises it, and is legal
            read_rvalue( func, statement.value, statement.span, flow, errors );

            // A projected target counts too: nothing can be partially moved, so `_1.x = ...` is a
            // step in building _1 rather than a write into a half-moved object. Without this a
            // struct literal is never Live - it is assembled entirely through projections - so in a
            // loop its temporary reports a false use-after-move on the second iteration.
            if( !statement.place.is_global() )
            {
                flow.state[statement.place.local.v]    = State::Live;
                flow.moved_at[statement.place.local.v] = Span {};
            }
            break;
        case Statement_kind::Storage_live:
        case Statement_kind::Storage_dead:
            // Storage begins and ends empty either way. Both only ever name a whole local -
            // verify's check_place already rejects a projection here.
            flow.state[statement.place.local.v]    = State::Uninitialised;
            flow.moved_at[statement.place.local.v] = Span {};
            break;
        case Statement_kind::Drop:
            // Deliberately silent. A drop of a moved value is not the author's mistake - eliding it
            // is drop elaboration's job, and reporting it here would blame them for where the
            // compiler chose to put a drop.
            break;
        }
    }

    // Read on the way out, after every statement. A non-Branch terminator leaves `condition` at its
    // default, which is Operand_kind::Constant, so read_operand returns immediately - no kind test
    // is needed here.
    read_operand( b.terminator.condition, b.terminator.span, flow, errors );
}

// Parameters Live, everything else Uninitialised. Locals 1..parameter_count are the parameters,
// in declaration order.
Flow entry_flow( const Function& func )
{
    Flow flow {
        std::vector<State>( func.locals.size(), State::Uninitialised ), std::vector<Span>( func.locals.size(), Span {} )
    };

    for( u32 i = 1; i <= func.parameter_count; ++i )
    {
        flow.state[i] = State::Live;
    }

    return flow;
}

} // namespace

std::vector<Move_error> check_moves( const Function& func )
{
    const Flow bottom = {
        std::vector<State>( func.locals.size(), State::Uninitialised ), std::vector<Span>( func.locals.size(), Span {} )
    };

    std::vector<Flow> in( func.blocks.size(), bottom );
    in[0] = entry_flow( func );

    std::vector<Block_id> work { Block_id { 0 } };
    std::vector<Block_id> next;

    // Forward, pushing into successors rather than pulling from predecessors, so no predecessor map
    // is needed. Terminates because there are four states, join never moves a local back down the
    // lattice, and there are finitely many blocks - so merge_into eventually returns false
    // everywhere and the worklist drains.
    while( !work.empty() )
    {
        const Block_id block = work.back();
        work.pop_back();

        Flow out = in[block.v];
        transfer_block( func, block.v, out, nullptr );

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

    // A second walk, each block exactly once. Reporting inside the loop above would emit one error
    // per visit for any block on a back edge - which is what the "reports once" test pins.
    std::vector<Move_error> errors;

    for( u32 block = 0; block < func.blocks.size(); ++block )
    {
        Flow flow = in[block];
        transfer_block( func, block, in[block], &errors );
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

// Real source rather than hand-built CFGs: what needs testing here is what happens at a branch, a
// join and a back edge, and those are precisely the graphs that are worst to assemble by hand.
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

    // The function under test is always the last one lowered: every fixture below puts the
    // interesting code in main, and declares whatever it calls above it.
    std::vector<Move_error> errors() const
    {
        return check_moves( functions.back() );
    }

    std::string_view text_at( Span span ) const
    {
        return sm.text( span );
    }
};

// A void sink, so a move has somewhere to go that says nothing else about the value.
constexpr std::string_view k_sink = "void sink( i32 x ) { }\n";

} // namespace

TEST_CASE( "move_check_accepts_a_program_with_no_moves", "[check][move]" )
{
    const Checked p( std::string( k_sink ) + "i32 main() { i32 a = 1; sink( a ); sink( a ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.errors().empty() );
}

TEST_CASE( "move_check_finds_a_use_after_move", "[check][move]" )
{
    SECTION( "a read after the move" )
    {
        const Checked p( std::string( k_sink ) + "i32 main() { i32 a = 1; sink( move a ); sink( a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::vector<Move_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE_FALSE( errors[0].maybe );
    }

    // Moving twice is itself a use of something already gone, which is why the report runs before
    // the state changes rather than instead of it.
    SECTION( "moving twice" )
    {
        const Checked p( std::string( k_sink ) + "i32 main() { i32 a = 1; sink( move a ); sink( move a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors().size() == 1 );
    }

    SECTION( "using it before the move is fine" )
    {
        const Checked p( std::string( k_sink ) + "i32 main() { i32 a = 1; sink( a ); sink( move a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors().empty() );
    }

    // Re-initialising clears both the state and the recorded move, or a later join would carry a
    // span pointing at a move the program already recovered from.
    SECTION( "assigning it again brings it back" )
    {
        const Checked p( std::string( k_sink ) + "i32 main() { i32 a = 1; sink( move a ); a = 2; sink( a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors().empty() );
    }

    // The read happens before the write, so a self-referential assignment is legal. Parenthesised
    // because a marker takes the whole expression to its boundary: `move a + 1` is `move ( a + 1 )`,
    // which is not a variable and is refused.
    SECTION( "a move on the right of an assignment to the same local" )
    {
        const Checked p( "i32 main() { i32 a = 1; a = ( move a ) + 1; return a; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }
}

// The join is the whole reason this is a dataflow rather than a walk.
TEST_CASE( "move_check_joins_branches", "[check][move]" )
{
    SECTION( "moved on one path only is a maybe" )
    {
        const Checked p(
            std::string( k_sink ) + "i32 main( ) { i32 a = 1; i32 c = 0;\n"
                                    "  if ( c == 0 ) { sink( move a ); }\n"
                                    "  sink( a );\n"
                                    "  return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::vector<Move_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].maybe );
    }

    SECTION( "moved on every path is not a maybe" )
    {
        const Checked p(
            std::string( k_sink ) + "i32 main( ) { i32 a = 1; i32 c = 0;\n"
                                    "  if ( c == 0 ) { sink( move a ); } else { sink( move a ); }\n"
                                    "  sink( a );\n"
                                    "  return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::vector<Move_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE_FALSE( errors[0].maybe );
    }

    // The other arm never sees the move, so nothing is wrong there.
    SECTION( "a use on the arm that did not move is clean" )
    {
        const Checked p(
            std::string( k_sink ) + "i32 main( ) { i32 a = 1; i32 c = 0;\n"
                                    "  if ( c == 0 ) { sink( move a ); } else { sink( a ); }\n"
                                    "  return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }
}

// A back edge is what makes this need a fixpoint rather than one pass.
TEST_CASE( "move_check_follows_a_back_edge", "[check][move]" )
{
    SECTION( "a move in a loop body is a use on the next iteration" )
    {
        const Checked p(
            std::string( k_sink ) + "i32 main( ) { i32 a = 1; i32 i = 0;\n"
                                    "  while ( i < 3 ) { sink( move a ); i = i + 1; }\n"
                                    "  return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE_FALSE( p.errors().empty() );
    }

    // Reaching here at all is the assertion: a lattice that did not stop rising would not return.
    SECTION( "and it terminates" )
    {
        const Checked p(
            std::string( k_sink ) + "i32 main( ) { i32 a = 1; i32 i = 0;\n"
                                    "  while ( i < 3 ) { sink( a ); i = i + 1; }\n"
                                    "  return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors().empty() );
    }

    // Two bad sites here, and both are real: `sink( a )` reads what the move above it killed, and
    // `sink( move a )` reads what the back edge left moved. What must not happen is either being
    // reported once per visit - which is why reporting is a second walk rather than part of the
    // fixpoint.
    SECTION( "each bad site is reported once, however often its block is visited" )
    {
        const Checked p(
            std::string( k_sink ) + "i32 main( ) { i32 a = 1; i32 i = 0;\n"
                                    "  while ( i < 3 ) { sink( move a ); sink( a ); i = i + 1; }\n"
                                    "  return 0; }"
        );

        INFO( p.rendered() );

        const std::vector<Move_error> errors = p.errors();

        REQUIRE( errors.size() == 2 );
        REQUIRE( errors[0].use.start != errors[1].use.start );
    }
}

TEST_CASE( "move_check_tracks_parameters", "[check][move]" )
{
    const Checked p(
        std::string( k_sink ) + "i32 take( i32 n ) { sink( move n ); sink( n ); return 0; }\n"
                                "i32 main() { return take( 1 ); }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( check_moves( p.functions[1] ).size() == 1 );
}

// The spans are what the driver turns into a message, so they have to name the right places.
TEST_CASE( "move_check_reports_both_ends", "[check][move]" )
{
    const Checked p( std::string( k_sink ) + "i32 main() { i32 a = 1; sink( move a ); sink( a ); return 0; }" );

    INFO( p.rendered() );

    const std::vector<Move_error> errors = p.errors();

    REQUIRE( errors.size() == 1 );
    REQUIRE( p.text_at( errors[0].moved ).find( "move a" ) != std::string_view::npos );
    REQUIRE( p.text_at( errors[0].use ).find( "sink( a )" ) != std::string_view::npos );
}

// Drop elaboration puts a drop at every scope exit, including for a local that has been moved out
// of. Eliding it is drop elaboration's job; blaming the author for it here would be wrong.
TEST_CASE( "move_check_says_nothing_about_drops", "[check][move]" )
{
    const Checked p( "class Buffer { u64 len; ~Buffer() { } };\n"
                     "void sink( move Buffer b ) { }\n"
                     "i32 main() { Buffer b = Buffer { 1 }; sink( move b ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.errors().empty() );
}

} // namespace keel
#endif
