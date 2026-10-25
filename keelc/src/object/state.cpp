// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "object/state.h"
#include <cassert>
#include <unordered_set>
#include "common/bytes.h"
#include "object/kir_state.h"
#include "object/refs.h"

// The state, in order: the source's path and text, the trees and symbols it names, the module's
// tree, the types, then the bodies' results and the KIR. Types come after the tree, as they name its
// declarations, and the reader names those declarations' types before interning any.

namespace keel
{
namespace
{

// A table of nodes with something to say, as a count then (local, value) pairs.
template <typename Each>
void write_table( Byte_writer& out, Ref_writer& refs, const std::vector<Node_id>& nodes, Each&& each )
{
    out.put_u32( narrow_cast<u32>( nodes.size() ) );
    for( const Node_id id : nodes )
    {
        refs.local( out, id );
        each( id );
    }
}

std::vector<Node_id> nodes_where( const Ref_writer& refs, auto&& keep )
{
    std::vector<Node_id> kept;
    for( u32 i = refs.first(); i < refs.end(); ++i )
    {
        if( keep( Node_id { i } ) )
        {
            kept.push_back( Node_id { i } );
        }
    }
    return kept;
}

void write_list( Byte_writer& out, Ref_writer& refs, std::span<const Type_id> types )
{
    out.put_u32( narrow_cast<u32>( types.size() ) );
    for( const Type_id type : types )
    {
        refs.type( out, type );
    }
}

std::vector<Type_id> read_list( Byte_reader& in, Ref_reader& refs )
{
    std::vector<Type_id> types;
    const u32            count = in.get_u32();
    for( u32 i = 0; i < count && !refs.failed(); ++i )
    {
        types.push_back( refs.type() );
    }
    return types;
}

// Reads a table write_table wrote, handing `each` the node.
template <typename Each>
void read_table( Byte_reader& in, Ref_reader& refs, Each&& each )
{
    const u32 count = in.get_u32();
    for( u32 i = 0; i < count && !refs.failed(); ++i )
    {
        const Node_id id = refs.local();
        if( !id.is_valid() )
        {
            refs.fail();
            return;
        }
        each( id );
    }
}

} // namespace

std::string write_state(
    const Program&            program,
    const Resolution&         resolution,
    const Types&              types,
    std::span<const Function> functions,
    File_id                   module,
    const Literal_pool&       literals,
    const Source_manager&     sm,
    const Interner&           interner
)
{
    const Ast& ast = program.ast;
    Ref_writer refs( program, types.table(), literals, sm, interner, module );

    Byte_writer tree;
    tree.put_u32( refs.end() - refs.first() );

    for( u32 i = refs.first(); i < refs.end(); ++i )
    {
        const Node_id id { i };
        const Span    span = ast.span( id );
        assert( span.file == module && "a module's nodes are all in its file" );

        tree.put_u32( u32( ast.kind( id ) ) );
        tree.put_u32( span.start );
        tree.put_u32( span.end );

        switch( aux_meaning( ast.kind( id ) ) )
        {
        case Aux_meaning::Symbol:
            refs.symbol( tree, Symbol_id { ast.aux( id ) } );
            break;
        case Aux_meaning::Literal:
            refs.literal( tree, Literal_id { ast.aux( id ) } );
            break;
        case Aux_meaning::Other:
            tree.put_u32( ast.aux( id ) );
            break;
        }

        const std::span<const Node_id> children = ast.children( id );
        tree.put_u32( narrow_cast<u32>( children.size() ) );
        for( const Node_id child : children )
        {
            assert( ( !child.is_valid() || child.v < id.v ) && "a child comes before its parent" );
            refs.local( tree, child );
        }
    }

    const auto put_span = [&]( Span span )
    {
        assert( span.file == module && "a side table's span is in the module's file" );
        tree.put_u32( span.start );
        tree.put_u32( span.end );
    };

    write_table(
        tree,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return ast.access( id ) != Access::Public; } ),
        [&]( Node_id id ) { tree.put_u8( u8( ast.access( id ) ) ); }
    );
    write_table( tree, refs, nodes_where( refs, [&]( Node_id id ) { return ast.is_located( id ); } ), []( Node_id ) {} );
    write_table(
        tree,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return ast.name_span( id ) != ast.span( id ); } ),
        [&]( Node_id id ) { put_span( ast.name_span( id ) ); }
    );
    write_table(
        tree,
        refs,
        nodes_where(
            refs,
            [&]( Node_id id ) { return ast.kind( id ) == Node_kind::Struct_literal && ast.type_name_span( id ).is_valid(); }
        ),
        [&]( Node_id id ) { put_span( ast.type_name_span( id ) ); }
    );
    write_table(
        tree,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return ast.doc_span( id ).is_valid(); } ),
        [&]( Node_id id ) { put_span( ast.doc_span( id ) ); }
    );

    std::vector<Node_id>    declarations;
    std::unordered_set<u32> top_level;
    for( const Node_id decl : ast.declarations( ast.root() ) )
    {
        if( ast.span( decl ).file == module )
        {
            declarations.push_back( decl );
            top_level.insert( decl.v );
        }
    }
    write_table( tree, refs, declarations, []( Node_id ) {} );

    // Each map is walked in node order, so the bytes depend only on the module.
    Byte_writer results;

    write_table(
        results,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return resolution.declaration_of( id ).is_valid(); } ),
        [&]( Node_id id ) { refs.node( results, resolution.declaration_of( id ) ); }
    );
    write_table(
        results,
        refs,
        nodes_where(
            refs, [&]( Node_id id ) { return !top_level.contains( id.v ) && resolution.next_overload( id ).is_valid(); }
        ),
        [&]( Node_id id ) { refs.node( results, resolution.next_overload( id ) ); }
    );
    write_table(
        results,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return types.type_of( id ).is_valid(); } ),
        [&]( Node_id id ) { refs.type( results, types.type_of( id ) ); }
    );
    write_table(
        results,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return types.constant_of( id ).has_value(); } ),
        [&]( Node_id id )
        {
            const Constant_value value = *types.constant_of( id );
            results.put_u8( u8( value.kind ) );
            results.put_u64( value.magnitude );
            results.put_u8( value.negative );
            results.put_f64( value.floating );
        }
    );
    write_table(
        results,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return types.callee_of( id ).is_valid(); } ),
        [&]( Node_id id ) { refs.node( results, types.callee_of( id ) ); }
    );
    write_table(
        results,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return types.through_of( id ).is_valid(); } ),
        [&]( Node_id id ) { refs.type( results, types.through_of( id ) ); }
    );
    write_table(
        results,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return types.listed().contains( id.v ); } ),
        [&]( Node_id id )
        {
            const std::vector<Node_id>& listed = types.listed_of( id );
            results.put_u32( narrow_cast<u32>( listed.size() ) );
            for( const Node_id function : listed )
            {
                refs.node( results, function );
            }
        }
    );
    write_table(
        results,
        refs,
        nodes_where( refs, [&]( Node_id id ) { return types.instantiation_of( id ).has_value(); } ),
        [&]( Node_id id )
        {
            const Instantiation& instance = types.instantiations()[*types.instantiation_of( id )];
            refs.node( results, instance.declaration );
            write_list( results, refs, instance.arguments );
        }
    );

    std::vector<const Generic_call*> calls;
    for( const Generic_call& call : types.generic_calls() )
    {
        if( call.from.v >= refs.first() && call.from.v < refs.end() )
        {
            calls.push_back( &call );
        }
    }
    results.put_u32( narrow_cast<u32>( calls.size() ) );
    for( const Generic_call* call : calls )
    {
        refs.local( results, call->from );
        refs.node( results, call->to );
        write_list( results, refs, call->arguments );
        refs.span( results, call->at );
    }

    results.put_u32( narrow_cast<u32>( functions.size() ) );
    for( const Function& function : functions )
    {
        write_function( results, refs, function );
    }

    Byte_writer out;
    out.put_string( sm.file( module ).path );
    out.put_string( sm.file( module ).text );
    refs.write_trees( out );
    refs.write_symbols( out );
    out.put_bytes( tree.take() );
    refs.write_types( out );
    out.put_bytes( results.take() );
    return out.take();
}

std::optional<Source_header> read_source( std::string_view state )
{
    Byte_reader            in( state );
    const std::string_view path = in.get_string();
    const std::string_view text = in.get_string();

    if( in.failed() )
    {
        return std::nullopt;
    }

    return Source_header { std::string( path ), std::string( text ) };
}

std::optional<Loaded_state> read_state(
    std::string_view      state,
    File_id               file,
    Program&              program,
    Interner&             interner,
    Literal_pool&         literals,
    Type_table&           table,
    const Source_manager& sm
)
{
    Byte_reader in( state );
    in.get_string();
    in.get_string();

    Ref_reader refs( in, program, interner, literals, table, sm, file );
    refs.read_trees();
    refs.read_symbols();

    Ast&      ast              = program.ast;
    const u32 size             = narrow_cast<u32>( sm.file( file ).text.size() );
    program.first_node[file.v] = narrow_cast<u32>( ast.node_count() );

    const u32 count = in.get_u32();
    for( u32 k = 0; k < count && !refs.failed(); ++k )
    {
        const u32 kind  = in.get_u32();
        const u32 start = in.get_u32();
        const u32 end   = in.get_u32();

        if( kind >= u32( Node_kind::Count ) || start > end || end > size )
        {
            return std::nullopt;
        }

        u32 aux = 0;
        switch( aux_meaning( Node_kind( kind ) ) )
        {
        case Aux_meaning::Symbol:
            aux = refs.symbol().v;
            break;
        case Aux_meaning::Literal:
            aux = refs.literal().v;
            break;
        case Aux_meaning::Other:
            aux = in.get_u32();
            break;
        }

        const u32            child_count = in.get_u32();
        std::vector<Node_id> children;
        for( u32 c = 0; c < child_count && !refs.failed(); ++c )
        {
            const Node_id child = refs.local();
            if( child.is_valid() && child.v >= ast.node_count() )
            {
                return std::nullopt;
            }
            children.push_back( child );
        }

        ast.add( Node_kind( kind ), Span { file, start, end }, aux, children );
    }

    const auto get_span = [&]
    {
        const u32 start = in.get_u32();
        const u32 end   = in.get_u32();
        return start <= end && end <= size ? Span { file, start, end } : Span {};
    };

    read_table( in, refs, [&]( Node_id id ) { ast.set_access( id, Access( in.get_u8() ) ); } );
    read_table( in, refs, [&]( Node_id id ) { ast.set_located( id ); } );
    read_table( in, refs, [&]( Node_id id ) { ast.set_name_span( id, get_span() ); } );
    read_table( in, refs, [&]( Node_id id ) { ast.set_type_name_span( id, get_span() ); } );
    read_table( in, refs, [&]( Node_id id ) { ast.set_doc_span( id, get_span() ); } );

    Loaded_state loaded;
    loaded.file = file;
    read_table( in, refs, [&]( Node_id id ) { loaded.declarations.push_back( id ); } );

    if( refs.failed() )
    {
        return std::nullopt;
    }

    name_declarations( table, ast, loaded.declarations, program.modules, sm, interner );
    refs.read_types();

    read_table( in, refs, [&]( Node_id id ) { loaded.bindings.emplace_back( id, refs.node() ); } );
    read_table( in, refs, [&]( Node_id id ) { loaded.overloads.emplace_back( id, refs.node() ); } );
    read_table( in, refs, [&]( Node_id id ) { loaded.types.emplace_back( id, refs.type() ); } );
    read_table(
        in,
        refs,
        [&]( Node_id id )
        {
            Constant_value value;
            const u8       kind = in.get_u8();
            if( kind > u8( Constant_value::Kind::Float ) )
            {
                refs.fail();
            }
            value.kind      = Constant_value::Kind( kind );
            value.magnitude = in.get_u64();
            value.negative  = in.get_u8() != 0;
            value.floating  = in.get_f64();
            loaded.constants.emplace_back( id, value );
        }
    );
    read_table( in, refs, [&]( Node_id id ) { loaded.callees.emplace_back( id, refs.node() ); } );
    read_table( in, refs, [&]( Node_id id ) { loaded.through.emplace_back( id, refs.type() ); } );
    read_table(
        in,
        refs,
        [&]( Node_id id )
        {
            std::vector<Node_id> listed;
            const u32            n = in.get_u32();
            for( u32 i = 0; i < n && !refs.failed(); ++i )
            {
                listed.push_back( refs.node() );
            }
            loaded.listed.emplace_back( id, std::move( listed ) );
        }
    );
    read_table(
        in,
        refs,
        [&]( Node_id id )
        {
            Instantiation instance;
            instance.declaration = refs.node();
            instance.arguments   = read_list( in, refs );
            loaded.instantiations.emplace_back( id, std::move( instance ) );
        }
    );

    const u32 calls = in.get_u32();
    for( u32 i = 0; i < calls && !refs.failed(); ++i )
    {
        Generic_call call;
        call.from      = refs.local();
        call.to        = refs.node();
        call.arguments = read_list( in, refs );
        call.at        = refs.span();
        loaded.generic_calls.push_back( std::move( call ) );
    }

    const u32 functions = in.get_u32();
    for( u32 i = 0; i < functions && !refs.failed(); ++i )
    {
        loaded.functions.push_back( read_function( in, refs ) );
    }

    if( refs.failed() )
    {
        return std::nullopt;
    }

    return loaded;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <fmt/format.h>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include "codegen_c/parts.h"
#include "common/temp_dir.h"
#include "front_end.h"
#include "ir/print.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr std::string_view k_shelf = R"(/// Holds one `T`, counted.
struct shelf<T> where T : copyable
{
    T   first;
    i32 count;

    i32 size() const { return count; }
    void put( T v ) { first = v; count += 1; }
    void put( T v, i32 n ) { first = v; count += n; }
};

enum Colour
{
    Red,
    Wide( f64 width )
};

i32 limit = 4 * 8;

T pick<T>( T a, T _ ) where T : copyable
{
    return a;
}

T pick_first<T>( T a ) where T : copyable
{
    return pick<T>( a, a );
}

i32 twice( i32 x ) { return x * 2; }
f64 twice( f64 x ) { return x * 2.0; }

private i32 hidden( i32 x ) { return x; }

i32 helper( i32 x )
{
    shelf<i32> s = shelf { x, 0 };
    s.put( x );
    s.put( x, 2 );
    _ = Colour::Wide( twice( 1.5 ) );
    return twice( x ) + pick_first( x ) + s.size() + limit + hidden( 1 );
}
)";

// A compile of its own, so equal ids between two of them mean nothing.
struct Compile
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;

    std::string rendered() const
    {
        std::ostringstream out;
        diags.render( sm, out );
        return out.str();
    }
};

std::string aux_text( const Compile& c, const Ast& ast, Node_id id )
{
    switch( aux_meaning( ast.kind( id ) ) )
    {
    case Aux_meaning::Symbol:
        return Symbol_id { ast.aux( id ) }.is_valid() ? std::string( c.interner.text( Symbol_id { ast.aux( id ) } ) ) : "";
    case Aux_meaning::Literal:
    {
        const Literal_id literal = ast.literal( id );
        switch( c.literals.kind( literal ) )
        {
        case Literal_kind::Integer:
            return std::to_string( c.literals.integer( literal ) );
        case Literal_kind::Float:
            return std::to_string( c.literals.floating( literal ) );
        case Literal_kind::String:
            return std::string( c.literals.string( literal ) );
        }
        return "";
    }
    case Aux_meaning::Other:
        return std::to_string( ast.aux( id ) );
    }
    return "";
}

std::string name_of( const Compile& c, const Ast& ast, Node_id decl )
{
    return decl.is_valid() && aux_meaning( ast.kind( decl ) ) == Aux_meaning::Symbol && ast.name( decl ).is_valid()
               ? fmt::format( "{} {}", node_kind_name( ast.kind( decl ) ), c.interner.text( ast.name( decl ) ) )
               : std::string( decl.is_valid() ? node_kind_name( ast.kind( decl ) ) : "none" );
}

// `kl::shelf` built in a program that imports it, and its state.
struct Built
{
    Front_end   front;
    File_id     shelf {};
    std::string state;

    // The part's functions, as the state holds them, printed in the compile that wrote them.
    std::vector<std::string> printed;
};

Built build_shelf( Compile& c, const Temp_dir& dir )
{
    dir.write( "kl/shelf.kl", k_shelf );
    const std::filesystem::path main_path =
        dir.write( "main.kl", "import kl::shelf;\ni32 main() { return kl::helper( 1 ); }\n" );
    const File_id main = *c.sm.load_file( main_path );
    const Package kl { "kl", dir.path / "kl" };

    Built built;
    built.front = run_front_end(
        main, Stage::Elaborated, { .keep_checked = true }, c.sm, c.interner, c.literals, c.diags, std::span( &kl, 1 )
    );
    INFO( c.rendered() );
    REQUIRE( c.rendered().empty() );

    for( const auto& [file, node] : built.front.program.first_node )
    {
        if( c.sm.file( File_id { file } ).path.ends_with( "shelf.kl" ) )
        {
            built.shelf = File_id { file };
        }
    }
    REQUIRE( built.shelf.is_valid() );

    Front_end&  front = built.front;
    const Parts parts( front.program.ast, front.types, front.program.modules, main, front.functions );

    std::vector<Function> part;
    for( std::size_t f = 0; f < front.functions.size(); ++f )
    {
        if( parts.of( front.functions[f] ) == built.shelf )
        {
            part.push_back( front.checked[f] );
            built.printed.push_back( print( front.checked[f], front.program.ast, front.types.table(), c.literals, c.interner )
            );
        }
    }

    built.state = write_state( front.program, front.resolution, front.types, part, built.shelf, c.literals, c.sm, c.interner );
    return built;
}

} // namespace

TEST_CASE( "state_round_trips_a_module_through_another_compile", "[object][state]" )
{
    Temp_dir           dir;
    Compile            first;
    const Built        built = build_shelf( first, dir );
    const Front_end&   front = built.front;
    const File_id      shelf = built.shelf;
    const std::string& state = built.state;

    Compile                     second;
    const std::filesystem::path empty = dir.write( "empty/main.kl", "" );
    Program program = load_program( *second.sm.load_file( empty ), second.sm, second.interner, second.literals, second.diags );
    const Resolution resolution = resolve( program.ast, second.sm, second.interner, second.diags, program.modules );
    Types            types = type_check( program.ast, resolution, second.literals, second.sm, second.interner, second.diags );
    REQUIRE( !second.diags.has_errors() );

    const std::optional<Source_header> source = read_source( state );
    REQUIRE( source );
    REQUIRE( source->text == k_shelf );

    const File_id file = second.sm.add_file( source->path, source->text );
    program.modules.add_package( second.interner.intern( "kl" ) );
    program.modules.place( file, second.interner.intern( "kl" ) );

    const std::optional<Loaded_state> loaded =
        read_state( state, file, program, second.interner, second.literals, types.table(), second.sm );
    REQUIRE( loaded );

    const Ast& a     = front.program.ast;
    const Ast& b     = program.ast;
    const u32  from  = front.program.first_node.at( shelf.v );
    const u32  to    = program.first_node.at( file.v );
    const u32  count = narrow_cast<u32>( b.node_count() ) - to;
    REQUIRE( count > 0 );

    std::unordered_map<u32, Type_id> types_b;
    for( const auto& [node, type] : loaded->types )
    {
        types_b[node.v] = type;
    }
    std::unordered_map<u32, Node_id> bindings_b;
    for( const auto& [use, decl] : loaded->bindings )
    {
        bindings_b[use.v] = decl;
    }
    std::unordered_map<u32, Node_id> callees_b;
    for( const auto& [call, callee] : loaded->callees )
    {
        callees_b[call.v] = callee;
    }

    for( u32 i = 0; i < count; ++i )
    {
        const Node_id x { from + i };
        const Node_id y { to + i };
        INFO( node_kind_name( a.kind( x ) ) << " at " << a.span( x ).start );

        REQUIRE( a.kind( x ) == b.kind( y ) );
        REQUIRE( a.span( x ).start == b.span( y ).start );
        REQUIRE( a.span( x ).end == b.span( y ).end );
        REQUIRE( aux_text( first, a, x ) == aux_text( second, b, y ) );
        REQUIRE( a.children( x ).size() == b.children( y ).size() );
        REQUIRE( a.access( x ) == b.access( y ) );
        REQUIRE( a.doc_span( x ).is_valid() == b.doc_span( y ).is_valid() );

        const Type_id type_a = front.types.type_of( x );
        const Type_id type_b = types_b.contains( y.v ) ? types_b.at( y.v ) : Type_id {};
        REQUIRE( type_a.is_valid() == type_b.is_valid() );
        if( type_a.is_valid() )
        {
            REQUIRE( front.types.table().name( type_a ) == types.table().name( type_b ) );
        }

        const Node_id bound = bindings_b.contains( y.v ) ? bindings_b.at( y.v ) : Node_id {};
        REQUIRE( name_of( first, a, front.resolution.declaration_of( x ) ) == name_of( second, b, bound ) );

        const Node_id callee = callees_b.contains( y.v ) ? callees_b.at( y.v ) : Node_id {};
        REQUIRE( name_of( first, a, front.types.callee_of( x ) ) == name_of( second, b, callee ) );
    }

    // `pick_first<T>` names `pick<T>`, in its own parameter.
    const auto call = std::ranges::find_if(
        loaded->generic_calls,
        [&]( const Generic_call& c ) { return name_of( second, b, c.from ) == "Function_decl pick_first"; }
    );
    REQUIRE( call != loaded->generic_calls.end() );
    REQUIRE( name_of( second, b, call->to ) == "Function_decl pick" );
    REQUIRE( call->arguments.size() == 1 );
    REQUIRE( types.table().name( call->arguments[0] ) == "T" );
    REQUIRE( second.sm.text( call->at ) == "pick<T>( a, a )" );

    // `pick_first( x )` instantiates at i32.
    bool instantiated = false;
    for( const auto& [at, instance] : loaded->instantiations )
    {
        if( name_of( second, b, instance.declaration ) == "Function_decl pick_first" )
        {
            REQUIRE( instance.arguments.size() == 1 );
            REQUIRE( types.table().name( instance.arguments[0] ) == "i32" );
            instantiated = true;
        }
    }
    REQUIRE( instantiated );

    // The member overloads chain as they did, and `limit` folded.
    REQUIRE( !loaded->overloads.empty() );
    const auto limit = std::ranges::find_if(
        loaded->constants, [&]( const auto& entry ) { return name_of( second, b, entry.first ) == "Var_decl limit"; }
    );
    REQUIRE( limit != loaded->constants.end() );
    REQUIRE( limit->second.magnitude == 32 );

    // The module's own types read as its: `kl::shelf<i32>`, not `shelf<i32>`.
    bool named = false;
    for( const auto& [node, type] : loaded->types )
    {
        named = named || types.table().name( type ) == "kl::shelf<i32>";
    }
    REQUIRE( named );

    // The part's KIR reads as it did, from the same declarations and types.
    REQUIRE( loaded->functions.size() == built.printed.size() );
    REQUIRE( !loaded->functions.empty() );
    for( std::size_t f = 0; f < loaded->functions.size(); ++f )
    {
        const Function& function = loaded->functions[f];
        REQUIRE( print( function, b, types.table(), second.literals, second.interner ) == built.printed[f] );
        REQUIRE( verify( function ).empty() );
    }
}

TEST_CASE( "state_writes_the_same_bytes_twice", "[object][state]" )
{
    Temp_dir dir;
    Compile  a;
    Compile  b;
    REQUIRE( build_shelf( a, dir ).state == build_shelf( b, dir ).state );
}

TEST_CASE( "state_refuses_a_cut_state", "[object][state]" )
{
    Temp_dir    dir;
    Compile     first;
    std::string state = build_shelf( first, dir ).state;

    Compile                     second;
    const std::filesystem::path empty = dir.write( "empty/main.kl", "" );
    Program program = load_program( *second.sm.load_file( empty ), second.sm, second.interner, second.literals, second.diags );
    Type_table table;

    const std::optional<Source_header> source = read_source( state );
    REQUIRE( source );
    const File_id file = second.sm.add_file( source->path, source->text );
    program.modules.place( file, second.interner.intern( "kl" ) );

    state.resize( state.size() - 1 );
    REQUIRE_FALSE( read_state( state, file, program, second.interner, second.literals, table, second.sm ) );
}

} // namespace keel
#endif
