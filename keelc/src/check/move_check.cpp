// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/move_check.h"

#include <span>

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
    if( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() )
    {
        const u32   local = value.a.place.local.v;
        const State state = flow.state[local];
        if( value.address_purpose != Address_purpose::Initialise && ( state == State::Moved || state == State::Maybe_moved ) &&
            errors != nullptr )
        {
            errors->push_back( Move_error {
                .local = value.a.place.local, .use = span, .moved = flow.moved_at[local], .maybe = state == State::Maybe_moved
            } );
        }

        // Only a whole local is refilled; `out b.n` into a moved `b` is a use, as `b.n = ...` is.
        if( value.address_purpose == Address_purpose::Initialise )
        {
            if( value.a.place.num_projections != 0 )
            {
                if( state == State::Moved || state == State::Maybe_moved )
                {
                    if( errors != nullptr )
                    {
                        errors->push_back( Move_error {
                            .local = value.a.place.local,
                            .use   = span,
                            .moved = flow.moved_at[local],
                            .maybe = state == State::Maybe_moved
                        } );
                    }
                }
            }
            else
            {
                flow.state[local]    = State::Live;
                flow.moved_at[local] = Span {};
            }
        }
    }

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

            // `_1.x = ...` fills a fresh or live _1, as a struct literal does, but is a use of a moved
            // one: it would fill a field and leave the rest gone.
            if( !statement.place.is_global() )
            {
                if( statement.place.num_projections != 0 )
                {
                    const State state = flow.state[statement.place.local.v];
                    if( state == State::Moved || state == State::Maybe_moved )
                    {
                        if( errors != nullptr )
                        {
                            errors->push_back( Move_error {
                                .local = statement.place.local,
                                .use   = statement.span,
                                .moved = flow.moved_at[statement.place.local.v],
                                .maybe = state == State::Maybe_moved
                            } );
                        }
                        break;
                    }
                }

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

    // Read on the way out, after every statement. A terminator leaves an operand it lacks at its
    // default, which is Operand_kind::Constant, so read_operand returns immediately - no kind test
    // is needed here.
    read_operand( b.terminator.condition, b.terminator.span, flow, errors );
    read_operand( b.terminator.message, b.terminator.span, flow, errors );
}

// What a pointer local borrows: a local's place (`of`), or what a call returned (`through`), never
// both. Valid only for one assigned exactly once, as a borrowed argument's temporary is, so the
// answer needs no flow.
struct Borrow
{
    Local_id        of {};
    Span            at {};
    u32             assignments = 0;
    Place           place {};                          // what is borrowed; only when `of` is valid
    Address_purpose purpose = Address_purpose::Borrow; // what the borrow is for
    const Rvalue*   through = nullptr;                 // the call that returned this address
};

std::vector<Borrow> borrows_of( const Function& func )
{
    std::vector<Borrow> borrows( func.locals.size() );

    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign || statement.place.is_global() || statement.place.num_projections != 0 )
        {
            continue;
        }

        Borrow&       borrow = borrows[statement.place.local.v];
        const Rvalue& value  = statement.value;
        const Place&  source = value.a.place;

        borrow.assignments++;
        borrow.of      = Local_id {};
        borrow.through = nullptr;

        if( borrow.assignments != 1 || source.is_global() )
        {
            continue;
        }

        const bool derefs =
            source.num_projections != 0 && func.projections[source.first_projection].kind == Projection_kind::Deref;
        // A call's or a constant's `a` names no local.
        const Borrow inner = source.local.is_valid() ? borrows[source.local.v] : Borrow {};

        if( value.kind == Rvalue_kind::Address_of )
        {
            borrow.at      = statement.span;
            borrow.purpose = value.address_purpose;

            // `&(*_t)` after `_t = call operator[](...)`: an element, reached through the call.
            if( derefs && inner.through != nullptr )
            {
                borrow.through = inner.through;
            }
            // Through a `ref` binding, `&(*r)`, it is the whole of what `r` borrows.
            else if( derefs && inner.of.is_valid() )
            {
                borrow.of    = inner.of;
                borrow.place = Place { .local = inner.of };
            }
            else
            {
                borrow.of    = source.local;
                borrow.place = source;
            }
        }
        // A conversion of the address, such as to a pointer to `const`, still points at the same local.
        else if( ( value.kind == Rvalue_kind::Use || value.kind == Rvalue_kind::Cast ) && value.a.kind == Operand_kind::Copy &&
                 source.num_projections == 0 )
        {
            borrow.of      = inner.of;
            borrow.place   = inner.place;
            borrow.at      = inner.at;
            borrow.purpose = inner.purpose;
            borrow.through = inner.through;
        }
        else if( value.kind == Rvalue_kind::Call || value.kind == Rvalue_kind::Indirect_call )
        {
            borrow.through = &value;
        }
    }

    return borrows;
}

// One place is a prefix of the other: `p` overlaps `p.b`, and `p.a` does not.
bool overlaps( const Function& func, const Place& a, const Place& b )
{
    if( a.local != b.local )
    {
        return false;
    }

    u32 min_projections = std::min( a.num_projections, b.num_projections );
    for( u32 k = 0; k < min_projections; ++k )
    {
        const Projection& pa = func.projections[a.first_projection + k];
        const Projection& pb = func.projections[b.first_projection + k];

        if( pa.kind != pb.kind )
        {
            return false;
        }

        if( pa.kind == Projection_kind::Field && pa.field != pb.field )
        {
            return false;
        }

        if( pa.kind == Projection_kind::Member && pa.member != pb.member )
        {
            return false;
        }
    }

    return true;
}

// D54: a call's returned address borrows each of its borrowed arguments, the receiver included.
// Ends because `through` names an earlier statement, assigned once.
bool reaches( const Function& func, const std::vector<Borrow>& borrows, const Borrow& element, const Place& whole )
{
    const std::span<const Operand> arguments(
        func.operands.data() + element.through->first_argument, element.through->argument_count
    );

    for( const Operand& operand : arguments )
    {
        if( operand.kind != Operand_kind::Copy || operand.place.is_global() || operand.place.num_projections != 0 )
        {
            continue;
        }

        const Borrow& inner = borrows[operand.place.local.v];

        if( inner.through != nullptr )
        {
            if( reaches( func, borrows, inner, whole ) )
            {
                return true;
            }
        }
        else if( inner.of.is_valid() && overlaps( func, inner.place, whole ) )
        {
            return true;
        }
    }

    return false;
}

// A call handed a local both by `move` and by address: the callee would own it and borrow it at once.
// Every borrowed argument is lowered to an address taken before the call, so the flow never sees it.
void find_moves_into_borrowing_calls(
    const Function& func, const std::vector<Borrow>& borrows, std::vector<Move_error>& errors
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
            if( moved.kind != Operand_kind::Move || moved.place.is_global() || moved.place.num_projections != 0 )
            {
                continue;
            }

            for( const Operand& borrowed : arguments )
            {
                if( borrowed.kind != Operand_kind::Copy || borrowed.place.is_global() || borrowed.place.num_projections != 0 )
                {
                    continue;
                }

                const Borrow& borrow = borrows[borrowed.place.local.v];

                if( borrow.of == moved.place.local ||
                    ( borrow.through != nullptr && reaches( func, borrows, borrow, Place { .local = moved.place.local } ) ) )
                {
                    errors.push_back( Move_error {
                        .local    = moved.place.local,
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
// borrow of its whole that could change it, and one place may not be `out` twice.
void find_aliased_arguments( const Function& func, const std::vector<Borrow>& borrows, std::vector<Move_error>& errors )
{
    const auto borrow_of = [&]( const Operand& operand ) -> const Borrow*
    {
        if( operand.kind != Operand_kind::Copy || operand.place.is_global() || operand.place.num_projections != 0 )
        {
            return nullptr;
        }

        const Borrow& borrow = borrows[operand.place.local.v];

        return borrow.of.is_valid() || borrow.through != nullptr ? &borrow : nullptr;
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

        for( std::size_t i = 0; i < arguments.size(); ++i )
        {
            for( std::size_t j = 0; j < arguments.size(); ++j )
            {
                const Borrow* a = borrow_of( arguments[i] );
                const Borrow* b = borrow_of( arguments[j] );

                if( i == j || a == nullptr || b == nullptr )
                {
                    continue;
                }

                // Reported at the later of the two.
                if( i < j && a->purpose == Address_purpose::Initialise && b->purpose == Address_purpose::Initialise &&
                    a->of.is_valid() && b->of.is_valid() && overlaps( func, a->place, b->place ) )
                {
                    errors.push_back(
                        Move_error { .local = b->of, .use = b->at, .conflict = Call_conflict::Out_twice, .other = a->at }
                    );
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

    const std::vector<Borrow> borrows = borrows_of( func );

    // A second walk, each block exactly once. Reporting inside the loop above would emit one error
    // per visit for any block on a back edge - which is what the "reports once" test pins.
    std::vector<Move_error> errors;

    for( u32 block = 0; block < func.blocks.size(); ++block )
    {
        Flow flow = in[block];
        transfer_block( func, block, in[block], &errors );
    }

    find_moves_into_borrowing_calls( func, borrows, errors );
    find_aliased_arguments( func, borrows, errors );

    return errors;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

// Real source rather than hand-built CFGs: what needs testing here is what happens at a branch, a
// join and a back edge, and those are precisely the graphs that are worst to assemble by hand.
struct Checked : Compiled
{
    explicit Checked( std::string_view source )
        : Compiled( source )
    {
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
// be passed beside a borrow of its whole that could change it, and one place may not be `out` twice.
TEST_CASE( "move_check_refuses_aliased_arguments_in_one_call", "[check][move][m9]" )
{
    constexpr std::string_view k_aliased =
        "class V { public i32 x; V() { x = 0; } ~V() { } public i32* operator[]( u64 i ) const { return nullptr; } "
        "public const ref i32 get() const { return x; } public void grow( const ref i32 e ) { } };\n"
        "class W { public V a; public V b; W() { a = V(); b = V(); } };\n"
        "struct P { i32 a; i32 b; };\n"
        "struct S { i32* q; i32* operator[]( u64 i ) const { return q; } };\n"
        "void pair( ref V v, ref i32 e ) { }\n"
        "void see( ref V v, const ref i32 e ) { }\n"
        "void look( const ref V v, ref i32 e ) { }\n"
        "void fill( out S s, ref i32 e ) { s = S { nullptr }; }\n"
        "void eat( move V v, ref i32 e ) { }\n"
        "void two( out P a, out P b ) { a = P { 1, 2 }; b = P { 1, 2 }; }\n"
        "void same( out P a, ref P b ) { a = P { 1, 2 }; }\n"
        "void outs( out i32 a, out i32 b ) { a = 1; b = 2; }\n"
        "void outp( out P a, out i32 b ) { a = P { 1, 2 }; b = 2; }\n"
        "void elems( ref i32 a, ref i32 b ) { }\n";

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

    // A whole only read, a field beside it, and an element of another field cannot reach the element.
    SECTION( "accepted" )
    {
        for( const char* call : {
                 "look( v, ref v[0] );",
                 "pair( ref w.a, ref w.b[0] );",
                 "elems( ref v[0], ref v[1] );",
                 "outs( out p.a, out p.b );",
                 "same( out p, ref p );",
                 "pair( ref v, ref v.x );",
                 "v.grow( v.x );",
                 "u.grow( v.get() );",
                 "see( ref u, v.get() );",
             } )
        {
            const Checked p = checked( call );

            INFO( call << "\n" << p.rendered() );
            REQUIRE( p.clean() );
            REQUIRE( p.errors().empty() );
        }
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

// Drop elaboration puts a drop at every scope exit, including for a local that has been moved out
// of. Eliding it is drop elaboration's job; blaming the author for it here would be wrong.
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
