#include "codegen_c/emit_kir.h"
#include <fmt/format.h>
#include "codegen_c/mangle.h"
#include "codegen_c/spelling.h"
#include "common/types.h"

namespace keel
{

namespace
{

class Kir_emitter
{
public:
    Kir_emitter(
        const std::vector<Function>& functions,
        const Ast&                   ast,
        const Types&                 types,
        const Literals&              literals,
        const Source_manager&        sm,
        const Interner&              interner
    );

    std::string run();

private:
    void write_line( std::string_view text );
    void line_directive( Span span );

    void emit_prologue();
    void emit_structs();
    void emit_enums();
    void emit_globals();
    void emit_prototypes();
    void emit_functions();
    void emit_main_shim();

    void emit_statement( const Statement& statement );
    void emit_terminator( const Terminator& terminator, const Function& function );
    void emit_function( const Function& function );

    std::string local_name( u32 index ) const;

    // A void local is never declared: C has no such object, and nothing reads one. Testing the
    // *kind* rather than is_valid() - a void local's Type_id is perfectly valid, it just names the
    // one type C cannot hold.
    bool is_void( Type_id type ) const;
    bool assigns_to_void( const Place& place ) const;
    // A place's type, walked the same way place() walks its text. Drop needs it to name the
    // destructor to call.
    Type_id     type_of( const Place& place ) const;
    std::string place( const Place& place ) const;
    std::string constant( Literal_id literal, Type_id type ) const;
    std::string operand( const Operand& operand ) const;
    std::string rvalue( const Rvalue& rvalue ) const;

    std::string prototype( Node_id decl ) const;

    std::string     out_;
    u32             indent_    = 0;
    u32             last_line_ = 0;       // so #line is emitted only when it changes
    const Function* current_   = nullptr; // The function currently being emitted.

    const std::vector<Function>& functions_;
    const Ast&                   ast_;
    const Types&                 types_;
    const Literals&              literals_;
    const Source_manager&        sm_;
    const Interner&              interner_;
    const Spelling               spelling_;
};

Kir_emitter::Kir_emitter(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    const Types&                 types,
    const Literals&              literals,
    const Source_manager&        sm,
    const Interner&              interner
)
    : functions_( functions ),
      ast_( ast ),
      types_( types ),
      literals_( literals ),
      sm_( sm ),
      interner_( interner ),
      spelling_( Spelling { ast, types, interner } )
{
}

std::string Kir_emitter::run()
{
    emit_prologue();
    emit_structs();
    emit_enums();
    emit_globals();
    emit_prototypes();
    emit_functions();
    emit_main_shim();

    return out_;
}

void Kir_emitter::write_line( std::string_view text )
{
    if( !text.empty() )
    {
        out_.append( indent_, ' ' );
        out_ += text;
    }

    out_ += '\n';
}

void Kir_emitter::line_directive( Span span )
{
    const Line_col loc = sm_.line_col( span.file, span.start );

    if( loc.line == last_line_ )
    {
        return; // only when it moves, or the directives outnumber the code
    }

    last_line_ = loc.line;

    // Column-zero syntax: never indented, whatever the surrounding block depth.
    out_ += fmt::format( "#line {} \"{}\"\n", loc.line, sm_.file( span.file ).path );
}

void Kir_emitter::emit_prologue()
{
    write_line( "#include <stdint.h>" );
    write_line( "#include <stdbool.h>" );
    write_line( "#include <stddef.h>" ); // NULL
    write_line( "" );
}

// D7. An enum that carries payloads is a struct in C: a tag saying which variant is live, then
// every payload field of every variant side by side.
//
// Side by side rather than in a union, which is what a tagged variant would normally be. The field
// names are already mangled with their node id, so two variants cannot collide, and the only cost
// is the space a union would have saved - invisible, because nothing guarantees an enum's layout
// and no FFI can see one. Moving to a union later is a change to this function and nothing else.
void Kir_emitter::emit_enums()
{
    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( decl ) != Node_kind::Enum_decl || !enum_has_payload( ast_, decl ) )
        {
            continue;
        }

        write_line( spelling_.structure( decl ) );
        write_line( "{" );
        indent_ += 4;

        write_line( fmt::format( "{} tag;", spelling_.type( types_.table().get( types_.type_of( decl ) ).element ) ) );

        for( const Node_id variant : ast_.children( decl ).subspan( 1 ) )
        {
            for( const Node_id field : ast_.children( variant ) )
            {
                write_line( fmt::format( "{} {};", spelling_.type( types_.type_of( field ) ), spelling_.field( field ) ) );
            }
        }

        indent_ -= 4;
        write_line( "};" );
        write_line( "" );
    }
}

void Kir_emitter::emit_structs()
{
    // Already in dependency order: the checker's cycle walk is a topological sort, and its
    // post-order is exactly what C needs for by-value members. Rebuilding the graph here would be
    // a second implementation of one rule.
    const std::vector<Node_id>& order = types_.struct_order();

    if( order.empty() )
    {
        return;
    }

    // §7.4's forward declarations. Nothing in v0 needs them - the definitions are already ordered
    // - but they cost a line each and become necessary the moment a struct holds a pointer to one.
    for( const Node_id decl : order )
    {
        write_line( fmt::format( "{};", spelling_.structure( decl ) ) );
    }

    write_line( "" );

    for( const Node_id decl : order )
    {
        write_line( spelling_.structure( decl ) );
        write_line( "{" );
        indent_ += 4;

        for( const Node_id field : ast_.children( decl ) )
        {
            if( ast_.kind( field ) != Node_kind::Field_decl )
            {
                continue;
            }

            write_line( fmt::format( "{} {};", spelling_.type( types_.type_of( field ) ), spelling_.field( field ) ) );
        }

        indent_ -= 4;
        write_line( "};" );
        write_line( "" );
    }
}

void Kir_emitter::emit_globals()
{
    bool any = false;

    for( const Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) != Node_kind::Var_decl )
        {
            continue;
        }

        write_line( spelling_.global_definition( child ) );

        any = true;
    }

    if( any )
    {
        write_line( "" );
    }
}

void Kir_emitter::emit_prototypes()
{
    // Every signature ahead of every body: C has no equivalent of D18, so this is what lets
    // `even` call `odd` from above it.
    for( const Function& function : functions_ )
    {

        write_line( prototype( function.declaration ) + ";" );
    }

    write_line( "" );
}

void Kir_emitter::emit_functions()
{
    for( const Function& function : functions_ )
    {
        emit_function( function );
        write_line( "" );
    }
}

void Kir_emitter::emit_main_shim()
{
    Node_id keel_main;

    for( const Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) != Node_kind::Function_decl )
        {
            continue; // aux is only a Symbol_id on a declaration
        }

        if( interner_.text( Symbol_id { ast_.aux( child ) } ) == "main" )
        {
            keel_main = child;
            break;
        }
    }

    // A program with no main is a library, not an error for the emitter to raise.
    if( !keel_main.is_valid() )
    {
        return;
    }

    // The symbol is derived from the node rather than hardcoded, so that a main with a different
    // signature calls the function that actually exists.
    const std::string symbol = spelling_.function( keel_main );

    write_line( "int main( void )" );
    write_line( "{" );
    indent_ += 4;
    write_line( fmt::format( "return (int) {}();", symbol ) );
    indent_ -= 4;
    write_line( "}" );
}

bool Kir_emitter::is_void( Type_id type ) const
{
    return type.is_valid() && types_.table().get( type ).kind == Type_kind::Void;
}

bool Kir_emitter::assigns_to_void( const Place& target ) const
{
    return !target.is_global() && target.num_projections == 0 && is_void( current_->locals[target.local.v].type );
}

void Kir_emitter::emit_statement( const Statement& statement )
{

    switch( statement.kind )
    {
    case Statement_kind::Assign:
    {
        line_directive( statement.span );
        const std::string value = rvalue( statement.value );

        // A void local is not declared, so there is nothing to assign into - the call is the whole
        // statement. Keeping Assign total in KIR is what confines this to one rule, in one place.
        if( assigns_to_void( statement.place ) )
        {
            write_line( fmt::format( "{};", value ) );
            return;
        }

        write_line( fmt::format( "{} = {};", place( statement.place ), value ) );
        return;
    }

    // By address, because a destructor takes the receiver as a pointer.
    case Statement_kind::Drop:
    {

        line_directive( statement.span );
        const std::string call =
            fmt::format( "{}( &{} );", spelling_.destructor_of( type_of( statement.place ) ), place( statement.place ) );

        // The flag is the whole of what a conditional drop costs in C.
        write_line(
            statement.drop_flag.is_valid() ? fmt::format( "if ( {} ) {}", local_name( statement.drop_flag.v ), call ) : call
        );
        return;
    }

    // The storage markers describe scopes for the drop pass rather than instructions - KIR has no
    // scoping, and every local is declared up front. Skipped before the line directive, which would
    // otherwise be left with nothing under it.
    case Statement_kind::Storage_live:
    case Statement_kind::Storage_dead:
        return;
    }
}

void Kir_emitter::emit_terminator( const Terminator& terminator, const Function& function )
{
    switch( terminator.kind )
    {
    case Terminator_kind::Goto:
        write_line( fmt::format( "goto bb{};", terminator.targets[0].v ) );
        return;

    case Terminator_kind::Branch:
        write_line( fmt::format(
            "if ( {} ) goto bb{}; else goto bb{};",
            operand( terminator.condition ),
            terminator.targets[0].v,
            terminator.targets[1].v
        ) );
        return;

    // The terminator carries no value: the lowerer put it in the return slot, which is local 0.
    case Terminator_kind::Return:
        if( is_void( types_.type_of( function.declaration ) ) )
        {
            write_line( "return;" );
            return;
        }

        write_line( fmt::format( "return {};", local_name( 0 ) ) );
        return;

    case Terminator_kind::Unreachable:
        assert( false && "the lowerer emits no unreachable terminator" );
        return;

    case Terminator_kind::Unset:
        assert( false && "block has no terminator; verify rejects this" );
        return;
    }
}

void Kir_emitter::emit_function( const Function& function )
{
    current_   = &function;
    last_line_ = 0;

    // Parameters named from KIR locals 1..parameter_count, NOT from Param_decl nodes: the body
    // names them by Local_id, and prototype() deliberately emits no names, so the two can never
    // disagree - nothing may name them from the Param_decl nodes, which is why parameter_types()
    // spells types only.
    std::string params;

    for( u32 i = 1; i <= function.parameter_count; ++i )
    {
        params += fmt::format( "{}{} {}", i == 1 ? "" : ", ", spelling_.type( function.locals[i].type ), local_name( i ) );
    }

    write_line( fmt::format(
        "{} {}( {} )",
        spelling_.return_type( function.declaration ),
        spelling_.function( function.declaration ), // the mangled symbol, matching the prototype
        params.empty() ? "void" : params
    ) );
    write_line( "{" );
    indent_ += 4;

    for( u32 i = 0; i < function.locals.size(); ++i )
    {
        // Parameters are declared by the signature already, and a void local cannot be declared at
        // all. Everything else, including the return slot, needs storage here - KIR has no scoping,
        // so there is one declaration per local at the top.
        if( i >= 1 && i <= function.parameter_count )
        {
            continue;
        }

        if( is_void( function.locals[i].type ) )
        {
            continue;
        }

        write_line( fmt::format( "{} {};", spelling_.type( function.locals[i].type ), local_name( i ) ) );
    }

    for( u32 i = 0; i < function.blocks.size(); ++i )
    {
        const Block& block = function.blocks[i];

        if( i != 0 )
        {
            write_line( fmt::format( "bb{}:", i ) );
        }

        for( u32 j = 0; j < block.statement_count; ++j )
        {
            const Statement& statement = function.statements[block.first_statement + j];
            emit_statement( statement );
        }

        emit_terminator( block.terminator, function );
    }
    indent_ -= 4;
    write_line( "}" );
}

std::string Kir_emitter::prototype( Node_id decl ) const
{
    const std::string params = spelling_.parameter_types( decl );

    return fmt::format(
        "{} {}( {} )",
        spelling_.return_type( decl ), // on the declaration, and a pointer when it returns a binding
        spelling_.function( decl ),
        params
    );
}

std::string Kir_emitter::local_name( u32 index ) const
{
    const Local& local = current_->locals[index];

    return local.name.is_valid() ? mangle_local( interner_.text( local.name ), index ) : fmt::format( "kl_t{}", index );
}

Type_id Kir_emitter::type_of( const Place& place ) const
{
    Type_id type = place.is_global() ? types_.type_of( place.global ) : current_->locals[place.local.v].type;

    for( u32 i = 0; i < place.num_projections; ++i )
    {
        const Projection& proj = current_->projections[place.first_projection + i];

        // Deref is the pointee, Field is the field's own recorded type, and a Tag is the enum's
        // underlying integer - which is also `element` for an enum, so the two share a branch.
        // Mirrors place()'s walk, and has to stay in step with it.
        type = proj.kind == Projection_kind::Field ? types_.type_of( proj.field ) : types_.table().get( type ).element;
    }

    return type;
}

std::string Kir_emitter::place( const Place& place ) const
{
    std::string text = place.is_global()
                           ? mangle_local( interner_.text( Symbol_id { ast_.aux( place.global ) } ), place.global.v )
                           : local_name( place.local.v );

    for( u32 i = 0; i < place.num_projections; ++i )
    {
        const Projection& proj = current_->projections[place.first_projection + i];

        switch( proj.kind )
        {
        case Projection_kind::Deref:
            text = fmt::format( "( *{} )", text );
            break;
        case Projection_kind::Field:
            text = fmt::format( "{}.{}", text, spelling_.field( proj.field ) );
            break;
        case Projection_kind::Tag:
            text = fmt::format( "{}.tag", text );
            break;
        }
    }

    return text;
}

std::string Kir_emitter::constant( Literal_id literal, Type_id type ) const
{
    switch( types_.table().get( type ).kind )
    {
    case Type_kind::Bool:
        return literals_.integer( literal ) != 0 ? "true" : "false";
    case Type_kind::Pointer:
        return "NULL";
    case Type_kind::Float:
        return c_float( literals_.floating( literal ) );
    case Type_kind::Int:
    case Type_kind::Enum: // a variant is its index; the C type is the underlying integer
        return c_integer( literals_.integer( literal ) );

    default:
        assert( false && "no C spelling for this type" );
        return "<unknown>";
    }
}

std::string Kir_emitter::operand( const Operand& operand ) const
{
    switch( operand.kind )
    {
    case Operand_kind::Constant:
        return constant( operand.constant, operand.type );
    case Operand_kind::Copy:
    case Operand_kind::Move:
        return place( operand.place );

    default:
        assert( false && "unknown operand kind" );
        return "<unknown>";
    }
}

std::string Kir_emitter::rvalue( const Rvalue& rvalue ) const
{
    switch( rvalue.kind )
    {
    case Rvalue_kind::Use:
        return operand( rvalue.a );

    case Rvalue_kind::Binary:
        return fmt::format( "{} {} {}", operand( rvalue.a ), token_kind_spelling( rvalue.op ), operand( rvalue.b ) );

    case Rvalue_kind::Unary:
        return fmt::format( "{}{}", token_kind_spelling( rvalue.op ), operand( rvalue.a ) );

    case Rvalue_kind::Cast:
        return fmt::format( "( {} ) {}", spelling_.type( rvalue.type ), operand( rvalue.a ) );

    case Rvalue_kind::Address_of:
        return fmt::format( "&{}", place( rvalue.a.place ) );

    case Rvalue_kind::Call:
    {
        std::string args;
        for( u32 i = 0; i < rvalue.argument_count; ++i )
        {
            if( i != 0 )
            {
                args += ", ";
            }
            args += operand( current_->operands[rvalue.first_argument + i] );
        }
        return fmt::format( "{}( {} )", spelling_.function( rvalue.callee ), args );
    }

    default:
        assert( false && "unknown rvalue kind" );
        return "<unknown>";
    }
}

} // namespace

std::string emit_c_from_kir(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    const Types&                 types,
    const Literals&              literals,
    const Source_manager&        sm,
    const Interner&              interner
)
{
    return Kir_emitter( functions, ast, types, literals, sm, interner ).run();
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "common/diagnostics.h"
#include "ir/lower.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/resolver.h"

namespace keel
{
namespace
{

// The whole pipeline, so what is emitted came from a real lowering rather than a hand-built
// Function. A backend bug that only shows on real input is the kind worth catching.
struct Generated
{
    Source_manager sm;
    Interner       interner;
    Literals       literals;
    Diagnostics    diags;
    Ast            ast;
    Resolution     resolution;
    Types          types;
    std::string    c;

    explicit Generated( std::string_view source )
    {
        const File_id file = sm.add_file( "t.kl", std::string( source ) );

        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
        resolution = resolve( ast, sm, interner, diags );
        types      = type_check( ast, resolution, literals, sm, interner, diags );

        if( !diags.has_errors() )
        {
            c = emit_c_from_kir( lower( ast, resolution, types, literals, interner ), ast, types, literals, sm, interner );
        }
    }

    bool clean() const
    {
        return !diags.has_errors();
    }

    bool has( std::string_view needle ) const
    {
        return c.find( needle ) != std::string::npos;
    }
};

} // namespace

TEST_CASE( "emit_kir_writes_a_function", "[codegen][kir]" )
{
    Generated g( "i32 main() { return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    // The definition's symbol matches the prototype's, because both come from Spelling::function.
    REQUIRE( g.has( "int32_t kl__main__( void );" ) );
    REQUIRE( g.has( "int32_t kl__main__( void )" ) );

    // The return slot is local 0, declared like any other local. The terminator carries no value.
    REQUIRE( g.has( "int32_t kl_t0;" ) );
    REQUIRE( g.has( "kl_t0 = 0;" ) );
    REQUIRE( g.has( "return kl_t0;" ) );
}

// The body names parameters by Local_id; the prototype names none at all. Naming them from the
// Param_decl nodes instead would declare one identifier and use another, and C would complain at
// the use rather than at the cause.
TEST_CASE( "emit_kir_names_parameters_from_kir_locals", "[codegen][kir]" )
{
    Generated g( "i32 add( i32 a, i32 b ) { return a + b; }\ni32 main() { return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    REQUIRE( g.has( "int32_t kl__add__i32_i32( int32_t, int32_t );" ) ); // the prototype, unnamed
    REQUIRE( g.has( "int32_t kl_a_1, int32_t kl_b_2" ) );                // the definition, named

    // Declared once, by the signature - not again as locals.
    REQUIRE_FALSE( g.has( "int32_t kl_a_1;" ) );
}

// Every block but the entry gets a label, and every edge is an explicit goto - so no label is left
// unused, which -Wall would make an error in the golden runner.
TEST_CASE( "emit_kir_writes_blocks_as_labels_and_gotos", "[codegen][kir]" )
{
    Generated g( "i32 f( i32 n ) { if ( n > 0 ) { return 1; } return 2; }\ni32 main() { return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    REQUIRE( g.has( "if ( " ) );
    REQUIRE( g.has( " ) goto bb" ) );
    REQUIRE( g.has( "else goto bb" ) );
    REQUIRE( g.has( "bb1:" ) );
    REQUIRE_FALSE( g.has( "bb0:" ) ); // the entry falls through from the opening brace
}

TEST_CASE( "emit_kir_writes_loops_as_back_edges", "[codegen][kir]" )
{
    Generated g( "i32 f( i32 n ) { i32 s = 0; while ( s < n ) { s = s + 1; } return s; }\ni32 main() { return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    // A back edge is a goto to a lower-numbered block. No labels are generated for the loop itself:
    // break and continue were already edges by the time this ran.
    REQUIRE( g.has( "goto bb1;" ) );
}

// The lowerer already inserted §6.4's conversions as Cast rvalues, so nothing here re-derives them
// and no operand carries a cast of its own.
TEST_CASE( "emit_kir_leaves_conversions_to_the_lowerer", "[codegen][kir]" )
{
    SECTION( "a widening operand is a cast statement, not a cast expression" )
    {
        Generated g( "i64 f( u8 a, i64 b ) { return a + b; }\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "= ( int64_t ) kl_a_1;" ) );
    }

    SECTION( "and operands already at one type get none" )
    {
        Generated g( "i32 f( i32 a, i32 b ) { return a + b; }\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE_FALSE( g.has( "( int32_t ) kl_a_1" ) );
    }
}

TEST_CASE( "emit_kir_writes_places", "[codegen][kir]" )
{
    SECTION( "a field through a pointer parenthesises the deref" )
    {
        Generated g( "struct N { i32 v; };\ni32 f( N* p ) { return (*p).v; }\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        // `.` binds tighter than unary `*`, so without the parentheses this means *( p.v ).
        REQUIRE( g.has( "( *kl_p_1 )." ) );
    }

    // A global is spelled from its declaration node, exactly as its definition is - which is what
    // makes the two backends agree on the symbol without coordinating.
    SECTION( "a global is spelled by its declaration" )
    {
        Generated g( "i32 counter = 1;\ni32 f() { return counter; }\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        const std::string name = "kl_counter_";

        REQUIRE( g.has( name + "2 = 1;" ) );    // the definition
        REQUIRE( g.has( "= " + name + "2;" ) ); // the read, same symbol
    }
}

// KIR keeps Assign total, so a call returning nothing still assigns - to a local C cannot declare.
// One rule here is the price of not having an optional destination in every consumer.
TEST_CASE( "emit_kir_drops_the_destination_of_a_void_call", "[codegen][kir]" )
{
    Generated g( "void nothing() { return; }\ni32 main() { nothing(); return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    // Two spaces: the argument list is empty and the format keeps its padding. The AST backend
    // spells it the same way, which is what matters - the two need to agree in behaviour, and here
    // they happen to agree in text as well.
    REQUIRE( g.has( "kl__nothing__(  );" ) );
    REQUIRE_FALSE( g.has( "void kl_t" ) ); // no void local is declared
    REQUIRE( g.has( "void kl__nothing__( void )" ) );
}

} // namespace keel
#endif
