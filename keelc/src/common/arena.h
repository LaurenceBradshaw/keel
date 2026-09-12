#pragma once
#include <cstddef>
#include <type_traits>
#include <vector>

namespace keel
{

class Arena
{
public:
    Arena();
    ~Arena();
    Arena( const Arena& )            = delete;
    Arena& operator=( const Arena& ) = delete;

    void* allocate( std::size_t size, std::size_t align );

    template <typename T>
    T* allocate_n( std::size_t count );

    std::size_t bytes_used() const;

private:
    void add_block( std::size_t size );

    struct Block
    {
        std::byte*  data;
        std::size_t size;
    };

    std::vector<Block> blocks_;
    std::byte*         cursor_ = nullptr;
    std::byte*         end_    = nullptr;
};

template <typename T>
inline T* Arena::allocate_n( std::size_t count )
{
    // Nothing in the arena is destroyed - the blocks are simply released - so a type with a
    // destructor would leak silently. Fail at compile time instead.
    static_assert( std::is_trivially_destructible_v<T>, "Arena can only allocate trivially destructible types" );

    void* p = allocate( count * sizeof( T ), alignof( T ) );

    return static_cast<T*>( p );
}

} // namespace keel