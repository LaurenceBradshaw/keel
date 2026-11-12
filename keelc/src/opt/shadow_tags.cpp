// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/shadow_tags.h"
#include <algorithm>
#include <map>
#include <optional>

namespace keel
{

namespace
{

// A local and the fields under it: `_L.f.g`.
using Path = std::pair<u32, std::vector<u32>>;

// A local's place, as its leading fields and how many projections follow them.
std::optional<std::pair<Path, u32>> leading( const Function& func, const Place& place )
{
    if( place.is_global() || !place.local.is_valid() )
    {
        return std::nullopt;
    }

    const Run<const Projection> run = func.projections_of( place );
    Path                        path { place.local.v, {} };
    u32                         i = 0;
    for( ; i < run.size() && run[i].kind == Projection_kind::Field; ++i )
    {
        path.second.push_back( run[i].field.v );
    }
    return std::pair { std::move( path ), run.size() - i };
}

// `_L.f.g`.
std::optional<Path> path_of( const Function& func, const Place& place )
{
    std::optional<std::pair<Path, u32>> l = leading( func, place );
    return l && l->second == 0 ? std::make_optional( std::move( l->first ) ) : std::nullopt;
}

// `_L.f.g.tag`, as `_L.f.g`.
std::optional<Path> tag_of( const Function& func, const Place& place )
{
    std::optional<std::pair<Path, u32>> l   = leading( func, place );
    const bool                          tag = l && l->second == 1 &&
                     func.projections[place.first_projection + place.num_projections - 1].kind == Projection_kind::Tag;
    return tag ? std::make_optional( std::move( l->first ) ) : std::nullopt;
}

bool under( const Path& prefix, const Path& path )
{
    return prefix.first == path.first && prefix.second.size() <= path.second.size() &&
           std::equal( prefix.second.begin(), prefix.second.end(), path.second.begin() );
}

Path joined( const Path& base, const Path& path, std::size_t from )
{
    Path out = base;
    out.second.insert( out.second.end(), path.second.begin() + static_cast<std::ptrdiff_t>( from ), path.second.end() );
    return out;
}

// Each tag's shadow: past the parameters, named, or moved into one that is, and no address of it or
// above it.
std::map<Path, Local_id> shadows( Function& func )
{
    std::map<Path, Type_id>            wanted;
    std::vector<Path>                  addressed;
    std::vector<std::pair<Path, Path>> moves;

    const auto note = [&]( const Operand& operand )
    {
        if( operand.has_place() )
        {
            if( std::optional<Path> tag = tag_of( func, operand.place ) )
            {
                wanted.emplace( std::move( *tag ), operand.type );
            }
        }
    };

    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        const Rvalue& value = func.rvalue( statement );

        if( std::optional<Path> tag = tag_of( func, statement.place ) )
        {
            wanted.emplace( std::move( *tag ), value.type );
        }

        if( value.kind == Rvalue_kind::Address_of )
        {
            if( std::optional<std::pair<Path, u32>> l = leading( func, value.a.place ) )
            {
                addressed.push_back( std::move( l->first ) );
            }
            continue;
        }

        if( value.kind == Rvalue_kind::Use && value.a.has_place() )
        {
            std::optional<Path> to   = path_of( func, statement.place );
            std::optional<Path> from = path_of( func, value.a.place );
            if( to && from )
            {
                moves.emplace_back( std::move( *to ), std::move( *from ) );
            }
        }

        for_each_operand( value, note );
    }

    for( const Block& block : func.blocks )
    {
        note( block.terminator.condition );
        note( block.terminator.message );
        note( block.terminator.at );
    }

    // A type is never its own part, so this stops.
    for( bool grew = true; grew; )
    {
        grew = false;
        for( const auto& [to, from] : moves )
        {
            for( const auto& [tag, type] : wanted )
            {
                if( under( to, tag ) )
                {
                    grew = wanted.emplace( joined( from, tag, to.second.size() ), type ).second || grew;
                }
            }
        }
    }

    std::map<Path, Local_id> shadow;
    for( const auto& [tag, type] : wanted )
    {
        if( tag.first > func.parameter_count &&
            std::ranges::none_of( addressed, [&]( const Path& a ) { return under( a, tag ); } ) )
        {
            const Span span = func.locals[Local_id { tag.first }].span;
            shadow.emplace( tag, Local_id { narrow_cast<u32>( func.locals.size() ) } );
            func.locals.push_back( Local { .type = type, .span = span } );
        }
    }

    return shadow;
}

} // namespace

void shadow_tags( Function& func )
{
    const std::map<Path, Local_id> shadow = shadows( func );

    const auto find = [&]( const std::optional<Path>& tag )
    {
        const auto it = tag ? shadow.find( *tag ) : shadow.end();
        return it == shadow.end() ? Local_id {} : it->second;
    };
    const auto read = [&]( Operand& operand )
    {
        if( operand.has_place() )
        {
            if( const Local_id tag = find( tag_of( func, operand.place ) ); tag.is_valid() )
            {
                operand = copy( Place { .local = tag }, operand.type );
            }
        }
    };

    std::vector<Statement> statements;
    statements.reserve( func.statements.size() );

    for( Block& block : func.blocks )
    {
        const u32 first       = block.first_statement;
        const u32 count       = block.statement_count;
        block.first_statement = narrow_cast<u32>( statements.size() );

        for( u32 s = first; s < first + count; ++s )
        {
            Statement statement = std::move( func.statements[s] );

            if( statement.kind != Statement_kind::Assign )
            {
                statements.push_back( std::move( statement ) );
                continue;
            }

            Rvalue& value = func.rvalue( statement );
            read( value.a );
            read( value.b );
            for( Operand& argument : value.arguments )
            {
                read( argument );
            }

            if( const Local_id tag = find( tag_of( func, statement.place ) ); tag.is_valid() )
            {
                const Place   place   = statement.place;
                const Type_id type    = func.locals[tag].type;
                Statement     written = func.copy_of( statement, use( copy( Place { .local = tag }, type ) ) );
                statement.place       = Place { .local = tag };
                statements.push_back( std::move( statement ) );
                statements.push_back( std::move( written ) );
                statements.back().place = place;
                continue;
            }

            const std::optional<Path> to = path_of( func, statement.place );
            const std::optional<Path> from =
                value.kind == Rvalue_kind::Use && value.a.has_place() ? path_of( func, value.a.place ) : std::nullopt;
            const Place local = statement.place;
            const Span  span  = statement.span;
            statements.push_back( std::move( statement ) );

            if( !to )
            {
                continue;
            }

            // Each tag the write reaches, from the source's shadow or read back.
            for( const auto& [tag, refreshed] : shadow )
            {
                if( !under( *to, tag ) )
                {
                    continue;
                }

                const Local_id moved = from ? find( joined( *from, tag, to->second.size() ) ) : Local_id {};
                Place          source { .local = moved };
                if( !moved.is_valid() )
                {
                    source = local;
                    for( std::size_t f = to->second.size(); f < tag.second.size(); ++f )
                    {
                        source = projected(
                            func, source, Projection { .kind = Projection_kind::Field, .field = Node_id { tag.second[f] } }
                        );
                    }
                    source = projected( func, source, Projection { .kind = Projection_kind::Tag } );
                }

                statements.push_back( Statement {
                    .kind  = Statement_kind::Assign,
                    .span  = span,
                    .place = Place { .local = refreshed },
                    .value = func.add_rvalue( use( copy( source, func.locals[refreshed].type ) ) ),
                } );
            }
        }

        block.statement_count = narrow_cast<u32>( statements.size() ) - block.first_statement;

        read( block.terminator.condition );
        read( block.terminator.message );
        read( block.terminator.at );
    }

    func.statements = std::move( statements );
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include "check/pipeline_test_support.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr std::string_view k_types = "class D { public i32 v; public D( i32 x ) { v = x; } ~D() { v = 0; } };\n"
                                     "enum E { full( D d ), empty };\n"
                                     "enum O { some( E e ), none };\n"
                                     "i32 main() { return 0; }\n";

// Released, then the branches of the function declaring a local `h`.
struct Shadowed : Compiled
{
    const Function* func = nullptr;

    explicit Shadowed( std::string_view source, Build build = { .level = 2 } )
        : Compiled( std::string( source ) + std::string( k_types ), Stage::Elaborated, build )
    {
        INFO( rendered() );
        REQUIRE( clean() );

        for( const Function& f : functions )
        {
            for( const Local& local : f.locals )
            {
                if( local.name.is_valid() && interner.text( local.name ) == "h" )
                {
                    func = &f;
                }
            }
        }

        REQUIRE( func != nullptr );
        REQUIRE( verify( *func ).empty() );
    }

    std::size_t branches() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            func->blocks, []( const Block& block ) { return block.terminator.kind == Terminator_kind::Branch; }
        ) );
    }
};

} // namespace

// `g` is moved from `h`, so its tag is `h`'s constant and only `full` runs.
TEST_CASE( "shadow_tags_carries_a_tag_through_a_move", "[opt][tags]" )
{
    const Shadowed p( "i32 f( i32 x ) { E h = E::full( D( x ) ); E g = move h; "
                      "switch( g ) { case E::full( d ): return d.v; case E::empty: return 0; } }\n" );
    REQUIRE( p.branches() == 0 );
}

// The payload's tag is built in its own local and moved in, then `o` is moved: still known.
TEST_CASE( "shadow_tags_carries_a_nested_tag", "[opt][tags]" )
{
    const Shadowed p( "i32 f( i32 h ) { O o = O::some( E::full( D( h ) ) ); O g = move o; "
                      "switch( move g ) { case O::some( e ): switch( move e ) { case E::full( d ): return d.v; "
                      "case E::empty: return 1; } case O::none: return 0; } }\n" );
    REQUIRE( p.branches() == 0 );
}

// `touch` may change the variant through its reference, so the test stays.
TEST_CASE( "shadow_tags_leaves_a_tag_a_reference_reaches", "[opt][tags]" )
{
    const Shadowed p(
        "void touch( ref E e ) { e = E::empty; }\n"
        "i32 f( i32 x ) { E h = E::full( D( x ) ); touch( ref h ); "
        "switch( h ) { case E::full( d ): return d.v; case E::empty: return 0; } }\n",
        Build { .level = 2, .inlining = std::nullopt }
    );
    REQUIRE( p.branches() == 1 );
}

} // namespace keel

#endif
