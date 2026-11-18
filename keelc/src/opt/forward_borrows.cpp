// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/forward_borrows.h"
#include <algorithm>
#include <cassert>
#include "ir/live.h"
#include "ir/simplify.h"

namespace keel
{
namespace
{
// The local each pointer can only point at, where the pointer never escapes. A pointer assigned
// once, a copy of another, points where that one does.
std::vector<Local_id> forwarded_to( const Function& func, const Type_table& table, bool pin_named )
{
    std::vector<Local_id>         to( func.locals.size() );
    std::vector<Local_id>         copy_of( func.locals.size() );
    std::vector<bool>             refused( func.locals.size(), false );
    std::vector<const Statement*> assigned_by( func.locals.size(), nullptr );

    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign || !statement.place.is_whole_local() )
        {
            continue;
        }

        const u32     p     = statement.place.local.v;
        const Rvalue& value = func.rvalue( statement );
        const Type_id type  = func.locals[statement.place.local].type;
        const bool    borrow =
            value.kind == Rvalue_kind::Address_of && value.a.place.is_whole_local() &&
            ( value.address_purpose == Address_purpose::Borrow || value.address_purpose == Address_purpose::Read ) &&
            table.is_pointer( type ) && table.get( type ).element == func.locals[value.a.place.local].type;
        const bool copy = value.kind == Rvalue_kind::Use && value.a.has_place() && value.a.place.is_whole_local() &&
                          table.is_pointer( type ) && func.locals[value.a.place.local].type == type;

        if( ( !borrow && !copy ) || assigned_by[p] != nullptr )
        {
            refused[p] = true;
            continue;
        }

        ( borrow ? to[p] : copy_of[p] ) = value.a.place.local;
        assigned_by[p]                  = &statement;
    }

    // A whole `_p` anywhere but its own assignment, storage markers and a copy into another is the
    // pointer escaping.
    std::vector<u32> uses( func.locals.size(), 0 );
    for_each_place(
        func,
        [&]( const Place& place, const Statement* owner )
        {
            if( !place.is_whole_local() || assigned_by[place.local.v] == owner )
            {
                return;
            }

            const bool marker = owner != nullptr &&
                                ( owner->kind == Statement_kind::Storage_live || owner->kind == Statement_kind::Storage_dead );
            uses[place.local.v] += !marker;
        }
    );

    for( const Local_id q : copy_of )
    {
        if( q.is_valid() )
        {
            uses[q.v]--;
        }
    }

    for( u32 p = 0; p < uses.size(); ++p )
    {
        refused[p] = refused[p] || uses[p] > 0 || ( pin_named && func.locals[Local_id { p }].name.is_valid() );
    }

    refused[k_return_slot.v] = true;

    // A copy is forwarded only with what it copies, and copying into one kept is an escape.
    for( bool changed = true; changed; )
    {
        changed = false;
        for( u32 p = 0; p < to.size(); ++p )
        {
            const Local_id q = copy_of[p];
            if( !q.is_valid() || refused[p] == refused[q.v] )
            {
                continue;
            }
            refused[p] = refused[q.v] = true;
            changed                   = true;
        }
    }

    // Each copy takes its source's target; a cycle of copies has none.
    for( bool changed = true; changed; )
    {
        changed = false;
        for( u32 p = 0; p < to.size(); ++p )
        {
            const Local_id q = copy_of[p];
            if( q.is_valid() && !to[p].is_valid() && to[q.v].is_valid() )
            {
                to[p]   = to[q.v];
                changed = true;
            }
        }
    }

    for( u32 p = 0; p < to.size(); ++p )
    {
        if( refused[p] )
        {
            to[p] = Local_id {};
        }
    }

    return to;
}

// Where a write of a pointer into a local array gets its element: `&a[i]`, or the pointer it copies,
// runs over, takes the data of, or offsets by `step`.
struct Offset
{
    Local_id array {}; // `&a[i]`'s `a`
    Local_id from {};  // else the pointer it derives from
    Local_id index {}; // `&a[i]`'s `i`
    Operand  step {};  // `from + step`, where `plus`
    bool     plus = false;
};

std::optional<Offset> offset_of( const Function& func, const Type_table& table, const Rvalue& value )
{
    const auto whole = []( const Operand& o ) { return o.has_place() && o.place.is_whole_local(); };
    if( value.kind == Rvalue_kind::Address_of &&
        ( value.address_purpose == Address_purpose::Borrow || value.address_purpose == Address_purpose::Read ) &&
        !value.a.place.is_global() && value.a.place.local.is_valid() && value.a.place.num_projections == 1 &&
        func.projections[value.a.place.first_projection].kind == Projection_kind::Index &&
        table.is_array( func.locals[value.a.place.local].type ) )
    {
        return Offset { .array = value.a.place.local, .index = func.projections[value.a.place.first_projection].index };
    }
    if( ( value.kind == Rvalue_kind::Run_of || value.kind == Rvalue_kind::Run_data || value.kind == Rvalue_kind::Use ) &&
        whole( value.a ) )
    {
        return Offset { .from = value.a.place.local };
    }
    const auto u64 = [&]( Type_id t )
    {
        const Type& type = table.get( t );
        return type.kind == Type_kind::Int && type.width == 64 && !type.is_signed;
    };
    if( value.kind == Rvalue_kind::Binary && value.op == Token_kind::Plus && whole( value.a ) &&
        table.is_many_pointer( value.a.type ) && u64( value.b.type ) )
    {
        return Offset { .from = value.a.place.local, .step = value.b, .plus = true };
    }
    return std::nullopt;
}

// `(*q)` becomes `a[k]` where every write of `q` derives it from `&a[i]`: `k` is a new local written
// beside each of them, so it always holds the element `q` points at.
void forward_offsets( Function& func, const Type_table& table )
{
    const u32 n = narrow_cast<u32>( func.locals.size() );
    struct Write
    {
        Block_id block;
        u32      index;
        Offset   offset;
    };
    std::vector<std::vector<Write>> writes( n );
    std::vector<bool>               refused( n, false );
    const Local_set                 addressed = addressed_locals( func );
    for( const Block_id b : func.blocks.ids() )
    {
        const auto statements = func.statements_of( func.blocks[b] );
        for( u32 s = 0; s < statements.size(); ++s )
        {
            const Statement& st = statements[s];
            if( ( st.kind != Statement_kind::Assign && st.kind != Statement_kind::Drop ) || st.place.is_global() ||
                !st.place.local.is_valid() || st.place.num_projections > 0 )
            {
                continue;
            }
            const u32                   p = st.place.local.v;
            const std::optional<Offset> offset =
                st.kind == Statement_kind::Assign ? offset_of( func, table, func.rvalue( st ) ) : std::nullopt;
            if( offset )
            {
                writes[p].push_back( Write { b, s, *offset } );
            }
            refused[p] = refused[p] || !offset;
        }
    }

    // The array each pointer points into, where every write agrees on one.
    std::vector<Local_id> array( n );
    for( bool changed = true; changed; )
    {
        changed = false;
        for( u32 p = 0; p < n; ++p )
        {
            if( refused[p] || writes[p].empty() )
            {
                continue;
            }
            Local_id a {};
            bool     known = true;
            for( const Write& w : writes[p] )
            {
                const Offset& o = w.offset;
                if( !o.array.is_valid() && refused[o.from.v] )
                {
                    refused[p] = true;
                    break;
                }
                const Local_id got = o.array.is_valid() ? o.array : array[o.from.v];
                if( !got.is_valid() )
                {
                    known = o.from.v == p && known; // its own last value, `q = q + 1`
                    continue;
                }
                if( a.is_valid() && a != got )
                {
                    refused[p] = true;
                    break;
                }
                a = got;
            }
            refused[p] = refused[p] || addressed[p] || p == k_return_slot.v;
            if( refused[p] )
            {
                array[p] = Local_id {};
                changed  = true;
                continue;
            }
            if( known && a.is_valid() && array[p] != a )
            {
                array[p] = a;
                changed  = true;
            }
        }
    }

    // Only the chains behind a pointer dereferenced to an element of its array.
    const auto element = [&]( u32 q )
    {
        const Type_id type = func.locals[Local_id { q }].type;
        return array[q].is_valid() && !refused[q] && ( table.is_pointer( type ) || table.is_many_pointer( type ) ) &&
               table.get( type ).element == table.get( func.locals[array[q]].type ).element;
    };
    std::vector<bool> needed( n, false );
    for_each_place(
        func,
        [&]( const Place& place, const Statement* )
        {
            if( !place.is_global() && place.local.is_valid() && place.num_projections > 0 && element( place.local.v ) )
            {
                needed[place.local.v] = true;
            }
        }
    );
    if( std::ranges::none_of( needed, []( bool b ) { return b; } ) )
    {
        return;
    }
    for( bool changed = true; changed; )
    {
        changed = false;
        for( u32 p = 0; p < n; ++p )
        {
            for( const Write& w : writes[p] )
            {
                if( needed[p] && w.offset.from.is_valid() && !needed[w.offset.from.v] )
                {
                    needed[w.offset.from.v] = changed = true;
                }
            }
        }
    }

    Type_id u64 {};
    for( u32 p = 0; p < n && !u64.is_valid(); ++p )
    {
        for( const Write& w : writes[p] )
        {
            u64 = w.offset.array.is_valid() ? func.locals[w.offset.index].type : u64;
        }
    }
    std::vector<Local_id> index( n );
    for( u32 p = 0; p < n; ++p )
    {
        if( needed[p] )
        {
            index[p] = func.locals.push_back( Local { .type = u64, .span = func.locals[Local_id { p }].span } );
        }
    }

    // Each write's element first, so both read what the write reads; a statement writes one pointer.
    std::vector<std::optional<Statement>> before( func.statements.size() );
    for( u32 p = 0; p < n; ++p )
    {
        for( const Write& w : writes[p] )
        {
            if( !needed[p] )
            {
                continue;
            }
            const Offset& o     = w.offset;
            const Operand from  = copy( Place { .local = o.array.is_valid() ? o.index : index[o.from.v] }, u64 );
            const Rvalue  value = o.plus ? binary( Token_kind::Plus, from, o.step, u64 ) : use( from );
            const u32     at    = func.blocks[w.block].first_statement + w.index;
            before[at]          = Statement {
                         .span = func.statements[at].span, .place = Place { .local = index[p] }, .value = func.add_rvalue( value )
            };
        }
    }
    std::vector<Statement> statements;
    statements.reserve( func.statements.size() );
    for( Block& block : func.blocks )
    {
        const u32 first = narrow_cast<u32>( statements.size() );
        for( u32 s = block.first_statement; s < block.first_statement + block.statement_count; ++s )
        {
            if( before[s] )
            {
                statements.push_back( std::move( *before[s] ) );
            }
            statements.push_back( std::move( func.statements[s] ) );
        }
        block.first_statement = first;
        block.statement_count = narrow_cast<u32>( statements.size() ) - first;
    }
    func.statements = std::move( statements );

    // `(*q).rest` is `a[k].rest`.
    for_each_place(
        func,
        [&]( Place& place, const Statement* )
        {
            if( place.is_global() || !place.local.is_valid() || place.num_projections == 0 || !element( place.local.v ) ||
                !index[place.local.v].is_valid() )
            {
                return;
            }
            assert( func.projections[place.first_projection].kind == Projection_kind::Deref );
            const u32 first = narrow_cast<u32>( func.projections.size() );
            func.projections.push_back( Projection { .kind = Projection_kind::Index, .index = index[place.local.v] } );
            for( u32 k = 1; k < place.num_projections; ++k )
            {
                const Projection next = func.projections[place.first_projection + k];
                func.projections.push_back( next );
            }
            place.local            = array[place.local.v];
            place.first_projection = first;
        }
    );
}
} // namespace

void forward_borrows( Function& func, const Type_table& table, bool pin_named )
{
    if( !pin_named )
    {
        forward_offsets( func, table );
    }
    const std::vector<Local_id> to = forwarded_to( func, table, pin_named );

    if( std::ranges::none_of( to, []( Local_id k ) { return k.is_valid(); } ) )
    {
        return;
    }

    std::vector<bool> deleted( func.statements.size(), false );

    for( u32 s = 0; s < func.statements.size(); ++s )
    {
        const Place& place = func.statements[s].place;
        deleted[s]         = place.is_whole_local() && to[place.local.v].is_valid();
    }

    // `(*_p).f` is `_k.f`: drop the leading `Deref`.
    for_each_place(
        func,
        [&]( Place& place, const Statement* )
        {
            if( place.is_global() || !place.local.is_valid() || !to[place.local.v].is_valid() || place.num_projections == 0 )
            {
                return;
            }

            assert( func.projections[place.first_projection].kind == Projection_kind::Deref );
            place.local = to[place.local.v];
            ++place.first_projection;
            --place.num_projections;
        }
    );

    remove_statements( func, deleted );
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

constexpr std::string_view k_types =
    "interface shape { i32 area() const; };\n"
    "class Box implements shape { public i32 v; public Box( i32 n ) { v = n; } public i32 area() const { return v; } };\n"
    "i32 look( i32* q ) { return 0; }\n"
    "i32 main() { return 0; }\n";

// Optimised, as a release build lowers it. The function under test is always the first.
struct Forwarded : Compiled
{
    explicit Forwarded( std::string_view source, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .level = 2, .inlining = inlining }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    std::size_t addresses() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            { return s.kind == Statement_kind::Assign && first().rvalue( s ).kind == Rvalue_kind::Address_of; }
        ) );
    }

    std::size_t derefs() const
    {
        const Function& func  = first();
        std::size_t     count = 0;
        for_each_place( func, [&]( const Place& place, const Statement* ) { count += has_deref_projection( func, place ); } );
        return count;
    }
};

} // namespace

// The two `ref`s go, and what is left folds to the sum.
TEST_CASE( "forward_borrows_names_the_local_a_ref_points_at", "[opt][forward]" )
{
    for( const char* source : {
             "i32 f() { i32 n = 1; ref i32 r1 = n; ref i32 r2 = n; r1 = 2; n = 3; return r1 + r2; }\n",
             "i32 f( i32 a ) { i32 t = 0; i32 n = 0; while( n < a ) { i32 k = n; ref i32 r = k; r = r + 1; t = t + r; n++; } "
             "return t; }\n",
         } )
    {
        const Forwarded p( source );
        INFO( source );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.first() ).empty() );
        REQUIRE( p.addresses() == 0 );
        REQUIRE( p.derefs() == 0 );
    }
}

// An inlined method's `this` is a copy of the caller's pointer, which points at the same local.
TEST_CASE( "forward_borrows_names_the_local_a_copied_pointer_points_at", "[opt][forward]" )
{
    const Forwarded p( "i64 f() { Counter a = Counter(); Counter b = Counter(); a.both( ref b ); return a.n + b.n; }\n"
                       "class Counter { public i64 n; public Counter() { n = 1; } public void times( i64 k ) { n = n * k; }"
                       " public void both( ref Counter o ) { times( 2 ); this.times( 3 ); o.times( 5 ); } };\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.addresses() == 0 );
    REQUIRE( p.derefs() == 0 );
}

// A borrow of a field, which only splitting makes a borrow of a whole local.
TEST_CASE( "forward_borrows_names_the_field_splitting_made_a_local", "[opt][forward]" )
{
    const Forwarded p(
        "i32 f() { Holder h = Holder(); return h.x() + deep( h ); }\n"
        "struct Pair { i32 x; i32 y; };\n"
        "class Holder { Pair p; public Holder() { p = Pair { 1, 2 }; } public const ref Pair get() const { return p; }"
        " public const ref i32 x() const { return get().x; } };\n"
        "const ref i32 deep( const ref Holder h ) { return h.get().x; }\n"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.addresses() == 0 );
    REQUIRE( p.derefs() == 0 );
}

// The inlined loop writes through a run of `a`, and each element it reaches is `a`'s own, so `a` keeps no address.
TEST_CASE( "forward_borrows_names_the_element_a_run_of_a_local_reaches", "[opt][forward]" )
{
    const Forwarded p( "u64 f( u64 x ) { u64[4] a = [ x, x, x, x ]; twice( ref a[0..4] ); return a[1] + a[2]; }\n"
                       "void twice( ref u64[] r ) { for( u64 i : 0..r.size ) { r[i] = r[i] * 2; } }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.addresses() == 0 );
    REQUIRE( p.derefs() == 0 );
}

// An element borrowed by an accessor is a pointer to one element, not a run, and is `a`'s own too.
TEST_CASE( "forward_borrows_names_the_element_an_accessor_borrows", "[opt][forward]" )
{
    const Forwarded p( "u64 f( u64 x ) { Four a = Four( x ); return a.at( x % 4 ) + a.at( 1 ); }\n"
                       "class Four { u64[4] e; public Four( u64 x ) { e = [ x, x, x, x ]; }"
                       " public const ref u64 at( u64 i ) const { return e[i]; } };\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.addresses() == 0 );
    REQUIRE( p.derefs() == 0 );
}

// A run of one array or another may reach either, so it keeps its address.
TEST_CASE( "forward_borrows_keeps_a_run_of_one_of_two_arrays", "[opt][forward]" )
{
    const Forwarded p( "u64 f( u64 x, bool c ) { u64[4] a = [ x, x, x, x ]; u64[4] b = [ x, x, x, x ];"
                       " put( ref pick( c, ref a, ref b ), x ); return a[1] + b[2]; }\n"
                       "ref u64[] pick( bool c, ref u64[4] a, ref u64[4] b ) { if( c ) { return a[0..4]; } return b[0..4]; }\n"
                       "void put( ref u64[] r, u64 x ) { r[x % 4] = 5; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.addresses() > 0 );
}

// Pointing at one of two, or passed on: each keeps its address.
TEST_CASE( "forward_borrows_keeps_a_pointer_that_may_point_elsewhere", "[opt][forward]" )
{
    for( const char* source : {
             "i32 f( i32 a ) { i32 x = 1; i32 y = 2; i32* p = &x; if( a > 0 ) { p = &y; } return *p; }\n",
         } )
    {
        const Forwarded p( source );
        INFO( source );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.first() ).empty() );
        REQUIRE( p.addresses() > 0 );
    }

    const Forwarded passed( "i32 f() { i32 n = 1; i32* p = &n; return look( p ); }\n", std::nullopt );
    REQUIRE( passed.clean() );
    REQUIRE( passed.addresses() == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
