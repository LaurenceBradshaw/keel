// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "object/kir_state.h"

namespace keel
{

static_assert( sizeof( Local ) == 24, "Local changed: update kir_state.cpp's walk" );
static_assert( sizeof( Projection ) == 16, "Projection changed: update kir_state.cpp's walk" );
static_assert( sizeof( Place ) == 20, "Place changed: update kir_state.cpp's walk" );
static_assert( sizeof( Operand ) == 32, "Operand changed: update kir_state.cpp's walk" );
static_assert( sizeof( Rvalue ) == 120, "Rvalue changed: update kir_state.cpp's walk" );
static_assert( sizeof( Statement ) == 168, "Statement changed: update kir_state.cpp's walk" );
static_assert( sizeof( Terminator ) == 120, "Terminator changed: update kir_state.cpp's walk" );
static_assert( sizeof( Block ) == 128, "Block changed: update kir_state.cpp's walk" );
static_assert( sizeof( Field_slot ) == 20, "Field_slot changed: update kir_state.cpp's walk" );
static_assert( sizeof( Function ) == 272, "Function changed: update kir_state.cpp's walk" );

namespace
{

// Local, block, projection and operand indices are the function's own, so they are copied as
// they are, as are the enums.

void put_types( Byte_writer& out, Ref_writer& refs, const std::vector<Type_id>& types )
{
    out.put_u32( narrow_cast<u32>( types.size() ) );
    for( const Type_id type : types )
    {
        refs.type( out, type );
    }
}

void put_locals( Byte_writer& out, const std::vector<Local_id>& locals )
{
    out.put_u32( narrow_cast<u32>( locals.size() ) );
    for( const Local_id local : locals )
    {
        out.put_u32( local.v );
    }
}

void put_place( Byte_writer& out, Ref_writer& refs, const Place& place )
{
    out.put_u32( place.local.v );
    refs.node( out, place.global );
    out.put_u32( place.first_projection );
    out.put_u32( place.num_projections );
    refs.type( out, place.owner );
}

void put_operand( Byte_writer& out, Ref_writer& refs, const Operand& operand )
{
    out.put_u8( u8( operand.kind ) );
    put_place( out, refs, operand.place );
    refs.literal( out, operand.constant );
    refs.type( out, operand.type );
}

void put_rvalue( Byte_writer& out, Ref_writer& refs, const Rvalue& value )
{
    out.put_u8( u8( value.kind ) );
    refs.type( out, value.type );
    out.put_u32( u32( value.op ) );
    put_operand( out, refs, value.a );
    put_operand( out, refs, value.b );
    out.put_u32( value.first_argument );
    out.put_u32( value.argument_count );
    refs.node( out, value.callee );
    put_types( out, refs, value.type_arguments );
    out.put_u8( u8( value.address_purpose ) );
}

void put_terminator( Byte_writer& out, Ref_writer& refs, const Terminator& terminator )
{
    out.put_u8( u8( terminator.kind ) );
    out.put_u8( u8( terminator.failure ) );
    refs.span( out, terminator.span );
    put_operand( out, refs, terminator.condition );
    out.put_u32( terminator.targets[0].v );
    out.put_u32( terminator.targets[1].v );
    put_operand( out, refs, terminator.message );
    put_operand( out, refs, terminator.at );
}

std::vector<Type_id> get_types( Byte_reader& in, Ref_reader& refs )
{
    std::vector<Type_id> types;
    const u32            count = in.get_u32();
    for( u32 i = 0; i < count && !refs.failed(); ++i )
    {
        types.push_back( refs.type() );
    }
    return types;
}

std::vector<Local_id> get_locals( Byte_reader& in, Ref_reader& refs )
{
    std::vector<Local_id> locals;
    const u32             count = in.get_u32();
    for( u32 i = 0; i < count && !refs.failed(); ++i )
    {
        locals.push_back( Local_id { in.get_u32() } );
    }
    return locals;
}

Place get_place( Byte_reader& in, Ref_reader& refs )
{
    Place place;
    place.local            = Local_id { in.get_u32() };
    place.global           = refs.node();
    place.first_projection = in.get_u32();
    place.num_projections  = in.get_u32();
    place.owner            = refs.type();
    return place;
}

Operand get_operand( Byte_reader& in, Ref_reader& refs )
{
    Operand operand;
    operand.kind     = Operand_kind( in.get_u8() );
    operand.place    = get_place( in, refs );
    operand.constant = refs.literal();
    operand.type     = refs.type();
    return operand;
}

Rvalue get_rvalue( Byte_reader& in, Ref_reader& refs )
{
    Rvalue value;
    value.kind            = Rvalue_kind( in.get_u8() );
    value.type            = refs.type();
    value.op              = Token_kind( in.get_u32() );
    value.a               = get_operand( in, refs );
    value.b               = get_operand( in, refs );
    value.first_argument  = in.get_u32();
    value.argument_count  = in.get_u32();
    value.callee          = refs.node();
    value.type_arguments  = get_types( in, refs );
    value.address_purpose = Address_purpose( in.get_u8() );
    return value;
}

Terminator get_terminator( Byte_reader& in, Ref_reader& refs )
{
    Terminator terminator;
    terminator.kind       = Terminator_kind( in.get_u8() );
    terminator.failure    = Failure( in.get_u8() );
    terminator.span       = refs.span();
    terminator.condition  = get_operand( in, refs );
    terminator.targets[0] = Block_id { in.get_u32() };
    terminator.targets[1] = Block_id { in.get_u32() };
    terminator.message    = get_operand( in, refs );
    terminator.at         = get_operand( in, refs );
    return terminator;
}

// A count, then each element; stops at the first failure, which the caller checks once.
template <typename T, typename Get>
std::vector<T> get_all( Byte_reader& in, Ref_reader& refs, Get&& get )
{
    std::vector<T> all;
    const u32      count = in.get_u32();
    for( u32 i = 0; i < count && !refs.failed(); ++i )
    {
        all.push_back( get() );
    }
    return all;
}

} // namespace

void write_function( Byte_writer& out, Ref_writer& refs, const Function& function )
{
    refs.node( out, function.declaration );
    put_types( out, refs, function.type_arguments );
    out.put_u32( function.parameter_count );
    put_locals( out, function.out_parameters );
    put_locals( out, function.one_path_temporaries );
    put_locals( out, function.walked );
    out.put_u32( function.constructed.v );

    out.put_u32( narrow_cast<u32>( function.fields.size() ) );
    for( const Field_slot& slot : function.fields )
    {
        out.put_u32( slot.local.v );
        out.put_u32( slot.parent );
        out.put_u32( slot.end );
        refs.node( out, slot.field );
        out.put_u8( slot.is_owning );
        out.put_u8( slot.is_const );
        out.put_u8( slot.built_whole );
    }

    out.put_u32( narrow_cast<u32>( function.locals.size() ) );
    for( const Local& local : function.locals )
    {
        refs.type( out, local.type );
        refs.span( out, local.span );
        refs.symbol( out, local.name );
        out.put_u8( local.built_whole );
    }

    out.put_u32( narrow_cast<u32>( function.blocks.size() ) );
    for( const Block& block : function.blocks )
    {
        out.put_u32( block.first_statement );
        out.put_u32( block.statement_count );
        put_terminator( out, refs, block.terminator );
    }

    out.put_u32( narrow_cast<u32>( function.statements.size() ) );
    for( const Statement& statement : function.statements )
    {
        out.put_u8( u8( statement.kind ) );
        out.put_u8( statement.replacing );
        refs.span( out, statement.span );
        put_place( out, refs, statement.place );
        put_rvalue( out, refs, statement.value );
        out.put_u32( statement.drop_flag.v );
    }

    out.put_u32( narrow_cast<u32>( function.projections.size() ) );
    for( const Projection& projection : function.projections )
    {
        out.put_u8( u8( projection.kind ) );
        refs.node( out, projection.field );
        refs.type( out, projection.member );
        out.put_u32( projection.index.v );
    }

    out.put_u32( narrow_cast<u32>( function.operands.size() ) );
    for( const Operand& operand : function.operands )
    {
        put_operand( out, refs, operand );
    }

    out.put_u8( function.returns_a_value );
    out.put_u8( function.diverges );
}

Function read_function( Byte_reader& in, Ref_reader& refs )
{
    Function function;
    function.declaration          = refs.node();
    function.type_arguments       = get_types( in, refs );
    function.parameter_count      = in.get_u32();
    function.out_parameters       = get_locals( in, refs );
    function.one_path_temporaries = get_locals( in, refs );
    function.walked               = get_locals( in, refs );
    function.constructed          = Local_id { in.get_u32() };

    function.fields = get_all<Field_slot>(
        in,
        refs,
        [&]
        {
            Field_slot slot;
            slot.local       = Local_id { in.get_u32() };
            slot.parent      = in.get_u32();
            slot.end         = in.get_u32();
            slot.field       = refs.node();
            slot.is_owning   = in.get_u8() != 0;
            slot.is_const    = in.get_u8() != 0;
            slot.built_whole = in.get_u8() != 0;
            return slot;
        }
    );

    function.locals = get_all<Local>(
        in,
        refs,
        [&]
        {
            Local local;
            local.type        = refs.type();
            local.span        = refs.span();
            local.name        = refs.symbol();
            local.built_whole = in.get_u8() != 0;
            return local;
        }
    );

    function.blocks = get_all<Block>(
        in,
        refs,
        [&]
        {
            Block block;
            block.first_statement = in.get_u32();
            block.statement_count = in.get_u32();
            block.terminator      = get_terminator( in, refs );
            return block;
        }
    );

    function.statements = get_all<Statement>(
        in,
        refs,
        [&]
        {
            Statement statement;
            statement.kind      = Statement_kind( in.get_u8() );
            statement.replacing = in.get_u8() != 0;
            statement.span      = refs.span();
            statement.place     = get_place( in, refs );
            statement.value     = get_rvalue( in, refs );
            statement.drop_flag = Local_id { in.get_u32() };
            return statement;
        }
    );

    function.projections = get_all<Projection>(
        in,
        refs,
        [&]
        {
            Projection projection;
            projection.kind   = Projection_kind( in.get_u8() );
            projection.field  = refs.node();
            projection.member = refs.type();
            projection.index  = Local_id { in.get_u32() };
            return projection;
        }
    );

    function.operands        = get_all<Operand>( in, refs, [&] { return get_operand( in, refs ); } );
    function.returns_a_value = in.get_u8() != 0;
    function.diverges        = in.get_u8() != 0;
    return function;
}

} // namespace keel
