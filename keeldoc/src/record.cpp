// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "record.h"
#include <cctype>

namespace keeldoc
{
namespace
{

class Parser
{
public:
    explicit Parser( std::string_view text )
        : text_( text )
    {
    }

    std::optional<Record> object();

private:
    std::optional<std::string> string();
    std::optional<std::string> number();
    void                       skip_space();
    bool                       eat( char c );

    // A \uXXXX escape below 0x80 is all keelc writes; anything wider is encoded as UTF-8.
    static void append_utf8( std::string& out, unsigned code );

    std::string_view text_;
    std::size_t      at_ = 0;
};

std::optional<Record> Parser::object()
{
    Record record;

    if( !eat( '{' ) )
    {
        return std::nullopt;
    }

    if( !eat( '}' ) )
    {
        do
        {
            std::optional<std::string> key = string();
            if( !key || !eat( ':' ) )
            {
                return std::nullopt;
            }

            skip_space();
            std::optional<std::string> value = at_ < text_.size() && text_[at_] == '"' ? string() : number();
            if( !value )
            {
                return std::nullopt;
            }

            record.fields.insert_or_assign( std::move( *key ), std::move( *value ) );
        } while( eat( ',' ) );

        if( !eat( '}' ) )
        {
            return std::nullopt;
        }
    }

    skip_space();
    if( at_ != text_.size() )
    {
        return std::nullopt;
    }

    return record;
}

std::optional<std::string> Parser::string()
{
    if( !eat( '"' ) )
    {
        return std::nullopt;
    }

    std::string out;
    while( at_ < text_.size() && text_[at_] != '"' )
    {
        const char c = text_[at_++];
        if( c != '\\' )
        {
            out.push_back( c );
            continue;
        }

        if( at_ == text_.size() )
        {
            return std::nullopt;
        }

        switch( const char escape = text_[at_++]; escape )
        {
        case 'n':
            out.push_back( '\n' );
            break;
        case 't':
            out.push_back( '\t' );
            break;
        case 'r':
            out.push_back( '\r' );
            break;
        case 'b':
            out.push_back( '\b' );
            break;
        case 'f':
            out.push_back( '\f' );
            break;
        case 'u':
        {
            if( at_ + 4 > text_.size() )
            {
                return std::nullopt;
            }

            unsigned code = 0;
            for( const char h : text_.substr( at_, 4 ) )
            {
                code *= 16;
                if( h >= '0' && h <= '9' )
                {
                    code += static_cast<unsigned>( h - '0' );
                }
                else if( h >= 'a' && h <= 'f' )
                {
                    code += static_cast<unsigned>( h - 'a' + 10 );
                }
                else if( h >= 'A' && h <= 'F' )
                {
                    code += static_cast<unsigned>( h - 'A' + 10 );
                }
                else
                {
                    return std::nullopt;
                }
            }

            at_ += 4;
            append_utf8( out, code );
            break;
        }
        default:
            out.push_back( escape ); // \" \\ \/
        }
    }

    if( !eat( '"' ) )
    {
        return std::nullopt;
    }

    return out;
}

std::optional<std::string> Parser::number()
{
    const std::size_t start = at_;
    while( at_ < text_.size() && ( std::isdigit( static_cast<unsigned char>( text_[at_] ) ) || text_[at_] == '-' ) )
    {
        ++at_;
    }

    if( at_ == start )
    {
        return std::nullopt;
    }

    return std::string( text_.substr( start, at_ - start ) );
}

void Parser::skip_space()
{
    while( at_ < text_.size() && ( text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' ) )
    {
        ++at_;
    }
}

bool Parser::eat( char c )
{
    skip_space();
    if( at_ < text_.size() && text_[at_] == c )
    {
        ++at_;
        return true;
    }

    return false;
}

void Parser::append_utf8( std::string& out, unsigned code )
{
    if( code < 0x80 )
    {
        out.push_back( static_cast<char>( code ) );
    }
    else if( code < 0x800 )
    {
        out.push_back( static_cast<char>( 0xC0 | ( code >> 6 ) ) );
        out.push_back( static_cast<char>( 0x80 | ( code & 0x3F ) ) );
    }
    else
    {
        out.push_back( static_cast<char>( 0xE0 | ( code >> 12 ) ) );
        out.push_back( static_cast<char>( 0x80 | ( ( code >> 6 ) & 0x3F ) ) );
        out.push_back( static_cast<char>( 0x80 | ( code & 0x3F ) ) );
    }
}

} // namespace

std::string_view Record::get( std::string_view key ) const
{
    const auto it = fields.find( key );
    return it == fields.end() ? std::string_view {} : std::string_view( it->second );
}

std::optional<Record> parse_record( std::string_view line )
{
    return Parser( line ).object();
}

} // namespace keeldoc

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keeldoc
{
namespace
{

TEST_CASE( "records_read_strings_and_numbers", "[record]" )
{
    const std::optional<Record> record =
        parse_record( R"({"kind":"declaration","line":12,"doc":"A \"pair\".\n\n- one\\two \u0007"})" );

    REQUIRE( record.has_value() );
    REQUIRE( record->get( "kind" ) == "declaration" );
    REQUIRE( record->get( "line" ) == "12" );
    REQUIRE( record->get( "doc" ) == "A \"pair\".\n\n- one\\two \a" );
    REQUIRE( record->get( "parent" ).empty() );
}

TEST_CASE( "records_refuse_what_is_not_a_flat_object", "[record]" )
{
    REQUIRE( parse_record( "" ) == std::nullopt );
    REQUIRE( parse_record( "keelc: no input file" ) == std::nullopt );
    REQUIRE( parse_record( R"({"kind":"file")" ) == std::nullopt );
    REQUIRE( parse_record( R"({"kind":["file"]})" ) == std::nullopt );
    REQUIRE( parse_record( R"({"kind":"file"} x)" ) == std::nullopt );
    REQUIRE( parse_record( "{}" ).has_value() );
}

} // namespace
} // namespace keeldoc
#endif // ENABLE_UNIT_TESTS
