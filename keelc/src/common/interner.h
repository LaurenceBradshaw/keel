#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "common/types.h"

namespace keel
{

inline constexpr u32 k_invalid_symbol = 0xFFFF'FFFFu;

struct Symbol_id
{
    u32 v = k_invalid_symbol;

    bool is_valid() const
    {
        return v != k_invalid_symbol;
    }

    auto operator<=>( const Symbol_id& other ) const = default;
};

// Order defines the interned ids: Keyword::If == the Symbol_id of "if".
enum class Keyword : u32
{
    If,
    Else,
    While,
    For,
    Return,
    Struct,
    Enum,
    Class,
    Switch,
    Case,
    Default,
    Break,
    Continue,
    Const,
    Auto,
    Template,
    True,
    False,
    Import,
    Unsafe,
    Nullptr,

    // Not names but operators, and they appear in expression position where an unknown identifier
    // gives a far worse message than a keyword the parser can recognise (PLAN §6.3 D10).
    // The C++ *type* names are deliberately absent: sema suggests the replacement from its
    // unknown-type path, which knows it is in type position (PLAN §6.3 D1).
    New,
    Delete,

    // New Keel keywords
    Move,
    Out,
    Ref,
    Cast,
    Wrap,
    This,

    Extern,
    Alloc,
    Free,

    Count
};

class Interner
{
public:
    Interner();
    Interner( const Interner& )            = delete;
    Interner& operator=( const Interner& ) = delete;

    Symbol_id        intern( std::string_view text );
    std::string_view text( Symbol_id id ) const;

    bool is_keyword( Symbol_id id ) const;

    static Symbol_id keyword( Keyword k )
    {
        return Symbol_id { static_cast<u32>( k ) };
    }

private:
    struct Sv_hash
    {
        using is_transparent = void;
        std::size_t operator()( std::string_view sv ) const;
    };

    // The map owns every string; unordered_map is node-based, so texts_ can hold views into its
    // keys and they stay valid across rehashing.
    std::unordered_map<std::string, Symbol_id, Sv_hash, std::equal_to<>> map_;
    std::vector<std::string_view>                                        texts_;
};

} // namespace keel

template <>
struct std::hash<keel::Symbol_id>
{
    std::size_t operator()( const keel::Symbol_id& s ) const noexcept
    {
        return std::hash<keel::u32>()( s.v );
    }
};