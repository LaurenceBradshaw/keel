#include "ir/print.h"
#include <fmt/format.h>
#include "lex/token.h"

namespace keel
{

namespace
{

// Everything the printer needs to turn a handle back into a name, bundled so the recursive helpers
// below do not each carry five parameters.
struct Printer
{
    const Function&   func;
    const Ast&        ast;
    const Type_table& types;
    const Literals&   literals;
    const Interner&   interner;

    std::string_view name_of( Node_id declaration ) const
    {
        const Symbol_id name { ast.aux( declaration ) };

        return name.is_valid() ? interner.text( name ) : "<unnamed>";
    }

    std::string_view type_name( Type_id type ) const
    {
        return type.is_valid() ? types.name( type ) : "<none>";
    }

    // `_1`, `_1.x`, `(*_1)`, `(*_1).y` - read outwards, the way the source spells it.
    std::string place( const Place& place ) const
    {
        std::string text = place.is_global() ? std::string( name_of( place.global ) ) : fmt::format( "_{}", place.local.v );

        for( u32 i = 0; i < place.num_projections; ++i )
        {
            const Projection& projection = func.projections[place.first_projection + i];

            switch( projection.kind )
            {
            case Projection_kind::Deref:
                text = fmt::format( "(*{})", text );
                break;

            case Projection_kind::Field:
                text = fmt::format( "{}.{}", text, name_of( projection.field ) );
                break;
            }
        }

        return text;
    }

    std::string operand( const Operand& operand ) const
    {
        switch( operand.kind )
        {
        case Operand_kind::Copy:
            return fmt::format( "copy {}", place( operand.place ) );

        case Operand_kind::Move:
            return fmt::format( "move {}", place( operand.place ) );

        case Operand_kind::Constant:
            if( !operand.constant.is_valid() )
            {
                return "const <unset>";
            }

            // The operand's type decides which table the literal is in; a bool arrives here as an
            // integer 0 or 1.
            return types.is_float( operand.type ) ? fmt::format( "const {}", literals.floating( operand.constant ) )
                                                  : fmt::format( "const {}", literals.integer( operand.constant ) );
        }

        return "<bad operand>";
    }

    std::string rvalue( const Rvalue& value ) const
    {
        switch( value.kind )
        {
        case Rvalue_kind::Use:
            return operand( value.a );

        case Rvalue_kind::Binary:
            return fmt::format( "{} {} {}", operand( value.a ), token_kind_spelling( value.op ), operand( value.b ) );

        case Rvalue_kind::Unary:
            return fmt::format( "{}{}", token_kind_spelling( value.op ), operand( value.a ) );

        case Rvalue_kind::Cast:
            return fmt::format( "{} as {}", operand( value.a ), type_name( value.type ) );

        case Rvalue_kind::Address_of:
            return fmt::format( "&{}", place( value.a.place ) );

        case Rvalue_kind::Call:
        {
            std::string arguments;

            for( u32 i = 0; i < value.argument_count; ++i )
            {
                arguments += fmt::format( "{}{}", i == 0 ? "" : ", ", operand( func.operands[value.first_argument + i] ) );
            }

            return fmt::format( "call {}({})", name_of( value.callee ), arguments );
        }
        }

        return "<bad rvalue>";
    }

    std::string statement( const Statement& statement ) const
    {
        switch( statement.kind )
        {
        case Statement_kind::Assign:
            return fmt::format( "{} = {}", place( statement.place ), rvalue( statement.value ) );

        // `drop _1 if _7` when the local can have been moved out of, plain otherwise - so a dump of a
        // function that moves nothing looks exactly as it did before drop flags existed.
        case Statement_kind::Drop:
            return statement.drop_flag.is_valid()
                       ? fmt::format( "drop {} if _{}", place( statement.place ), statement.drop_flag.v )
                       : fmt::format( "drop {}", place( statement.place ) );

        case Statement_kind::Storage_live:
            return fmt::format( "storage_live {}", place( statement.place ) );

        case Statement_kind::Storage_dead:
            return fmt::format( "storage_dead {}", place( statement.place ) );
        }

        return "<bad statement>";
    }

    std::string terminator( const Terminator& terminator ) const
    {
        const auto target = []( Block_id block )
        { return block.is_valid() ? fmt::format( "bb{}", block.v ) : std::string( "<unset>" ); };

        switch( terminator.kind )
        {
        // Printed rather than skipped: a dump of a broken function is exactly when this is being
        // read, so it has to say what is wrong instead of showing an empty block.
        case Terminator_kind::Unset:
            return "<no terminator>";

        case Terminator_kind::Goto:
            return fmt::format( "goto -> {}", target( terminator.targets[0] ) );

        case Terminator_kind::Branch:
            return fmt::format(
                "branch {} -> {}, {}",
                operand( terminator.condition ),
                target( terminator.targets[0] ),
                target( terminator.targets[1] )
            );

        case Terminator_kind::Return:
            return "return";

        case Terminator_kind::Unreachable:
            return "unreachable";
        }

        return "<bad terminator>";
    }
};

} // namespace

std::string
print( const Function& func, const Ast& ast, const Type_table& types, const Literals& literals, const Interner& interner )
{
    const Printer printer { func, ast, types, literals, interner };

    // A destructor prints as `~Buffer`, the way it is written. Its aux holds the *type's* name, so
    // without the tilde it is indistinguishable from a free function of the same name - and unlike
    // name_of, which globals, fields and callees also use, this is the one place that matters.
    const bool destructor = func.declaration.is_valid() && ast.kind( func.declaration ) == Node_kind::Destructor_decl;

    std::string out = fmt::format(
        "fn {}{} {{\n", destructor ? "~" : "", func.declaration.is_valid() ? printer.name_of( func.declaration ) : "<unnamed>"
    );

    for( u32 i = 0; i < func.locals.size(); ++i )
    {
        const Local& local = func.locals[i];

        // The role is positional, so it is spelled out here rather than left to be counted.
        const std::string_view role = i == 0 ? " // return slot" : i <= func.parameter_count ? " // parameter" : "";

        out += fmt::format(
            "    let _{}: {};{}{}\n",
            i,
            printer.type_name( local.type ),
            role,
            local.name.is_valid() ? fmt::format( "{} {}", role.empty() ? " //" : "", interner.text( local.name ) ) : ""
        );
    }

    for( u32 i = 0; i < func.blocks.size(); ++i )
    {
        const Block& block = func.blocks[i];

        out += fmt::format( "\n    bb{}:\n", i );

        for( u32 j = 0; j < block.statement_count; ++j )
        {
            out += fmt::format( "        {}\n", printer.statement( func.statements[block.first_statement + j] ) );
        }

        out += fmt::format( "        {}\n", printer.terminator( block.terminator ) );
    }

    out += "}\n";

    return out;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <array>
#include "common/source_manager.h"
#include "ir/builder.h"
#include "ir/verify.h"
#include "lex/lexer.h"
#include "parse/parser.h"

namespace keel
{
namespace
{

// A real parse, so the Node_ids the printer resolves back to names are genuine ones. The source is
// only ever used for its declarations - nothing here lowers it.
struct Fixture
{
    Source_manager sm;
    Interner       interner;
    Literals       literals;
    Diagnostics    diags;
    Type_table     table;
    Ast            ast;

    explicit Fixture( std::string_view source )
    {
        const File_id file = sm.add_file( "t.kl", std::string( source ) );
        ast                = parse( lex( file, sm, interner, literals, diags ), sm, diags );
    }

    Node_id first( Node_kind kind ) const
    {
        for( u32 i = 0; i < ast.node_count(); ++i )
        {
            if( ast.kind( Node_id { i } ) == kind )
            {
                return Node_id { i };
            }
        }

        return Node_id {};
    }

    std::string text( const Function& func )
    {
        return print( func, ast, table, literals, interner );
    }
};

bool has( const std::string& text, std::string_view needle )
{
    return text.find( needle ) != std::string::npos;
}

} // namespace

TEST_CASE( "print_names_locals_by_role", "[ir][print]" )
{
    Fixture f( "i32 add( i32 a, i32 b ) { return a; }" );
    Builder builder( f.first( Node_kind::Function_decl ), f.table.integer( 32, true ), Span {} );

    builder.add_parameter( f.table.integer( 32, true ), Span {}, f.interner.intern( "a" ) );
    builder.add_local( f.table.integer( 32, true ), Span {} );
    builder.terminate_return( Span {} );

    const std::string text = f.text( builder.finish() );

    INFO( text );
    REQUIRE( has( text, "fn add {" ) );
    REQUIRE( has( text, "let _0: i32; // return slot" ) );
    REQUIRE( has( text, "let _1: i32; // parameter a" ) );
    REQUIRE( has( text, "let _2: i32;" ) ); // a temporary, with no role and no name
    REQUIRE( has( text, "return" ) );
}

TEST_CASE( "print_spells_places_the_way_the_source_does", "[ir][print]" )
{
    Fixture f( "struct Point { i32 x; };\ni32 main() { return 0; }" );
    Builder builder( f.first( Node_kind::Function_decl ), f.table.integer( 32, true ), Span {} );

    const Local_id local = builder.add_local( f.table.integer( 32, true ), Span {} );
    const Node_id  field = f.first( Node_kind::Field_decl );

    SECTION( "a field" )
    {
        builder.assign(
            builder.field( builder.place( local ), field ),
            use( copy( builder.place( local ), f.table.integer( 32, true ) ) ),
            Span {}
        );
        builder.terminate_return( Span {} );

        const std::string text = f.text( builder.finish() );

        INFO( text );
        REQUIRE( has( text, "_1.x = copy _1" ) );
    }

    // Names, not handles: a Node_id shifts whenever anything earlier in the file changes, which
    // would churn every golden on unrelated edits.
    SECTION( "a deref, then a field" )
    {
        builder.assign(
            builder.field( builder.deref( builder.place( local ) ), field ),
            use( copy( builder.place( local ), f.table.integer( 32, true ) ) ),
            Span {}
        );
        builder.terminate_return( Span {} );

        const std::string text = f.text( builder.finish() );

        INFO( text );
        REQUIRE( has( text, "(*_1).x =" ) );
    }
}

TEST_CASE( "print_spells_each_rvalue", "[ir][print]" )
{
    Fixture f( "i32 helper( i32 a ) { return a; }\ni32 main() { return 0; }" );

    const Type_id i32 = f.table.integer( 32, true );
    Builder       builder( f.first( Node_kind::Function_decl ), i32, Span {} );

    const Local_id local = builder.add_local( i32, Span {} );
    const Operand  two   = constant( f.literals.add_integer( 2 ), i32 );
    const Place    p     = builder.place( local );

    builder.assign( p, use( copy( p, i32 ) ), Span {} );
    builder.assign( p, use( move( p, i32 ) ), Span {} );
    builder.assign( p, use( two ), Span {} );
    builder.assign( p, binary( Token_kind::Plus, copy( p, i32 ), two, i32 ), Span {} );
    builder.assign( p, unary( Token_kind::Minus, copy( p, i32 ), i32 ), Span {} );
    builder.assign( p, cast_to( copy( p, i32 ), f.table.integer( 8, false ) ), Span {} );
    builder.assign( p, address_of( p, i32 ), Span {} );

    const u32 first = builder.add_operands( std::array { two, two } );
    builder.assign( p, call( f.first( Node_kind::Function_decl ), first, 2, i32 ), Span {} );

    builder.terminate_return( Span {} );

    const std::string text = f.text( builder.finish() );

    INFO( text );
    REQUIRE( has( text, "_1 = copy _1" ) );
    REQUIRE( has( text, "_1 = move _1" ) );
    REQUIRE( has( text, "_1 = const 2" ) );
    REQUIRE( has( text, "_1 = copy _1 + const 2" ) );
    REQUIRE( has( text, "_1 = -copy _1" ) );
    REQUIRE( has( text, "_1 = copy _1 as u8" ) );
    REQUIRE( has( text, "_1 = &_1" ) );
    REQUIRE( has( text, "_1 = call helper(const 2, const 2)" ) );
}

TEST_CASE( "print_spells_each_statement_and_terminator", "[ir][print]" )
{
    Fixture f( "i32 main() { return 0; }" );

    const Type_id i32 = f.table.integer( 32, true );
    Builder       builder( f.first( Node_kind::Function_decl ), i32, Span {} );

    const Local_id local  = builder.add_local( i32, Span {} );
    const Block_id second = builder.add_block();

    builder.storage_live( local, Span {} );
    builder.drop( builder.place( local ), Span {} );
    builder.storage_dead( local, Span {} );
    builder.terminate_goto( second, Span {} );

    builder.switch_to( second );
    builder.terminate_return( Span {} );

    const std::string text = f.text( builder.finish() );

    INFO( text );
    REQUIRE( has( text, "storage_live _1" ) );
    REQUIRE( has( text, "drop _1" ) );
    REQUIRE( has( text, "storage_dead _1" ) );
    REQUIRE( has( text, "goto -> bb1" ) );
    REQUIRE( has( text, "bb0:" ) );
    REQUIRE( has( text, "bb1:" ) );
}

TEST_CASE( "print_spells_a_branch", "[ir][print]" )
{
    Fixture f( "i32 main() { return 0; }" );

    const Type_id boolean = f.table.builtin( Type_kind::Bool );
    Builder       builder( f.first( Node_kind::Function_decl ), f.table.integer( 32, true ), Span {} );

    const Local_id condition = builder.add_local( boolean, Span {} );
    const Block_id yes       = builder.add_block();
    const Block_id no        = builder.add_block();

    builder.terminate_branch( copy( builder.place( condition ), boolean ), yes, no, Span {} );

    builder.switch_to( yes );
    builder.terminate_return( Span {} );

    builder.switch_to( no );
    builder.terminate_return( Span {} );

    const std::string text = f.text( builder.finish() );

    INFO( text );
    REQUIRE( has( text, "branch copy _1 -> bb1, bb2" ) );
}

// The dump is read precisely when something is wrong, so it has to survive a function that does
// not verify rather than assert or print an empty block.
TEST_CASE( "print_survives_a_malformed_function", "[ir][print]" )
{
    Fixture f( "i32 main() { return 0; }" );
    Builder builder( f.first( Node_kind::Function_decl ), f.table.integer( 32, true ), Span {} );

    const Function func = builder.finish(); // terminated by nobody

    const std::string text = f.text( func );

    INFO( text );
    REQUIRE( has( text, "<no terminator>" ) );
    REQUIRE_FALSE( verify( func ).empty() );
}

} // namespace keel
#endif
