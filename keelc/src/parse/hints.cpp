// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parse/hints.h"
#include <array>
#include <ranges>

namespace keel
{

struct Word_hint
{
    std::string_view word;
    std::string_view hint;
};

struct Stop_hint
{
    Token_kind       wanted;
    Token_kind       found;
    std::string_view hint;
};

std::string_view dropped_word_hint( std::string_view word )
{
    constexpr std::array<Word_hint, 6> hints = {
        Word_hint { "protected", "a member is `public` or `private`" },
        Word_hint { "template", "type parameters are written after the name, as in `class Box<T>` or `T f<T>( T a )`" },
        Word_hint { "mutable", "remove it: a field is mutable unless it is `const`, though fields cannot be `const` yet" },
        Word_hint { "unsigned", "write a sized type, such as `u32`" },
        Word_hint { "long", "write a sized type, such as `i64`" },
        Word_hint { "short", "write a sized type, such as `i16`" },
    };

    const auto it = std::ranges::find_if( hints, [word]( const Word_hint& h ) { return h.word == word; } );
    return it != hints.end() ? it->hint : std::string_view {};
}

std::string_view member_stop_hint( Token_kind wanted, Token_kind found )
{
    constexpr std::array<Stop_hint, 4> hints = {
        Stop_hint {
            Token_kind::Semicolon, Token_kind::Equal, "a field cannot be given a value here yet: set it in the constructor"
        },
        Stop_hint { Token_kind::Semicolon, Token_kind::Comma, "declare each field with its own type" },
        Stop_hint { Token_kind::Semicolon, Token_kind::L_bracket, "a field cannot be a fixed-size array yet" },
        Stop_hint { Token_kind::L_brace, Token_kind::Semicolon, "Keel has no forward declarations: write the definition here" },
    };

    const auto it =
        std::ranges::find_if( hints, [wanted, found]( const Stop_hint& h ) { return h.wanted == wanted && h.found == found; } );
    return it != hints.end() ? it->hint : std::string_view {};
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{

TEST_CASE( "hints_name_the_keel_spelling", "[parse][hints]" )
{
    const auto has = []( std::string_view hint, std::string_view part ) { return hint.find( part ) != std::string_view::npos; };

    SECTION( "for a dropped word" )
    {
        REQUIRE( has( dropped_word_hint( "protected" ), "`public` or `private`" ) );
        REQUIRE( has( dropped_word_hint( "template" ), "`T f<T>( T a )`" ) );
        REQUIRE( has( dropped_word_hint( "mutable" ), "fields cannot be `const` yet" ) );
        REQUIRE( has( dropped_word_hint( "unsigned" ), "`u32`" ) );
        REQUIRE( has( dropped_word_hint( "long" ), "`i64`" ) );
        REQUIRE( has( dropped_word_hint( "short" ), "`i16`" ) );
    }

    SECTION( "for a member that stopped short" )
    {
        REQUIRE( has( member_stop_hint( Token_kind::Semicolon, Token_kind::Equal ), "set it in the constructor" ) );
        REQUIRE( has( member_stop_hint( Token_kind::Semicolon, Token_kind::Comma ), "its own type" ) );
        REQUIRE( has( member_stop_hint( Token_kind::Semicolon, Token_kind::L_bracket ), "fixed-size array" ) );
        REQUIRE( has( member_stop_hint( Token_kind::L_brace, Token_kind::Semicolon ), "no forward declarations" ) );
    }

    // No Keel spelling to point at yet, so nothing rather than a promise.
    SECTION( "nothing where Keel has no alternative" )
    {
        REQUIRE( dropped_word_hint( "virtual" ).empty() );
        REQUIRE( dropped_word_hint( "frobnicate" ).empty() );
        REQUIRE( member_stop_hint( Token_kind::Semicolon, Token_kind::Identifier ).empty() );
    }
}

} // namespace keel

#endif // ENABLE_UNIT_TESTS
