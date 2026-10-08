// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/interner.h"
#include <cassert>
#include <iterator>

namespace keel
{
Interner::Interner()
{
    // Reserve keywords first, so their ids match the Keyword enum.
    // clang-format off
    static constexpr std::string_view k_keyword_spellings[] = {
        "if",       "else",     "while",   "for",         "return",   "struct",  "enum",
        "class",    "switch",   "case",    "default",     "break",    "continue","const",
        "auto",     "template", "true",    "false",       "import",   "unsafe",  "nullptr",

        // See the Keyword enum: the C++ type names are handled by sema, not reserved here.
        "new",      "delete",

        // New Keel keywords
        "move",     "out",      "ref",     "cast",        "wrap",    "this",     "try",

        // Back to old existing keywords. TODO: split properly between regular programming keywords, ones new to keel, 
        // and ones from c++ that do not exist in keel.
        "extern",   "alloc",    "free",    "fallthrough", "where",   "static",
        "private",  "public",   "fn",      "field",       "assert",  "operator", "destroy",
        "_",        "panic"
    };
    // clang-format on

    static_assert(
        std::size( k_keyword_spellings ) == static_cast<std::size_t>( Keyword::Count ),
        "keyword spelling table out of sync with the Keyword enum"
    );

    for( std::size_t i = 0; i < std::size( k_keyword_spellings ); ++i )
    {
        // maybe_unused: the assert is the only reader, and NDEBUG removes it. The intern() call
        // must stay outside the assert - it is the whole point of the loop.
        [[maybe_unused]] const Symbol_id id = intern( k_keyword_spellings[i] );
        assert( id.v == narrow_cast<u32>( i ) && "keyword id out of order" );
    }

    // Catches a duplicate spelling in the table, which the static_assert cannot see.
    assert( texts_.size() == static_cast<std::size_t>( Keyword::Count ) && "duplicate keyword spelling" );

    [[maybe_unused]] const Symbol_id op_equal_equal = intern( "operator==" );
    assert( op_equal_equal.v == operator_name( Operator_name::Equal_equal ).v && "operator name id out of order" );

    [[maybe_unused]] const Symbol_id op_index = intern( "operator[]" );
    assert( op_index.v == operator_name( Operator_name::Index ).v && "operator name id out of order" );
}

Symbol_id Interner::intern( std::string_view text )
{
    if( const auto it = map_.find( text ); it != map_.end() )
    {
        return it->second;
    }

    const Symbol_id id { narrow_cast<u32>( texts_.size() ) };

    const auto [it, inserted] = map_.emplace( std::string( text ), id );
    assert( inserted && "map_ should not already contain this text" );

    // The view must point at the map's stored key, not at the caller's buffer.
    texts_.push_back( it->first );

    return id;
}

Symbol_id Interner::find( std::string_view text ) const
{
    if( const auto it = map_.find( text ); it != map_.end() )
    {
        return it->second;
    }

    return Symbol_id {};
}

std::string_view Interner::text( Symbol_id id ) const
{
    assert( id.is_valid() && "invalid Symbol_id" );
    assert( id.v < texts_.size() && "Symbol_id out of range" );

    return texts_[id.v];
}

bool Interner::is_keyword( Symbol_id id ) const
{
    // The invalid sentinel is far above Count, so it needs no separate check.
    return id.v < static_cast<u32>( Keyword::Count );
}

bool Interner::is_operator_name( Symbol_id id )
{
    return id.v >= static_cast<u32>( Keyword::Count ) &&
           id.v < static_cast<u32>( Keyword::Count ) + static_cast<u32>( Operator_name::Count );
}

std::size_t Interner::Sv_hash::operator()( std::string_view sv ) const
{
    // Hashes the view directly. Going via std::string( sv ) would allocate on every lookup of any
    // identifier past the SSO limit, which is the whole thing is_transparent exists to avoid.
    return std::hash<std::string_view>()( sv );
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace keel
{

TEST_CASE( "interner_same_text_gives_same_id", "[common][interner]" )
{
    Interner in;

    const Symbol_id a = in.intern( "widget" );
    const Symbol_id b = in.intern( "widget" );

    REQUIRE( a.is_valid() );
    REQUIRE( a == b );
}

TEST_CASE( "interner_different_text_gives_different_ids", "[common][interner]" )
{
    Interner in;

    REQUIRE( in.intern( "alpha" ) != in.intern( "beta" ) );
    REQUIRE( in.intern( "x" ) != in.intern( "X" ) ); // case sensitive
}

TEST_CASE( "interner_text_round_trips", "[common][interner]" )
{
    Interner in;

    REQUIRE( in.text( in.intern( "widget" ) ) == "widget" );
    REQUIRE( in.text( in.intern( "" ) ).empty() );

    // Long enough to defeat SSO, so both storage paths are covered.
    const std::string long_name( 200, 'q' );
    REQUIRE( in.text( in.intern( long_name ) ) == long_name );
}

// The reason map_ owns the strings and texts_ holds views into its keys: unordered_map is
// node-based, so rehashing must not move them. Short names on purpose - those are the SSO case that
// actually dangles if the storage is a vector<std::string>.
// A lookup that interns nothing, for a pass that holds the interner const.
TEST_CASE( "interner_finds_only_what_was_interned", "[common][interner]" )
{
    Interner in;

    const Symbol_id widget = in.intern( "widget" );

    REQUIRE( std::as_const( in ).find( "widget" ) == widget );
    REQUIRE_FALSE( std::as_const( in ).find( "gadget" ).is_valid() );
    REQUIRE( std::as_const( in ).find( "if" ) == Interner::keyword( Keyword::If ) );
}

TEST_CASE( "interner_views_survive_growth", "[common][interner]" )
{
    Interner in;

    const Symbol_id        first = in.intern( "x" );
    const std::string_view held  = in.text( first );

    for( int i = 0; i < 2000; ++i )
    {
        in.intern( "id" + std::to_string( i ) );
    }

    REQUIRE( held == "x" );
    REQUIRE( in.text( first ) == "x" );
    REQUIRE( in.intern( "x" ) == first );
}

TEST_CASE( "interner_keyword_ids_match_the_enum", "[common][interner]" )
{
    Interner in;

    REQUIRE( in.intern( "if" ) == Interner::keyword( Keyword::If ) );
    REQUIRE( in.intern( "else" ) == Interner::keyword( Keyword::Else ) );
    REQUIRE( in.intern( "while" ) == Interner::keyword( Keyword::While ) );
    REQUIRE( in.intern( "struct" ) == Interner::keyword( Keyword::Struct ) );
    REQUIRE( in.intern( "return" ) == Interner::keyword( Keyword::Return ) );
    REQUIRE( in.intern( "const" ) == Interner::keyword( Keyword::Const ) );
    REQUIRE( in.intern( "auto" ) == Interner::keyword( Keyword::Auto ) );
    REQUIRE( in.intern( "template" ) == Interner::keyword( Keyword::Template ) );
    REQUIRE( in.intern( "true" ) == Interner::keyword( Keyword::True ) );
    REQUIRE( in.intern( "extern" ) == Interner::keyword( Keyword::Extern ) );
    REQUIRE( in.intern( "fallthrough" ) == Interner::keyword( Keyword::Fallthrough ) );
    REQUIRE( in.intern( "where" ) == Interner::keyword( Keyword::Where ) );
}

// Every enum value must have a spelling, and interning that spelling must give the id back. This is
// what catches the spelling table drifting out of sync with the enum.
TEST_CASE( "interner_all_keywords_are_preinterned", "[common][interner]" )
{
    Interner in;

    for( u32 i = 0; i < static_cast<u32>( Keyword::Count ); ++i )
    {
        const Symbol_id        id       = Symbol_id { i };
        const std::string_view spelling = in.text( id );

        INFO( "keyword index " << i << " spelling '" << spelling << "'" );
        REQUIRE_FALSE( spelling.empty() );
        REQUIRE( in.is_keyword( id ) );
        REQUIRE( in.intern( spelling ) == id );
    }
}

// Keywords occupy exactly [0, Count), so is_keyword can stay a single integer comparison.
TEST_CASE( "interner_user_identifiers_follow_the_keywords", "[common][interner]" )
{
    Interner in;

    const Symbol_id first_user = in.intern( "widget" );
    REQUIRE( first_user.v == static_cast<u32>( Keyword::Count ) + static_cast<u32>( Operator_name::Count ) );

    REQUIRE_FALSE( in.is_keyword( first_user ) );
    REQUIRE_FALSE( in.is_keyword( in.intern( "iff" ) ) );
    REQUIRE_FALSE( in.is_keyword( in.intern( "If" ) ) );
    REQUIRE_FALSE( in.is_keyword( in.intern( "while_" ) ) );
    REQUIRE_FALSE( in.is_keyword( Symbol_id {} ) ); // the invalid sentinel
}

// An operator method's name is reserved like a keyword's, so the parser needs no interner to name one.
TEST_CASE( "interner_reserves_operator_names_after_the_keywords", "[common][interner]" )
{
    Interner in;

    const Symbol_id equal_equal = Interner::operator_name( Operator_name::Equal_equal );

    REQUIRE( equal_equal.v == static_cast<u32>( Keyword::Count ) );
    REQUIRE( in.text( equal_equal ) == "operator==" );
    REQUIRE( in.intern( "operator==" ) == equal_equal );
    REQUIRE_FALSE( in.is_keyword( equal_equal ) );
    REQUIRE( in.is_keyword( in.intern( "operator" ) ) );

    const Symbol_id index = Interner::operator_name( Operator_name::Index );

    REQUIRE( in.text( index ) == "operator[]" );
    REQUIRE( in.intern( "operator[]" ) == index );
    REQUIRE( Interner::is_operator_name( equal_equal ) );
    REQUIRE( Interner::is_operator_name( index ) );
    REQUIRE_FALSE( Interner::is_operator_name( in.intern( "operator" ) ) );
    REQUIRE_FALSE( Interner::is_operator_name( in.intern( "index" ) ) );
}

// PLAN §6.3 D10: `new` and `delete` are operators appearing in expression position, so the parser
// needs to recognise them.
TEST_CASE( "interner_reserves_new_and_delete", "[common][interner]" )
{
    Interner in;

    REQUIRE( in.is_keyword( in.intern( "new" ) ) );
    REQUIRE( in.is_keyword( in.intern( "delete" ) ) );
}

// D1's "use `i32` instead" suggestion lives on sema's unknown-type path, which knows it is in type
// position. Reserving these here would only produce a worse message from a place with less context.
TEST_CASE( "interner_does_not_reserve_cpp_type_names", "[common][interner]" )
{
    Interner in;

    for( const std::string_view word : { "int", "long", "short", "char", "signed", "unsigned", "float", "double" } )
    {
        INFO( "word '" << word << "'" );
        REQUIRE_FALSE( in.is_keyword( in.intern( word ) ) );
    }
}

TEST_CASE( "interner_ids_are_dense_and_sequential", "[common][interner]" )
{
    Interner in;

    const u32 base = in.intern( "aaa" ).v;
    REQUIRE( in.intern( "bbb" ).v == base + 1 );
    REQUIRE( in.intern( "ccc" ).v == base + 2 );
    REQUIRE( in.intern( "bbb" ).v == base + 1 ); // no new id for a repeat
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
