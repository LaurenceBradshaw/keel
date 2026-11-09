// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/inline_calls.h"
#include "ir/call_graph.h"
#include "ir/simplify.h"

namespace keel
{
namespace
{

// What a pick chose: no callee leaves the statement; a receiver stands in for the callee's `this`.
struct Site
{
    const Function*      callee = nullptr;
    std::optional<Place> receiver;
};

// Calls and drops dominate; storage markers, gotos and returns are free.
u32 inline_cost( const Function& callee, const Inline_weights& weights )
{
    u32 cost      = 0;
    u32 call_like = 0;

    for( const Statement& statement : callee.statements )
    {
        switch( statement.kind )
        {
        case Statement_kind::Assign:
        {
            const Rvalue_kind kind = callee.rvalue( statement ).kind;
            if( kind == Rvalue_kind::Call || kind == Rvalue_kind::Indirect_call || kind == Rvalue_kind::Allocate ||
                kind == Rvalue_kind::Release )
            {
                cost += weights.call;
                call_like++;
            }
            // A dynamic call's own operands, which lower_dispatch spelt out: the call already counts them.
            else if( kind != Rvalue_kind::Vtable_slot && kind != Rvalue_kind::Interface_self )
            {
                cost += weights.statement;
            }
            break;
        }
        case Statement_kind::Drop:
            cost += weights.call;
            call_like++;
            break;
        case Statement_kind::Storage_live:
        case Statement_kind::Storage_dead:
            break;
        }
    }

    for( const Block& block : callee.blocks )
    {
        if( block.terminator.kind == Terminator_kind::Branch )
        {
            cost += weights.branch;
        }
        else if( block.terminator.kind == Terminator_kind::Panic )
        {
            cost += weights.panic;
        }
    }

    // Forwarder bonus: a function that only calls another is cheaper to inline than one that does the same and more.
    if( call_like == 1 )
    {
        cost -= weights.call;
    }

    return cost;
}

// Measured against the caller's size before any splice, so one caller's limit cannot move mid-way.
u32 threshold( const u32 statements_at_start, const Function& callee, const Inline_weights& weights )
{
    if( statements_at_start >= weights.big_caller )
    {
        return weights.big_caller_threshold;
    }

    if( narrow_cast<u32>( callee.blocks.size() ) <= weights.few_blocks )
    {
        return weights.threshold + weights.threshold / 4;
    }

    return weights.threshold;
}

// An `out` parameter is reached through `&init`, whose pointer `addressed_locals` trusts to die with its call.
bool inlinable( const Function& callee )
{
    if( callee.out_parameters.size() > 0 )
    {
        return false;
    }

    return true;
}

// The offset in `block`, before `index`, of the `&init` that last wrote `pointer`'s local; none if
// another write comes between, or the `&init` is in an earlier block.
std::optional<u32> initialised( const Function& caller, const Block& block, u32 index, const Operand& pointer )
{
    if( pointer.kind != Operand_kind::Copy || pointer.place.is_global() || pointer.place.num_projections != 0 )
    {
        return std::nullopt;
    }

    for( u32 s = index; s-- > 0; )
    {
        const Statement& statement = caller.statements_of( block )[s];

        if( statement.kind != Statement_kind::Assign || statement.place.is_global() || statement.place.num_projections != 0 ||
            statement.place.local != pointer.place.local )
        {
            continue;
        }

        const Rvalue& value = caller.rvalue( statement );
        if( value.kind == Rvalue_kind::Address_of && value.address_purpose == Address_purpose::Initialise )
        {
            return s;
        }

        return std::nullopt;
    }

    return std::nullopt;
}

// The call at statement `index` of block `at` becomes: `at` up to the call, then the parameters
// assigned from the arguments; the callee's blocks, each `return` a goto; and a continuation that
// moves the result into the call's place, then runs the rest of `at`. `at`'s old range is left
// unreferenced, for the caller to sweep. With a `receiver`, `(*this)` is that place and `this` gets
// no local; a constructor's `&init` moves to the continuation, so the body writes no borrowed local.
void splice( Function& caller, Block_id at, u32 index, const Function& callee, std::optional<Place> receiver )
{
    // Copies: `blocks`, `statements` and `rvalues` grow below.
    const Block              block      = caller.blocks[at];
    const Statement_kind     call_kind  = caller.statements_of( block )[index].kind;
    const Span               call_span  = caller.statements_of( block )[index].span;
    const Place              call_place = caller.statements_of( block )[index].place;
    const Rvalue             call_value = caller.rvalue( caller.statements_of( block )[index] );
    const std::optional<u32> built      = receiver && call_kind == Statement_kind::Assign
                                              ? initialised( caller, block, index, call_value.arguments[0] )
                                              : std::nullopt;

    // A clone: `at`'s old range stays whole, unreferenced, until the caller sweeps it.
    const auto take = [&]( u32 s )
    {
        Statement statement = caller.clone( caller.statements_of( block )[s] );
        caller.statements.push_back( std::move( statement ) );
    };

    const u32      local_base = narrow_cast<u32>( caller.locals.size() );
    const u32      block_base = narrow_cast<u32>( caller.blocks.size() );
    const u32      proj_base  = narrow_cast<u32>( caller.projections.size() );
    const Local_id result { local_base };
    const Block_id continuation { block_base + narrow_cast<u32>( callee.blocks.size() ) };

    for( const Local& local : callee.locals )
    {
        caller.locals.push_back( local );
    }

    for( const Projection& projection : callee.projections )
    {
        Projection copy = projection;
        if( copy.kind == Projection_kind::Index )
        {
            copy.index.v += local_base;
        }
        caller.projections.push_back( copy );
    }

    // A global's local is invalid but its projections still move.
    const auto renumber_place = [&]( Place& place )
    {
        if( receiver && !place.is_global() && place.local == Local_id { 1 } )
        {
            assert( place.num_projections > 0 );
            const u32 rest  = place.num_projections - 1;
            const u32 first = narrow_cast<u32>( caller.projections.size() );
            for( u32 k = 0; k < receiver->num_projections; k++ )
            {
                const Projection projection = caller.projections[receiver->first_projection + k];
                caller.projections.push_back( projection );
            }
            for( u32 k = 0; k < rest; k++ )
            {
                const Projection projection = caller.projections[proj_base + place.first_projection + 1 + k];
                caller.projections.push_back( projection );
            }

            place                  = *receiver;
            place.first_projection = first;
            place.num_projections  = receiver->num_projections + rest;
            return;
        }

        if( place.local.is_valid() )
        {
            place.local.v += local_base;
        }
        if( place.num_projections > 0 )
        {
            place.first_projection += proj_base;
        }
    };

    // By place, not by kind: an `Address_of` operand is a Constant holding one.
    const auto renumber = [&]( Operand& operand ) { renumber_place( operand.place ); };

    for( const Block& from : callee.blocks )
    {
        Block into           = from;
        into.first_statement = narrow_cast<u32>( caller.statements.size() );

        for( const Statement& from_statement : callee.statements_of( from ) )
        {
            Rvalue value = callee.rvalue( from_statement );
            renumber( value.a );
            renumber( value.b );
            for( Operand& argument : value.arguments )
            {
                renumber( argument );
            }

            Statement statement = caller.copy_of( from_statement, std::move( value ) );
            renumber_place( statement.place );
            if( statement.drop_flag.is_valid() )
            {
                statement.drop_flag.v += local_base;
            }
            caller.statements.push_back( std::move( statement ) );
        }

        Terminator& terminator = into.terminator;
        if( terminator.kind == Terminator_kind::Return )
        {
            terminator.kind       = Terminator_kind::Goto;
            terminator.targets[0] = continuation;
        }
        else
        {
            for( Block_id& target : terminator.targets )
            {
                if( target.is_valid() )
                {
                    target.v += block_base;
                }
            }
        }
        renumber( terminator.condition );
        renumber( terminator.message );
        renumber( terminator.at );

        caller.blocks.push_back( into );
    }

    // The continuation: the result into the call's place, then the rest of `at`.
    Block after { .first_statement = narrow_cast<u32>( caller.statements.size() ), .terminator = block.terminator };

    if( built )
    {
        take( *built );
    }

    if( callee.returns_a_value )
    {
        const Place from { .local = result };
        caller.statements.push_back( Statement {
            .kind  = Statement_kind::Assign,
            .span  = call_span,
            .place = call_place,
            .value = caller.add_rvalue( use( move( from, callee.locals[k_return_slot].type ) ) )
        } );
        caller.statements.push_back( Statement { .kind = Statement_kind::Storage_dead, .span = call_span, .place = from } );
    }

    for( u32 s = index + 1; s < block.statement_count; ++s )
    {
        take( s );
    }

    after.statement_count = narrow_cast<u32>( caller.statements.size() ) - after.first_statement;
    caller.blocks.push_back( after );

    // `at`: its statements before the call, then the parameters' storage and values.
    const u32 head_first = narrow_cast<u32>( caller.statements.size() );

    for( u32 s = 0; s < index; ++s )
    {
        if( built && s == *built )
        {
            continue;
        }

        take( s );
    }

    for( u32 p = callee.returns_a_value ? 0 : 1; p <= callee.parameter_count; ++p )
    {
        if( p == 1 && receiver )
        {
            continue;
        }

        caller.statements.push_back( Statement {
            .kind = Statement_kind::Storage_live, .span = call_span, .place = Place { .local = Local_id { local_base + p } }
        } );
    }

    assert( call_kind == Statement_kind::Drop || call_value.arguments.size() == callee.parameter_count );
    for( u32 i = 0; i < call_value.arguments.size(); ++i )
    {
        if( i == 0 && receiver )
        {
            continue;
        }

        caller.statements.push_back( Statement {
            .kind  = Statement_kind::Assign,
            .span  = call_span,
            .place = Place { .local = Local_id { local_base + 1 + i } },
            .value = caller.add_rvalue( use( call_value.arguments[i] ) ),
        } );
    }

    Block& head          = caller.blocks[at];
    head.first_statement = head_first;
    head.statement_count = narrow_cast<u32>( caller.statements.size() ) - head_first;
    head.terminator = Terminator { .kind = Terminator_kind::Goto, .span = call_span, .targets = { Block_id { block_base } } };

    for( const Local_id temporary : callee.one_path_temporaries )
    {
        caller.one_path_temporaries.push_back( Local_id { temporary.v + local_base } );
    }

    // A local's run moves as one, so `parent` and `end` keep their distance from the slot. The
    // receiver's run describes a referent, which the caller's place now is.
    for( u32 i = 0; i < callee.fields.size(); ++i )
    {
        Field_slot slot = callee.fields[i];

        if( slot.local == callee.constructed )
        {
            continue;
        }

        const u32 placed = narrow_cast<u32>( caller.fields.size() );
        slot.local.v += local_base;
        slot.end = placed + ( slot.end - i );
        if( slot.parent != k_no_field )
        {
            slot.parent = placed - ( i - slot.parent );
        }
        caller.fields.push_back( slot );
    }
}

// Whether the receiver is only ever dereferenced, so the place it points at can stand in for it.
bool replaceable_receiver( const Function& callee )
{
    const auto ok = [&]( const Place& place )
    {
        if( !place.local.is_valid() || place.local != Local_id { 1 } ||
            ( place.num_projections > 0 && callee.projections[place.first_projection].kind == Projection_kind::Deref ) )
        {
            return true;
        }
        return false;
    };

    for( const Statement& statement : callee.statements )
    {
        if( !ok( statement.place ) )
        {
            return false;
        }

        bool all_ok = true;
        for_each_operand(
            callee.rvalue( statement ),
            [&]( const Operand& operand )
            {
                if( !ok( operand.place ) )
                {
                    all_ok = false;
                }
            }
        );

        if( !all_ok )
        {
            return false;
        }
    }

    for( const Block& block : callee.blocks )
    {
        if( !ok( block.terminator.condition.place ) || !ok( block.terminator.message.place ) ||
            !ok( block.terminator.at.place ) )
        {
            return false;
        }
    }

    return true;
}

// The `&f` an indirect call's callee always holds: its local assigned once, that or a copy of one
// that does, and never addressed. Spliced-out statements are skipped, so the blocks are walked.
const Rvalue* known_function( const Function& caller, const Operand& callee )
{
    if( !callee.has_place() || !callee.place.is_whole_local() )
    {
        return nullptr;
    }

    Local_id local = callee.place.local;

    for( u32 hops = 0; hops < caller.locals.size(); ++hops )
    {
        const Rvalue* only    = nullptr;
        u32           writes  = 0;
        bool          address = false;

        for( const Block& block : caller.blocks )
        {
            for( const Statement& statement : caller.statements_of( block ) )
            {
                if( statement.kind != Statement_kind::Assign )
                {
                    continue;
                }

                const Rvalue& value = caller.rvalue( statement );
                address             = address || ( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() &&
                                       value.a.place.local == local );
                if( !statement.place.is_global() && statement.place.local == local )
                {
                    ++writes;
                    only = statement.place.is_whole_local() ? &value : nullptr;
                }
            }
        }

        if( writes != 1 || address || only == nullptr )
        {
            return nullptr;
        }

        if( only->kind == Rvalue_kind::Function_address )
        {
            return only;
        }

        if( only->kind != Rvalue_kind::Use || !only->a.has_place() || !only->a.place.is_whole_local() )
        {
            return nullptr;
        }

        local = only->a.place.local;
    }

    return nullptr;
}

// Splices whatever `pick` chooses, then rescans the continuation for more. A spliced body was
// inlined into already, so only its indirect calls are offered again: one may now be known.
template <typename Pick>
void inline_each( Function& caller, const Literal_pool& literals, Pick pick )
{
    std::vector<Block_id> worklist( caller.blocks.size() );
    std::vector<bool>     spliced( caller.blocks.size(), false );

    for( const Block_id i : caller.blocks.ids() )
    {
        worklist[i.v] = i;
    }

    while( !worklist.empty() )
    {
        const Block_id at = worklist.back();
        worklist.pop_back();

        Block& block = caller.blocks[at];

        for( u32 s = 0; s < block.statement_count; ++s )
        {
            const Statement& statement = caller.statements_of( block )[s];
            if( spliced[at.v] && ( statement.kind != Statement_kind::Assign ||
                                   std::as_const( caller ).rvalue( statement ).kind != Rvalue_kind::Indirect_call ) )
            {
                continue;
            }

            const Site site = pick( block, s );
            if( site.callee )
            {
                const bool inner      = spliced[at.v];
                const u32  block_base = narrow_cast<u32>( caller.blocks.size() );
                splice( caller, at, s, *site.callee, site.receiver );
                spliced.resize( caller.blocks.size(), true );
                spliced.back() = inner;
                for( u32 b = block_base; b < caller.blocks.size(); ++b )
                {
                    worklist.push_back( Block_id { b } );
                }
                break;
            }
        }
    }

    remove_statements( caller, std::vector<bool>( caller.statements.size(), false ) );
    simplify( caller, literals );
}

} // namespace

void inline_calls(
    Function&                                   caller,
    const std::vector<std::optional<Function>>& saved,
    const std::vector<Function>&                functions,
    const Literal_pool&                         literals,
    const Inline_weights&                       weights
)
{
    const u32 statement_count = narrow_cast<u32>( caller.statements.size() );
    inline_each(
        caller,
        literals,
        [&]( const Block& block, u32 s ) -> Site
        {
            const Statement& statement = caller.statements_of( block )[s];
            const Rvalue&    value     = std::as_const( caller ).rvalue( statement );

            // A call through a local holding a known function is a call to it, inlined or not.
            if( statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Indirect_call )
            {
                if( const Rvalue* known = known_function( caller, value.a ) )
                {
                    caller.rvalue( statement ) = call( known->callee, value.arguments, value.type, known->type_arguments );
                }
            }

            if( statement.kind != Statement_kind::Assign || value.kind != Rvalue_kind::Call )
            {
                return {};
            }

            for( u32 j = 0; j < saved.size(); ++j )
            {
                if( !saved[j] )
                {
                    continue;
                }

                if( calls( caller, statement, *saved[j] ) )
                {
                    const Function&      callee   = *saved[j];
                    const Function&      finished = functions[j];
                    std::optional<Place> receiver;
                    bool                 fits = inlinable( callee ) &&
                                inline_cost( finished, weights ) <= threshold( statement_count, finished, weights );
                    if( fits && callee.constructed.is_valid() )
                    {
                        assert( callee.constructed == Local_id { 1 } );
                        const std::optional<u32> built = initialised( caller, block, s, value.arguments[0] );
                        if( built )
                        {
                            receiver = caller.rvalue( caller.statements_of( block )[*built] ).a.place;
                        }
                        fits = receiver.has_value() && replaceable_receiver( callee );
                    }
                    if( fits )
                    {
                        return Site { .callee = &callee, .receiver = receiver };
                    }
                    else
                    {
                        return {};
                    }
                }
            }

            return {};
        }
    );
}

void inline_drops(
    Function&                    caller,
    const std::vector<Function>& functions,
    const std::vector<bool>&     finished,
    const Literal_pool&          literals,
    const Inline_weights&        weights
)
{
    const u32 statement_count = narrow_cast<u32>( caller.statements.size() );
    inline_each(
        caller,
        literals,
        [&]( const Block& block, u32 s ) -> Site
        {
            const Statement& statement = caller.statements_of( block )[s];
            if( statement.kind != Statement_kind::Drop || statement.drop_flag.is_valid() ||
                !caller.rvalue( statement ).callee.is_valid() )
            {
                return {};
            }

            for( u32 j = 0; j < functions.size(); ++j )
            {
                if( !finished[j] )
                {
                    continue;
                }

                if( calls( caller, statement, functions[j] ) )
                {
                    if( replaceable_receiver( functions[j] ) &&
                        inline_cost( functions[j], weights ) <= threshold( statement_count, functions[j], weights ) )
                    {
                        return Site { .callee = &functions[j], .receiver = statement.place };
                    }
                    else
                    {
                        return {};
                    }
                }
            }

            return {};
        }
    );
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr std::string_view k_types = "class Box { public i64 value; public Box( i64 v ) { value = v; } ~Box() { } };\n"
                                     "i32 twice( i32 x ) { return x * 2; }\n"
                                     "void set( out i32 n ) { n = 9; }\n"
                                     "i32 mixed( i32 a, i32 b ) { i32 c = a * b + a; i32 d = c - b * 3; i32 e = d * d + c;"
                                     " i32 f = e / 7 + d; i32 g = f * a - e; i32 h = g * b + f * c; return h + c * d - f; }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Inlined : Compiled
{
    explicit Inlined( std::string_view source, Inline_weights weights = {}, u32 rounds = 2 )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build { .optimise = true, .inlining = weights, .rounds = rounds }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    // Direct calls left in the function under test.
    std::size_t calls() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            { return s.kind == Statement_kind::Assign && first().rvalue( s ).kind == Rvalue_kind::Call; }
        ) );
    }

    std::size_t indirect_calls() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            { return s.kind == Statement_kind::Assign && first().rvalue( s ).kind == Rvalue_kind::Indirect_call; }
        ) );
    }

    std::size_t drops( bool flagged ) const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s ) { return s.kind == Statement_kind::Drop && s.drop_flag.is_valid() == flagged; }
        ) );
    }
};

} // namespace

TEST_CASE( "inline_calls_inlines_both_calls_in_one_block", "[opt][inline]" )
{
    const Inlined p( "i32 f( i32 a ) { return twice( a ) + twice( a + 1 ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 0 );
}

// The call inside `f` is into its own group, so the body never takes itself.
TEST_CASE( "inline_calls_keeps_a_recursive_call", "[opt][inline]" )
{
    const Inlined p( "i64 f( i64 n ) { if( n <= 1 ) { return 1; } return n * f( n - 1 ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

// The weights are the knobs: raise the threshold and the same call goes.
TEST_CASE( "inline_calls_keeps_a_call_over_the_threshold", "[opt][inline]" )
{
    const std::string_view source = "i32 f( i32 a ) { return mixed( a, 3 ); }\n";

    const Inlined kept( source );
    REQUIRE( kept.clean() );
    REQUIRE( kept.calls() == 1 );

    const Inlined raised( source, Inline_weights { .threshold = 100 } );
    REQUIRE( raised.clean() );
    REQUIRE( verify( raised.first() ).empty() );
    REQUIRE( raised.calls() == 0 );
}

// Both are reached through `&init`, whose pointer the passes trust to die with its call.
TEST_CASE( "inline_calls_keeps_a_call_that_initialises_through_out", "[opt][inline]" )
{
    const Inlined p( "i32 f() { i32 n = 0; set( out n ); return n; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

// `(*this).value` becomes `b.value`, so `b` is built where it lies.
TEST_CASE( "inline_calls_inlines_a_constructor", "[opt][inline]" )
{
    const Inlined p( "i64 f() { Box b = Box( 4 ); return b.value; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 0 );
}

// The callee's `p` arrives with its fields; the inlined constructor's receiver, a pointer, brings none.
// No rounds, so `q` is not folded away, and its slots with it.
TEST_CASE( "inline_calls_carries_the_callees_field_table", "[opt][inline]" )
{
    const Inlined p(
        "i32 f() { Box b = Box( 4 ); return sum(); }\n"
        "struct Pr { i32 x; i32 y; };\n"
        "i32 sum() { Pr q = Pr { 1, 2 }; return q.x + q.y; }\n",
        {},
        0
    );
    REQUIRE( p.clean() );
    REQUIRE( p.calls() == 0 );

    const Function& func = p.first();
    std::size_t     xs   = 0;

    for( u32 i = 0; i < func.fields.size(); ++i )
    {
        const Field_slot& slot = func.fields[i];
        REQUIRE( slot.local.v < func.locals.size() );
        REQUIRE( slot.end > i );
        REQUIRE( slot.end <= func.fields.size() );

        if( slot.parent == k_no_field )
        {
            const Type_id type = func.locals[slot.local].type;
            REQUIRE( p.types.table().is_struct( type ) );
            REQUIRE( p.ast.enclosing_aggregate( slot.field ) == p.types.table().get( type ).declaration );
        }
        else
        {
            REQUIRE( slot.parent < i );
        }

        xs += p.ast.name( slot.field ) == p.interner.find( "x" ) ? 1 : 0;
    }

    REQUIRE( xs > 0 ); // `q`'s, and the literal's temporary's
}

// `fill` is over the threshold and takes `this` itself, which no place can stand in for.
TEST_CASE( "inline_calls_keeps_a_constructor_that_passes_this_on", "[opt][inline]" )
{
    const Inlined p( "i32 f() { Reg r = Reg( 4 ); return r.value; }\n"
                     "class Reg { public i32 value; public Reg( i32 v ) { value = v; fill(); }"
                     " void fill() { value = mixed( 1, 2 ) + mixed( 3, 4 ) + mixed( 5, 6 ); } };\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

// Weighed as lowered, `Two()` is over the threshold: each assignment drops the field it replaces.
// Its own passes leave two stores, which is what its callers weigh; and no local is left behind
// that only storage markers name.
TEST_CASE( "inline_calls_weighs_a_callee_as_its_passes_leave_it", "[opt][inline]" )
{
    const Inlined p( "i64 f() { Two t = Two(); return t.a.value + t.b.value; }\n"
                     "class Two { public Box a; public Box b; public Two() { a = Box( 1 ); b = Box( 2 ); } };\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 0 );

    for( const Function& function : p.functions )
    {
        std::vector<bool> named( function.locals.size(), false );
        for( u32 l = 0; l <= function.parameter_count; ++l )
        {
            named[l] = true;
        }
        for_each_place(
            function,
            [&]( const Place& place, const Statement* statement )
            {
                if( place.local.is_valid() && ( !statement || ( statement->kind != Statement_kind::Storage_live &&
                                                                statement->kind != Statement_kind::Storage_dead ) ) )
                {
                    named[place.local.v] = true;
                }
            }
        );
        for( const Statement& statement : function.statements )
        {
            if( statement.drop_flag.is_valid() )
            {
                named[statement.drop_flag.v] = true;
            }
        }
        REQUIRE( std::ranges::all_of( named, []( bool n ) { return n; } ) );
    }
}

// A local holding `&twice`, its own or an inlined callee's parameter, calls `twice`, which then inlines.
TEST_CASE( "inline_calls_inlines_a_call_through_a_known_function", "[opt][inline]" )
{
    for( const char* source : {
             "i32 f( i32 a ) { fn( i32 ) -> i32 g = &twice; return g( a ); }\n",
             "i32 f( i32 a ) { return apply( &twice, a ); }\n"
             "i32 apply( fn( i32 ) -> i32 g, i32 x ) { return g( x ); }\n",
         } )
    {
        const Inlined p( source );
        INFO( source );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.first() ).empty() );
        REQUIRE( p.calls() == 0 );
        REQUIRE( p.indirect_calls() == 0 );
    }
}

// Assigned twice, the local may hold either.
TEST_CASE( "inline_calls_keeps_a_call_through_a_reassigned_local", "[opt][inline]" )
{
    const Inlined p( "i32 f( i32 a ) { fn( i32 ) -> i32 g = &twice; if( a > 0 ) { g = &thrice; } return g( a ); }\n"
                     "i32 thrice( i32 x ) { return x * 3; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.indirect_calls() == 1 );
}

TEST_CASE( "inline_drops_inlines_an_unconditional_drop", "[opt][inline]" )
{
    const Inlined p( "i64 f() { Box b = Box( 4 ); return b.value; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( false ) == 0 );
}

// Whether `~Box` runs is only known at run time, so the drop stays.
TEST_CASE( "inline_drops_keeps_a_flagged_drop", "[opt][inline]" )
{
    const Inlined p( "i64 f( bool c ) { Box b = Box( 3 ); if( c ) { consume( move b ); } return 0; }\n"
                     "void consume( move Box b ) { }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( true ) == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
