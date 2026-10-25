// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "object/refs.h"
#include <cassert>
#include "parse/module_graph.h"

// Every reference is written one above its index, so 0 is the invalid one.

namespace keel
{

Ref_writer::Ref_writer(
    const Program&        program,
    const Type_table&     table,
    const Literal_pool&   literals,
    const Source_manager& sm,
    const Interner&       interner,
    File_id               module
)
    : program_( program ),
      table_( table ),
      literals_( literals ),
      sm_( sm ),
      interner_( interner ),
      module_( module )
{
    first_ = program.first_node.at( module.v );
    end_   = narrow_cast<u32>( program.ast.node_count() );

    for( const auto& [file, first] : program.first_node )
    {
        if( first > first_ && first < end_ )
        {
            end_ = first;
        }
    }

    // The root is added after every file, so it lies past the run unless the module was parsed last.
    while( end_ > first_ && program.ast.span( Node_id { end_ - 1 } ).file != module )
    {
        --end_;
    }

    tree( module );
}

u32 Ref_writer::first() const
{
    return first_;
}

u32 Ref_writer::end() const
{
    return end_;
}

u32 Ref_writer::tree( File_id file )
{
    const auto [it, added] = tree_of_.emplace( file.v, narrow_cast<u32>( trees_.size() ) );

    if( added )
    {
        trees_.push_back( module_name( program_.modules, sm_, interner_, file ) );
    }

    return it->second;
}

void Ref_writer::node( Byte_writer& out, Node_id id )
{
    if( !id.is_valid() )
    {
        out.put_u32( 0 );
        return;
    }

    const File_id file = program_.ast.span( id ).file;
    out.put_u32( tree( file ) + 1 );
    out.put_u32( id.v - program_.first_node.at( file.v ) );
}

void Ref_writer::local( Byte_writer& out, Node_id id )
{
    if( !id.is_valid() )
    {
        out.put_u32( 0 );
        return;
    }

    assert( id.v >= first_ && id.v < end_ && "a local reference outside the module" );
    out.put_u32( id.v - first_ + 1 );
}

void Ref_writer::symbol( Byte_writer& out, Symbol_id id )
{
    if( !id.is_valid() )
    {
        out.put_u32( 0 );
        return;
    }

    const auto [it, added] = symbol_of_.emplace( id.v, narrow_cast<u32>( symbols_.size() ) );

    if( added )
    {
        symbols_.push_back( id );
    }

    out.put_u32( it->second + 1 );
}

void Ref_writer::literal( Byte_writer& out, Literal_id id )
{
    if( !id.is_valid() )
    {
        out.put_u8( 0 );
        return;
    }

    const Literal_kind kind = literals_.kind( id );
    out.put_u8( u8( kind ) + 1 );

    switch( kind )
    {
    case Literal_kind::Integer:
        out.put_u64( literals_.integer( id ) );
        break;
    case Literal_kind::Float:
        out.put_f64( literals_.floating( id ) );
        break;
    case Literal_kind::String:
        out.put_string( literals_.string( id ) );
        break;
    }
}

void Ref_writer::type( Byte_writer& out, Type_id id )
{
    out.put_u32( id.is_valid() ? type_index( id ) + 1 : 0 );
}

// The type's parts are written before it, so the reader interns each entry from ones it has.
u32 Ref_writer::type_index( Type_id id )
{
    if( const auto found = type_of_.find( id.v ); found != type_of_.end() )
    {
        return found->second;
    }

    const Type& described = table_.get( id );

    // A local writer, as the parts' own entries go to types_ while this one is composed.
    Byte_writer entry;
    entry.put_u8( u8( described.kind ) );

    switch( described.kind )
    {
    case Type_kind::Error:
    case Type_kind::Void:
    case Type_kind::Never:
    case Type_kind::Bool:
        break;
    case Type_kind::Int:
        entry.put_u8( described.width );
        entry.put_u8( described.is_signed );
        break;
    case Type_kind::Float:
        entry.put_u8( described.width );
        break;
    case Type_kind::Pointer:
    case Type_kind::Many_pointer:
        type( entry, described.element );
        entry.put_u8( described.const_element );
        break;
    case Type_kind::Run:
        type( entry, described.element );
        break;
    case Type_kind::Array:
        type( entry, described.element );
        entry.put_u64( described.count );
        break;
    case Type_kind::Struct:
    case Type_kind::Enum:
    case Type_kind::Interface:
        // The open form first: the table takes a generic's first instance for it.
        if( const Type_id open = table_.declared( described.declaration ); open != id )
        {
            type_index( open );
        }
        node( entry, described.declaration );
        entry.put_u32( narrow_cast<u32>( described.arguments.size() ) );
        for( const Type_id argument : described.arguments )
        {
            type( entry, argument );
        }
        if( described.kind == Type_kind::Enum )
        {
            type( entry, described.element );
        }
        break;
    case Type_kind::Parameter:
        node( entry, described.declaration );
        break;
    case Type_kind::Function:
        type( entry, described.element );
        entry.put_u32( narrow_cast<u32>( described.arguments.size() ) );
        for( std::size_t i = 0; i < described.arguments.size(); ++i )
        {
            type( entry, described.arguments[i] );
            entry.put_u8( u8( described.modes[i] ) );
        }
        entry.put_u8( u8( described.return_mode ) );
        break;
    case Type_kind::Field:
        type( entry, described.arguments[0] );
        type( entry, described.element );
        break;
    case Type_kind::Union:
        entry.put_u32( narrow_cast<u32>( described.arguments.size() ) );
        for( const Type_id member : described.arguments )
        {
            type( entry, member );
        }
        break;
    }

    types_ += entry.take();

    const u32 index = type_count_++;
    type_of_.emplace( id.v, index );
    return index;
}

void Ref_writer::span( Byte_writer& out, Span span )
{
    if( !span.file.is_valid() )
    {
        out.put_u32( 0 );
        return;
    }

    out.put_u32( tree( span.file ) + 1 );
    out.put_u32( span.start );
    out.put_u32( span.end );
}

void Ref_writer::write_trees( Byte_writer& out ) const
{
    out.put_u32( narrow_cast<u32>( trees_.size() ) );
    for( const std::string& name : trees_ )
    {
        out.put_string( name );
    }
}

void Ref_writer::write_symbols( Byte_writer& out ) const
{
    out.put_u32( narrow_cast<u32>( symbols_.size() ) );
    for( const Symbol_id id : symbols_ )
    {
        out.put_string( interner_.text( id ) );
    }
}

void Ref_writer::write_types( Byte_writer& out ) const
{
    out.put_u32( type_count_ );
    out.put_bytes( types_ );
}

Ref_reader::Ref_reader(
    Byte_reader&          in,
    Program&              program,
    Interner&             interner,
    Literal_pool&         literals,
    Type_table&           table,
    const Source_manager& sm,
    File_id               file
)
    : in_( in ),
      program_( program ),
      interner_( interner ),
      literals_( literals ),
      table_( table ),
      sm_( sm ),
      file_( file )
{
}

bool Ref_reader::failed() const
{
    return failed_ || in_.failed();
}

void Ref_reader::fail()
{
    failed_ = true;
}

void Ref_reader::read_trees()
{
    std::unordered_map<std::string, File_id> loaded;
    for( const auto& [file, first] : program_.first_node )
    {
        if( file != file_.v )
        {
            loaded.emplace( module_name( program_.modules, sm_, interner_, File_id { file } ), File_id { file } );
        }
    }

    const u32 count = in_.get_u32();
    for( u32 i = 0; i < count && !failed(); ++i )
    {
        const std::string_view name = in_.get_string();

        if( i == 0 )
        {
            trees_.push_back( file_ );
            continue;
        }

        const auto found = loaded.find( std::string( name ) );
        if( found == loaded.end() )
        {
            fail();
            return;
        }

        trees_.push_back( found->second );
    }
}

void Ref_reader::read_symbols()
{
    const u32 count = in_.get_u32();
    for( u32 i = 0; i < count && !failed(); ++i )
    {
        symbols_.push_back( interner_.intern( in_.get_string() ) );
    }
}

void Ref_reader::read_types()
{
    const u32 count = in_.get_u32();
    for( u32 i = 0; i < count && !failed(); ++i )
    {
        const Type_kind kind = Type_kind( in_.get_u8() );

        const auto declaration = [&]
        {
            const Node_id id = node();
            if( !id.is_valid() )
            {
                fail();
            }
            return id;
        };

        const auto list = [&]
        {
            std::vector<Type_id> all;
            const u32            n = in_.get_u32();
            for( u32 k = 0; k < n && !failed(); ++k )
            {
                all.push_back( type() );
            }
            return all;
        };

        Type_id id {};

        switch( kind )
        {
        case Type_kind::Error:
        case Type_kind::Void:
        case Type_kind::Never:
        case Type_kind::Bool:
            id = table_.builtin( kind );
            break;
        case Type_kind::Int:
        {
            const u8   width     = in_.get_u8();
            const bool is_signed = in_.get_u8() != 0;
            if( width != 8 && width != 16 && width != 32 && width != 64 )
            {
                fail();
                break;
            }
            id = table_.integer( width, is_signed );
            break;
        }
        case Type_kind::Float:
        {
            const u8 width = in_.get_u8();
            if( width != 32 && width != 64 )
            {
                fail();
                break;
            }
            id = table_.floating( width );
            break;
        }
        case Type_kind::Pointer:
        case Type_kind::Many_pointer:
        {
            const Type_id element       = type();
            const bool    const_element = in_.get_u8() != 0;
            if( !failed() )
            {
                id = kind == Type_kind::Pointer ? table_.pointer_to( element, const_element )
                                                : table_.many_pointer_to( element, const_element );
            }
            break;
        }
        case Type_kind::Run:
        {
            const Type_id element = type();
            if( !failed() )
            {
                id = table_.run_of( element );
            }
            break;
        }
        case Type_kind::Array:
        {
            const Type_id element = type();
            const u64     n       = in_.get_u64();
            if( !failed() )
            {
                id = table_.array_of( element, n );
            }
            break;
        }
        case Type_kind::Struct:
        case Type_kind::Enum:
        case Type_kind::Interface:
        {
            const Node_id              decl       = declaration();
            const std::vector<Type_id> args       = list();
            const Type_id              underlying = kind == Type_kind::Enum ? type() : Type_id {};
            if( failed() )
            {
                break;
            }

            const std::string_view name = interner_.text( program_.ast.name( decl ) );
            id                          = kind == Type_kind::Struct ? table_.structure( decl, args, name )
                                          : kind == Type_kind::Enum ? table_.enumeration( decl, args, name, underlying )
                                                                    : table_.interface( decl, args, name );
            break;
        }
        case Type_kind::Parameter:
        {
            const Node_id decl = declaration();
            if( failed() )
            {
                break;
            }

            id = program_.ast.kind( decl ) == Node_kind::Interface_decl
                     ? table_.self_of( decl )
                     : table_.parameter( decl, interner_.text( program_.ast.name( decl ) ) );
            break;
        }
        case Type_kind::Function:
        {
            const Type_id          returned = type();
            std::vector<Parameter> parameters;
            const u32              n = in_.get_u32();
            for( u32 k = 0; k < n && !failed(); ++k )
            {
                const Type_id    parameter = type();
                const Param_mode mode      = Param_mode( in_.get_u8() );
                parameters.push_back( { parameter, mode } );
            }
            const Param_mode return_mode = Param_mode( in_.get_u8() );
            if( !failed() )
            {
                id = table_.function( returned, parameters, return_mode );
            }
            break;
        }
        case Type_kind::Field:
        {
            const Type_id aggregate = type();
            const Type_id member    = type();
            if( !failed() )
            {
                id = table_.field( aggregate, member );
            }
            break;
        }
        case Type_kind::Union:
        {
            const std::vector<Type_id> members = list();
            if( !failed() )
            {
                id = table_.error_union( members );
            }
            break;
        }
        default:
            fail();
            break;
        }

        types_.push_back( id );
    }
}

Node_id Ref_reader::node()
{
    const u32 tree = in_.get_u32();
    if( tree == 0 )
    {
        return Node_id {};
    }

    const u32 index = in_.get_u32();
    if( tree > trees_.size() )
    {
        fail();
        return Node_id {};
    }

    const File_id file  = trees_[tree - 1];
    const auto    first = program_.first_node.find( file.v );
    if( first == program_.first_node.end() )
    {
        fail();
        return Node_id {};
    }

    const Node_id id { first->second + index };
    if( id.v >= program_.ast.node_count() || program_.ast.span( id ).file != file )
    {
        fail();
        return Node_id {};
    }

    return id;
}

Node_id Ref_reader::local()
{
    const u32 index = in_.get_u32();
    if( index == 0 )
    {
        return Node_id {};
    }

    const Node_id id { program_.first_node.at( file_.v ) + index - 1 };
    if( id.v >= program_.ast.node_count() )
    {
        fail();
        return Node_id {};
    }

    return id;
}

Symbol_id Ref_reader::symbol()
{
    const u32 index = in_.get_u32();
    if( index == 0 )
    {
        return Symbol_id {};
    }

    if( index > symbols_.size() )
    {
        fail();
        return Symbol_id {};
    }

    return symbols_[index - 1];
}

Literal_id Ref_reader::literal()
{
    const u8 kind = in_.get_u8();

    switch( kind )
    {
    case 0:
        return Literal_id {};
    case u8( Literal_kind::Integer ) + 1:
        return literals_.add_integer( in_.get_u64() );
    case u8( Literal_kind::Float ) + 1:
        return literals_.add_float( in_.get_f64() );
    case u8( Literal_kind::String ) + 1:
        return literals_.add_string( std::string( in_.get_string() ) );
    default:
        fail();
        return Literal_id {};
    }
}

Type_id Ref_reader::type()
{
    const u32 index = in_.get_u32();
    if( index == 0 )
    {
        return Type_id {};
    }

    if( index > types_.size() || !types_[index - 1].is_valid() )
    {
        fail();
        return Type_id {};
    }

    return types_[index - 1];
}

Span Ref_reader::span()
{
    const u32 tree = in_.get_u32();
    if( tree == 0 )
    {
        return Span {};
    }

    const u32 start = in_.get_u32();
    const u32 end   = in_.get_u32();
    if( tree > trees_.size() || start > end || end > sm_.file( trees_[tree - 1] ).text.size() )
    {
        fail();
        return Span {};
    }

    return Span { trees_[tree - 1], start, end };
}

} // namespace keel
