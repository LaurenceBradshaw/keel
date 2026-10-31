// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/move_check.h"

#include <span>
#include "check/borrows.h"
#include "ir/slots.h"

namespace keel
{

namespace
{

// PLAN §8. Uninitialised is the bottom, so a block starts empty and is filled by its predecessors.
enum class State : u8
{
    Uninitialised,
    Live,
    Moved,
    Maybe_moved
};

// Per-slot state, plus where the move happened, so a diagnostic can point at both ends.
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

// Merges `from` into `into`; whether anything changed is the worklist's termination signal.
bool merge_into( Flow& into, const Flow& from )
{
    bool changed = false;

    for( std::size_t i = 0; i < into.state.size(); ++i )
    {
        const State merged = join( into.state[i], from.state[i] );
        const Span  span   = join_span( into.moved_at[i], from.moved_at[i] );

        // Two paths can agree a local is Moved and disagree about where, and the message names the place.
        changed = changed || merged != into.state[i] || span.start != into.moved_at[i].start;

        into.state[i]    = merged;
        into.moved_at[i] = span;
    }

    return changed;
}

// Reports a read of `range` if a leaf of it is moved; whether one was. The error names the place
// read, and the part moved when one move did not take all of it.
bool check_leaves(
    const Slot_map& slots, Local_id local, const Slots& range, Span span, const Flow& flow, std::vector<Move_error>* errors
)
{
    u32  first = k_no_field;
    bool all   = true;
    bool maybe = false;

    for( u32 leaf = range.first; leaf < range.last; ++leaf )
    {
        if( !slots.is_leaf( leaf ) )
        {
            continue;
        }

        const State state = flow.state[leaf];
        if( state == State::Moved || state == State::Maybe_moved )
        {
            first = first == k_no_field ? leaf : first;
            maybe = maybe || state == State::Maybe_moved;
        }
        else
        {
            all = false;
        }
    }

    if( first != k_no_field && errors != nullptr )
    {
        const Span moved     = flow.moved_at[first];
        const auto same_move = [&]( u32 slot )
        {
            const Slots under = slots.under( slot );
            for( u32 leaf = under.first; leaf < under.last; ++leaf )
            {
                if( slots.is_leaf( leaf ) && ( flow.state[leaf] == State::Live || flow.state[leaf] == State::Uninitialised ||
                                               flow.moved_at[leaf].start != moved.start ) )
                {
                    return false;
                }
            }
            return true;
        };

        // The largest part under the place read that the first move took whole.
        u32 part = k_no_field;
        if( !same_move( range.node ) )
        {
            part = first;
            while( slots.parent( part ) != range.node && same_move( slots.parent( part ) ) )
            {
                part = slots.parent( part );
            }
        }

        errors->push_back( Move_error {
            .local      = local,
            .field      = slots.field( range.node ),
            .moved_part = part == k_no_field ? k_no_field : slots.field( part ),
            .use        = span,
            .moved      = moved,
            .maybe      = all ? maybe : flow.state[first] == State::Maybe_moved
        } );
    }

    return first != k_no_field;
}

void set_leaves( const Slots& range, Flow& flow, State state, Span span )
{
    for( u32 leaf = range.first; leaf < range.last; ++leaf )
    {
        flow.state[leaf]    = state;
        flow.moved_at[leaf] = span;
    }
}

// Reports a read of a moved leaf, then applies a move. `errors` is null during the fixpoint, so
// both walks share one traversal.
void read_operand(
    const Function& func, const Slot_map& slots, const Operand& operand, Span span, Flow& flow, std::vector<Move_error>* errors
)
{
    // Only a copy or move names a local, and a global has no scope to be moved out of.
    if( !operand.has_place() || operand.place.is_global() )
    {
        return;
    }

    const Slots range = slots.of( operand.place );
    check_leaves( slots, operand.place.local, range, span, flow, errors );

    // Moving twice is a use after move, so this runs after the check.
    if( operand.kind == Operand_kind::Move && is_field_path( func, operand.place ) && range.exact )
    {
        set_leaves( range, flow, State::Moved, span );
    }
}

// value.a, value.b, and operands[first_argument .. + argument_count].
void read_rvalue(
    const Function& func, const Slot_map& slots, const Rvalue& value, Span span, Flow& flow, std::vector<Move_error>* errors
)
{
    if( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() )
    {
        const Slots range = slots.of( value.a.place );

        // Only a tracked node is refilled; `out b.n` into a moved `b` is a use, as `b.n = ...` is.
        if( value.address_purpose == Address_purpose::Initialise && range.exact )
        {
            set_leaves( range, flow, State::Live, Span {} );
        }
        else
        {
            check_leaves( slots, value.a.place.local, range, span, flow, errors );
        }
    }

    // a and b cover Use, Binary, Unary and Cast, the argument range Call; an unused operand is a
    // Constant, so reading all of them needs no switch.
    read_operand( func, slots, value.a, span, flow, errors );
    read_operand( func, slots, value.b, span, flow, errors );

    for( u32 i = 0; i < value.argument_count; ++i )
    {
        read_operand( func, slots, func.operands[value.first_argument + i], span, flow, errors );
    }

    // Taking a moved local's address is not a use of its value, so Address_of reads nothing here.
}

// Every statement of a block, then its terminator's condition. Mutates `flow` in place.
void transfer_block( const Function& func, const Slot_map& slots, Block_id block, Flow& flow, std::vector<Move_error>* errors )
{
    const Block& b = func.blocks[block];

    for( const Statement& statement : func.statements_of( b ) )
    {
        switch( statement.kind )
        {
        case Statement_kind::Assign:
        {
            // Reads before the write: `_1 = move _1 + 1` is legal.
            read_rvalue( func, slots, statement.value, statement.span, flow, errors );

            if( statement.place.is_global() )
            {
                break;
            }

            // `p.a = ...` revives `p.a`. `b.n = ...` into a moved `b` tracked whole is a use: it would
            // fill a field and leave the rest gone.
            const Slots range = slots.of( statement.place );
            if( !range.exact && check_leaves( slots, statement.place.local, range, statement.span, flow, errors ) )
            {
                break;
            }

            set_leaves( range, flow, State::Live, Span {} );
            break;
        }
        case Statement_kind::Storage_live:
        case Statement_kind::Storage_dead:
            // Both name a whole local (verify's check_place), and storage begins and ends empty.
            set_leaves( slots.under( statement.place.local.v ), flow, State::Uninitialised, Span {} );
            break;
        case Statement_kind::Drop:
            // Silent: eliding a drop of a moved value is drop elaboration's job, not the author's mistake.
            break;
        }
    }

    // The terminator's operand, after every statement; one it lacks is a Constant and reads nothing.
    read_operand( func, slots, b.terminator.condition, b.terminator.span, flow, errors );
    read_operand( func, slots, b.terminator.message, b.terminator.span, flow, errors );
    read_operand( func, slots, b.terminator.at, b.terminator.span, flow, errors );
}

// A call handed a local both by `move` and by address: the callee would own it and borrow it at once.
// Every borrowed argument is lowered to an address taken before the call, so the flow never sees it.
void find_moves_into_borrowing_calls(
    const Function& func, const Slot_map& slots, const std::vector<Borrow>& borrows, std::vector<Move_error>& errors
)
{
    for( const Statement& statement : func.statements )
    {
        const Rvalue& value = statement.value;

        if( statement.kind != Statement_kind::Assign ||
            ( value.kind != Rvalue_kind::Call && value.kind != Rvalue_kind::Indirect_call ) )
        {
            continue;
        }

        const std::span<const Operand> arguments( func.operands.data() + value.first_argument, value.argument_count );

        for( const Operand& moved : arguments )
        {
            if( moved.kind != Operand_kind::Move || !is_field_path( func, moved.place ) )
            {
                continue;
            }

            for( const Operand& borrowed : arguments )
            {
                if( borrowed.kind != Operand_kind::Copy || borrowed.place.is_global() || borrowed.place.num_projections != 0 )
                {
                    continue;
                }

                const auto hit = [&]( const Borrow& borrow )
                {
                    return ( borrow.of == moved.place.local && overlaps( func, borrows, borrow.place, moved.place ) ) ||
                           ( borrow.through != nullptr && reaches( func, borrows, borrow, moved.place ) );
                };
                const Borrow& borrow = borrows[borrowed.place.local.v];

                if( any_choice( borrows, borrow, hit ) )
                {
                    errors.push_back( Move_error {
                        .local    = moved.place.local,
                        .field    = slots.field( slots.of( moved.place ).node ),
                        .use      = borrow.at,
                        .moved    = statement.span,
                        .conflict = Call_conflict::Moved_and_borrowed
                    } );
                }
            }
        }
    }
}

// D54: each borrowed argument is a loan for the whole call, so an element may not travel beside a
// borrow of its whole that could change it, one place may not be `out` twice, and one object may
// not travel under two names when the call could change or, through `out`, destroy it.
void find_aliased_arguments(
    const Function& func, const std::vector<Borrow>& borrows, const std::vector<bool>& owning, std::vector<Move_error>& errors
)
{
    const auto borrow_of = [&]( const Operand& operand ) -> const Borrow*
    {
        if( operand.kind != Operand_kind::Copy || operand.place.is_global() || operand.place.num_projections != 0 )
        {
            return nullptr;
        }

        const Borrow& borrow = borrows[operand.place.local.v];

        return borrow.of.is_valid() || borrow.through != nullptr || !borrow.choices.empty() ? &borrow : nullptr;
    };

    for( const Statement& statement : func.statements )
    {
        const Rvalue& value = statement.value;

        if( statement.kind != Statement_kind::Assign ||
            ( value.kind != Rvalue_kind::Call && value.kind != Rvalue_kind::Indirect_call ) )
        {
            continue;
        }

        const std::span<const Operand> arguments( func.operands.data() + value.first_argument, value.argument_count );

        // D75: each arm of a chosen argument is an argument of its own.
        std::vector<std::pair<std::size_t, const Borrow*>> loans;
        for( std::size_t i = 0; i < arguments.size(); ++i )
        {
            if( const Borrow* borrow = borrow_of( arguments[i] ); borrow != nullptr )
            {
                for_each_choice( borrows, *borrow, [&]( const Borrow& choice ) { loans.emplace_back( i, &choice ); } );
            }
        }

        for( std::size_t x = 0; x < loans.size(); ++x )
        {
            for( std::size_t y = 0; y < loans.size(); ++y )
            {
                const auto [i, a] = loans[x];
                const auto [j, b] = loans[y];

                if( i == j || ( !a->of.is_valid() && a->through == nullptr ) || ( !b->of.is_valid() && b->through == nullptr ) )
                {
                    continue;
                }

                // Only an owner can be freed under the other name, so `v.grow( v.x )` is accepted.
                if( i < j && a->of.is_valid() && b->of.is_valid() && overlaps( func, borrows, a->place, b->place ) &&
                    owning[arguments[i].place.local.v] && owning[arguments[j].place.local.v] &&
                    ( a->purpose != Address_purpose::Read || b->purpose != Address_purpose::Read ) &&
                    !( a->purpose == Address_purpose::Initialise && b->purpose == Address_purpose::Initialise ) )
                {
                    errors.push_back( Move_error {
                        .local    = b->of,
                        .use      = b->at,
                        .maybe    = may_differ( func, borrows, a->place, b->place ),
                        .conflict = Call_conflict::Aliased,
                        .other    = a->at
                    } );
                }

                // Reported at the later of the two.
                if( i < j && a->purpose == Address_purpose::Initialise && b->purpose == Address_purpose::Initialise &&
                    a->of.is_valid() && b->of.is_valid() && overlaps( func, borrows, a->place, b->place ) )
                {
                    errors.push_back( Move_error {
                        .local    = b->of,
                        .use      = b->at,
                        .maybe    = may_differ( func, borrows, a->place, b->place ),
                        .conflict = Call_conflict::Out_twice,
                        .other    = a->at
                    } );
                }

                // Only `a` can be the element, so a pair is reported once.
                if( a->through != nullptr && b->through == nullptr && b->of.is_valid() && b->purpose != Address_purpose::Read &&
                    reaches( func, borrows, *a, b->place ) )
                {
                    errors.push_back( Move_error {
                        .local = b->of, .use = a->at, .conflict = Call_conflict::Element_and_whole, .other = b->at
                    } );
                }
            }
        }
    }
}

Flow bottom( const Slot_map& slots )
{
    return Flow { std::vector<State>( slots.count(), State::Uninitialised ), std::vector<Span>( slots.count(), Span {} ) };
}

// Parameters Live, everything else Uninitialised. Locals 1..parameter_count are the parameters,
// in declaration order.
Flow entry_flow( const Function& func, const Slot_map& slots )
{
    Flow flow = bottom( slots );

    for( u32 i = 1; i <= func.parameter_count; ++i )
    {
        set_leaves( slots.under( i ), flow, State::Live, Span {} );
    }

    return flow;
}

} // namespace

std::vector<Move_error> check_moves(
    const Function& func, const std::vector<bool>& owning, const std::vector<bool>& addressing, const Literal_pool& literals
)
{
    const Slot_map    slots( func );
    std::vector<Flow> in( func.blocks.size(), bottom( slots ) );
    in[0] = entry_flow( func, slots );

    std::vector<Block_id> work { Block_id { 0 } };
    std::vector<Block_id> next;

    // Forward, pushing into successors. Terminates: four states, join only rises, finitely many blocks.
    while( !work.empty() )
    {
        const Block_id block = work.back();
        work.pop_back();

        Flow out = in[block.v];
        transfer_block( func, slots, block, out, nullptr );

        next.clear();
        successors( func.blocks[block].terminator, next );

        for( const Block_id target : next )
        {
            if( merge_into( in[target.v], out ) )
            {
                work.push_back( target );
            }
        }
    }

    const std::vector<Borrow> borrows = borrows_of( func, addressing, literals );

    // A second walk, each block once, so a block on a back edge reports once.
    std::vector<Move_error> errors;

    for( const Block_id block : func.blocks.ids() )
    {
        Flow flow = in[block.v];
        transfer_block( func, slots, block, in[block.v], &errors );
    }

    find_moves_into_borrowing_calls( func, slots, borrows, errors );
    find_aliased_arguments( func, borrows, owning, errors );

    return errors;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/borrow_check.h"
#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

// Real source rather than hand-built CFGs: branches, joins and back edges are worst built by hand.
struct Checked : Compiled
{
    explicit Checked( std::string_view source )
        : Compiled( source )
    {
        for( const Function& function : functions )
        {
            owning.push_back( owning_locals( function, ast, types ) );
            addressing.push_back( addressing_locals( function, ast, types ) );
        }
    }

    // The function under test is the last one lowered, `main`.
    std::vector<Move_error> errors() const
    {
        return check_moves( functions.back(), owning.back(), addressing.back(), literals );
    }

    // Every function's: an array's drop, `~V[2]`, is lowered after `main`.
    std::vector<Move_error> all_errors() const
    {
        std::vector<Move_error> errors;
        for( std::size_t f = 0; f < functions.size(); ++f )
        {
            const std::vector<Move_error> found = check_moves( functions[f], owning[f], addressing[f], literals );
            errors.insert( errors.end(), found.begin(), found.end() );
        }
        return errors;
    }

    std::vector<std::vector<bool>> owning;     // per function
    std::vector<std::vector<bool>> addressing; // per function

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

    // Reported before the state changes, not instead of changing it.
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

    // Re-initialising clears the recorded move too, or a later join would name a move already undone.
    SECTION( "assigning it again brings it back" )
    {
        const Checked p( std::string( k_sink ) + "i32 main() { i32 a = 1; sink( move a ); a = 2; sink( a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors().empty() );
    }

    // Read before written, so this is legal. Parenthesised because `move a + 1` is `move ( a + 1 )`.
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

    // Both sites are real; neither may be reported once per visit.
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

    // Constructed through its address, so only its storage starting says it is fresh.
    SECTION( "a constructed temporary is a new value on each iteration" )
    {
        const Checked p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                         "class H { public u64 n; H() { n = 0; } public void take( move B b ) { } };\n"
                         "void take( move B b ) { }\n"
                         "i32 main( ) { H h = H(); u64 i = 0;\n"
                         "  while ( i < 3 ) { take( move B( i ) ); h.take( move B( i ) ); i = i + 1; }\n"
                         "  return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }

    // Built field by field, so again only its storage starting says it is fresh.
    SECTION( "a struct literal is a new value on each iteration" )
    {
        const Checked p( "class P { public u64 n; ~P() { } };\n"
                         "void eat( move P p ) { }\n"
                         "i32 main( ) { u64 i = 0;\n"
                         "  while ( i < 3 ) { eat( move P { i } ); i = i + 1; }\n"
                         "  return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }
}

// A write through a projection fills part of a value, so it cannot bring a moved one back.
TEST_CASE( "move_check_finds_a_write_into_a_moved_local", "[check][move]" )
{
    constexpr std::string_view k_owned = "class B { public u64 n; B() { n = 0; } ~B() { } };\n"
                                         "void take( move B b ) { }\n"
                                         "void fill( out u64 n ) { n = 1; }\n";

    for( const char* write : { "b.n = 4;", "fill( out b.n );" } )
    {
        const Checked moved( std::string( k_owned ) + "i32 main() { B b = B(); take( move b ); " + write + " return 0; }" );

        INFO( write << "\n" << moved.rendered() );
        REQUIRE( moved.clean() );
        REQUIRE( moved.errors().size() == 1 );
        REQUIRE_FALSE( moved.errors()[0].maybe );

        const Checked maybe(
            std::string( k_owned ) + "i32 main() { B b = B(); u64 c = 0; if ( c == 0 ) { take( move b ); } " + write +
            " return 0; }"
        );

        INFO( maybe.rendered() );
        REQUIRE( maybe.clean() );
        REQUIRE( maybe.errors().size() == 1 );
        REQUIRE( maybe.errors()[0].maybe );

        const Checked live( std::string( k_owned ) + "i32 main() { B b = B(); " + write + " take( move b ); return 0; }" );

        INFO( live.rendered() );
        REQUIRE( live.errors().empty() );
    }
}

// A struct without a destructor is tracked per field, so writing one field of a moved value revives
// that field alone, and a whole read then names the field still moved.
TEST_CASE( "move_check_tracks_each_field_of_a_struct", "[check][move]" )
{
    constexpr std::string_view k_pair = "struct P { i32 a; i32 b; };\n"
                                        "void take( move P p ) { }\n"
                                        "void look( P p ) { }\n";

    const Checked half(
        std::string( k_pair ) + "i32 main() { P p = P { 1, 2 }; take( move p ); p.a = 3; look( p ); return 0; }"
    );

    INFO( half.rendered() );
    REQUIRE( half.clean() );

    const std::vector<Move_error> errors = half.errors();

    REQUIRE( errors.size() == 1 );
    REQUIRE( errors[0].moved_part != k_no_field );
    REQUIRE( half.ast.name( half.functions.back().fields[errors[0].moved_part].field ) == half.interner.find( "b" ) );

    const Checked whole( std::string( k_pair ) + "i32 main() { P p = P { 1, 2 }; take( move p ); look( p ); return 0; }" );

    INFO( whole.rendered() );
    REQUIRE( whole.errors().size() == 1 );
    REQUIRE( whole.errors()[0].field == k_no_field );

    const Checked refilled(
        std::string( k_pair ) + "i32 main() { P p = P { 1, 2 }; take( move p ); p.a = 3; p.b = 4; look( p ); return 0; }"
    );

    INFO( refilled.rendered() );
    REQUIRE( refilled.errors().empty() );
}

TEST_CASE( "move_check_tracks_a_field_moved_on_its_own", "[check][move]" )
{
    constexpr std::string_view k_pair = "class Owned { u64 id; Owned( u64 n ) { id = n; } ~Owned() { } };\n"
                                        "class P { public Owned a; public Owned b; };\n"
                                        "void take( move Owned o ) { }\n"
                                        "void keep( move P p ) { }\n";

    const Checked other(
        std::string( k_pair ) +
        "i32 main() { P p; p.a = Owned( 1 ); p.b = Owned( 2 ); take( move p.a ); take( move p.b ); return 0; }"
    );

    INFO( other.rendered() );
    REQUIRE( other.clean() );
    REQUIRE( other.errors().empty() );

    const Checked whole(
        std::string( k_pair ) +
        "i32 main() { P p; p.a = Owned( 1 ); p.b = Owned( 2 ); take( move p.a ); keep( move p ); return 0; }"
    );

    INFO( whole.rendered() );
    REQUIRE( whole.clean() );
    REQUIRE( whole.errors().size() == 1 );
    REQUIRE( whole.errors()[0].moved_part != k_no_field );
    REQUIRE(
        whole.ast.name( whole.functions.back().fields[whole.errors()[0].moved_part].field ) == whole.interner.find( "a" )
    );
}

// Every borrowed argument's address is taken before the call, so the move inside it is checked
// against the call's own arguments rather than the flow.
TEST_CASE( "move_check_refuses_a_move_and_a_borrow_in_one_call", "[check][move]" )
{
    constexpr std::string_view k_owned = "class B { public u64 n; B() { n = 0; } ~B() { } public void eat( move B o ) { } "
                                         "public bool operator==( move B o ) const { return true; } };\n"
                                         "void two( move B a, ref B b ) { }\n"
                                         "void owt( ref B b, move B a ) { }\n"
                                         "void read( const ref B b, move B a ) { }\n"
                                         "void count( u64 n, move B a ) { }\n"
                                         "void take( move B b ) { }\n";

    for( const char* call :
         { "two( move b, ref b );",
           "owt( ref b, move b );",
           "read( b, move b );",
           "b.eat( move b );",
           "bool r = b == move b;",
           "ref B r = b; owt( ref r, move b );",
           "fn( ref B, move B ) -> void f = &owt; f( ref b, move b );" } )
    {
        const Checked p( std::string( k_owned ) + "i32 main() { B b = B(); " + call + " return 0; }" );

        INFO( call << "\n" << p.rendered() );
        REQUIRE( p.clean() );

        const std::vector<Move_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].conflict == Call_conflict::Moved_and_borrowed );
    }

    // A copied field is read before the call, and borrowing one local while moving another is fine.
    for( const char* call :
         { "count( b.n, move b );", "B c = B(); two( move b, ref c );", "take( move b ); b = B(); owt( ref b, move B() );" } )
    {
        const Checked p( std::string( k_owned ) + "i32 main() { B b = B(); " + call + " return 0; }" );

        INFO( call << "\n" << p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }
}

// D54's one-call rule: each borrowed argument is a loan for the whole call, so an element may not
// be passed beside a borrow of its whole that could change it, one place may not be `out` twice, and
// an owner may not reach the call under two names when the call could change it.
// D75: each arm of a conditional borrow is an argument of its own.
TEST_CASE( "move_check_refuses_an_arm_aliasing_another_argument", "[check][move][d75]" )
{
    constexpr std::string_view k_both = "class V { public i32 x; V() { x = 0; } ~V() { } };\n"
                                        "void both( ref V a, ref V b ) { }\n";

    const Checked refused(
        std::string( k_both ) + "i32 main() { V v = V(); V u = V(); i32 i = 0; "
                                "both( ref v, i == 0 ? ref u : ref v ); return 0; }"
    );
    INFO( refused.rendered() );
    REQUIRE( refused.errors().size() == 1 );
    REQUIRE( refused.errors()[0].conflict == Call_conflict::Aliased );

    const Checked accepted(
        std::string( k_both ) + "i32 main() { V v = V(); V u = V(); V w = V(); i32 i = 0; "
                                "both( ref v, i == 0 ? ref u : ref w ); return 0; }"
    );
    INFO( accepted.rendered() );
    REQUIRE( accepted.errors().empty() );
}

TEST_CASE( "move_check_refuses_aliased_arguments_in_one_call", "[check][move][m9]" )
{
    constexpr std::string_view k_aliased =
        "class V { public i32 x; V() { x = 0; } ~V() { } public ref i32 operator[]( u64 i ) const { return x; } "
        "public const ref i32 get() const { return x; } public void grow( const ref i32 e ) { } "
        "public void absorb( ref V o ) { } public i32 size() const { return x; } public void resize( i32 n ) { } "
        "public P span() const { return P { 1, 2 }; } public void place( P q ) { } "
        "public S view() const { return S { nullptr }; } public void put( S s ) { } };\n"
        "struct P { i32 a; i32 b; };\n"
        "struct S { i32* q; ref i32 operator[]( u64 i ) const { return *q; } };\n"
        "class W { public V a; public V b; W() { a = V(); b = V(); } };\n"
        "void pair( ref V v, ref i32 e ) { }\n"
        "void see( ref V v, const ref i32 e ) { }\n"
        "void look( const ref V v, ref i32 e ) { }\n"
        "void fill( out S s, ref i32 e ) { s = S { nullptr }; }\n"
        "void eat( move V v, ref i32 e ) { }\n"
        "void two( out P a, out P b ) { a = P { 1, 2 }; b = P { 1, 2 }; }\n"
        "void same( out P a, ref P b ) { a = P { 1, 2 }; }\n"
        "void outs( out i32 a, out i32 b ) { a = 1; b = 2; }\n"
        "void outp( out P a, out i32 b ) { a = P { 1, 2 }; b = 2; }\n"
        "void elems( ref i32 a, ref i32 b ) { }\n"
        "void both( ref V a, ref V b ) { }\n"
        "void read_both( const ref V a, const ref V b ) { }\n"
        "void mixed( const ref V a, ref V b ) { }\n"
        "void part( ref W w, ref V a ) { }\n";

    constexpr std::string_view k_locals = "V v = V(); V u = V(); W w = W(); P p = P { 1, 2 }; S s = S { nullptr }; ";

    const auto checked = [&]( std::string_view call )
    {
        return Checked(
            std::string( k_aliased ) + "i32 main() { " + std::string( k_locals ) + std::string( call ) + " return 0; }"
        );
    };

    struct Refused
    {
        const char*   call;
        Call_conflict conflict;
        const char*   use;   // the argument refused
        const char*   other; // the argument it conflicts with
    };

    SECTION( "refused, naming both arguments" )
    {
        for( const Refused& refused : {
                 Refused { "pair( ref v, ref v[0] );", Call_conflict::Element_and_whole, "ref v[0]", "ref v" },
                 Refused { "see( ref v, v.get() );", Call_conflict::Element_and_whole, "v.get()", "ref v" },
                 Refused { "v.grow( v.get() );", Call_conflict::Element_and_whole, "v.get()", "v" },
                 Refused { "v.put( v.view() );", Call_conflict::Element_and_whole, "v.view()", "v" },
                 Refused { "fill( out s, ref s[0] );", Call_conflict::Element_and_whole, "ref s[0]", "out s" },
                 Refused { "ref V r = v; pair( ref r, ref v[0] );", Call_conflict::Element_and_whole, "ref v[0]", "ref r" },
                 Refused { "pair( ref w.b, ref w.b[0] );", Call_conflict::Element_and_whole, "ref w.b[0]", "ref w.b" },
                 Refused {
                     "fn( ref V, ref i32 ) -> void f = &pair; f( ref v, ref v[0] );",
                     Call_conflict::Element_and_whole,
                     "ref v[0]",
                     "ref v"
                 },
                 Refused { "two( out p, out p );", Call_conflict::Out_twice, "out p", "out p" },
                 Refused { "outs( out p.a, out p.a );", Call_conflict::Out_twice, "out p.a", "out p.a" },
                 Refused { "outp( out p, out p.b );", Call_conflict::Out_twice, "out p.b", "out p" },
                 Refused { "both( ref v, ref v );", Call_conflict::Aliased, "ref v", "ref v" },
                 Refused { "ref V r = v; both( ref v, ref r );", Call_conflict::Aliased, "ref r", "ref v" },
                 Refused { "mixed( v, ref v );", Call_conflict::Aliased, "ref v", "v" },
                 Refused { "part( ref w, ref w.a );", Call_conflict::Aliased, "ref w.a", "ref w" },
                 Refused { "ref V r = v; v.absorb( ref r );", Call_conflict::Aliased, "ref r", "v" },
             } )
        {
            const Checked p = checked( refused.call );

            INFO( refused.call << "\n" << p.rendered() );
            REQUIRE( p.clean() );

            const std::vector<Move_error> errors = p.errors();

            REQUIRE( errors.size() == 1 );
            REQUIRE( errors[0].conflict == refused.conflict );
            REQUIRE( p.text_at( errors[0].use ) == refused.use );
            REQUIRE( p.text_at( errors[0].other ) == refused.other );

            // A place `out` twice is refused at the second.
            if( refused.conflict == Call_conflict::Out_twice )
            {
                REQUIRE( errors[0].other.start < errors[0].use.start );
            }
        }
    }

    SECTION( "an element moved with its whole is a move into a borrowing call" )
    {
        const Checked p = checked( "eat( move v, ref v[0] );" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::vector<Move_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].conflict == Call_conflict::Moved_and_borrowed );
        REQUIRE( p.text_at( errors[0].use ) == "ref v[0]" );
    }

    // A whole only read, a field beside it, an element of another field, and a number or a struct of
    // numbers a call returned cannot reach the element.
    SECTION( "accepted" )
    {
        for( const char* call : {
                 "look( v, ref v[0] );",
                 "v.resize( v.size() );",
                 "pair( ref w.a, ref w.b[0] );",
                 "elems( ref v[0], ref v[1] );",
                 "outs( out p.a, out p.b );",
                 "same( out p, ref p );",
                 "pair( ref v, ref v.x );",
                 "look( v, ref v.x );",
                 "v.grow( v.x );",
                 "elems( ref p.a, ref p.a );",
                 "read_both( v, v );",
                 "u.grow( v.get() );",
                 "see( ref u, v.get() );",
                 "v.place( v.span() );",
             } )
        {
            const Checked p = checked( call );

            INFO( call << "\n" << p.rendered() );
            REQUIRE( p.clean() );
            REQUIRE( p.errors().empty() );
        }
    }
}

// D54: elements at distinct constant indices are distinct places; variable indices may be one.
TEST_CASE( "move_check_tells_constant_indices_apart", "[check][move][m9]" )
{
    constexpr std::string_view k_array = "class V { public i32 x; V() { x = 0; } ~V() { } };\n"
                                         "void both( ref V a, ref V b ) { }\n"
                                         "void outs( out V a, out V b ) { a = V(); b = V(); }\n";

    const auto checked = [&]( std::string_view call )
    {
        return Checked(
            std::string( k_array ) + "i32 main() { V[2] a = [ V(), .. ]; u64 i = 0; u64 j = 1; " + std::string( call ) +
            " return 0; }"
        );
    };

    struct Refused
    {
        const char*   call;
        Call_conflict conflict;
        bool          maybe;
    };

    for( const Refused& refused : {
             Refused { "both( ref a[0], ref a[0] );", Call_conflict::Aliased, false },
             Refused { "both( ref a[i], ref a[j] );", Call_conflict::Aliased, true },
             Refused { "both( ref a[i], ref a[0] );", Call_conflict::Aliased, true },
             Refused { "outs( out a[i], out a[j] );", Call_conflict::Out_twice, true },
             Refused { "outs( out a[1], out a[1] );", Call_conflict::Out_twice, false },
         } )
    {
        const Checked p = checked( refused.call );

        INFO( refused.call << "\n" << p.rendered() );
        REQUIRE( p.clean() );

        const std::vector<Move_error> errors = p.all_errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].conflict == refused.conflict );
        REQUIRE( errors[0].maybe == refused.maybe );
    }

    for( const char* call : { "both( ref a[0], ref a[1] );", "outs( out a[0], out a[1] );" } )
    {
        const Checked p = checked( call );

        INFO( call << "\n" << p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.all_errors().empty() );
    }
}

// D54: a `const`'s value is a constant index, and two copies of one unaddressed local are one index.
TEST_CASE( "move_check_follows_named_and_repeated_indices", "[check][move][m9]" )
{
    constexpr std::string_view k_array = "class V { public i32 x; V() { x = 0; } ~V() { } };\n"
                                         "const u64 second = 1;\n"
                                         "void both( ref V a, ref V b ) { }\n"
                                         "void outs( out V a, out V b ) { a = V(); b = V(); }\n";

    const auto checked = [&]( std::string_view call )
    {
        return Checked(
            std::string( k_array ) + "i32 main() { V[2] a = [ V(), .. ]; const u64 one = 1; u64 i = 0; " + std::string( call ) +
            " return 0; }"
        );
    };

    struct Refused
    {
        const char*   call;
        Call_conflict conflict;
    };

    for( const Refused& refused : {
             Refused { "both( ref a[1], ref a[one] );", Call_conflict::Aliased },
             Refused { "both( ref a[second], ref a[one] );", Call_conflict::Aliased },
             Refused { "both( ref a[i], ref a[i] );", Call_conflict::Aliased },
             Refused { "outs( out a[i], out a[i] );", Call_conflict::Out_twice },
         } )
    {
        const Checked p = checked( refused.call );

        INFO( refused.call << "\n" << p.rendered() );
        REQUIRE( p.clean() );

        const std::vector<Move_error> errors = p.all_errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].conflict == refused.conflict );
        REQUIRE_FALSE( errors[0].maybe );
    }

    for( const char* call : { "both( ref a[0], ref a[one] );", "outs( out a[0], out a[second] );" } )
    {
        const Checked p = checked( call );

        INFO( call << "\n" << p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.all_errors().empty() );
    }
}

// A payload-less variant's temporary ends with its statement, so the next turn builds a new one.
TEST_CASE( "move_check_accepts_an_empty_variant_built_each_turn", "[check][move]" )
{
    constexpr std::string_view k_payload = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                                           "enum H { Full( B b ), Empty };\n"
                                           "void take( move H h ) { }\n";

    for( const char* body : {
             "H[2] hs = [ H::Empty, .. ];",
             "u64 n = 0; while( n < 2 ) { take( H::Empty ); n++; }",
         } )
    {
        const Checked p( std::string( k_payload ) + "i32 main() { " + body + " return 0; }" );

        INFO( body << "\n" << p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.all_errors().empty() );
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
    REQUIRE( check_moves( p.functions[1], p.owning[1], p.addressing[1], p.literals ).size() == 1 );
}

// A borrow reads the value, so one after a move is a use; only a constructor or an `out` argument
// gives the local a value again.
TEST_CASE( "move_check_finds_a_borrow_after_move", "[check][move]" )
{
    constexpr std::string_view k_owned =
        "class B { public u64 n; B() { n = 0; } ~B() { } public u64 get() const { return n; } };\n"
        "void take( move B b ) { }\n"
        "u64 look( B b ) { return b.n; }\n"
        "void poke( ref B b ) { }\n";

    for( const char* use : { "u64 r = look( b );", "poke( ref b );", "u64 r = b.get();", "B* p = &b;", "u64 r = b.n;" } )
    {
        const Checked p( std::string( k_owned ) + "i32 main() { B b = B(); take( move b ); " + use + " return 0; }" );

        INFO( use << "\n" << p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().size() == 1 );
    }

    const Checked refilled(
        std::string( k_owned ) + "i32 main() { B b = B(); take( move b ); b = B(); u64 r = look( b ); return 0; }"
    );

    INFO( refilled.rendered() );
    REQUIRE( refilled.errors().empty() );

    const Checked out( "void init( out i32 n ) { n = 1; }\nvoid bump( ref i32 n ) { }\n"
                       "i32 main() { i32 a = 1; i32 c = move a; init( out a ); bump( ref a ); return a; }" );

    INFO( out.rendered() );
    REQUIRE( out.clean() );
    REQUIRE( out.errors().empty() );
}

// The spans are what check/report turns into a message, so they have to name the right places.
TEST_CASE( "move_check_reports_both_ends", "[check][move]" )
{
    const Checked p( std::string( k_sink ) + "i32 main() { i32 a = 1; sink( move a ); sink( a ); return 0; }" );

    INFO( p.rendered() );

    const std::vector<Move_error> errors = p.errors();

    REQUIRE( errors.size() == 1 );
    REQUIRE( p.text_at( errors[0].moved ).find( "move a" ) != std::string_view::npos );
    REQUIRE( p.text_at( errors[0].use ).find( "sink( a )" ) != std::string_view::npos );
}

// Drop elaboration drops a moved local at every scope exit; eliding it is its job, not an error.
TEST_CASE( "move_check_says_nothing_about_drops", "[check][move]" )
{
    const Checked p( "class Buffer { public u64 len; ~Buffer() { } };\n"
                     "void sink( move Buffer b ) { }\n"
                     "i32 main() { Buffer b = Buffer { 1 }; sink( move b ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.errors().empty() );
}

} // namespace keel
#endif
