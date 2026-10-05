// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parse/hints.h"
#include <array>
#include <optional>
#include <ranges>

namespace keel
{

struct Word_hint
{
    std::string_view          word;
    std::string_view          hint;
    std::optional<Hint_place> only; // empty means it holds everywhere
};

struct Stop_hint
{
    Token_kind       wanted;
    Token_kind       found;
    std::string_view hint;
    Hint_place       place;
};

std::string_view dropped_word_hint( std::string_view word, Hint_place place )
{
    // clang-format off
    constexpr std::array<Word_hint, 7> hints = {
        Word_hint { "protected", "a member is `public` or `private`", Hint_place::Member },
        Word_hint { "template", "type parameters are written after the name, as in `class Box<T>` or `T f<T>( T a )`", std::nullopt },
        Word_hint { "mutable", "remove it: a field is mutable unless it is `const`", Hint_place::Member },
        Word_hint { "unsigned", "write a sized type, such as `u32`", std::nullopt },
        Word_hint { "long", "write a sized type, such as `i64`", std::nullopt },
        Word_hint { "short", "write a sized type, such as `i16`", std::nullopt },
        Word_hint { "extern", "write `extern` at file scope, where it declares a function defined in C", Hint_place::Member },
    };
    // clang-format on

    const auto it = std::ranges::find_if(
        hints, [word, place]( const Word_hint& h ) { return h.word == word && ( !h.only || *h.only == place ); }
    );
    return it != hints.end() ? it->hint : std::string_view {};
}

std::string_view stop_hint( Token_kind wanted, Token_kind found, Hint_place place )
{
    // clang-format off
    constexpr std::array<Stop_hint, 8> hints = {
        Stop_hint { Token_kind::Semicolon, Token_kind::Equal, "a field cannot be given a value here yet: set it in the constructor", Hint_place::Member },
        Stop_hint { Token_kind::Semicolon, Token_kind::Comma, "declare each field with its own type", Hint_place::Member },
        Stop_hint { Token_kind::Semicolon, Token_kind::L_bracket, "a field cannot be a fixed-size array yet", Hint_place::Member },
        Stop_hint { Token_kind::L_brace, Token_kind::Semicolon, "Keel has no forward declarations: write the definition here", Hint_place::Member },
        Stop_hint { Token_kind::Semicolon, Token_kind::Comma, "declare each variable with its own type", Hint_place::Declaration },
        Stop_hint { Token_kind::Semicolon, Token_kind::L_bracket, "a variable cannot be a fixed-size array yet", Hint_place::Declaration },
        Stop_hint { Token_kind::L_brace, Token_kind::Semicolon, "Keel has no forward declarations: write the definition here, or `extern` if it is defined in C", Hint_place::Declaration },
        Stop_hint { Token_kind::L_brace, Token_kind::Colon, "Keel has no initialiser lists: put the initialisation in the constructor's body", Hint_place::Member },
    };
    // clang-format on

    const auto it = std::ranges::find_if(
        hints,
        [wanted, found, place]( const Stop_hint& h ) { return h.wanted == wanted && h.found == found && h.place == place; }
    );
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

    constexpr Hint_place member      = Hint_place::Member;
    constexpr Hint_place declaration = Hint_place::Declaration;

    SECTION( "for a dropped word" )
    {
        REQUIRE( has( dropped_word_hint( "protected", member ), "`public` or `private`" ) );
        REQUIRE( has( dropped_word_hint( "template", member ), "`T f<T>( T a )`" ) );
        REQUIRE( has( dropped_word_hint( "mutable", member ), "a field is mutable unless it is `const`" ) );
        REQUIRE_FALSE( has( dropped_word_hint( "mutable", member ), "yet" ) );
        REQUIRE( has( dropped_word_hint( "unsigned", member ), "`u32`" ) );
        REQUIRE( has( dropped_word_hint( "long", member ), "`i64`" ) );
        REQUIRE( has( dropped_word_hint( "short", member ), "`i16`" ) );
        REQUIRE( has( dropped_word_hint( "extern", member ), "at file scope" ) );
    }

    // D36: a function, never a variable.
    SECTION( "`extern` declares a function defined in C" )
    {
        REQUIRE( has( dropped_word_hint( "extern", member ), "a function defined in C" ) );
        REQUIRE_FALSE( has( dropped_word_hint( "extern", member ), "variable" ) );
    }

    SECTION( "a word that is not about members holds at file scope too" )
    {
        REQUIRE( has( dropped_word_hint( "template", declaration ), "`T f<T>( T a )`" ) );
        REQUIRE( has( dropped_word_hint( "unsigned", declaration ), "`u32`" ) );
        REQUIRE( has( dropped_word_hint( "long", declaration ), "`i64`" ) );
        REQUIRE( has( dropped_word_hint( "short", declaration ), "`i16`" ) );
    }

    SECTION( "a member's word says nothing at file scope" )
    {
        REQUIRE( dropped_word_hint( "protected", declaration ).empty() );
        REQUIRE( dropped_word_hint( "mutable", declaration ).empty() );
        REQUIRE( dropped_word_hint( "extern", declaration ).empty() );
    }

    SECTION( "for a member that stopped short" )
    {
        REQUIRE( has( stop_hint( Token_kind::Semicolon, Token_kind::Equal, member ), "set it in the constructor" ) );
        REQUIRE( has( stop_hint( Token_kind::Semicolon, Token_kind::Comma, member ), "each field with its own type" ) );
        REQUIRE(
            has( stop_hint( Token_kind::Semicolon, Token_kind::L_bracket, member ), "a field cannot be a fixed-size array" )
        );
        REQUIRE( has( stop_hint( Token_kind::L_brace, Token_kind::Semicolon, member ), "no forward declarations" ) );
        REQUIRE_FALSE( has( stop_hint( Token_kind::L_brace, Token_kind::Semicolon, member ), "extern" ) );
    }

    SECTION( "for a declaration that stopped short" )
    {
        REQUIRE( has( stop_hint( Token_kind::Semicolon, Token_kind::Comma, declaration ), "each variable with its own type" ) );
        REQUIRE( has(
            stop_hint( Token_kind::Semicolon, Token_kind::L_bracket, declaration ), "a variable cannot be a fixed-size array"
        ) );
        REQUIRE( has( stop_hint( Token_kind::L_brace, Token_kind::Semicolon, declaration ), "no forward declarations" ) );
        REQUIRE( has( stop_hint( Token_kind::L_brace, Token_kind::Semicolon, declaration ), "`extern` if it is defined in C" )
        );
    }

    SECTION( "for a constructor with an initialiser list" )
    {
        REQUIRE( has( stop_hint( Token_kind::L_brace, Token_kind::Colon, member ), "in the constructor's body" ) );
        REQUIRE( stop_hint( Token_kind::L_brace, Token_kind::Colon, declaration ).empty() );
    }

    // A file-scope variable may have a value, so there is nothing to say.
    SECTION( "a field's rule is not a variable's" )
    {
        REQUIRE( stop_hint( Token_kind::Semicolon, Token_kind::Equal, declaration ).empty() );
    }

    // No Keel spelling to point at yet, so nothing rather than a promise.
    SECTION( "nothing where Keel has no alternative" )
    {
        REQUIRE( dropped_word_hint( "virtual", member ).empty() );
        REQUIRE( dropped_word_hint( "frobnicate", member ).empty() );
        REQUIRE( dropped_word_hint( "static", declaration ).empty() );
        REQUIRE( dropped_word_hint( "inline", declaration ).empty() );
        REQUIRE( stop_hint( Token_kind::Semicolon, Token_kind::Identifier, member ).empty() );
    }
}

} // namespace keel

#endif // ENABLE_UNIT_TESTS
