#include "common/source_manager.h"
#include <algorithm>
#include <cassert>
#include <fstream>

namespace keel
{

File_id Source_manager::add_file( std::string path, std::string text )
{
    assert( text.size() <= UINT32_MAX && "source file too large" );

    std::vector<u32> starts = build_line_starts( text );

    files_.push_back( std::make_unique<Source_file>( std::move( path ), std::move( text ), std::move( starts ) ) );

    return File_id { narrow_cast<u32>( files_.size() - 1 ) };
}

std::optional<File_id> Source_manager::load_file( const std::filesystem::path& path )
{
    std::error_code       ec;
    std::filesystem::path canonical_path = std::filesystem::weakly_canonical( path, ec );

    if( ec )
    {
        return std::nullopt;
    }

    if( auto it = by_path_.find( canonical_path.string() ); it != by_path_.end() )
    {
        return it->second;
    }

    // Directories and devices open fine and only fail on the first read, so check up front.
    bool is_regular = std::filesystem::is_regular_file( canonical_path, ec );
    if( ec || !is_regular )
    {
        return std::nullopt;
    }

    u64 file_size = std::filesystem::file_size( canonical_path, ec );
    if( ec || file_size > UINT32_MAX )
    {
        return std::nullopt;
    }

    std::ifstream file( canonical_path, std::ios::in | std::ios::binary );
    if( !file )
    {
        return std::nullopt;
    }

    std::string text;
    text.resize( static_cast<std::size_t>( file_size ) );
    file.read( text.data(), static_cast<std::streamsize>( file_size ) );

    // An I/O error mid-read yields a silently truncated file.
    if( file.gcount() != static_cast<std::streamsize>( file_size ) )
    {
        return std::nullopt;
    }

    // Store the path as given for display, but use the canonical path for dedup.
    File_id id = add_file( path.string(), std::move( text ) );
    by_path_.emplace( canonical_path.string(), id );

    return id;
}

const Source_file& Source_manager::file( File_id id ) const
{
    assert( id.is_valid() && "invalid file id" );
    assert( id.v < files_.size() && "file id out of range" );
    return *files_[id.v];
}

std::string_view Source_manager::text( Span s ) const
{
    const Source_file& f = file( s.file );
    assert( s.start <= s.end && s.end <= f.text.size() && "span out of range" );
    return std::string_view( f.text ).substr( s.start, s.len() );
}

Line_col Source_manager::line_col( File_id id, u32 offset ) const
{
    const Source_file& f = file( id );
    assert( offset <= f.text.size() && "offset out of range" );

    auto it = std::upper_bound( f.line_starts.begin(), f.line_starts.end(), offset );

    assert( it != f.line_starts.begin() && "offset is before the first line start" );

    const std::size_t idx = static_cast<std::size_t>( ( it - f.line_starts.begin() ) - 1 );

    return Line_col { narrow_cast<u32>( idx + 1 ), offset - f.line_starts[idx] + 1 }; // 1-based
}

std::string_view Source_manager::line_text( File_id id, u32 line ) const
{
    // Line is 1-based here.
    const Source_file& f = file( id );
    assert( line >= 1 && line <= f.line_starts.size() && "line number out of range" );

    u32 start = f.line_starts[line - 1];
    u32 end   = ( line < f.line_starts.size() ) ? f.line_starts[line] : narrow_cast<u32>( f.text.size() );

    // Strip \n then \r, so CRLF never reaches the renderer and caret alignment holds.
    if( end > start && f.text[end - 1] == '\n' )
    {
        --end;
    }

    if( end > start && f.text[end - 1] == '\r' )
    {
        --end;
    }

    return std::string_view( f.text ).substr( start, end - start );
}

// Always starts at 0, so an empty file has one empty line. A trailing newline pushes text.size(),
// creating a phantom final line - that is what makes line_col() at the EOF offset resolve without a
// special case.
std::vector<u32> Source_manager::build_line_starts( std::string_view text )
{
    std::vector<u32> line_starts;
    line_starts.push_back( 0 );
    for( std::size_t i = 0; i < text.size(); ++i )
    {
        if( text[i] == '\n' )
        {
            line_starts.push_back( narrow_cast<u32>( i + 1 ) );
        }
    }

    return line_starts;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>

namespace keel
{
namespace
{

// Removes itself on scope exit; load_file is the only part of Source_manager that needs real files.
struct Temp_dir
{
    std::filesystem::path path;

    Temp_dir()
    {
        static std::atomic<int> counter { 0 };
        path = std::filesystem::temp_directory_path() / ( "keel_sm_test_" + std::to_string( counter++ ) );
        std::filesystem::remove_all( path );
        std::filesystem::create_directories( path );
    }

    ~Temp_dir()
    {
        std::error_code ec;
        std::filesystem::remove_all( path, ec );
    }

    std::filesystem::path write( const std::string& name, std::string_view contents ) const
    {
        const std::filesystem::path p = path / name;
        std::ofstream               out( p, std::ios::binary );
        out.write( contents.data(), static_cast<std::streamsize>( contents.size() ) );
        return p;
    }
};

} // namespace

TEST_CASE( "source_manager_add_file_assigns_sequential_ids", "[common][source_manager]" )
{
    Source_manager sm;

    const File_id a = sm.add_file( "a.kl", "i32 x;" );
    const File_id b = sm.add_file( "b.kl", "i32 y;" );

    REQUIRE( a.is_valid() );
    REQUIRE( b.is_valid() );
    REQUIRE( a.v == 0 );
    REQUIRE( b.v == 1 );

    REQUIRE( sm.file( a ).path == "a.kl" );
    REQUIRE( sm.file( a ).text == "i32 x;" );
    REQUIRE( sm.file( b ).path == "b.kl" );
}

TEST_CASE( "source_manager_text_extracts_span", "[common][source_manager]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 add( i32 a );" );

    REQUIRE( sm.text( Span { f, 0, 3 } ) == "i32" );
    REQUIRE( sm.text( Span { f, 4, 7 } ) == "add" );
    REQUIRE( sm.text( Span { f, 0, 17 } ) == "i32 add( i32 a );" );

    // Zero-length spans are legitimate, not an error.
    REQUIRE( sm.text( Span::point( f, 4 ) ).empty() );
    REQUIRE( sm.text( Span::point( f, 17 ) ).empty() );
}

TEST_CASE( "source_manager_line_col_empty_file", "[common][source_manager]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "empty.kl", "" );

    const Line_col lc = sm.line_col( f, 0 );
    REQUIRE( lc.line == 1 );
    REQUIRE( lc.col == 1 );

    REQUIRE( sm.line_text( f, 1 ).empty() );
}

TEST_CASE( "source_manager_line_col_single_line_no_newline", "[common][source_manager]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 x;" );

    REQUIRE( sm.line_col( f, 0 ).line == 1 );
    REQUIRE( sm.line_col( f, 0 ).col == 1 );
    REQUIRE( sm.line_col( f, 4 ).col == 5 );

    // offset == text.size() is the EOF position and must resolve, not assert.
    const Line_col eof = sm.line_col( f, 6 );
    REQUIRE( eof.line == 1 );
    REQUIRE( eof.col == 7 );

    REQUIRE( sm.line_text( f, 1 ) == "i32 x;" );
}

TEST_CASE( "source_manager_line_col_across_newlines", "[common][source_manager]" )
{
    Source_manager sm;
    //                                     0123 4567 89
    const File_id f = sm.add_file( "a.kl", "ab\ncd\nef" );

    REQUIRE( sm.line_col( f, 0 ).line == 1 ); // 'a'
    REQUIRE( sm.line_col( f, 1 ).col == 2 );  // 'b'
    REQUIRE( sm.line_col( f, 2 ).line == 1 ); // the '\n' itself belongs to line 1
    REQUIRE( sm.line_col( f, 2 ).col == 3 );

    REQUIRE( sm.line_col( f, 3 ).line == 2 ); // 'c', first byte after the newline
    REQUIRE( sm.line_col( f, 3 ).col == 1 );

    REQUIRE( sm.line_col( f, 6 ).line == 3 ); // 'e'
    REQUIRE( sm.line_col( f, 6 ).col == 1 );

    REQUIRE( sm.line_text( f, 1 ) == "ab" );
    REQUIRE( sm.line_text( f, 2 ) == "cd" );
    REQUIRE( sm.line_text( f, 3 ) == "ef" );
}

// A file ending in a newline has a phantom final empty line - this is what makes line_col at the EOF
// offset resolve without a special case.
TEST_CASE( "source_manager_trailing_newline_creates_final_empty_line", "[common][source_manager]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "ab\n" );

    const Line_col eof = sm.line_col( f, 3 );
    REQUIRE( eof.line == 2 );
    REQUIRE( eof.col == 1 );

    REQUIRE( sm.line_text( f, 1 ) == "ab" );
    REQUIRE( sm.line_text( f, 2 ).empty() );
}

TEST_CASE( "source_manager_line_text_strips_terminators", "[common][source_manager]" )
{
    Source_manager sm;

    SECTION( "lf" )
    {
        const File_id f = sm.add_file( "a.kl", "ab\ncd\n" );
        REQUIRE( sm.line_text( f, 1 ) == "ab" );
        REQUIRE( sm.line_text( f, 2 ) == "cd" );
    }

    SECTION( "crlf" )
    {
        const File_id f = sm.add_file( "a.kl", "ab\r\ncd\r\n" );
        REQUIRE( sm.line_text( f, 1 ) == "ab" );
        REQUIRE( sm.line_text( f, 2 ) == "cd" );
    }

    SECTION( "last line without a terminator" )
    {
        const File_id f = sm.add_file( "a.kl", "ab\ncd" );
        REQUIRE( sm.line_text( f, 2 ) == "cd" );
    }
}

// Columns count bytes, by design. A multi-byte character advances the column by its byte length.
TEST_CASE( "source_manager_columns_count_bytes", "[common][source_manager]" )
{
    Source_manager sm;
    // "a" + U+03C0 (2 bytes) + "b"; split literals so the hex escape does not swallow 'b'.
    const File_id f = sm.add_file(
        "a.kl",
        "a\xCF\x80"
        "b"
    );

    REQUIRE( sm.file( f ).text.size() == 4 );
    REQUIRE( sm.line_col( f, 0 ).col == 1 ); // 'a'
    REQUIRE( sm.line_col( f, 1 ).col == 2 ); // first byte of the two-byte char
    REQUIRE( sm.line_col( f, 3 ).col == 4 ); // 'b'
}

// The reason files_ holds unique_ptr rather than Source_file by value. The first file is deliberately
// short enough for SSO, which is the case that actually dangles.
TEST_CASE( "source_manager_references_survive_growth", "[common][source_manager]" )
{
    Source_manager sm;

    const File_id          first = sm.add_file( "a.kl", "i32 x;" );
    const std::string_view view  = sm.text( Span { first, 0, 6 } );
    const Source_file&     ref   = sm.file( first );

    for( int i = 0; i < 64; ++i )
    {
        sm.add_file( "f" + std::to_string( i ) + ".kl", "i32 y;" );
    }

    REQUIRE( view == "i32 x;" );
    REQUIRE( ref.path == "a.kl" );
    REQUIRE( ref.text == "i32 x;" );
    REQUIRE( sm.file( first ).text == "i32 x;" );
}

TEST_CASE( "source_manager_load_file_reads_from_disk", "[common][source_manager]" )
{
    const Temp_dir dir;
    const auto     p = dir.write( "a.kl", "i32 x;\ni32 y;\n" );

    Source_manager sm;
    const auto     id = sm.load_file( p );

    REQUIRE( id.has_value() );
    REQUIRE( sm.file( *id ).text == "i32 x;\ni32 y;\n" );
    REQUIRE( sm.line_text( *id, 2 ) == "i32 y;" );
}

TEST_CASE( "source_manager_load_file_fails_gracefully", "[common][source_manager]" )
{
    const Temp_dir dir;
    Source_manager sm;

    REQUIRE_FALSE( sm.load_file( dir.path / "does_not_exist.kl" ).has_value() );
    REQUIRE_FALSE( sm.load_file( dir.path ).has_value() ); // a directory, not a file
}

// One file means one File_id: diagnostics and #line emission both depend on it.
TEST_CASE( "source_manager_load_file_deduplicates", "[common][source_manager]" )
{
    const Temp_dir dir;
    const auto     p = dir.write( "a.kl", "i32 x;" );

    Source_manager sm;

    const auto first  = sm.load_file( p );
    const auto second = sm.load_file( p );

    REQUIRE( first.has_value() );
    REQUIRE( second.has_value() );
    REQUIRE( first->v == second->v );

    SECTION( "non-canonical spellings resolve to the same id" )
    {
        const auto via_dot   = sm.load_file( dir.path / "." / "a.kl" );
        const auto via_updir = sm.load_file( dir.path / "sub" / ".." / "a.kl" );

        REQUIRE( via_dot.has_value() );
        REQUIRE( via_dot->v == first->v );
        REQUIRE( via_updir.has_value() );
        REQUIRE( via_updir->v == first->v );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
