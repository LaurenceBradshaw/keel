// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/main_shim.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
#include "ir/builder.h"

namespace keel
{
namespace
{
// Each module's build or drop functions, dependencies first, or for drops last.
std::vector<const Function*>
global_functions( const Ast& ast, const Module_graph& modules, File_id input, std::span<const Function> functions, bool drops )
{
    std::vector<const Function*> found;
    for( const Function& function : functions )
    {
        if( function.globals.is_valid() && function.drops_globals == drops )
        {
            found.push_back( &function );
        }
    }

    const std::vector<File_id> order = modules.dependencies_first( input );
    const auto                 rank  = [&]( const Function* f )
    { return std::ranges::find( order, ast.span( f->globals ).file ) - order.begin(); };
    std::ranges::sort( found, {}, rank ); // one per module
    if( drops )
    {
        std::ranges::reverse( found );
    }
    return found;
}

Node_id field_named( const Ast& ast, const Interner& interner, Node_id aggregate, std::string_view name )
{
    for( const Node_id field : ast.contained_fields( aggregate ) )
    {
        if( interner.text( ast.name( field ) ) == name )
        {
            return field;
        }
    }
    return {};
}
} // namespace

std::optional<Function> main_shim(
    const Ast&                ast,
    Types&                    types,
    Literal_pool&             literals,
    const Interner&           interner,
    const Module_graph&       modules,
    File_id                   input,
    std::span<const Function> functions
)
{
    const auto is_main = [&]( const Function& f )
    {
        return f.declaration.is_valid() && f.type_arguments.empty() && ast.kind( f.declaration ) == Node_kind::Function_decl &&
               ast.span( f.declaration ).file == input && interner.text( ast.name( f.declaration ) ) == "main";
    };
    const auto found = std::ranges::find_if( functions, is_main );
    if( found == functions.end() )
    {
        return std::nullopt;
    }

    const Function& keel_main = *found;
    Type_table&     table     = types.table();
    const Span      span      = ast.span( keel_main.declaration );
    const Type_id   integer   = table.integer( 32, true );
    const Type_id   word      = table.integer( 64, false );
    const Type_id   byte      = table.integer( 8, false );
    const Type_id   boolean   = table.builtin( Type_kind::Bool );
    const Type_id   nothing   = table.builtin( Type_kind::Void );
    const auto number = [&]( std::uint64_t value, Type_id type ) { return constant( literals.add_integer( value ), type ); };

    Builder builder( Node_id {}, integer, span );

    const auto calls = [&]( bool drops )
    {
        for( const Function* function : global_functions( ast, modules, input, functions, drops ) )
        {
            builder.into_temp( globals_call( function->globals, drops, nothing ), nothing, span );
        }
    };

    std::vector<Operand> arguments;
    Local_id             args {};

    // `argv` past the program's name, each as a `str` of its bytes before the terminator.
    if( keel_main.parameter_count > 0 )
    {
        const Type_id run = keel_main.locals[Local_id { 1 }].type;
        assert( table.is_run_pointer( run ) && "`main` takes its arguments as a run of `str`" );

        const Type_id str   = table.get( table.get( run ).element ).element;
        const Node_id data  = field_named( ast, interner, table.get( str ).declaration, "data" );
        const Node_id size  = field_named( ast, interner, table.get( str ).declaration, "size" );
        const Type_id bytes = field_type( ast, table, str, data, types.recorded() );
        const Type_id many  = table.many_pointer_to( str );

        const Local_id argc  = builder.add_parameter( integer, span, {} );
        const Local_id argv  = builder.add_parameter( table.many_pointer_to( bytes ), span, {} );
        const Local_id count = builder.add_local( word, span );
        const Local_id i     = builder.add_local( word, span );
        const Local_id text  = builder.add_local( bytes, span );
        const Local_id n     = builder.add_local( word, span );
        args                 = builder.add_local( many, span );

        const auto read = [&]( Local_id local ) { return copy( builder.place( local ), builder.type_of( local ) ); };
        const auto temp = [&]( Rvalue value, Type_id type )
        { return read( builder.into_temp( std::move( value ), type, span ) ); };
        const auto jump = [&]( Block_id to )
        {
            builder.terminate_goto( to, span );
            builder.switch_to( to );
        };

        const Block_id named    = builder.add_block();
        const Block_id nameless = builder.add_block();
        const Block_id counted  = builder.add_block();
        const Block_id each     = builder.add_block();
        const Block_id arg      = builder.add_block();
        const Block_id measure  = builder.add_block();
        const Block_id longer   = builder.add_block();
        const Block_id measured = builder.add_block();
        const Block_id built    = builder.add_block();

        // An empty `argv` has no name.
        builder.terminate_branch(
            temp( binary( Token_kind::Greater, read( argc ), number( 0, integer ), boolean ), boolean ), named, nameless, span
        );
        builder.switch_to( named );
        builder.assign(
            builder.place( count ),
            binary( Token_kind::Minus, temp( cast_to( read( argc ), word ), word ), number( 1, word ), word ),
            span
        );
        builder.terminate_goto( counted, span );
        builder.switch_to( nameless );
        builder.assign( builder.place( count ), use( number( 0, word ) ), span );
        jump( counted );

        builder.assign( builder.place( args ), allocate( many, read( count ) ), ast.name_span( keel_main.declaration ) );
        builder.assign( builder.place( i ), use( number( 0, word ) ), span );
        jump( each );

        builder.terminate_branch(
            temp( binary( Token_kind::Less, read( i ), read( count ), boolean ), boolean ), arg, built, span
        );
        builder.switch_to( arg );
        const Operand  next  = temp( binary( Token_kind::Plus, read( i ), number( 1, word ), word ), word );
        const Local_id entry = builder.into_temp(
            binary( Token_kind::Plus, read( argv ), next, builder.type_of( argv ) ), builder.type_of( argv ), span
        );
        builder.assign( builder.place( text ), use( copy( builder.deref( builder.place( entry ) ), bytes ) ), span );
        builder.assign( builder.place( n ), use( number( 0, word ) ), span );
        jump( measure );

        const Local_id at = builder.into_temp( binary( Token_kind::Plus, read( text ), read( n ), bytes ), bytes, span );
        const Operand  c  = temp( use( copy( builder.deref( builder.place( at ) ), byte ) ), byte );
        builder.terminate_branch(
            temp( binary( Token_kind::Bang_equal, c, number( 0, byte ), boolean ), boolean ), longer, measured, span
        );
        builder.switch_to( longer );
        builder.assign( builder.place( n ), binary( Token_kind::Plus, read( n ), number( 1, word ), word ), span );
        builder.terminate_goto( measure, span );

        builder.switch_to( measured );
        const Local_id slot   = builder.into_temp( binary( Token_kind::Plus, read( args ), read( i ), many ), many, span );
        const Place    object = builder.deref( builder.place( slot ) );
        builder.assign( builder.field( object, data ), use( read( text ) ), span );
        builder.assign( builder.field( object, size ), use( read( n ) ), span );
        builder.assign( builder.place( i ), binary( Token_kind::Plus, read( i ), number( 1, word ), word ), span );
        builder.terminate_goto( each, span );

        builder.switch_to( built );
        calls( false );
        arguments.push_back( temp( run_of( read( args ), read( count ), run ), run ) );
    }
    else
    {
        calls( false );
    }

    // D21: `main` returns `i32`.
    assert( keel_main.locals[k_return_slot].type == integer );
    builder.assign( builder.place( k_return_slot ), call( keel_main.declaration, std::move( arguments ), integer ), span );

    calls( true );
    if( args.is_valid() )
    {
        builder.into_temp( release( copy( builder.place( args ), builder.type_of( args ) ) ), nothing, span );
    }
    builder.terminate_return( span );

    Function shim        = builder.finish();
    shim.entry           = true;
    shim.returns_a_value = true;
    return shim;
}

} // namespace keel
