// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/assign_check.h"
#include <optional>

namespace keel
{

namespace
{

// A *must* analysis: a block starts "assigned everywhere" and merging only lowers it. `reached`
// skips a block not yet visited, whose identity would be a lie to intersect with; `ever` is the
// same bits joined the other way, so a diagnostic can say "on some paths".
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

// Merges `from` into `into`; whether anything changed is the worklist's termination signal.
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

// A constructor's fields get a slot each after the locals, so the lattice and the worklist need not
// know they exist.
std::size_t slots( const Function& func )
{
    return func.locals.size() + func.owed_fields.size();
}

// The slot of the constructor field `place` reaches, `(*this).f` and anything further into it.
std::optional<u32> field_slot( const Function& func, const Place& place )
{
    if( !func.constructed.is_valid() || place.is_global() || place.local != func.constructed || place.num_projections < 2 )
    {
        return std::nullopt;
    }

    const Projection& deref = func.projections[place.first_projection];
    const Projection& field = func.projections[place.first_projection + 1];

    if( deref.kind != Projection_kind::Deref || field.kind != Projection_kind::Field )
    {
        return std::nullopt;
    }

    for( std::size_t i = 0; i < func.owed_fields.size(); ++i )
    {
        if( func.owed_fields[i].field == field.field )
        {
            return static_cast<u32>( func.locals.size() + i );
        }
    }

    return std::nullopt;
}

void read_slot(
    const Function& func, u32 slot, Span span, bool whole, const Flow& flow, std::vector<Uninitialised_read>* reads
)
{
    if( flow.always[slot] != 0 )
    {
        return;
    }

    reads->push_back( Uninitialised_read {
        .local = func.constructed,
        .field = func.owed_fields[slot - func.locals.size()].field,
        .at    = span,
        .maybe = flow.ever[slot] != 0,
        .whole = whole,
    } );
}

// A constructor's receiver: one field through it, or `this` whole, which reads every field.
void read_receiver(
    const Function& func, const Place& place, Span span, const Flow& flow, std::vector<Uninitialised_read>* reads
)
{
    if( const std::optional<u32> slot = field_slot( func, place ) )
    {
        read_slot( func, *slot, span, false, flow, reads );
        return;
    }

    for( std::size_t slot = func.locals.size(); slot < slots( func ); ++slot )
    {
        read_slot( func, static_cast<u32>( slot ), span, true, flow, reads );
    }
}

// D9's half. A constant or a global cannot be unassigned; a projection reads its local.
void read_operand(
    const Function& func, const Operand& operand, Span span, const Flow& flow, std::vector<Uninitialised_read>* reads
)
{
    if( reads == nullptr || operand.kind == Operand_kind::Constant || operand.place.is_global() )
    {
        return;
    }

    if( func.constructed.is_valid() && operand.place.local == func.constructed )
    {
        read_receiver( func, operand.place, span, flow, reads );
        return;
    }

    const u32 local = operand.place.local.v;

    if( flow.always[local] != 0 )
    {
        return;
    }

    reads->push_back( Uninitialised_read { .local = operand.place.local, .at = span, .maybe = flow.ever[local] != 0 } );
}

// a and b cover Use, Binary, Unary and Cast, the argument range Call; an unused operand is a
// Constant. A borrow reads its place; an Initialise address is the rule below's.
void read_rvalue(
    const Function& func, const Rvalue& value, Span span, const Flow& flow, std::vector<Uninitialised_read>* reads
)
{
    if( value.kind == Rvalue_kind::Address_of && value.address_purpose != Address_purpose::Initialise )
    {
        Operand borrowed = value.a;
        borrowed.kind    = Operand_kind::Copy;
        read_operand( func, borrowed, span, flow, reads );
    }

    read_operand( func, value.a, span, flow, reads );
    read_operand( func, value.b, span, flow, reads );

    for( u32 i = 0; i < value.argument_count; ++i )
    {
        read_operand( func, func.operands[value.first_argument + i], span, flow, reads );
    }
}

// Only an Assign writes, a projected target included: an `out` struct is built through `(*_1).x`.
// Storage_live and Storage_dead clear, so a loop carries no answer round. `reads` is null during
// the fixpoint and set for the one reporting walk.
void transfer_block(
    const Function&                  func,
    u32                              block,
    Flow&                            flow,
    std::vector<Uninitialised_read>* reads,
    std::vector<Reassigned_field>*   reassigned
)
{
    const Block& b = func.blocks[block];

    for( u32 i = 0; i < b.statement_count; ++i )
    {
        const Statement& statement = func.statements[b.first_statement + i];

        // Reads before the write: `_1 = _1 + 1`. Outside the is_global guard, since a global target's
        // rvalue still reads locals.
        if( statement.kind == Statement_kind::Assign )
        {
            read_rvalue( func, statement.value, statement.span, flow, reads );
        }

        if( statement.place.is_global() )
        {
            continue;
        }

        // A constructor's field. Written whole it is assigned, and an owning or `const` one must not already
        // hold a value; written in part it is read, since the part changes what is already there.
        if( const std::optional<u32> slot =
                statement.kind == Statement_kind::Assign ? field_slot( func, statement.place ) : std::nullopt )
        {
            if( statement.place.num_projections > 2 )
            {
                if( reads != nullptr )
                {
                    read_slot( func, *slot, statement.span, false, flow, reads );
                }

                continue;
            }

            const Owed_field& owed = func.owed_fields[*slot - func.locals.size()];

            if( reassigned != nullptr && flow.ever[*slot] != 0 && ( owed.is_owning || owed.is_const ) )
            {
                reassigned->push_back( Reassigned_field {
                    .field    = owed.field,
                    .at       = statement.span,
                    .maybe    = flow.always[*slot] == 0,
                    .is_const = owed.is_const,
                } );
            }

            flow.always[*slot] = 1;
            flow.ever[*slot]   = 1;
            continue;
        }

        const u32 local = statement.place.local.v;

        switch( statement.kind )
        {
        case Statement_kind::Assign:
            flow.always[local] = 1;
            flow.ever[local]   = 1;

            // Only an `out` argument and a constructor's target assign through an address, so
            // `void forward( out i32 n ) { init( out n ); }` works.
            if( statement.value.kind == Rvalue_kind::Address_of && !statement.value.a.place.is_global() &&
                statement.value.address_purpose == Address_purpose::Initialise )
            {
                const std::optional<u32> slot  = field_slot( func, statement.value.a.place );
                const u32                taken = slot ? *slot : statement.value.a.place.local.v;

                flow.always[taken] = 1;
                flow.ever[taken]   = 1;
            }

            break;
        case Statement_kind::Storage_dead:
        case Statement_kind::Storage_live:
            flow.always[local] = 0;
            flow.ever[local]   = 0;
            break;
        case Statement_kind::Drop:
            // Silent: whether a drop of something unassigned is reachable is drop elaboration's question.
            break;
        }
    }

    // The terminator's operand, after every statement; one it lacks is a Constant and reads nothing.
    read_operand( func, b.terminator.condition, b.terminator.span, flow, reads );
    read_operand( func, b.terminator.message, b.terminator.span, flow, reads );
}

Flow entry_flow( const Function& func )
{
    // A constructor's field slots start empty, like the locals.
    Flow flow { std::vector<u8>( slots( func ), 0 ), std::vector<u8>( slots( func ), 0 ), true };

    // Parameters arrive holding a value.
    for( u32 i = 1; i <= func.parameter_count; ++i )
    {
        flow.always[i] = 1;
        flow.ever[i]   = 1;
    }

    // An `out` parameter's referent is empty until the body writes it.
    for( const Local_id local : func.out_parameters )
    {
        flow.always[local.v] = 0;
        flow.ever[local.v]   = 0;
    }

    // The return slot is the function's own `out` parameter, owed at every way out.
    if( func.returns_a_value )
    {
        flow.always[k_return_slot.v] = 0;
        flow.ever[k_return_slot.v]   = 0;
    }

    return flow;
}

} // namespace

Assignment_report check_assignment( const Function& func )
{
    Assignment_report report;

    // D9 applies to every function, so every function pays for the fixpoint.
    std::vector<Flow> in( func.blocks.size(), bottom( slots( func ) ) );
    in[0] = entry_flow( func );

    std::vector<Block_id> work { Block_id { 0 } };
    std::vector<Block_id> next;

    // Forward, pushing into successors. Terminates: `always` only falls and `ever` only rises.
    while( !work.empty() )
    {
        const Block_id block = work.back();
        work.pop_back();

        Flow out = in[block.v];
        transfer_block( func, block.v, out, nullptr, nullptr );

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

    // One walk, each block once, so a block on a back edge reports once. An unreached block is
    // skipped: it holds nothing assigned, so every read in it would report.
    for( u32 block = 0; block < func.blocks.size(); ++block )
    {
        const Block& b = func.blocks[block];

        if( !in[block].reached )
        {
            continue;
        }

        Flow flow = in[block];
        transfer_block( func, block, flow, &report.reads, &report.reassigned );

        if( b.terminator.kind != Terminator_kind::Return )
        {
            continue;
        }

        if( func.diverges )
        {
            report.diverging_returns.push_back( b.terminator.span );
            continue;
        }

        // What this exit owes. One lambda for both, so the set cannot drift from what entry_flow seeds.
        const auto owed = [&]( Local_id local )
        {
            if( flow.always[local.v] != 0 )
            {
                return;
            }

            report.unassigned.push_back(
                Unassigned_error { .local = local, .at = b.terminator.span, .maybe = flow.ever[local.v] != 0 }
            );
        };

        for( const Local_id local : func.out_parameters )
        {
            owed( local );
        }

        if( func.returns_a_value )
        {
            owed( k_return_slot );
        }

        // And every field, when this is a constructor: the instance it returns is whole.
        for( std::size_t i = 0; i < func.owed_fields.size(); ++i )
        {
            const std::size_t slot = func.locals.size() + i;

            if( flow.always[slot] != 0 )
            {
                continue;
            }

            report.unassigned.push_back( Unassigned_error {
                .local = func.constructed,
                .field = func.owed_fields[i].field,
                .at    = b.terminator.span,
                .maybe = flow.ever[slot] != 0,
            } );
        }
    }

    return report;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

// Real source rather than hand-built CFGs, as check_moves' tests use.
struct Checked : Compiled
{
    explicit Checked( std::string_view source )
        : Compiled( source )
    {
    }

    // Every function of the input file, as the driver checks, so a fixture's helpers must be clean too.
    std::vector<Unassigned_error> errors() const
    {
        std::vector<Unassigned_error> all;

        for( const Function& function : functions )
        {
            for( const Unassigned_error& error : check_assignment( function ).unassigned )
            {
                all.push_back( error );
            }
        }

        return all;
    }

    // D9's half, gathered the same way.
    std::vector<Uninitialised_read> reads() const
    {
        std::vector<Uninitialised_read> all;

        for( const Function& function : functions )
        {
            for( const Uninitialised_read& read : check_assignment( function ).reads )
            {
                all.push_back( read );
            }
        }

        return all;
    }

    // The name a read is about, for the same reason name_of exists below.
    std::string_view name_of_read( const Uninitialised_read& read ) const
    {
        for( const Function& function : functions )
        {
            if( read.local.v < function.locals.size() && function.locals[read.local.v].name.is_valid() )
            {
                return interner.text( function.locals[read.local.v].name );
            }
        }

        return {};
    }

    // Which parameter an error is about, so a case can check check/report's naming.
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

    // A constructor's field obligations, gathered like the two above.
    std::vector<Reassigned_field> reassigned() const
    {
        std::vector<Reassigned_field> all;

        for( const Function& function : functions )
        {
            for( const Reassigned_field& error : check_assignment( function ).reassigned )
            {
                all.push_back( error );
            }
        }

        return all;
    }

    std::string_view field_name( Node_id field ) const
    {
        return field.is_valid() ? interner.text( Symbol_id { ast.aux( field ) } ) : std::string_view {};
    }
};

} // namespace

// The return slot is local 0 and `return x` assigns it, so a missing return is this pass's question
// asked of one more local.
TEST_CASE( "assign_check_reports_a_read_before_initialisation", "[check][assign][d9]" )
{
    SECTION( "a plain read" )
    {
        const Checked c( "i32 main() { i32 x; return x; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
        REQUIRE( c.name_of_read( c.reads()[0] ) == "x" );
        REQUIRE_FALSE( c.reads()[0].maybe );
    }

    SECTION( "assigned on one branch only" )
    {
        const Checked c( "i32 f( bool b ) { i32 x; if( b ) { x = 1; } return x; }\ni32 main() { return f( true ); }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
        REQUIRE( c.name_of_read( c.reads()[0] ) == "x" );

        // One path did assign it, so this is an ambiguity, not a certainty.
        REQUIRE( c.reads()[0].maybe );
    }

    SECTION( "read in a condition" )
    {
        // The terminator's operand is a read too.
        const Checked c( "i32 main() { i32 x; if( x > 0 ) { return 1; } return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
    }

    SECTION( "read as a call argument" )
    {
        const Checked c( "i32 g( i32 v ) { return v; }\ni32 main() { i32 x; return g( x ); }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
    }

    SECTION( "a field of an uninitialised struct" )
    {
        const Checked c( "struct P { i32 x; };\ni32 main() { P p; return p.x; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
        REQUIRE( c.name_of_read( c.reads()[0] ) == "p" );
    }

    SECTION( "dereferencing an uninitialised pointer" )
    {
        const Checked c( "i32 main() { i32* p; return *p; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
    }

    SECTION( "a local declared after another has gone out of scope" )
    {
        // Storage_live clears, so the second local does not inherit the first's answer.
        const Checked c( "i32 main() { i32 y = 0; { i32 x = 1; y = x; } i32 z; return z; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
        REQUIRE( c.name_of_read( c.reads()[0] ) == "z" );
    }

    SECTION( "reported once, not once per visit" )
    {
        // One report despite the back edge.
        const Checked c( "i32 main() { i32 x; i32 i = 0; i32 t = 0; while( i < 3 ) { t = t + x; i = i + 1; } return t; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
    }
}

// An `out` parameter holds storage, not a value, so reading one before the body writes it is D9's
// error.
TEST_CASE( "assign_check_reports_a_read_of_an_unwritten_out_parameter", "[check][assign][d9]" )
{
    SECTION( "read before assigning" )
    {
        const Checked c( "void f( out i32 a ) { i32 y = a; a = 1; }\ni32 main() { i32 x; f( out x ); return x; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
        REQUIRE( c.name_of_read( c.reads()[0] ) == "a" );
    }

    SECTION( "assigned first, then read" )
    {
        const Checked c( "void f( out i32 a ) { a = 1; i32 y = a; a = y; }\ni32 main() { i32 x; f( out x ); return x; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().empty() );
    }

    SECTION( "forwarding still works" )
    {
        // `init( out n )` takes `&n`, and the address-of rule counts the callee's promise as the write.
        const Checked c( "void init( out i32 n ) { n = 1; }\n"
                         "void forward( out i32 n ) { init( out n ); }\n"
                         "i32 main() { i32 x; forward( out x ); return x; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().empty() );
        REQUIRE( c.errors().empty() );
    }
}

// The false-positive set: each shape initialises in a way the analysis cannot see directly, and a
// rule slightly too strict here breaks working programs.
TEST_CASE( "assign_check_accepts_what_is_initialised_indirectly", "[check][assign][d9]" )
{
    for( const char* source : {
             // A constructor writes through a pointer; only the Address_of rule connects the two.
             "class C { i32 v; C( i32 n ) { v = n; } ~C() { } };\ni32 main() { C c = C( 3 ); return c.v; }",
             // A struct literal is assembled entirely through projections.
             "struct P { i32 x; i32 y; };\ni32 main() { P p = P { 1, 2 }; return p.x; }",
             // Nested, so the inner temporary is built the same way.
             "struct I { i32 v; };\nstruct P { I inner; };\ni32 main() { P p = P { I { 1 } }; return p.inner.v; }",
             "i32 main() { i32 x; x = 1; return x; }",
             "i32 f( bool b ) { i32 x; if( b ) { x = 1; } else { x = 2; } return x; }\ni32 main() { return f( true ); }",
             "i32 main() { i32 t = 0; i32 i = 0; while( i < 3 ) { t = t + i; i = i + 1; } return t; }",
             // Fresh every iteration. Passes whether or not Storage_live clears; the clear is kept because a
             // local entering scope with last time's answer is wrong on its face.
             "i32 main() { i32 t = 0; i32 i = 0; while( i < 3 ) { i32 z; z = i; t = t + z; i = i + 1; } return t; }",
             // The address is taken and written through; the analysis cannot follow the pointer.
             "void bump( ref i32 v ) { v = v + 1; }\ni32 main() { i32 x = 1; bump( ref x ); return x; }",
             "struct N { i32 v; };\n"
             "i32 main() { i32 r = 0; unsafe { N* n = alloc<N>(); n.v = 7; r = n.v; free( n ); } return r; }",
             // A pattern binding is initialised by the match itself.
             "enum S { A( i32 r ), B };\n"
             "i32 main() { S s = S::A( 3 ); switch( s ) { case S::A( r ): return r; case S::B: return 0; } }",
             // Moved away, then given a new value before being read again.
             "class C { i32 v; C( i32 n ) { v = n; } ~C() { } };\n"
             "i32 main() { C a = C( 1 ); C b = move a; a = C( 2 ); return a.v; }",
         } )
    {
        const Checked c( source );

        INFO( source << "\n" << c.rendered() );
        REQUIRE( c.reads().empty() );
    }
}

// The lowerer discards statements after a terminator, so this is one block. The `reached` guard
// in the reporting walk is unexercised until something leaves an unreached block behind.
TEST_CASE( "assign_check_ignores_unreachable_code", "[check][assign][d9]" )
{
    const Checked c( "i32 f( i32 n ) { return n; i32 x; return x; }\ni32 main() { return f( 1 ); }" );

    INFO( c.rendered() );
    REQUIRE( c.reads().empty() );

    // The premise above: one block, so there is nothing unreached to skip.
    REQUIRE( c.functions.front().blocks.size() == 1 );
}

// A `T*` points at a live `T`, so borrowing an unassigned local is the read.
TEST_CASE( "assign_check_reads_a_borrowed_place", "[check][assign][d9]" )
{
    for( const char* source : {
             "i32 main() { i32 x; i32* q = &x; return x; }",
             "i32 main() { i32 x; i32* q = &x; *q = 1; return x; }",
             "i32 peek( const ref i32 n ) { return n; }\ni32 main() { i32 x; return peek( x ); }",
             "void bump( ref i32 v ) { v = v + 1; }\ni32 main() { i32 x; bump( ref x ); return x; }",
         } )
    {
        const Checked c( source );

        INFO( source << "\n" << c.rendered() );
        REQUIRE_FALSE( c.reads().empty() );
    }
}

// Recorded rather than fixed (§12): it can miss a mistake, never invent one.
TEST_CASE( "assign_check_is_shallow_in_one_known_way", "[check][assign][d9]" )
{
    SECTION( "writing one field counts as initialising the whole struct" )
    {
        const Checked c( "struct P { i32 x; i32 y; };\ni32 main() { P p; p.x = 1; return p.y; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().empty() );
    }
}

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

    SECTION( "an arm that breaks out of the switch and returns nothing" )
    {
        // The arm leaves the `switch`, not the function, so the end of `f` is reached unwritten.
        const Checked c( "enum E { A, B };\n"
                         "i32 f( E e ) { switch( e ) { case E::A: break; case E::B: return 2; } }\n"
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

// Shapes that could plausibly report and must not. The enum switch has no `default` and no block
// after it.
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

// A `void` return slot is owed nothing; constructors and destructors have one too.
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

// `while( true )` once left a reachable loop-exit block; simplify() folds the branch and prunes it,
// so both spellings answer alike.
TEST_CASE( "assign_check_accepts_a_constantly_true_loop", "[check][assign][returns]" )
{
    const Checked folded( "i32 f() { while( true ) { return 1; } }\ni32 main() { return f(); }" );

    INFO( folded.rendered() );
    REQUIRE( folded.errors().empty() );

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

// A missed path and a parameter never written are different mistakes, which is why the pass tracks
// a second bit.
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

    // A *may* analysis gets this wrong: the loop assigns it, and can run zero times.
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

// One error per return reached unassigned, not one per worklist visit.
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

TEST_CASE( "assign_check_says_nothing_about_a_function_with_no_out_parameters", "[check][assign]" )
{
    Checked p( "i32 add( i32 a, i32 b ) { return a + b; }\ni32 main() { return add( 1, 2 ); }" );

    INFO( p.rendered() );
    REQUIRE( p.errors().empty() );
}

// Passing an `out` parameter on as an `out` argument satisfies it: the address-of rule again.
TEST_CASE( "assign_check_accepts_a_forwarded_out_parameter", "[check][assign]" )
{
    Checked p( "void init( out i32 n ) { n = 1; }\n"
               "void forward( out i32 n ) { init( out n ); }\n"
               "i32 main() { i32 x = 0; forward( out x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.errors().empty() );
}

// Only `out` assigns through an address: a `const ref` argument reads, so passing an unwritten
// `out` parameter to one is a read before assignment.
TEST_CASE( "assign_check_is_not_satisfied_by_a_borrow", "[check][assign]" )
{
    Checked p( "i32 peek( const ref i32 n ) { return n; }\n"
               "void init( out i32 n ) { i32 ignored = peek( n ); n = 1; }\n"
               "i32 main() { i32 x = 0; init( out x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors().empty() );
    REQUIRE( p.reads().size() == 1 );
}

// The pass asks whether a place was written, never with what: `nullptr` satisfies a pointer `out`.
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

    // Deliberate: a raw pointer is outside §8's rule, and `T*` is the spelling that can dangle.
    SECTION( "including the address of a local, which dangles and is still accepted" )
    {
        Checked p( "void escapes( out i32* p ) { i32 local = 7; p = &local; }\n"
                   "i32 main() { i32* a = nullptr; escapes( out a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.errors().empty() );
    }
}

// Known limitation (§12): the analysis is per local, so writing one field of a struct `out`
// parameter satisfies it.
TEST_CASE( "assign_check_accepts_a_partly_initialised_struct_for_now", "[check][assign]" )
{
    Checked p( "struct P { i32 x; i32 y; };\n"
               "void init( out P p ) { p.x = 1; }\n"
               "i32 main() { P v = P { 0, 0 }; init( out v ); return v.x; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors().empty() );
}

// D9 for a constructor's fields: each is owed at every way out, as an `out` referent is.
TEST_CASE( "assign_check_requires_a_constructor_to_assign_every_field", "[check][assign][constructor]" )
{
    SECTION( "a field never assigned" )
    {
        const Checked c( "class C { i32 a; i32 b; C() { a = 1; } };\ni32 main() { C c = C(); return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 1 );
        REQUIRE( c.field_name( c.errors()[0].field ) == "b" );
        REQUIRE_FALSE( c.errors()[0].maybe );
    }

    SECTION( "assigned on one path only" )
    {
        const Checked c( "class C { i32 a; i32 b; C( bool x ) { a = 1; if( x ) { b = 2; } } };\n"
                         "i32 main() { C c = C( true ); return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 1 );
        REQUIRE( c.field_name( c.errors()[0].field ) == "b" );
        REQUIRE( c.errors()[0].maybe );
    }

    SECTION( "an early return before the assignment" )
    {
        const Checked c( "class C { i32 a; i32 b; C( bool x ) { a = 1; if( x ) { return; } b = 2; } };\n"
                         "i32 main() { C c = C( true ); return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 1 );
        REQUIRE( c.field_name( c.errors()[0].field ) == "b" );
    }

    SECTION( "each field reported on its own" )
    {
        const Checked c( "class C { i32 a; i32 b; C() { } };\ni32 main() { C c = C(); return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.errors().size() == 2 );
        REQUIRE( c.field_name( c.errors()[0].field ) == "a" );
        REQUIRE( c.field_name( c.errors()[1].field ) == "b" );
    }
}

TEST_CASE( "assign_check_reports_a_field_read_before_its_constructor_assigns_it", "[check][assign][constructor]" )
{
    SECTION( "a plain read" )
    {
        const Checked c( "class C { i32 a; i32 b; C() { a = b; b = 1; } };\ni32 main() { C c = C(); return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
        REQUIRE( c.field_name( c.reads()[0].field ) == "b" );
        REQUIRE_FALSE( c.reads()[0].whole );
    }

    SECTION( "a compound assignment reads first" )
    {
        const Checked c( "class C { i32 a; C() { a += 1; } };\ni32 main() { C c = C(); return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
        REQUIRE( c.field_name( c.reads()[0].field ) == "a" );
    }

    SECTION( "the explicit spelling is the same read" )
    {
        const Checked c( "class C { i32 a; i32 b; C() { a = this.b; b = 1; } };\ni32 main() { C c = C(); return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
        REQUIRE( c.field_name( c.reads()[0].field ) == "b" );
    }

    SECTION( "writing into a field reads it" )
    {
        // `p.x = 1` changes part of what `p` holds, and there is nothing there yet. Stricter than a
        // local, deliberately: the lowerer drops an owning part before replacing it.
        const Checked c( "struct P { i32 x; i32 y; };\n"
                         "class C { P p; C() { p.x = 1; p.y = 2; } };\ni32 main() { C c = C(); return 0; }" );

        INFO( c.rendered() );
        REQUIRE_FALSE( c.reads().empty() );
        REQUIRE( c.field_name( c.reads()[0].field ) == "p" );
    }

    SECTION( "`this` used whole names every field still missing" )
    {
        const Checked c( "class C { i32 a; i32 b; i32 d; C() { a = 1; touch(); b = 2; d = 3; } void touch() { } };\n"
                         "i32 main() { C c = C(); return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 2 );
        REQUIRE( c.reads()[0].whole );
        REQUIRE( c.field_name( c.reads()[0].field ) == "b" );
        REQUIRE( c.field_name( c.reads()[1].field ) == "d" );
    }

    SECTION( "assigned on one path, then read" )
    {
        const Checked c( "class C { i32 a; i32 b; C( bool x ) { if( x ) { b = 1; } a = b; b = 2; } };\n"
                         "i32 main() { C c = C( true ); return 0; }" );

        INFO( c.rendered() );
        REQUIRE( c.reads().size() == 1 );
        REQUIRE( c.reads()[0].maybe );
    }
}

// An owning field is written once per path. A second write would have to drop the first value, and
// whether there is one differs by path - so the constructor is refused rather than given a flag.
TEST_CASE( "assign_check_refuses_an_owning_field_a_constructor_may_already_have_assigned", "[check][assign][constructor]" )
{
    constexpr const char* buf =
        "class Buf { u8[*] p; Buf() { unsafe { p = alloc<u8>( 4 ); } } ~Buf() { unsafe { free( p ); } } };\n";

    SECTION( "assigned twice" )
    {
        const Checked c(
            std::string( buf ) + "class H { Buf b; H() { b = Buf(); b = Buf(); } };\n"
                                 "i32 main() { H h = H(); return 0; }"
        );

        INFO( c.rendered() );
        REQUIRE( c.reassigned().size() == 1 );
        REQUIRE( c.field_name( c.reassigned()[0].field ) == "b" );
    }

    SECTION( "assigned on one path, then on all" )
    {
        const Checked c(
            std::string( buf ) + "class H { Buf b; H( bool x ) { if( x ) { b = Buf(); } b = Buf(); } };\n"
                                 "i32 main() { H h = H( true ); return 0; }"
        );

        INFO( c.rendered() );
        REQUIRE( c.reassigned().size() == 1 );
        REQUIRE( c.errors().empty() );
    }

    SECTION( "inside a loop" )
    {
        const Checked c(
            std::string( buf ) +
            "class H { Buf b; H( i32 n ) { b = Buf(); i32 i = 0; while( i < n ) { b = Buf(); i = i + 1; } } };\n"
            "i32 main() { H h = H( 2 ); return 0; }"
        );

        INFO( c.rendered() );
        REQUIRE( c.reassigned().size() == 1 );
    }

    SECTION( "once on each arm is fine" )
    {
        const Checked c(
            std::string( buf ) + "class H { Buf b; H( bool x ) { if( x ) { b = Buf(); } else { b = Buf(); } } };\n"
                                 "i32 main() { H h = H( true ); return 0; }"
        );

        INFO( c.rendered() );
        REQUIRE( c.reassigned().empty() );
        REQUIRE( c.errors().empty() );
        REQUIRE( c.reads().empty() );
    }

    SECTION( "a method may reassign it: the referent always holds a value there" )
    {
        const Checked c(
            std::string( buf ) + "class H { Buf b; H() { b = Buf(); } void reset() { b = Buf(); b = Buf(); } };\n"
                                 "i32 main() { H h = H(); h.reset(); return 0; }"
        );

        INFO( c.rendered() );
        REQUIRE( c.reassigned().empty() );
    }
}

// A `const` field is written once per path, as an owning one is, whatever its type.
TEST_CASE( "assign_check_refuses_a_const_field_a_constructor_may_already_have_assigned", "[check][assign][constructor]" )
{
    for( const char* source : {
             "class C { const i32 n; C() { n = 1; n = 2; } };",
             "class C { const i32 n; C( bool x ) { if( x ) { n = 1; } n = 2; } };",
             "class C { const i32 n; C( i32 k ) { n = 0; i32 i = 0; while( i < k ) { n = i; i = i + 1; } } };",
         } )
    {
        const Checked c( std::string( source ) + "\ni32 main() { return 0; }" );

        INFO( source << "\n" << c.rendered() );
        REQUIRE( c.reassigned().size() == 1 );
        REQUIRE( c.reassigned()[0].is_const );
        REQUIRE( c.field_name( c.reassigned()[0].field ) == "n" );
    }

    const Checked c( "class C { const i32 n; C( bool x ) { if( x ) { n = 1; } else { n = 2; } } };\ni32 main() { return 0; }" );

    INFO( c.rendered() );
    REQUIRE( c.reassigned().empty() );
    REQUIRE( c.errors().empty() );
}

// Its address is a `const` pointer, so taking it reads the field rather than assigning it.
TEST_CASE( "assign_check_reads_a_const_field_whose_address_is_taken", "[check][assign][constructor]" )
{
    const Checked early( "class C { const i32 n; C() { const i32* p = &n; n = 1; } };\ni32 main() { return 0; }" );

    INFO( early.rendered() );
    REQUIRE( early.reads().size() == 1 );
    REQUIRE( early.field_name( early.reads()[0].field ) == "n" );
    REQUIRE( early.reassigned().empty() );

    const Checked late( "class C { const i32 n; C() { n = 1; const i32* p = &n; } };\ni32 main() { return 0; }" );

    INFO( late.rendered() );
    REQUIRE( late.reads().empty() );
    REQUIRE( late.reassigned().empty() );
}

// The false-positive set for fields, as the case above it is for locals.
TEST_CASE( "assign_check_accepts_a_constructor_that_assigns_every_field", "[check][assign][constructor]" )
{
    for( const char* source : {
             "class C { i32 a; i32 b; C() { a = 1; b = a; } };\ni32 main() { C c = C(); return 0; }",
             // A field that holds no ownership may be written as often as any local.
             "class C { i32 a; C() { a = 1; a = 2; a += 1; } };\ni32 main() { C c = C(); return 0; }",
             "class C { i32 a; C( bool x ) { if( x ) { a = 1; } else { a = 2; } } };\ni32 main() { C c = C( true ); return 0; "
             "}",
             // Every field first, then `this` whole.
             "class C { i32 a; C() { a = 1; touch(); } void touch() { } };\ni32 main() { C c = C(); return 0; }",
             // Forwarded through `out`, which is the address-of rule again.
             "void init( out i32 n ) { n = 1; }\n"
             "class C { i32 a; C() { init( out a ); } };\ni32 main() { C c = C(); return 0; }",
             "struct P { i32 x; i32 y; };\n"
             "class C { P p; C() { p = P { 1, 2 }; p.x = 3; } };\ni32 main() { C c = C(); return 0; }",
             "class Box<T> { T v; Box( move T x ) { v = move x; } };\ni32 main() { Box<i32> b = Box<i32>( 1 ); return 0; }",
             // A static is not the instance's, so it is owed nothing and may be read at once.
             "class C { static i32 made = 0; i32 a; C() { made = made + 1; a = made; } };\n"
             "i32 main() { C c = C(); return 0; }",
             // Every field before an early return.
             "class C { i32 a; C( bool x ) { a = 1; if( x ) { return; } a = 2; } };\ni32 main() { C c = C( true ); return 0; }",
         } )
    {
        const Checked c( source );

        INFO( source << "\n" << c.rendered() );
        REQUIRE( c.clean() );
        REQUIRE( c.errors().empty() );
        REQUIRE( c.reads().empty() );
        REQUIRE( c.reassigned().empty() );
    }
}

} // namespace keel
#endif
