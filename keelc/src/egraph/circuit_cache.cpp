// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "egraph/circuit_cache.h"
#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include "common/bytes.h"
#include "common/temp_dir.h"
#include "common/version.h"

namespace keel
{
namespace
{
// The map by its coefficients, not its elements' ids, which depend on the order they were made.
std::string key( const Field& field, const std::vector<Row>& rows, const Solve_options& options )
{
    std::string out = "keelc " + std::string( build_id() ) + '\n';
    out += "options " + std::to_string( options.fuel ) + ' ' + std::to_string( options.graph_fuel ) + ' ' +
           std::to_string( options.restart_fuel ) + ' ' + std::to_string( options.restarts ) + ' ' +
           std::to_string( options.direct_max ) + '\n';
    out += "ring " + std::to_string( static_cast<u32>( field.kind() ) ) + ' ' + std::to_string( field.n() ) + ' ' +
           std::to_string( field.width() ) + '\n';
    for( const Row& row : rows )
    {
        out += 'r';
        for( const Element e : row )
        {
            out += ' ';
            for( u32 k = 0; k < field.degree(); ++k )
            {
                out += std::to_string( field.coefficient( e, k ).first ) + ',';
            }
            out += std::to_string( field.coefficient( e, 0 ).second );
        }
        out += '\n';
    }
    return out + "circuit\n";
}

std::filesystem::path entry( const std::filesystem::path& folder, const std::string& key )
{
    return folder / "circuits" / hex( fnv1a( key ) );
}

// Each output, on each unit input, is its row's entry, to rounding.
bool computes( const Circuit& circuit, const Field& field, const std::vector<Row>& rows )
{
    const size_t columns = rows.empty() ? 0 : rows.front().size();
    for( size_t j = 0; j < columns; ++j )
    {
        std::vector<std::complex<f64>> x( columns );
        x[j]                                       = 1;
        const std::vector<std::complex<f64>> value = circuit.evaluate( x );
        for( const Row& row : rows )
        {
            const auto              found = circuit.outputs.find( row );
            const std::complex<f64> want  = field.value( row[j] );
            const std::complex<f64> got   = found == circuit.outputs.end() ? 0 : value[found->second];
            if( !( std::abs( got - want ) <= 1e-9 * ( 1 + std::abs( want ) ) ) )
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace

std::optional<Circuit>
cached_circuit( const std::filesystem::path& folder, Field& field, const std::vector<Row>& rows, const Solve_options& options )
{
    const std::string k = key( field, rows, options );
    std::ifstream     in( entry( folder, k ), std::ios::binary );
    const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    if( !text.starts_with( k ) )
    {
        return std::nullopt;
    }
    std::optional<Circuit> circuit = Circuit::load( std::string_view( text ).substr( k.size() ), field, rows );
    if( !circuit || !computes( *circuit, field, rows ) )
    {
        return std::nullopt;
    }
    return circuit;
}

void cache_circuit(
    const std::filesystem::path& folder,
    const Field&                 field,
    const std::vector<Row>&      rows,
    const Solve_options&         options,
    const Circuit&               circuit
)
{
    static std::atomic<u32>     counter { 0 };
    const std::string           k    = key( field, rows, options );
    const std::filesystem::path file = entry( folder, k );
    std::filesystem::path       part = file;
    part += "." + std::to_string( pid() ) + "_" + std::to_string( counter++ );

    std::error_code ec;
    std::filesystem::create_directories( file.parent_path(), ec );
    {
        std::ofstream     out( part, std::ios::binary );
        const std::string text = k + circuit.saved( rows );
        out.write( text.data(), static_cast<std::streamsize>( text.size() ) );
        if( !out.flush() )
        {
            out.close();
            std::filesystem::remove( part, ec );
            return;
        }
    }
    std::filesystem::rename( part, file, ec );
    if( ec )
    {
        std::filesystem::remove( part, ec );
    }
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "check/pipeline_test_support.h"
#include "ir/print.h"

namespace keel
{
namespace
{
std::vector<Row> dft8( Field& field )
{
    std::vector<Row> rows( 8 );
    for( u32 k = 0; k < 8; ++k )
    {
        for( u32 j = 0; j < 8; ++j )
        {
            rows[k].push_back( field.root( j * k ) );
        }
    }
    return rows;
}

std::string read( const std::filesystem::path& file )
{
    std::ifstream in( file, std::ios::binary );
    return { std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
}

TEST_CASE( "a_cached_circuit_is_read_back_as_itself_and_a_damaged_one_is_a_miss", "[egraph][circuit_cache]" )
{
    const Temp_dir         dir;
    Field                  field( Ring_kind::Complex, 8 );
    const std::vector<Row> rows    = dft8( field );
    const Circuit          circuit = solve( field, rows ).circuit;
    REQUIRE_FALSE( cached_circuit( dir.path, field, rows, {} ) );

    cache_circuit( dir.path, field, rows, {}, circuit );
    const std::optional<Circuit> hit = cached_circuit( dir.path, field, rows, {} );
    REQUIRE( hit );
    REQUIRE( hit->text() == circuit.text() );
    REQUIRE( hit->cost() == circuit.cost() );

    REQUIRE_FALSE( cached_circuit( dir.path, field, rows, { .restarts = 3 } ) );

    const std::filesystem::path file    = *std::filesystem::directory_iterator( dir.path / "circuits" );
    const std::string           whole   = read( file );
    const auto                  damaged = [&]( std::string text )
    {
        dir.write( "circuits/" + file.filename().string(), text );
        return !cached_circuit( dir.path, field, rows, {} );
    };
    REQUIRE( damaged( whole.substr( 0, whole.size() - 10 ) ) );
    REQUIRE( damaged( "not an entry" ) );
    const size_t y = whole.find( "\ny 0 " ) + 5;
    REQUIRE( damaged( whole.substr( 0, y ) + "0" + whole.substr( whole.find( '\n', y ) ) ) );
    REQUIRE( !damaged( whole ) );
}

constexpr std::string_view k_two_dfts = R"(
@trig_cos
f64 cosine( f64 x ) { return x; }
@trig_sin
f64 sine( f64 x ) { return x; }

@real_arithmetic
void dft8( const ref f64[8] re, const ref f64[8] im, ref f64[8] yr, ref f64[8] yi )
{
    for( u64 k = 0; k < 8; k += 1 )
    {
        f64 sr = 0.0;
        f64 si = 0.0;
        for( u64 j = 0; j < 8; j += 1 )
        {
            const f64 t = -2.0 * 3.141592653589793 * cast<f64>( j * k ) / 8.0;
            sr += re[j] * cosine( t ) - im[j] * sine( t );
            si += re[j] * sine( t ) + im[j] * cosine( t );
        }
        yr[k] = sr;
        yi[k] = si;
    }
}

@real_arithmetic
void dft<u64 N>( const ref f64[N] re, const ref f64[N] im, ref f64[N] yr, ref f64[N] yi )
{
    for( u64 k = 0; k < N; k += 1 )
    {
        f64 sr = 0.0;
        f64 si = 0.0;
        for( u64 j = 0; j < N; j += 1 )
        {
            const f64 c = cosine( -2.0 * 3.141592653589793 * cast<f64>( j * k ) / cast<f64>( N ) );
            const f64 s = sine( -2.0 * 3.141592653589793 * cast<f64>( j * k ) / cast<f64>( N ) );
            sr += re[j] * c - im[j] * s;
            si += re[j] * s + im[j] * c;
        }
        yr[k] = sr;
        yi[k] = si;
    }
}

i32 main()
{
    const f64[8] x = [ 1.0, .. ];
    f64[8] yr = [ 0.0, .. ];
    f64[8] yi = [ 0.0, .. ];
    dft8( x, x, ref yr, ref yi );
    dft<8>( x, x, ref yr, ref yi );
    return yr[0] == 8.0 ? 0 : 1;
}
)";

std::vector<std::string> compiled_kir( const std::filesystem::path& cache )
{
    const Compiled c( k_two_dfts, Stage::Elaborated, Build { .level = 3, .cache = cache } );
    INFO( c.rendered() );
    REQUIRE( c.clean() );
    std::vector<std::string> kir;
    for( const Function& f : c.functions )
    {
        kir.push_back( print( f, c.ast, c.types.table(), c.literals, c.interner ) );
    }
    return kir;
}

TEST_CASE( "a_written_dft8_and_dft_at_8_share_an_entry_that_a_second_compile_reads", "[egraph][circuit_cache]" )
{
    const Temp_dir                 dir;
    const std::vector<std::string> first = compiled_kir( dir.path );
    REQUIRE( std::distance( std::filesystem::directory_iterator( dir.path / "circuits" ), {} ) == 1 );
    REQUIRE( compiled_kir( dir.path ) == first );
    REQUIRE( compiled_kir( {} ) == first );
}
} // namespace
} // namespace keel
#endif // ENABLE_UNIT_TESTS
