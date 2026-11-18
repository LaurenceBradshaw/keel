// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/remove_dead_stores.h"
#include "ir/live.h"
#include "ir/simplify.h"

#include <optional>
#include <span>

namespace keel
{
namespace
{
constexpr u32 k_untracked     = ~u32 { 0 };
constexpr u64 k_most_elements = 64;

// A slot per element of each small local array no writable pointer reaches, and per local that
// indexes one in a store; each slot holds its constant, or nothing.
struct Elements
{
    const Function&     func;
    const Literal_pool& literals;
    std::vector<u32>    slot;  // per local: its first slot, or k_untracked
    std::vector<u64>    count; // per local: a tracked array's elements, 0 for an index
    u32                 slots = 0;
};

Elements elements_of( const Function& func, const Type_table& types, const Literal_pool& literals )
{
    const u32 n = static_cast<u32>( func.locals.size() );
    Elements  e {
         .func = func, .literals = literals, .slot = std::vector<u32>( n, k_untracked ), .count = std::vector<u64>( n, 0 )
    };
    Local_set written( n, false ); // a pointer that may write it was taken
    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }
        const Rvalue& value = func.rvalue( statement );
        if( value.kind == Rvalue_kind::Address_of && value.address_purpose != Address_purpose::Read && value.a.has_place() &&
            !value.a.place.is_global() )
        {
            written[value.a.place.local.v] = true;
        }
    }
    for( u32 l = 0; l < n; ++l )
    {
        const Type_id type = func.locals[Local_id { l }].type;
        if( !written[l] && types.is_sized_array( type ) && types.get( type ).count <= k_most_elements )
        {
            e.slot[l]  = e.slots;
            e.count[l] = types.get( type ).count;
            e.slots += static_cast<u32>( e.count[l] );
        }
    }
    for( const Statement& statement : func.statements )
    {
        const Place& place = statement.place;
        if( place.is_global() || !place.local.is_valid() || place.num_projections == 0 || e.count[place.local.v] == 0 )
        {
            continue;
        }
        const Projection& first = func.projections[place.first_projection];
        if( first.kind == Projection_kind::Index && !written[first.index.v] && e.slot[first.index.v] == k_untracked )
        {
            e.slot[first.index.v] = e.slots++;
        }
    }
    return e;
}

Literal_id stored( const Function& func, const Statement& statement )
{
    const Rvalue& value = func.rvalue( statement );
    return value.kind == Rvalue_kind::Use && value.a.kind == Operand_kind::Constant ? value.a.constant : Literal_id {};
}

// Moves `state` past `statement`; whether it stores what every element it may write already holds.
bool step( const Elements& e, const Statement& statement, std::vector<Literal_id>& state )
{
    const Place& place = statement.place;
    if( place.is_global() || !place.local.is_valid() || e.slot[place.local.v] == k_untracked )
    {
        return false;
    }
    const u32 first = e.slot[place.local.v];
    if( e.count[place.local.v] == 0 )
    {
        state[first] =
            statement.kind == Statement_kind::Assign && place.is_whole_local() ? stored( e.func, statement ) : Literal_id {};
        return false;
    }

    const std::span<Literal_id> held( state.data() + first, e.count[place.local.v] );
    const Literal_id            value = statement.kind == Statement_kind::Assign ? stored( e.func, statement ) : Literal_id {};
    const auto                  same  = [&]( Literal_id h ) { return h.is_valid() && e.literals.same( h, value ); };
    const Projection*           index = place.num_projections == 1 ? &e.func.projections[place.first_projection] : nullptr;
    if( !value.is_valid() || index == nullptr || index->kind != Projection_kind::Index )
    {
        std::ranges::fill( held, Literal_id {} );
        return false;
    }

    const u32        at = e.slot[index->index.v];
    const Literal_id k  = at == k_untracked ? Literal_id {} : state[at];
    if( k.is_valid() && e.literals.integer( k ) < held.size() )
    {
        const bool kept               = same( held[e.literals.integer( k )] );
        held[e.literals.integer( k )] = value;
        return kept;
    }
    // A store past the end panics first.
    const bool kept = std::ranges::all_of( held, same );
    for( Literal_id& h : held )
    {
        h = same( h ) ? h : Literal_id {};
    }
    return kept;
}

// Forward over the blocks, an element's constant kept at a join where every path agrees.
void remove_redundant_stores( Function& func, const Type_table& types, const Literal_pool& literals )
{
    const Elements e = elements_of( func, types, literals );
    if( e.slots == 0 )
    {
        return;
    }

    std::vector<std::optional<std::vector<Literal_id>>> entry( func.blocks.size() );
    entry[0] = std::vector<Literal_id>( e.slots );
    std::vector<Block_id> work { Block_id { 0 } };
    std::vector<bool>     queued( func.blocks.size(), false );
    std::vector<Block_id> targets;
    while( !work.empty() )
    {
        const Block_id b = work.back();
        work.pop_back();
        queued[b.v]                   = false;
        std::vector<Literal_id> state = *entry[b.v];
        for( const Statement& statement : func.statements_of( b ) )
        {
            step( e, statement, state );
        }
        targets.clear();
        successors( func.blocks[b].terminator, targets );
        for( const Block_id t : targets )
        {
            bool changed = !entry[t.v];
            if( changed )
            {
                entry[t.v] = state;
            }
            for( u32 i = 0; i < e.slots; ++i )
            {
                Literal_id& held = ( *entry[t.v] )[i];
                if( held.is_valid() && !( state[i].is_valid() && literals.same( held, state[i] ) ) )
                {
                    held    = {};
                    changed = true;
                }
            }
            if( changed && !queued[t.v] )
            {
                queued[t.v] = true;
                work.push_back( t );
            }
        }
    }

    std::vector<bool> deleted( func.statements.size(), false );
    bool              removed = false;
    for( const Block_id b : func.blocks.ids() )
    {
        if( !entry[b.v] )
        {
            continue;
        }
        std::vector<Literal_id> state = *entry[b.v];
        const Block&            block = func.blocks[b];
        for( u32 s = block.first_statement; s < block.first_statement + block.statement_count; ++s )
        {
            deleted[s] = step( e, func.statements[s], state );
            removed |= deleted[s];
        }
    }
    if( removed )
    {
        remove_statements( func, deleted );
    }
}

bool remove_once( Function& func, bool pin_named, const Purity& purity )
{
    const Liveness    live      = live_locals( func );
    const Local_set   addressed = addressed_locals( func );
    std::vector<bool> deleted( func.statements.size(), false );
    bool              removed = false;

    for( u32 s = 0; s < func.statements.size(); ++s )
    {
        const Statement& statement = func.statements[s];
        const Rvalue&    value     = std::as_const( func ).rvalue( statement );

        // Liveness sees locals, not pointees, so a write a pointer may read stays; so does a call or an
        // allocation, which does more than write, unless the call is of a function that only returns.
        if( statement.kind != Statement_kind::Assign || statement.place.is_global() ||
            has_deref_projection( func, statement.place ) || addressed[statement.place.local.v] ||
            ( pin_named && func.locals[statement.place.local].name.is_valid() ) ||
            ( value.kind == Rvalue_kind::Call && !purity.removable( value ) ) || value.kind == Rvalue_kind::Indirect_call ||
            value.kind == Rvalue_kind::Allocate || value.kind == Rvalue_kind::Release ||
            live.after[s][statement.place.local.v] )
        {
            continue;
        }

        deleted[s] = true;
        removed    = true;
    }

    if( removed )
    {
        remove_statements( func, deleted );
    }

    return removed;
}
} // namespace

// A write that only fed a dead one is dead next round.
// Liveness is recomputed per round, quadratic at worst; a backward walk deleting in place if it shows.
void remove_dead_stores(
    Function& func, bool pin_named, const Purity& purity, const Type_table& types, const Literal_pool& literals
)
{
    remove_redundant_stores( func, types, literals );
    while( remove_once( func, pin_named, purity ) )
    {
    }
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <algorithm>
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/loops.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr std::string_view k_types = "struct Point { i32 x; i32 y; };\n"
                                     "i32 side( i32 n ) { return n; }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it, but with addresses kept. The function under test is always the first.
struct Stored : Compiled
{
    explicit Stored( std::string_view source, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build { .level = 2, .inlining = inlining, .forwarding = false }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    std::size_t assigns( Rvalue_kind kind ) const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s ) { return s.kind == Statement_kind::Assign && first().rvalue( s ).kind == kind; }
        ) );
    }

    std::size_t element_writes() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            {
                return s.kind == Statement_kind::Assign && s.place.num_projections == 1 &&
                       first().projections[s.place.first_projection].kind == Projection_kind::Index;
            }
        ) );
    }

    std::size_t field_writes() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            []( const Statement& s ) { return s.kind == Statement_kind::Assign && s.place.num_projections == 1; }
        ) );
    }
};

} // namespace

// `y`'s first value is overwritten unread, and `x` fed only that.
TEST_CASE( "remove_dead_stores_removes_a_write_only_a_dead_write_reads", "[opt][dead]" )
{
    const Stored p( "i32 f( i32 a ) { i32 x = a + 1; i32 y = x * 2; y = a; return y; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.assigns( Rvalue_kind::Binary ) == 0 );
}

// Nothing reads `q` after `q.x = 7` but `p`, which points at it.
TEST_CASE( "remove_dead_stores_keeps_a_write_a_pointer_reads", "[opt][dead]" )
{
    const Stored p( "i32 f( i32 a ) { Point q = Point { a, a }; Point* p = &q; q.x = 7; return p.x; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.field_writes() == 3 );
}

// Not inlined, so the call is still one; `side` only returns.
TEST_CASE( "remove_dead_stores_removes_an_unread_call_that_only_returns", "[opt][dead]" )
{
    const Stored p( "i32 f( i32 a ) { _ = side( a ); return a; }\n", std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.assigns( Rvalue_kind::Call ) == 0 );
}

// Pure, but a loop or recursion may not end, a panic ends the program, and a write is an effect.
TEST_CASE( "remove_dead_stores_keeps_an_unread_call_that_may_not_return", "[opt][dead]" )
{
    for( const std::string_view callee :
         { "i32 g( i32 n ) { i32 s = 0; while( n > s ) { s += 1; } return s; }\n",
           "i32 g( i32 n ) { return n <= 0 ? 0 : g( n - 1 ); }\n",
           "i32 g( i32 n ) { return 10 / n; }\n",
           "i32 t = 0;\ni32 g( i32 n ) { t = n; return n; }\n" } )
    {
        const Stored p( "i32 f( i32 a ) { _ = g( a ); return a; }\n" + std::string( callee ), std::nullopt );
        INFO( callee );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.first() ).empty() );
        REQUIRE( p.assigns( Rvalue_kind::Call ) == 1 );
    }
}

// Every element already holds 0, so `a[k] = 0` goes at any `k`; 1 differs, and -0.0 is not 0.0.
TEST_CASE( "remove_dead_stores_removes_a_store_its_elements_already_hold", "[opt][dead]" )
{
    const Stored same( "i32 f( u64 k ) { i32[4] a = [ 0, 0, 0, 0 ]; a[k] = 0; return a[k ^ 1]; }\n" );
    const Stored other( "i32 f( u64 k ) { i32[4] a = [ 0, 0, 0, 0 ]; a[k] = 1; return a[k ^ 1]; }\n" );
    const Stored sign( "f64 f( u64 k ) { f64[4] a = [ 0.0, 0.0, 0.0, 0.0 ]; a[k] = -0.0; return a[k ^ 1]; }\n" );
    for( const Stored* p : { &same, &other, &sign } )
    {
        REQUIRE( p->clean() );
        REQUIRE( verify( p->first() ).empty() );
    }
    REQUIRE( same.element_writes() + 1 == other.element_writes() );
    REQUIRE( sign.element_writes() == other.element_writes() );
}

// The fill unrolls to constant stores only in the loop phase, so the second loop empties after it.
TEST_CASE( "remove_dead_stores_empties_a_loop_unrolling_made_redundant", "[opt][dead]" )
{
    const Stored p( "f64 f( u64 k ) { f64[16] a = [ 0.0, .. ]; for( u64 i : 0..16 ) { a[i] = 0.0; } a[k] += 1.0; "
                    "return a[k ^ 1]; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( find_loops( p.first() ).loops.empty() );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS