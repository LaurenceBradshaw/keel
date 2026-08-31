#include "common/arena.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include "common/types.h"

namespace keel
{
namespace
{

// Large enough that a typical parse takes a handful of blocks, small enough that compiling a tiny
// file does not reserve much.
constexpr std::size_t k_default_block_size = 64 * 1024;

std::byte* align_up( std::byte* p, std::size_t align )
{
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>( p );
    const std::uintptr_t mask  = static_cast<std::uintptr_t>( align ) - 1;

    return reinterpret_cast<std::byte*>( ( value + mask ) & ~mask );
}

} // namespace

Arena::Arena() {} // Empty

Arena::~Arena()
{
    for( Block& block : blocks_ )
    {
        delete[] block.data;
    }
}

void* Arena::allocate( std::size_t size, std::size_t align )
{
    assert( align > 0 && ( align & ( align - 1 ) ) == 0 && "alignment must be a power of two" );

    std::byte* aligned = align_up( cursor_, align );

    // Done in uintptr_t rather than on the pointers: relational comparison of pointers into
    // different blocks is unspecified, and "remaining >= size" cannot overflow the way
    // "aligned + size <= end_" can. A fresh arena has cursor_ == end_ == nullptr, which gives
    // remaining == 0 and so takes the add_block path without needing a special case.
    const std::uintptr_t at        = reinterpret_cast<std::uintptr_t>( aligned );
    const std::uintptr_t limit     = reinterpret_cast<std::uintptr_t>( end_ );
    const std::size_t    remaining = at > limit ? 0 : static_cast<std::size_t>( limit - at );

    if( remaining < size )
    {
        // + align of slack: new[] only guarantees 16-byte alignment, so rounding up inside the
        // fresh block can consume up to align-1 bytes before the request even starts.
        add_block( std::max( size + align, k_default_block_size ) );
        aligned = align_up( cursor_, align );
    }

    // Whatever was left in the previous block is abandoned. That is normal for an arena and not
    // worth reclaiming.
    cursor_ = aligned + size;
    return aligned;
}

std::size_t Arena::bytes_used() const
{
    std::size_t used = 0;
    for( const Block& block : blocks_ )
    {
        used += block.size;
    }

    const std::size_t unused = narrow_cast<std::size_t>( end_ - cursor_ );
    return used - unused;
}

void Arena::add_block( std::size_t size )
{
    blocks_.push_back( Block { new std::byte[size], size } );

    cursor_ = blocks_.back().data;
    end_    = cursor_ + size;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include "common/types.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

namespace keel
{
namespace
{

bool is_aligned( const void* p, std::size_t align )
{
    return reinterpret_cast<std::uintptr_t>( p ) % align == 0;
}

// Enough to cross whatever block size the implementation picks.
constexpr std::size_t k_big = 1u << 20;

struct Aligned16
{
    alignas( 16 ) std::byte bytes[16];
};

} // namespace

TEST_CASE( "arena_starts_empty", "[common][arena]" )
{
    const Arena arena;
    REQUIRE( arena.bytes_used() == 0 );
}

TEST_CASE( "arena_returns_usable_memory", "[common][arena]" )
{
    Arena arena;

    void* p = arena.allocate( 32, 8 );

    REQUIRE( p != nullptr );
    std::memset( p, 0xAB, 32 ); // must not fault; ASan catches an undersized block
}

TEST_CASE( "arena_honours_alignment", "[common][arena]" )
{
    Arena arena;

    for( const std::size_t align : { 1u, 2u, 4u, 8u, 16u, 32u, 64u } )
    {
        // A 1-byte allocation between each, so the cursor is left misaligned on purpose.
        arena.allocate( 1, 1 );

        void* p = arena.allocate( align * 2, align );

        INFO( "alignment " << align );
        REQUIRE( p != nullptr );
        REQUIRE( is_aligned( p, align ) );
    }
}

TEST_CASE( "arena_allocations_do_not_overlap", "[common][arena]" )
{
    constexpr std::size_t k_count = 500;
    constexpr std::size_t k_size  = 48;

    Arena                   arena;
    std::vector<std::byte*> blocks;

    for( std::size_t i = 0; i < k_count; ++i )
    {
        auto* p = static_cast<std::byte*>( arena.allocate( k_size, 8 ) );
        REQUIRE( p != nullptr );
        std::memset( p, static_cast<int>( i & 0xFF ), k_size );
        blocks.push_back( p );
    }

    // One assertion per allocation rather than one per byte: a mismatch anywhere in a block shows
    // up as a wrong count, and 500 REQUIREs beat 24000 for both runtime and failure output.
    for( std::size_t i = 0; i < k_count; ++i )
    {
        const auto        expected = static_cast<std::byte>( i & 0xFF );
        const std::size_t matching = static_cast<std::size_t>( std::count( blocks[i], blocks[i] + k_size, expected ) );

        INFO( "allocation " << i );
        REQUIRE( matching == k_size );
    }

    const std::set<std::byte*> distinct( blocks.begin(), blocks.end() );
    REQUIRE( distinct.size() == k_count );
}

// The arena grows by adding blocks; pointers into earlier blocks must stay valid, which is the
// whole reason nodes can hold arena pointers at all.
TEST_CASE( "arena_pointers_survive_growth", "[common][arena]" )
{
    Arena arena;

    auto* first = static_cast<std::byte*>( arena.allocate( 64, 8 ) );
    REQUIRE( first != nullptr );
    std::memset( first, 0x5A, 64 );

    std::size_t nulls = 0;
    for( std::size_t i = 0; i < 4096; ++i )
    {
        void* p = arena.allocate( 64, 8 );
        if( p == nullptr )
        {
            ++nulls;
        }
        else
        {
            std::memset( p, 0x11, 64 );
        }
    }

    REQUIRE( nulls == 0 );

    const std::size_t intact = static_cast<std::size_t>( std::count( first, first + 64, static_cast<std::byte>( 0x5A ) ) );
    REQUIRE( intact == 64 );
}

// Larger than any sensible block size: must get its own block rather than failing or looping.
TEST_CASE( "arena_handles_oversized_requests", "[common][arena]" )
{
    Arena arena;

    auto* small = static_cast<std::byte*>( arena.allocate( 16, 8 ) );
    std::memset( small, 0x77, 16 );

    auto* huge = static_cast<std::byte*>( arena.allocate( k_big, 8 ) );
    REQUIRE( huge != nullptr );
    REQUIRE( is_aligned( huge, 8 ) );
    std::memset( huge, 0x22, k_big );

    // The oversized allocation must not have disturbed what came before, and the arena must still
    // work afterwards.
    const std::size_t intact = static_cast<std::size_t>( std::count( small, small + 16, static_cast<std::byte>( 0x77 ) ) );
    REQUIRE( intact == 16 );

    void* after = arena.allocate( 16, 8 );
    REQUIRE( after != nullptr );
    REQUIRE( after != huge );
}

TEST_CASE( "arena_bytes_used_tracks_allocations", "[common][arena]" )
{
    Arena arena;

    REQUIRE( arena.bytes_used() == 0 );

    arena.allocate( 100, 8 );
    const std::size_t after_first = arena.bytes_used();
    REQUIRE( after_first >= 100 );

    arena.allocate( 200, 8 );
    const std::size_t after_second = arena.bytes_used();
    REQUIRE( after_second >= after_first + 200 );

    // Monotonic: there is no free, so it can never go down.
    arena.allocate( 1, 1 );
    REQUIRE( arena.bytes_used() >= after_second );
}

TEST_CASE( "arena_allocate_n_is_typed_and_aligned", "[common][arena]" )
{
    Arena arena;

    arena.allocate( 1, 1 ); // misalign the cursor first

    u32* ids = arena.allocate_n<u32>( 10 );
    REQUIRE( ids != nullptr );
    REQUIRE( is_aligned( ids, alignof( u32 ) ) );

    for( u32 i = 0; i < 10; ++i )
    {
        ids[i] = i * 7;
    }
    for( u32 i = 0; i < 10; ++i )
    {
        REQUIRE( ids[i] == i * 7 );
    }

    arena.allocate( 1, 1 );

    Aligned16* wide = arena.allocate_n<Aligned16>( 4 );
    REQUIRE( wide != nullptr );
    REQUIRE( is_aligned( wide, 16 ) );

    // The u32 array must be untouched by the later allocations.
    for( u32 i = 0; i < 10; ++i )
    {
        REQUIRE( ids[i] == i * 7 );
    }
}

TEST_CASE( "arena_allocate_n_of_zero_is_harmless", "[common][arena]" )
{
    Arena arena;

    u32* none = arena.allocate_n<u32>( 0 );
    (void) none; // may be null or a valid cursor; must not crash

    u32* one = arena.allocate_n<u32>( 1 );
    REQUIRE( one != nullptr );
    *one = 42;
    REQUIRE( *one == 42 );
}

// Empty child lists are common in the AST (a function with no parameters), so this path runs often.
TEST_CASE( "arena_zero_size_allocation", "[common][arena]" )
{
    Arena arena;

    void* p = arena.allocate( 0, 1 );
    (void) p;

    // Whatever it returns, the arena must still be usable and non-overlapping afterwards.
    auto* a = static_cast<std::byte*>( arena.allocate( 8, 8 ) );
    auto* b = static_cast<std::byte*>( arena.allocate( 8, 8 ) );

    REQUIRE( a != nullptr );
    REQUIRE( b != nullptr );
    REQUIRE( a != b );
    std::memset( a, 1, 8 );
    std::memset( b, 2, 8 );
    REQUIRE( a[0] == static_cast<std::byte>( 1 ) );
    REQUIRE( b[0] == static_cast<std::byte>( 2 ) );
}

TEST_CASE( "arena_independent_instances", "[common][arena]" )
{
    Arena first;
    Arena second;

    auto* a = static_cast<std::byte*>( first.allocate( 128, 8 ) );
    auto* b = static_cast<std::byte*>( second.allocate( 128, 8 ) );

    std::memset( a, 0xAA, 128 );
    std::memset( b, 0xBB, 128 );

    REQUIRE( a[0] == static_cast<std::byte>( 0xAA ) );
    REQUIRE( b[0] == static_cast<std::byte>( 0xBB ) );
    REQUIRE( first.bytes_used() >= 128 );
    REQUIRE( second.bytes_used() >= 128 );
}

// It owns raw blocks and is passed by reference, like Source_manager and Interner.
TEST_CASE( "arena_is_not_copyable", "[common][arena]" )
{
    STATIC_REQUIRE_FALSE( std::is_copy_constructible_v<Arena> );
    STATIC_REQUIRE_FALSE( std::is_copy_assignable_v<Arena> );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
