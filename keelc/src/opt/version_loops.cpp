// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/version_loops.h"
#include <algorithm>
#include <optional>
#include <vector>
#include "ir/simplify.h"
#include "opt/forward_reads.h"
#include "opt/hoist_invariants.h"
#include "opt/reduce_inductions.h"

namespace keel
{
namespace
{

constexpr u32 k_none = ~u32 { 0 };

// What the preheader reads for a value the loop uses: an operand, or the read at `read`.
struct Source
{
    Operand operand {};
    u32     read = k_none;

    bool same( const Source& other ) const
    {
        return read == other.read &&
               ( read != k_none || ( operand.place.is_whole_local() && other.operand.place.is_whole_local() &&
                                     operand.place.local == other.operand.place.local ) );
    }
};

bool is_local( const Operand& operand, Local_id local )
{
    return operand.kind == Operand_kind::Copy && operand.place.is_whole_local() && operand.place.local == local;
}

// `(*p)` then only fields: a part of the one object `p` points at.
bool fields_after_deref( const Function& func, const Place& place )
{
    const Run<const Projection> projections = func.projections_of( place );
    if( place.is_global() || projections.size() == 0 || projections[0].kind != Projection_kind::Deref )
    {
        return false;
    }
    for( u32 i = 1; i < projections.size(); ++i )
    {
        if( projections[i].kind != Projection_kind::Field )
        {
            return false;
        }
    }
    return true;
}

// By local: the statements in `loop`, or in the function without one, that write any of it or take its address.
std::vector<u32> writes_in( const Function& func, const Loop* loop )
{
    std::vector<u32> writes( func.locals.size(), 0 );
    for( const Block_id b : func.blocks.ids() )
    {
        if( loop != nullptr && !loop->contains( b ) )
        {
            continue;
        }
        for( const Statement& statement : func.statements_of( b ) )
        {
            if( statement.kind == Statement_kind::Storage_live || statement.kind == Statement_kind::Storage_dead )
            {
                continue;
            }
            if( !statement.place.is_global() && statement.place.local.is_valid() &&
                !has_deref_projection( func, statement.place ) )
            {
                ++writes[statement.place.local.v];
            }
            if( statement.kind == Statement_kind::Assign )
            {
                const Rvalue& value = func.rvalue( statement );
                if( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() &&
                    !has_deref_projection( func, value.a.place ) )
                {
                    writes[value.a.place.local.v] += 2;
                }
            }
        }
    }
    return writes;
}

void append( Function& func, Block_id block, Statement statement )
{
    insert_statement( func, block, func.blocks[block].statement_count, std::move( statement ) );
}

bool version(
    Function& func, Type_table& types, Literal_pool& literals, const Loops& found, u32 index, const Loop_weights& weights
)
{
    const Loop& loop = found.loops[index];
    if( !loop.preheader.is_valid() || !loop.latch.is_valid() || func.blocks[loop.header].versioned )
    {
        return false;
    }
    const Terminator test = func.blocks[loop.latch].terminator;
    if( test.kind != Terminator_kind::Branch || test.targets[0] != loop.header || loop.contains( test.targets[1] ) ||
        test.condition.kind != Operand_kind::Copy || !test.condition.place.is_whole_local() )
    {
        return false;
    }

    u64 size = 0;
    for( const Block_id b : loop.blocks )
    {
        for( const Statement& statement : func.statements_of( b ) )
        {
            size += statement.kind == Statement_kind::Assign || statement.kind == Statement_kind::Drop;
        }
    }
    if( size > weights.versioning )
    {
        return false;
    }

    const std::optional<std::vector<Blocked_read>> blocked = blocked_reads( func, found, index );
    if( !blocked )
    {
        return false;
    }
    const Local_set        addressed = addressed_locals_of( func );
    const std::vector<u32> all       = writes_in( func, nullptr );
    const std::vector<u32> inside    = writes_in( func, &loop );

    // A read the preheader may make: one the loop makes on its first turn before anything is seen,
    // or a part of a parameter passed by address, which holds a live object for the whole call.
    std::vector<bool> usable( blocked->size(), false );
    bool              any = false;
    for( u32 k = 0; k < blocked->size(); ++k )
    {
        const Blocked_read& read      = ( *blocked )[k];
        const Local_id      root      = read.place.local;
        const bool          reference = fields_after_deref( func, read.place ) && all[root.v] == 0 &&
                               std::ranges::find( func.references, root ) != func.references.end();
        const Type& type   = types.get( read.type );
        const bool  scalar = type.kind == Type_kind::Bool || type.kind == Type_kind::Int || type.kind == Type_kind::Float ||
                            type.kind == Type_kind::Many_pointer ||
                            ( type.kind == Type_kind::Pointer && !types.is_interface_pointer( read.type ) );
        usable[k] = scalar && ( read.early || reference );
        any |= usable[k] && read.reached;
    }
    if( !any )
    {
        return false;
    }

    // `local`'s one write in the loop, at an index before `before` in `block`.
    const auto assignment = [&]( Local_id local, Block_id block, u32 before ) -> std::optional<u32>
    {
        if( inside[local.v] != 1 || is_addressed( addressed, local ) )
        {
            return std::nullopt;
        }
        const Run<const Statement> statements = func.statements_of( block );
        for( u32 s = 0; s < before; ++s )
        {
            if( statements[s].kind == Statement_kind::Assign && statements[s].place.is_whole_local() &&
                statements[s].place.local == local )
            {
                return s;
            }
        }
        return std::nullopt;
    };
    const auto read_of = [&]( const Place& place ) -> std::optional<Source>
    {
        for( u32 k = 0; k < blocked->size(); ++k )
        {
            if( usable[k] && same_place( func, ( *blocked )[k].place, place ) )
            {
                return Source { .read = k };
            }
        }
        return std::nullopt;
    };
    // `operand`'s value where `block` reads it before `before`, as the preheader can give it.
    const auto source = [&]( const Operand& operand, Block_id block, u32 before ) -> std::optional<Source>
    {
        if( operand.kind == Operand_kind::Constant )
        {
            return Source { .operand = operand };
        }
        if( operand.kind != Operand_kind::Copy )
        {
            return std::nullopt;
        }
        if( !operand.place.is_whole_local() )
        {
            return read_of( operand.place );
        }
        const Local_id local = operand.place.local;
        if( inside[local.v] == 0 && !is_addressed( addressed, local ) )
        {
            return Source { .operand = operand };
        }
        const std::optional<u32> at = assignment( local, block, before );
        if( !at )
        {
            return std::nullopt;
        }
        const Rvalue& value = func.rvalue( func.statements_of( block )[*at] );
        return value.kind == Rvalue_kind::Use && value.a.kind == Operand_kind::Copy ? read_of( value.a.place ) : std::nullopt;
    };

    // `i`, stepped by 1 in the latch, and the latch's `i < n` after the step.
    const std::vector<Induction> inductions = inductions_of( func, loop, addressed );
    const Induction*             i          = nullptr;
    std::optional<Source>        limit;
    const Local_id               condition  = test.condition.place.local;
    const u32                    latch_size = func.blocks[loop.latch].statement_count;
    const std::optional<u32>     tested     = assignment( condition, loop.latch, latch_size );
    for( const Induction& candidate : inductions )
    {
        const Type& type = types.get( func.locals[candidate.local].type );
        if( !tested || candidate.block != loop.latch || candidate.index >= *tested || type.kind != Type_kind::Int ||
            type.width != 64 || candidate.step.kind != Operand_kind::Constant ||
            literals.integer( candidate.step.constant ) != 1 )
        {
            continue;
        }
        const Rvalue& value = func.rvalue( func.statements_of( loop.latch )[*tested] );
        if( value.kind == Rvalue_kind::Binary && value.op == Token_kind::Less && is_local( value.a, candidate.local ) )
        {
            limit = source( value.b, loop.latch, *tested );
            i     = &candidate;
            break;
        }
    }
    if( !i || !limit )
    {
        return false;
    }

    // Every store is to a part of `*( b + i )`, one `b` for all, with `i` as the turn began.
    std::optional<Source> base;
    for( const Block_id b : loop.blocks )
    {
        const Run<const Statement> statements = func.statements_of( b );
        for( u32 s = 0; s < statements.size(); ++s )
        {
            const Statement& store = statements[s];
            if( store.kind != Statement_kind::Assign || !has_deref_projection( func, store.place ) )
            {
                continue;
            }
            if( !fields_after_deref( func, store.place ) )
            {
                return false;
            }
            std::optional<Source> found_base;
            Local_id              q = store.place.local;
            for( u32 at = s;; )
            {
                const std::optional<u32> d = assignment( q, b, at );
                if( !d || ( b == loop.latch && *d > i->index ) )
                {
                    return false;
                }
                at                  = *d;
                const Rvalue& value = func.rvalue( statements[at] );
                if( value.kind == Rvalue_kind::Address_of && fields_after_deref( func, value.a.place ) )
                {
                    q = value.a.place.local;
                }
                else if( value.kind == Rvalue_kind::Use && value.a.kind == Operand_kind::Copy &&
                         value.a.place.is_whole_local() )
                {
                    q = value.a.place.local;
                }
                else if( value.kind == Rvalue_kind::Binary && value.op == Token_kind::Plus && is_local( value.b, i->local ) &&
                         types.is_many_pointer( value.a.type ) )
                {
                    found_base = source( value.a, b, at );
                    break;
                }
                else
                {
                    return false;
                }
            }
            if( !found_base || ( base && !base->same( *found_base ) ) )
            {
                return false;
            }
            base = found_base;
        }
    }
    if( !base )
    {
        return false;
    }

    // In the preheader: each read once, then whether `[ b + i, b + n )` holds any a store may reach.
    const Block_id preheader = loop.preheader;
    const Span     span      = test.span;
    const Type_id  word      = types.integer( 64, false );
    const Type_id  boolean   = types.builtin( Type_kind::Bool );
    const auto     temporary = [&]( Type_id type, Rvalue value )
    {
        const Local_id local = func.locals.push_back( Local { .type = type, .span = span } );
        append(
            func,
            preheader,
            Statement { .span = span, .place = Place { .local = local }, .value = func.add_rvalue( std::move( value ) ) }
        );
        return copy( Place { .local = local }, type );
    };
    std::vector<Operand> held( blocked->size() );
    for( u32 k = 0; k < blocked->size(); ++k )
    {
        if( usable[k] )
        {
            const Blocked_read& read = ( *blocked )[k];
            held[k]                  = temporary( read.type, use( copy( read.place, read.type ) ) );
        }
    }
    const auto    value_of = [&]( const Source& s ) { return s.read == k_none ? s.operand : held[s.read]; };
    const Operand b        = value_of( *base );
    const Operand n        = value_of( *limit );
    const Operand first    = copy( Place { .local = i->local }, func.locals[i->local].type );
    const auto    address  = [&]( const Operand& pointer, const Operand& offset )
    { return temporary( word, cast_to( temporary( b.type, binary( Token_kind::Plus, pointer, offset, b.type ) ), word ) ); };
    const Operand lo   = address( b, first );
    const Operand hi   = address( b, n );
    const Operand one  = constant( literals.add_integer( 1 ), word );
    Operand       none = temporary( boolean, binary( Token_kind::Less, first, n, boolean ) );
    for( u32 k = 0; k < blocked->size(); ++k )
    {
        if( !usable[k] || !( *blocked )[k].reached )
        {
            continue;
        }
        const Blocked_read& read  = ( *blocked )[k];
        const Type_id       many  = types.many_pointer_to( read.type );
        const Operand       at    = temporary( many, address_of( read.place, many, Address_purpose::Read ) );
        const Operand       start = temporary( word, cast_to( at, word ) );
        const Operand end   = temporary( word, cast_to( temporary( many, binary( Token_kind::Plus, at, one, many ) ), word ) );
        const Operand below = temporary( boolean, binary( Token_kind::Less_equal, end, lo, boolean ) );
        const Operand above = temporary( boolean, binary( Token_kind::Greater_equal, start, hi, boolean ) );
        const Operand apart = temporary( boolean, binary( Token_kind::Pipe, below, above, boolean ) );
        none                = temporary( boolean, binary( Token_kind::Amp, none, apart, boolean ) );
    }

    // insert_statement leaves the preheader's old run behind.
    remove_statements( func, std::vector<bool>( func.statements.size(), false ) );

    std::vector<Block_id> copies;
    clone_blocks( func, loop, copies );
    func.blocks[loop.header].versioned           = true;
    func.blocks[copies[loop.header.v]].versioned = true;
    func.blocks[preheader].terminator            = Terminator {
                   .kind = Terminator_kind::Branch, .span = span, .condition = none, .targets = { loop.header, copies[loop.header.v] }
    };

    // The loop that runs when none overlaps reads the preheader's values.
    const auto replace = [&]( Operand& operand )
    {
        if( operand.kind != Operand_kind::Copy || !has_deref_projection( func, operand.place ) )
        {
            return;
        }
        if( const std::optional<Source> read = read_of( operand.place ) )
        {
            const Span at = operand.span;
            operand       = held[read->read];
            operand.span  = at;
        }
    };
    for( const Block_id block : loop.blocks )
    {
        for( Statement& statement : func.statements_of( block ) )
        {
            if( statement.kind != Statement_kind::Assign || func.rvalue( statement ).kind == Rvalue_kind::Address_of )
            {
                continue;
            }
            Rvalue& value = func.rvalue( statement );
            replace( value.a );
            replace( value.b );
            for( Operand& argument : value.arguments )
            {
                replace( argument );
            }
        }
        Terminator& terminator = func.blocks[block].terminator;
        replace( terminator.condition );
        replace( terminator.message );
        replace( terminator.at );
    }
    return true;
}

} // namespace

bool version_loops( Function& func, Type_table& types, Literal_pool& literals, const Loop_weights& weights )
{
    bool versioned = false;
    for( bool again = true; again; )
    {
        again             = false;
        const Loops found = find_loops( func );
        for( u32 l = 0; l < found.loops.size(); ++l )
        {
            if( version( func, types, literals, found, l, weights ) )
            {
                normalise_loops( func );
                again = versioned = true;
                break;
            }
        }
    }
    return versioned;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/print.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

// Optimised and elaborated at -O3 without unrolling: by loop in the first function, the operands it reads through a pointer.
std::vector<std::size_t> reads_by_loop( std::string_view source, u32 versioning = Loop_weights {}.versioning, u32 level = 3 )
{
    const Compiled c(
        std::string( source ) + "struct Buf { i64[*] data; u64 count; i64 last; };\ni32 g = 0;\nvoid touch() { g += 1; }\ni32 "
                                "main() { return 0; }\n",
        Stage::Elaborated,
        Build { .level = level, .loops = Loop_weights { .unrolling = 0, .versioning = versioning } }
    );
    INFO( c.rendered() );
    REQUIRE( c.clean() );
    const Function& func = c.functions.front();
    INFO( print( func, c.ast, c.types.table(), c.literals, c.interner ) );
    REQUIRE( verify( func ).empty() );

    std::vector<std::size_t> reads;
    for( const Loop& loop : find_loops( func ).loops )
    {
        std::size_t through = 0;
        for( const Block_id b : loop.blocks )
        {
            for( const Statement& statement : func.statements_of( b ) )
            {
                if( statement.kind == Statement_kind::Assign && func.rvalue( statement ).kind != Rvalue_kind::Address_of )
                {
                    for_each_operand(
                        func.rvalue( statement ),
                        [&]( const Operand& operand )
                        { through += operand.kind == Operand_kind::Copy && has_deref_projection( func, operand.place ); }
                    );
                }
            }
        }
        reads.push_back( through );
    }
    std::ranges::sort( reads );
    return reads;
}

constexpr std::string_view k_moved = "void f( ref Buf ys, const ref Buf xs ) { for( u64 i = 0; i < xs.count; i += 1 ) "
                                     "{ unsafe { ys.data[ i ] = xs.data[ i ] + 1; } } }\n";

} // namespace

// The copy taken when no header lies in `ys.data[ 0 .. xs.count ]` reads only `xs.data[ i ]`; the
// other reads both headers each turn, as before.
TEST_CASE( "version_loops_copies_a_loop_storing_through_one_reference_and_reading_another", "[opt][version_loops]" )
{
    REQUIRE( reads_by_loop( k_moved ) == std::vector<std::size_t> { 1, 4 } );
}

// One header both read and stored through: the check fails at run time only if the buffer holds it.
TEST_CASE( "version_loops_copies_a_loop_reading_and_storing_one_reference", "[opt][version_loops]" )
{
    REQUIRE(
        reads_by_loop( "void f( ref Buf b ) { for( u64 i = 0; i < b.count; i += 1 ) "
                       "{ unsafe { b.data[ i ] = b.data[ i ] * 2; } } }\n" ) == std::vector<std::size_t> { 1, 3 }
    );
}

TEST_CASE( "version_loops_keeps_a_loop_over_its_budget", "[opt][version_loops]" )
{
    REQUIRE( reads_by_loop( k_moved, 0 ) == std::vector<std::size_t> { 4 } );
}

TEST_CASE( "version_loops_keeps_a_loop_below_O3", "[opt][version_loops]" )
{
    REQUIRE( reads_by_loop( k_moved, Loop_weights {}.versioning, 2 ) == std::vector<std::size_t> { 4 } );
}

// A call may write anything, so no range check covers it.
TEST_CASE( "version_loops_keeps_a_loop_that_calls", "[opt][version_loops]" )
{
    REQUIRE(
        reads_by_loop( "void f( ref Buf ys, const ref Buf xs ) { for( u64 i = 0; i < xs.count; i += 1 ) "
                       "{ unsafe { ys.data[ i ] = xs.data[ i ] + 1; } touch(); } }\n" ) == std::vector<std::size_t> { 4 }
    );
}

// The store's index is not the loop's, so its range is unknown.
TEST_CASE( "version_loops_keeps_a_loop_storing_at_another_index", "[opt][version_loops]" )
{
    REQUIRE(
        reads_by_loop( "void f( ref Buf ys, const ref Buf xs, u64 j ) { for( u64 i = 0; i < xs.count; i += 1 ) "
                       "{ unsafe { ys.data[ j ] = xs.data[ i ] + 1; } } }\n" ) == std::vector<std::size_t> { 4 }
    );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
