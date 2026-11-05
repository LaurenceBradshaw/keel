// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/shadow_tags.h"

namespace keel
{

namespace
{

// `_L.tag`, of a whole local.
bool names_tag( const Function& func, const Place& place )
{
    return !place.is_global() && place.local.is_valid() && place.num_projections == 1 &&
           func.projections[place.first_projection].kind == Projection_kind::Tag;
}

// Each local's shadow, invalid for one without: past the parameters, its tag named, and no address of it whole
// or of its tag.
std::vector<Local_id> shadows( Function& func, const Type_table& types )
{
    std::vector<bool> named( func.locals.size(), false );
    std::vector<bool> addressed( func.locals.size(), false );

    const auto note = [&]( const Operand& operand )
    {
        if( operand.has_place() && names_tag( func, operand.place ) )
        {
            named[operand.place.local.v] = true;
        }
    };

    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        const Rvalue& value = func.rvalue( statement );

        if( names_tag( func, statement.place ) )
        {
            named[statement.place.local.v] = true;
        }

        if( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() &&
            ( value.a.place.is_whole_local() || names_tag( func, value.a.place ) ) )
        {
            addressed[value.a.place.local.v] = true;
            continue;
        }

        for_each_operand( value, note );
    }

    for( const Block& block : func.blocks )
    {
        note( block.terminator.condition );
        note( block.terminator.message );
        note( block.terminator.at );
    }

    std::vector<Local_id> shadow( func.locals.size() );
    for( u32 l = func.parameter_count + 1; l < named.size(); ++l )
    {
        if( named[l] && !addressed[l] )
        {
            shadow[l] = Local_id { narrow_cast<u32>( func.locals.size() ) };
            func.locals.push_back( Local {
                .type = types.get( func.locals[Local_id { l }].type ).element,
                .span = func.locals[Local_id { l }].span,
            } );
        }
    }

    return shadow;
}

} // namespace

void shadow_tags( Function& func, const Type_table& types )
{
    const std::vector<Local_id> shadow = shadows( func, types );

    const auto of    = [&]( const Place& place ) { return names_tag( func, place ) ? shadow[place.local.v] : Local_id {}; };
    const auto whole = [&]( const Place& place ) {
        return !place.is_global() && place.is_whole_local() && place.local.v < shadow.size() ? shadow[place.local.v]
                                                                                             : Local_id {};
    };
    const auto read = [&]( Operand& operand )
    {
        if( operand.has_place() && of( operand.place ).is_valid() )
        {
            operand = copy( Place { .local = of( operand.place ) }, operand.type );
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

            if( const Local_id tag = of( statement.place ); tag.is_valid() )
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

            const Local_id refreshed = whole( statement.place );
            const Local_id from  = value.kind == Rvalue_kind::Use && value.a.has_place() ? whole( value.a.place ) : Local_id {};
            const Place    local = statement.place;
            const Span     span  = statement.span;
            statements.push_back( std::move( statement ) );

            if( refreshed.is_valid() )
            {
                const Type_id type   = func.locals[refreshed].type;
                const Place   source = from.is_valid() ? Place { .local = from }
                                                       : projected( func, local, Projection { .kind = Projection_kind::Tag } );
                statements.push_back( Statement {
                    .kind  = Statement_kind::Assign,
                    .span  = span,
                    .place = Place { .local = refreshed },
                    .value = func.add_rvalue( use( copy( source, type ) ) ),
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
                                     "i32 main() { return 0; }\n";

// Released, then the branches of the function declaring a local `h`.
struct Shadowed : Compiled
{
    const Function* func = nullptr;

    explicit Shadowed( std::string_view source, Build build = { .optimise = true } )
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

// `touch` may change the variant through its reference, so the test stays.
TEST_CASE( "shadow_tags_leaves_a_tag_a_reference_reaches", "[opt][tags]" )
{
    const Shadowed p(
        "void touch( ref E e ) { e = E::empty; }\n"
        "i32 f( i32 x ) { E h = E::full( D( x ) ); touch( ref h ); "
        "switch( h ) { case E::full( d ): return d.v; case E::empty: return 0; } }\n",
        Build { .optimise = true, .inlining = std::nullopt }
    );
    REQUIRE( p.branches() == 1 );
}

} // namespace keel

#endif
