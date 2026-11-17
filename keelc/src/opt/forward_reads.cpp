// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/forward_reads.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>
#include "ir/initialised.h"
#include "ir/loops.h"
#include "ir/simplify.h"
#include "opt/held_table.h"

namespace keel
{

bool mentions( const Function& func, const Place& place, Local_id local )
{
    if( !place.is_global() && place.local == local )
    {
        return true;
    }

    return std::ranges::any_of(
        func.projections_of( place ),
        [&]( const Projection& projection ) { return projection.kind == Projection_kind::Index && projection.index == local; }
    );
}

bool reads( const Function& func, const Statement& statement, Local_id local )
{
    if( statement.kind == Statement_kind::Drop )
    {
        return statement.drop_flag == local || mentions( func, statement.place, local );
    }
    if( !statement.place.is_whole_local() && mentions( func, statement.place, local ) )
    {
        return true;
    }
    if( !statement.value.is_valid() )
    {
        return false;
    }

    bool read = false;
    for_each_operand(
        func.rvalue( statement ),
        [&]( const Operand& operand ) { read |= operand.has_place() && mentions( func, operand.place, local ); }
    );
    return read;
}

bool reads( const Function& func, const Terminator& terminator, Local_id local )
{
    bool read = false;
    for( const Operand* operand : { &terminator.condition, &terminator.message, &terminator.at } )
    {
        read |= operand->has_place() && mentions( func, operand->place, local );
    }
    return read;
}

Local_set addressed_locals_of( const Function& func )
{
    Local_set addressed( func.locals.size(), false );
    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        const Rvalue& value = func.rvalue( statement );
        if( value.kind == Rvalue_kind::Address_of && value.address_purpose != Address_purpose::Initialise &&
            !value.a.place.is_global() && !has_deref_projection( func, value.a.place ) )
        {
            addressed[value.a.place.local.v] = true;
        }
    }

    return addressed;
}

bool is_addressed( const Local_set& addressed, Local_id local )
{
    return local.v < addressed.size() && addressed[local.v];
}

bool clobbers( const Function& func, const Local_set& addressed, const Statement& statement )
{
    if( statement.kind == Statement_kind::Drop )
    {
        return true;
    }

    if( statement.kind != Statement_kind::Assign )
    {
        return false;
    }

    // An Allocate writes only the memory it returns.
    const Rvalue_kind kind = func.rvalue( statement ).kind;
    if( kind == Rvalue_kind::Call || kind == Rvalue_kind::Indirect_call || kind == Rvalue_kind::Release )
    {
        return true;
    }

    const Place& place = statement.place;
    return place.is_global() || has_deref_projection( func, place ) || is_addressed( addressed, place.local );
}

namespace
{
// A scalar's kind of memory: an integer's or `bool`'s width, a float's width beside it, or a thin
// pointer; 0 for anything else.
u32 memory_kind( const Type_table& table, Type_id type )
{
    if( !type.is_valid() )
    {
        return 0;
    }
    const Type& t = table.get( type );
    switch( t.kind )
    {
    case Type_kind::Bool:
        return 8;
    case Type_kind::Int:
        return t.width;
    case Type_kind::Float:
        return 1000 + t.width;
    case Type_kind::Pointer:
        return table.is_interface_pointer( type ) || table.is_run_pointer( type ) ? 0 : 2000; // a fat pointer is two values
    case Type_kind::Many_pointer:
    case Type_kind::Function:
        return 2000;
    default:
        return 0;
    }
}
} // namespace

bool distinct_memory( const Type_table& table, Type_id stored, Type_id read )
{
    const u32 a = memory_kind( table, stored );
    const u32 b = memory_kind( table, read );
    return a != 0 && b != 0 && a != b;
}

namespace
{
struct Place_traits
{
    const Function& func;

    u64 hash( const Place& p ) const
    {
        return place_hash( func, p );
    }

    bool same( const Place& a, const Place& b ) const
    {
        return same_place( func, a, b );
    }

    void mentioned( const Place& p, std::vector<Local_id>& out ) const
    {
        mentioned_by( func, p, out );
    }

    static bool in_memory( const Place& )
    {
        return true;
    }
};

// A place read through a pointer, and the local holding what it read. A place without a deref is
// propagate_values's, which forwards the other way.
using Reads = Held_table<Place, Place_traits>;

const Local_id* held( const Reads& reads, const Place& place )
{
    const Reads::Entry* e = reads.find( place );
    return e ? &e->local : nullptr;
}

// Whether `statement` stores through a pointer and does nothing else that may change memory.
bool stores_through( const Function& func, const Statement& statement )
{
    if( statement.kind != Statement_kind::Assign || !has_deref_projection( func, statement.place ) )
    {
        return false;
    }
    const Rvalue_kind kind = func.rvalue( statement ).kind;
    return kind != Rvalue_kind::Call && kind != Rvalue_kind::Indirect_call && kind != Rvalue_kind::Release;
}

void step( const Function& func, const Type_table& table, const Local_set& addressed, const Statement& statement, Reads& reads )
{
    if( stores_through( func, statement ) )
    {
        const Type_id stored = func.rvalue( statement ).type;
        reads.erase_if( [&]( const Reads::Entry& read )
                        { return !distinct_memory( table, stored, func.locals[read.local].type ); } );
    }
    else if( clobbers( func, addressed, statement ) )
    {
        reads.clear();
    }
    else if( !statement.place.is_global() && statement.place.local.is_valid() )
    {
        reads.forget( statement.place.local );
    }

    if( statement.kind != Statement_kind::Assign || !statement.place.is_whole_local() )
    {
        return;
    }

    const Rvalue&  value = func.rvalue( statement );
    const Operand& from  = value.a;
    const Local_id into  = statement.place.local;

    if( value.kind == Rvalue_kind::Use && from.kind == Operand_kind::Copy && has_deref_projection( func, from.place ) &&
        from.type == func.locals[into].type && !mentions( func, from.place, into ) && !held( reads, from.place ) )
    {
        reads.add( from.place, into );
    }
}

std::vector<std::optional<Reads>>
reads_on_entry( const Function& func, const Type_table& table, const Local_set& addressed, const Place_traits& traits )
{
    std::vector<std::optional<Reads>> entry( func.blocks.size(), std::nullopt );
    entry[0].emplace( traits );

    // Reads only go away, and a block is reached once, so this ends.
    bool                  changed = true;
    std::vector<Block_id> targets;
    std::vector<bool>     dirty( func.blocks.size(), true );
    Reads                 reads( traits );
    while( changed )
    {
        changed = false;

        for( const Block_id b : func.blocks.ids() )
        {
            if( !entry[b.v] || !dirty[b.v] )
            {
                continue;
            }
            dirty[b.v] = false;

            const Block& block = func.blocks[b];
            reads              = *entry[b.v];
            for( const Statement& statement : func.statements_of( block ) )
            {
                step( func, table, addressed, statement, reads );
            }

            targets.clear();
            successors( block.terminator, targets );
            for( const Block_id target : targets )
            {
                std::optional<Reads>& into = entry[target.v];
                if( !into )
                {
                    into            = reads;
                    changed         = true;
                    dirty[target.v] = true;
                    continue;
                }

                // A read survives a join only if this path holds it in the same local.
                const std::size_t before = into->size();
                into->erase_if(
                    [&]( const Reads::Entry& read )
                    {
                        const Local_id* local = held( reads, read.key );
                        return !local || *local != read.local;
                    }
                );
                if( into->size() != before )
                {
                    changed         = true;
                    dirty[target.v] = true;
                }
            }
        }
    }

    return entry;
}

bool rewrite( const Function& func, const Reads& reads, Operand& operand )
{
    if( operand.kind != Operand_kind::Copy || !has_deref_projection( func, operand.place ) )
    {
        return false;
    }

    const Local_id* local = held( reads, operand.place );
    if( !local || func.locals[*local].type != operand.type )
    {
        return false;
    }

    operand = copy( Place { .local = *local }, operand.type );
    return true;
}
} // namespace

namespace
{
// The held reads forwarded, once.
bool forward( Function& func, const Type_table& table )
{
    const Local_set                         addressed = addressed_locals_of( func );
    const Place_traits                      traits { func };
    const std::vector<std::optional<Reads>> entry   = reads_on_entry( func, table, addressed, traits );
    bool                                    changed = false;
    std::vector<bool>                       deleted( func.statements.size(), false );

    for( const Block_id b : func.blocks.ids() )
    {
        if( !entry[b.v] )
        {
            continue;
        }

        Block& block = func.blocks[b];
        Reads  reads = *entry[b.v];

        for( u32 s = 0; s < block.statement_count; ++s )
        {
            Statement& statement = func.statements_of( block )[s];

            // Reads first, so `_4 = copy (*_1).n` reads the old `_4`. An address names its place, not a read.
            if( statement.kind == Statement_kind::Assign && func.rvalue( statement ).kind != Rvalue_kind::Address_of )
            {
                Rvalue& value = func.rvalue( statement );
                changed |= rewrite( func, reads, value.a );
                changed |= rewrite( func, reads, value.b );
                for( Operand& argument : value.arguments )
                {
                    changed |= rewrite( func, reads, argument );
                }

                // A latch's `_4 = copy (*_1).n` the guard already read is now `_4 = copy _4`.
                if( value.kind == Rvalue_kind::Use && value.a.kind == Operand_kind::Copy && statement.place.is_whole_local() &&
                    value.a.place.is_whole_local() && value.a.place.local == statement.place.local )
                {
                    deleted[block.first_statement + s] = true;
                    continue;
                }
            }

            step( func, table, addressed, statement, reads );
        }

        changed |= rewrite( func, reads, block.terminator.condition );
        changed |= rewrite( func, reads, block.terminator.message );
        changed |= rewrite( func, reads, block.terminator.at );
    }

    if( changed )
    {
        remove_statements( func, deleted );
    }

    return changed;
}

// A read through a pointer inside a larger rvalue or a terminator: `slot` 0 and 1 are `a` and `b`,
// then the arguments; at the terminator (`index` the block's statement count), the condition, the
// message and the location.
struct Split
{
    Block_id block;
    u32      index;
    u32      slot;
};

Operand* operand_at( Function& func, Block_id block, u32 index, u32 slot )
{
    Block& b = func.blocks[block];
    if( index == b.statement_count )
    {
        Operand* const at[] = { &b.terminator.condition, &b.terminator.message, &b.terminator.at };
        return at[slot];
    }
    Rvalue& value = func.rvalue( func.statements_of( b )[index] );
    return slot == 0 ? &value.a : slot == 1 ? &value.b : &value.arguments[slot - 2];
}

bool through_pointer( const Function& func, const Operand& operand )
{
    return operand.kind == Operand_kind::Copy && has_deref_projection( func, operand.place );
}

// Each read in a loop that only a larger rvalue holds, of a place some other operand reads too.
std::vector<Split> splits_of( Function& func )
{
    // In a loop only: a read split elsewhere gains a turn's worth at most, and would make a small
    // callee look too big to inline.
    std::vector<bool> looped( func.blocks.size(), false );
    for( const Loop& loop : find_loops( func ).loops )
    {
        for( const Block_id b : loop.blocks )
        {
            looped[b.v] = true;
        }
    }

    std::vector<Split>   found;
    std::vector<Operand> all; // every read through a pointer, held or not
    for( const Block_id b : func.blocks.ids() )
    {
        const Block& block = func.blocks[b];
        for( u32 s = 0; s <= block.statement_count; ++s )
        {
            u32  slots      = 3;
            bool held_whole = false;
            if( s < block.statement_count )
            {
                const Statement& statement = func.statements_of( block )[s];
                if( statement.kind != Statement_kind::Assign || func.rvalue( statement ).kind == Rvalue_kind::Address_of )
                {
                    continue;
                }
                const Rvalue& value = func.rvalue( statement );
                slots               = 2 + static_cast<u32>( value.arguments.size() );
                // A whole read into a local is already one `forward` keeps.
                held_whole = value.kind == Rvalue_kind::Use && statement.place.is_whole_local();
            }
            for( u32 slot = 0; slot < slots; ++slot )
            {
                const Operand& operand = *operand_at( func, b, s, slot );
                if( !through_pointer( func, operand ) )
                {
                    continue;
                }
                all.push_back( operand );
                if( !held_whole && looped[b.v] )
                {
                    found.push_back( Split { .block = b, .index = s, .slot = slot } );
                }
            }
        }
    }

    std::erase_if(
        found,
        [&]( const Split& split )
        {
            const Place& place = operand_at( func, split.block, split.index, split.slot )->place;
            return std::ranges::count_if( all, [&]( const Operand& o ) { return same_place( func, o.place, place ); } ) < 2;
        }
    );
    return found;
}

// Each of `splits` read into a new local just before it; the locals.
std::vector<Local_id> apply( Function& func, const std::vector<Split>& splits )
{
    std::vector<Local_id> made;
    std::vector<u32>      order( splits.size() );
    for( u32 i = 0; i < order.size(); ++i )
    {
        order[i] = i;
    }
    // Last first, so an insertion moves no split still to come; one statement's reads in slot order.
    std::ranges::sort(
        order,
        [&]( u32 x, u32 y )
        {
            const Split& a = splits[x];
            const Split& b = splits[y];
            return a.block.v != b.block.v ? a.block.v > b.block.v : a.index != b.index ? a.index > b.index : a.slot < b.slot;
        }
    );

    std::vector<u32> counted( func.blocks.size() ); // each block's statements before any insertion
    for( const Block_id b : func.blocks.ids() )
    {
        counted[b.v] = func.blocks[b].statement_count;
    }

    for( u32 k = 0; k < order.size(); ++k )
    {
        const Split& split = splits[order[k]];
        // Reads of this statement already moved out sit before it, each pushing it one on.
        u32 before = 0;
        for( u32 j = 0; j < k; ++j )
        {
            const Split& other = splits[order[j]];
            before += other.block == split.block && other.index == split.index;
        }
        const bool terminator = split.index == counted[split.block.v];
        const u32  index      = terminator ? func.blocks[split.block].statement_count : split.index + before;

        Operand&      operand = *operand_at( func, split.block, index, split.slot );
        const Operand from    = operand;
        const Span span = terminator ? func.blocks[split.block].terminator.span : func.statements_of( split.block )[index].span;
        const Local_id local = func.locals.push_back( Local { .type = from.type, .span = span } );
        operand              = copy( Place { .local = local }, from.type );
        made.push_back( local );

        Statement read {
            .span  = span,
            .place = Place { .local = local },
            .value = func.add_rvalue( Rvalue { .kind = Rvalue_kind::Use, .type = from.type, .a = from } )
        };
        insert_statement( func, split.block, terminator ? index : split.index, std::move( read ) );
    }
    remove_statements( func, std::vector<bool>( func.statements.size(), false ) );
    return made;
}

// A split whose read `forward` took from another local, `_t = copy _h`, has its one reader read `_h`.
void unsplit( Function& func, const std::vector<Local_id>& made )
{
    std::vector<Local_id> to( func.locals.size() );
    std::vector<bool>     deleted( func.statements.size(), false );
    for( u32 i = 0; i < func.statements.size(); ++i )
    {
        const Statement& statement = func.statements[i];
        if( statement.kind != Statement_kind::Assign || !statement.place.is_whole_local() ||
            std::ranges::find( made, statement.place.local ) == made.end() )
        {
            continue;
        }
        const Rvalue& value = func.rvalue( statement );
        if( value.kind == Rvalue_kind::Use && value.a.kind == Operand_kind::Copy && value.a.place.is_whole_local() )
        {
            to[statement.place.local.v] = value.a.place.local;
            deleted[i]                  = true;
        }
    }

    const auto redirect = [&]( Operand& operand )
    {
        while( operand.kind == Operand_kind::Copy && operand.place.is_whole_local() && to[operand.place.local.v].is_valid() )
        {
            operand.place.local = to[operand.place.local.v];
        }
    };
    for( u32 i = 0; i < func.statements.size(); ++i )
    {
        if( !deleted[i] && func.statements[i].kind == Statement_kind::Assign )
        {
            Rvalue& value = func.rvalue( func.statements[i] );
            redirect( value.a );
            redirect( value.b );
            for( Operand& argument : value.arguments )
            {
                redirect( argument );
            }
        }
    }
    for( Block& block : func.blocks )
    {
        redirect( block.terminator.condition );
        redirect( block.terminator.message );
        redirect( block.terminator.at );
    }
    remove_statements( func, deleted );
}

} // namespace

bool forward_reads( Function& func, const Type_table& table )
{
    // A read inside a larger rvalue is held only once it has a local of its own.
    const std::vector<Split>    splits  = splits_of( func );
    const std::vector<Local_id> made    = apply( func, splits );
    const bool                  changed = forward( func, table );
    unsplit( func, made );
    return changed || !splits.empty();
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

constexpr std::string_view k_types = "struct Pair { i32 a; i32 b; i32 size() const { return a; } };\n"
                                     "struct Real { f64 f; u32 u; };\n"
                                     "i32 g = 0;\n"
                                     "void touch() { g += 1; }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Forwarded : Compiled
{
    explicit Forwarded( std::string_view source, u32 level = 3, std::optional<Loop_weights> loops = Loop_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .level = level, .loops = loops }
          )
    {
        INFO( rendered() );
        REQUIRE( clean() );
        REQUIRE( verify( functions.front() ).empty() );
    }

    // Operands that read through a pointer, anywhere in the function.
    std::size_t reads_through() const
    {
        const Function& func = functions.front();
        std::size_t     n    = 0;
        for( const Statement& statement : func.statements )
        {
            if( statement.kind == Statement_kind::Assign && func.rvalue( statement ).kind != Rvalue_kind::Address_of )
            {
                for_each_operand(
                    func.rvalue( statement ),
                    [&]( const Operand& o ) { n += o.kind == Operand_kind::Copy && has_deref_projection( func, o.place ); }
                );
            }
        }
        return n;
    }

    std::size_t branches() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            functions.front().blocks, []( const Block& block ) { return block.terminator.kind == Terminator_kind::Branch; }
        ) );
    }
};

constexpr std::string_view k_checked =
    "i32 f( const ref Pair p, i32 i ) { if( i < p.size() ) { if( i >= p.a ) { return -1; } return 1; } return 0; }\n";

} // namespace

// The second read of `p.a` takes the size's local, so the inner test is the outer's negation.
TEST_CASE( "forward_reads_folds_a_test_a_read_through_a_reference_repeats", "[opt][forward_reads]" )
{
    REQUIRE( Forwarded( k_checked ).branches() == 1 );
    REQUIRE( Forwarded( k_checked, 3, std::nullopt ).branches() == 2 );
}

// In a loop, both tests read `p.a` inside the comparison: the first read gets a local of its own, which
// the second takes, so the inner test is the outer's negation and `p.a` is read once.
TEST_CASE( "forward_reads_folds_a_test_whose_reads_sit_inside_comparisons", "[opt][forward_reads]" )
{
    constexpr std::string_view source = "i32 f( const ref Pair p, i32 n ) { i32 t = 0; for( i32 i = 0; i < n; i += 1 ) "
                                        "{ if( i < p.a ) { if( i >= p.a ) { t += 2; } t += 1; } } return t; }\n";
    REQUIRE( Forwarded( source ).reads_through() == 1 );
    REQUIRE( Forwarded( source, 3, std::nullopt ).reads_through() == 2 );
}

// `touch` writes a global, which `p` may reach.
TEST_CASE( "forward_reads_keeps_a_read_a_global_store_separates", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( const ref Pair p, i32 i ) { if( i < p.size() ) { touch(); if( i >= p.a ) { return -1; } return "
                       "1; } return 0; }\n" );
    REQUIRE( f.branches() == 2 );
}

// `q` may be `p`.
TEST_CASE( "forward_reads_keeps_a_read_a_store_through_a_pointer_separates", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( const ref Pair p, ref Pair q, i32 i ) { if( i < p.size() ) { q.a = 0; if( i >= p.a ) { "
                       "return -1; } return 1; } return 0; }\n" );
    REQUIRE( f.branches() == 2 );
}

// `q.f` is an `f64` and `p.a` an `i32`, which no store of the one may change (D89).
TEST_CASE( "forward_reads_folds_a_test_past_a_store_of_another_kind", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( const ref Pair p, ref Real q, i32 i ) { if( i < p.size() ) { q.f = 0.0; if( i >= p.a ) { "
                       "return -1; } return 1; } return 0; }\n" );
    REQUIRE( f.branches() == 1 );
}

// `u32` and `i32` are one kind, signed or not.
TEST_CASE( "forward_reads_keeps_a_read_a_store_of_the_other_sign_separates", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( const ref Pair p, ref Real q, i32 i ) { if( i < p.size() ) { q.u = 0; if( i >= p.a ) { "
                       "return -1; } return 1; } return 0; }\n" );
    REQUIRE( f.branches() == 2 );
}

// One path into the inner test stores through `q`, so the join keeps the read.
TEST_CASE( "forward_reads_keeps_a_read_one_path_stores_before", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( const ref Pair p, ref Pair q, i32 i, bool c ) { if( i < p.size() ) { if( c ) { q.b = 0; } "
                       "if( i >= p.a ) { return -1; } return 1; } return 0; }\n" );
    REQUIRE( f.branches() == 3 );
}

// `r` may point at `x`, so a store to `x` reaches `(*r).a`.
TEST_CASE( "forward_reads_keeps_a_read_a_store_to_an_addressed_local_separates", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( i32 i, i32 k, bool c ) { Pair x = Pair { k, 0 }; Pair y = Pair { k, 1 }; Pair* r = &x; "
                       "if( c ) { r = &y; } if( i < r.size() ) { x.a = 0; if( i >= r.a ) { return -1; } return 1; } "
                       "return 0; }\n" );
    REQUIRE( f.branches() == 3 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
