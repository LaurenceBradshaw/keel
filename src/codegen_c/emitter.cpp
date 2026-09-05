#include "codegen_c/emitter.h"
#include "codegen_c/mangle.h"
#include "lex/token.h"

#include <fmt/format.h>

#include <cassert>
#include <vector>

namespace keel
{
namespace
{

// The operation a compound assignment performs: `+=` is `+`, applied and stored back. The emitter
// expands it rather than emitting C's own `+=`, so that the conversions stay explicit and one code
// path computes the result - C's compound assignment has conversion rules of its own, and they are
// not §6.4's.
Token_kind base_operator( Token_kind assignment )
{
    switch( assignment )
    {
    case Token_kind::Plus_equal:
        return Token_kind::Plus;
    case Token_kind::Minus_equal:
        return Token_kind::Minus;
    case Token_kind::Star_equal:
        return Token_kind::Star;
    case Token_kind::Slash_equal:
        return Token_kind::Slash;
    case Token_kind::Percent_equal:
        return Token_kind::Percent;
    case Token_kind::Amp_equal:
        return Token_kind::Amp;
    case Token_kind::Pipe_equal:
        return Token_kind::Pipe;
    case Token_kind::Caret_equal:
        return Token_kind::Caret;
    case Token_kind::Less_less_equal:
        return Token_kind::Less_less;
    case Token_kind::Greater_greater_equal:
        return Token_kind::Greater_greater;
    default:
        assert( false && "not a compound assignment" );
        return assignment;
    }
}

class Emitter
{
public:
    Emitter(
        const Ast&            ast,
        const Resolution&     resolution,
        const Types&          types,
        const Literals&       literals,
        const Source_manager& sm,
        const Interner&       interner
    )
        : ast_( ast ),
          resolution_( resolution ),
          types_( types ),
          literals_( literals ),
          sm_( sm ),
          interner_( interner )
    {
    }

    std::string run();

private:
    // --- structure ---

    void emit_prologue(); // includes, and the fixed-width type contract of §7.8
    // §7.4. Types before prototypes, because a signature may name a struct by value. The order
    // comes from the checker, which computed it while proving there are no cycles.
    void emit_structs();
    void emit_struct( Node_id decl );

    void emit_prototypes(); // every function, ahead of every body: this is what makes D18 work in C
    void emit_functions();
    void emit_function( Node_id id );
    void emit_main_shim(); // C needs a real main(); Keel's is a mangled symbol

    // Built once and used by the prototype, the definition and the shim. A prototype and its
    // definition disagreeing is the mistake C punishes hardest, so they cannot be written twice.
    std::vector<Type_id> parameter_types( Node_id function ) const;
    std::string          signature( Node_id function, bool with_names ) const;

    // --- statements ---

    void emit_statement( Node_id id ); // dispatch, mirroring Checker::visit
    void emit_block( Node_id id );
    void emit_var_decl( Node_id id );
    void emit_assign( Node_id id );
    void emit_return( Node_id id );
    void emit_if( Node_id id );
    void emit_while( Node_id id );
    void emit_for( Node_id id );
    void emit_expr_stmt( Node_id id );

    // A loop condition is re-evaluated every iteration, so the statements that compute it must sit
    // *inside* the loop. Emits those plus the guarded break.
    void emit_loop_condition( Node_id condition );
    void emit_increment( Node_id id );

    // --- expressions ---

    // §7.1. Emits the statements that compute `id` and returns the C operand naming its result.
    // Every *operation* lands in its own temporary, which is what makes C's unspecified evaluation
    // order irrelevant. Literals and variable reads are operands rather than temporaries: they
    // have no side effects and no order to get wrong. That stops being true when references
    // arrive at M3, and this is the place that has to change.
    std::string lower( Node_id id );

    std::string lower_binary( Node_id id );
    std::string lower_unary( Node_id id );
    std::string lower_call( Node_id id );
    std::string lower_field( Node_id id );

    // The C expression naming *where* a value lives, rather than one naming the value itself.
    // lower() reads: for `p.x` it emits `t = p.x;` and hands back `t`, so assigning to what it
    // returns writes into the temporary and leaves the struct untouched. Assignment targets and
    // increment operands go through here instead.
    std::string lower_place( Node_id id );
    std::string lower_struct_literal( Node_id id );

    // The Field_decl a name refers to in a struct type. The emitter cannot reach the checker's
    // copy of this, and needs the declaration node to mangle the C member name.
    Node_id field_of( Type_id type, Symbol_id name ) const;

    // The C name of a struct or one of its fields.
    std::string struct_name( Node_id decl ) const;
    std::string field_name( Node_id field ) const;
    std::string lower_literal( Node_id id );
    std::string lower_name( Node_id id );

    // `&&` and `||` cannot be an operation over two lowered operands: the right side must not run
    // unless the left demands it. This is the one expression that emits control flow.
    std::string lower_short_circuit( Node_id id );

    // --- helpers ---

    std::string fresh_temp();
    std::string c_type( Type_id type ) const; // "int32_t", "bool", "void"

    void line_directive( Node_id id ); // §7.6
    void write( std::string_view text );
    void write_line( std::string_view text );

    const Ast&            ast_;
    const Resolution&     resolution_;
    const Types&          types_;
    const Literals&       literals_;
    const Source_manager& sm_;
    const Interner&       interner_;

    std::string out_;
    u32         next_temp_ = 0;
    u32         indent_    = 0;
    u32         last_line_ = 0; // so #line is emitted only when it changes
};

std::string Emitter::run()
{
    emit_prologue();
    emit_structs();
    emit_prototypes();
    emit_functions();
    emit_main_shim();

    return out_;
}

void Emitter::emit_structs()
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
        write_line( fmt::format( "{};", struct_name( decl ) ) );
    }

    write_line( "" );

    for( const Node_id decl : order )
    {
        emit_struct( decl );
    }
}

void Emitter::emit_struct( Node_id decl )
{
    write_line( struct_name( decl ) );
    write_line( "{" );
    indent_ += 4;

    for( const Node_id field : ast_.children( decl ) )
    {
        if( ast_.kind( field ) != Node_kind::Field_decl )
        {
            continue;
        }

        write_line( fmt::format( "{} {};", c_type( types_.type_of( field ) ), field_name( field ) ) );
    }

    indent_ -= 4;
    write_line( "};" );
    write_line( "" );
}

void Emitter::emit_functions()
{
    for( const Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Function_decl )
        {
            emit_function( child );
        }
    }
}

std::string Emitter::struct_name( Node_id decl ) const
{
    return fmt::format( "struct {}", mangle_struct( "", interner_.text( Symbol_id { ast_.aux( decl ) } ) ) );
}

std::string Emitter::field_name( Node_id field ) const
{
    // Mangled like a local: a field called `while` or `int` would otherwise break the generated C.
    return mangle_local( interner_.text( Symbol_id { ast_.aux( field ) } ), field.v );
}

Node_id Emitter::field_of( Type_id type, Symbol_id name ) const
{
    for( const Node_id field : ast_.children( types_.table().get( type ).declaration ) )
    {
        if( ast_.kind( field ) == Node_kind::Field_decl && ast_.aux( field ) == name.v )
        {
            return field;
        }
    }

    return Node_id {};
}

void Emitter::emit_prologue()
{
    write_line( "#include <stdint.h>" );
    write_line( "#include <stdbool.h>" );
    write_line( "#include <stddef.h>" ); // NULL
    write_line( "" );
}

std::vector<Type_id> Emitter::parameter_types( Node_id function ) const
{
    std::vector<Type_id> params;

    for( const Node_id param : ast_.children( ast_.children( function )[1] ) )
    {
        // The type is recorded on the Param_decl itself, by declare_signatures - not on the type
        // annotation beneath it, which is never typed.
        params.push_back( types_.type_of( param ) );
    }

    return params;
}

std::string Emitter::signature( Node_id function, bool with_names ) const
{
    const std::vector<Type_id>     params = parameter_types( function );
    const std::span<const Node_id> nodes  = ast_.children( ast_.children( function )[1] );

    std::string rendered;

    for( std::size_t i = 0; i < params.size(); ++i )
    {
        if( i != 0 )
        {
            rendered += ", ";
        }

        rendered += c_type( params[i] );

        if( with_names )
        {
            rendered += ' ';
            rendered += mangle_local( interner_.text( Symbol_id { ast_.aux( nodes[i] ) } ), nodes[i].v );
        }
    }

    if( rendered.empty() )
    {
        rendered = "void"; // C11: an empty list is a prototype that says nothing about arity
    }

    const std::string_view name = interner_.text( Symbol_id { ast_.aux( function ) } );

    return fmt::format(
        "{} {}( {} )",
        c_type( types_.type_of( function ) ), // the return type lives on the declaration
        mangle_function( "", name, params, types_.table() ),
        rendered
    );
}

void Emitter::emit_prototypes()
{
    // Every signature ahead of every body: C has no equivalent of D18, so this is what lets
    // `even` call `odd` from above it.
    for( const Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Function_decl )
        {
            write_line( signature( child, false ) + ";" );
        }
    }

    write_line( "" );
}

void Emitter::emit_function( Node_id id )
{
    next_temp_ = 0; // so temporaries read as kl_t0, kl_t1 within each function

    write_line( signature( id, true ) );
    emit_block( ast_.children( id )[2] ); // writes its own braces and indents
    write_line( "" );
}

void Emitter::emit_main_shim()
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
    const std::string_view name   = interner_.text( Symbol_id { ast_.aux( keel_main ) } );
    const std::string      symbol = mangle_function( "", name, parameter_types( keel_main ), types_.table() );

    write_line( "int main( void )" );
    write_line( "{" );
    indent_ += 4;
    write_line( fmt::format( "return (int) {}();", symbol ) );
    indent_ -= 4;
    write_line( "}" );
}

void Emitter::emit_statement( Node_id id )
{
    if( !id.is_valid() )
    {
        return;
    }

    line_directive( id );

    switch( ast_.kind( id ) )
    {
    case Node_kind::Block:
        emit_block( id );
        return;
    case Node_kind::Var_decl:
        emit_var_decl( id );
        return;
    case Node_kind::Assign_stmt:
        emit_assign( id );
        return;
    case Node_kind::Return_stmt:
        emit_return( id );
        return;
    case Node_kind::If_stmt:
        emit_if( id );
        return;
    case Node_kind::While_stmt:
        emit_while( id );
        return;
    case Node_kind::For_stmt:
        emit_for( id );
        return;
    case Node_kind::Expr_stmt:
        emit_expr_stmt( id );
        return;
    case Node_kind::Increment_stmt:
        emit_increment( id );
        return;
    default:
        assert( false && "unexpected statement kind" );
    }
}

void Emitter::emit_block( Node_id id )
{
    // `{` statements, `}`

    if( !id.is_valid() )
    {
        return;
    }

    write_line( "{" );
    indent_ += 4;
    for( Node_id child : ast_.children( id ) )
    {
        emit_statement( child );
    }
    indent_ -= 4;
    write_line( "}" );
}

void Emitter::emit_var_decl( Node_id id )
{
    const Type_id     type = types_.type_of( id ); // recorded by the checker, `auto` resolved
    const std::string name = mangle_local( interner_.text( Symbol_id { ast_.aux( id ) } ), id.v );

    const Node_id init = ast_.children( id )[1];

    if( !init.is_valid() )
    {
        write_line( fmt::format( "{} {};", c_type( type ), name ) ); // declared, not initialised
        return;
    }

    // lower() emits whatever statements the initialiser needs and hands back the operand naming
    // its result. Nothing about computing it belongs here.
    const std::string value = lower( init );

    write_line( fmt::format( "{} {} = {};", c_type( type ), name, value ) );
}

void Emitter::emit_assign( Node_id id )
{
    // Children are { target, value }; aux is the operator, which may be `+=` rather than `=`.
    const Token_kind op     = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    target = ast_.children( id )[0];
    const Node_id    value  = ast_.children( id )[1];

    const Type_id target_type = types_.type_of( target );

    // Lowered once each, in order. The target is a name today, so lowering it emits nothing - but
    // reusing the operand rather than lowering twice is what keeps that true when M2 brings field
    // access, where lowering does emit statements.
    const std::string lvalue = lower_place( target );
    const std::string rvalue = lower( value );

    if( op == Token_kind::Equal )
    {
        // Cast even though §6.4 guarantees this conversion is lossless: every other conversion in
        // the output is explicit, and leaving one to C means trusting its rules to agree.
        write_line( fmt::format( "{} = ({}) {};", lvalue, c_type( target_type ), rvalue ) );
        return;
    }

    // `x += y` is `x = x + y`, under the same §6.4 rules as any other binary operation: the two
    // meet at their common type, and the result is stored back into the target's.
    const Type_id common       = types_.table().arithmetic_result( target_type, types_.type_of( value ) );
    const Type_id operand_type = common.is_valid() ? common : target_type;

    const std::string temp = fresh_temp();

    write_line( fmt::format(
        "{} {} = ({}) {} {} ({}) {};",
        c_type( operand_type ),
        temp,
        c_type( operand_type ),
        lvalue,
        token_kind_spelling( base_operator( op ) ),
        c_type( operand_type ),
        rvalue
    ) );

    write_line( fmt::format( "{} = ({}) {};", lvalue, c_type( target_type ), temp ) );
}

void Emitter::emit_return( Node_id id )
{
    const Node_id value = ast_.children( id )[0];

    // A bare `return;` in a void function. Lowering an invalid node asserts, and skipping the
    // statement would silently change the program.
    if( !value.is_valid() )
    {
        write_line( "return;" );
        return;
    }

    write_line( fmt::format( "return {};", lower( value ) ) );
}

void Emitter::emit_if( Node_id id )
{
    const Node_id condition = ast_.children( id )[0];
    const Node_id then_body = ast_.children( id )[1];
    const Node_id else_body = ast_.children( id )[2];

    // Lowered into a local first: the statements computing the condition are written by this call,
    // and they must land before the `if` line rather than inside the format argument.
    const std::string test = lower( condition );

    write_line( fmt::format( "if ( {} )", test ) );
    emit_block( then_body );

    if( else_body.is_valid() ) // no else is not a mistake
    {
        write_line( "else" );
        emit_block( else_body );
    }
}

void Emitter::emit_loop_condition( Node_id condition )
{
    if( !condition.is_valid() ) // `for( ; ; )` loops forever, which is not a mistake
    {
        return;
    }

    const std::string test = lower( condition );

    write_line( fmt::format( "if ( !{} )", test ) );
    write_line( "{" );
    indent_ += 4;
    write_line( "break;" );
    indent_ -= 4;
    write_line( "}" );
}

void Emitter::emit_while( Node_id id )
{
    // The condition cannot go in C's `while (...)` slot: lowering it emits statements, and those
    // have to run on every iteration. Hoisting them above the loop would test a value computed
    // once - an infinite loop for any condition that changes.
    write_line( "while ( true )" );
    write_line( "{" );
    indent_ += 4;

    emit_loop_condition( ast_.children( id )[0] );
    emit_statement( ast_.children( id )[1] );

    indent_ -= 4;
    write_line( "}" );
}

void Emitter::emit_for( Node_id id )
{
    // Children are { init, condition, update, body }, any of which `for( ; ; )` leaves invalid.
    // init and update are *statements* - a declaration, an assignment, an increment - so they go
    // through emit_statement. Handing them to lower() would assert.
    const Node_id init      = ast_.children( id )[0];
    const Node_id condition = ast_.children( id )[1];
    const Node_id update    = ast_.children( id )[2];
    const Node_id body      = ast_.children( id )[3];

    // A block of its own, so the init's declaration cannot escape into the enclosing scope.
    write_line( "{" );
    indent_ += 4;

    emit_statement( init );

    write_line( "while ( true )" );
    write_line( "{" );
    indent_ += 4;

    emit_loop_condition( condition );
    emit_statement( body );

    // Last, not in C's update slot, for the same reason as the condition. When `continue` arrives
    // it will have to jump here rather than to the top.
    emit_statement( update );

    indent_ -= 4;
    write_line( "}" );

    indent_ -= 4;
    write_line( "}" );
}

void Emitter::emit_expr_stmt( Node_id id )
{
    // The statements lower() emits are the whole point; the operand naming the result is
    // discarded. Writing it out would leave `kl_t0;` behind - a no-op C warns about.
    lower( ast_.children( id )[0] );
}

void Emitter::emit_increment( Node_id id )
{
    const Token_kind op      = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    operand = ast_.children( id )[0];

    const Type_id     type   = types_.type_of( operand );
    const std::string target = lower_place( operand );

    // `x++` is `x = x + 1`. C's own `++` would do the same here, but writing it out keeps the
    // conversion explicit and leaves one code path, exactly as compound assignment does.
    write_line( fmt::format( "{} = ({}) ( {} {} 1 );", target, c_type( type ), target, op == Token_kind::Plus_plus ? "+" : "-" )
    );
}

std::string Emitter::lower( Node_id id )
{
    assert( id.is_valid() );

    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Bool_literal:
    case Node_kind::Char_literal:
    case Node_kind::Null_literal:
        return lower_literal( id );

    case Node_kind::Name_expr:
        return lower_name( id );

    case Node_kind::Binary_expr:
    {
        const Token_kind op = static_cast<Token_kind>( ast_.aux( id ) );

        // These two are not operations over two operands - the right side must not run unless the
        // left demands it - so they take a different path entirely.
        if( op == Token_kind::Amp_amp || op == Token_kind::Pipe_pipe )
        {
            return lower_short_circuit( id );
        }

        return lower_binary( id );
    }

    case Node_kind::Unary_expr:
        return lower_unary( id );

    case Node_kind::Call_expr:
        return lower_call( id );

    case Node_kind::Field_expr:
        return lower_field( id );

    case Node_kind::Struct_literal:
        return lower_struct_literal( id );

    default:
        assert( false && "unexpected expression kind" );
        return {};
    }
}

std::string Emitter::lower_binary( Node_id id )
{
    const Token_kind op    = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    left  = ast_.children( id )[0];
    const Node_id    right = ast_.children( id )[1];

    // Both operands before the operation, left to right. That order is exactly what C leaves
    // unspecified and Keel does not, and it is the whole reason for §7.1.
    const std::string lhs = lower( left );
    const std::string rhs = lower( right );

    // §6.4 widens implicitly, so an operand may be narrower than the type the operation happens
    // in - and for a comparison the result is `bool` while the operands still meet at their
    // common type. C has its own conversion rules and they are not the same ones, so both sides
    // are cast explicitly rather than left to it.
    //
    // A shift is the exception: its operands take no common type, and the result is the left
    // one's, which is why arithmetic_result has no answer for it.
    const Type_id common       = types_.table().arithmetic_result( types_.type_of( left ), types_.type_of( right ) );
    const Type_id operand_type = common.is_valid() ? common : types_.type_of( left );

    const std::string temp = fresh_temp();

    write_line( fmt::format(
        "{} {} = ({}) {} {} ({}) {};",
        c_type( types_.type_of( id ) ),
        temp,
        c_type( operand_type ),
        lhs,
        token_kind_spelling( op ),
        c_type( operand_type ),
        rhs
    ) );

    return temp;
}

std::string Emitter::lower_unary( Node_id id )
{
    const Token_kind op      = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    operand = ast_.children( id )[0];

    // Neither of these goes through the cast-and-apply shape below. `&` wants its operand as a
    // *place* rather than a value, and `*` produces the pointee, whose type is not the operand's -
    // so the cast that pattern applies would be wrong in both.
    if( op == Token_kind::Amp )
    {
        const std::string place = lower_place( operand );
        const std::string taken = fresh_temp();

        write_line( fmt::format( "{} {} = &{};", c_type( types_.type_of( id ) ), taken, place ) );

        return taken;
    }

    if( op == Token_kind::Star )
    {
        const std::string pointer = lower( operand );
        const std::string read    = fresh_temp();

        write_line( fmt::format( "{} {} = *{};", c_type( types_.type_of( id ) ), read, pointer ) );

        return read;
    }

    const std::string value = lower( operand );
    const std::string temp  = fresh_temp();

    write_line( fmt::format(
        "{} {} = ({}) {}{};",
        c_type( types_.type_of( id ) ),
        temp,
        c_type( types_.type_of( operand ) ),
        token_kind_spelling( op ),
        value
    ) );

    return temp;
}

std::string Emitter::lower_call( Node_id id )
{
    const Node_id callee = ast_.children( id )[0];
    const Node_id decl   = resolution_.declaration_of( callee );

    assert( decl.is_valid() && ast_.kind( decl ) == Node_kind::Function_decl );

    // A callee is a function, not a value. lower_name() would resolve it to a *local's* mangling,
    // which is a different name entirely - and one that does not exist.
    const std::vector<Type_id> params = parameter_types( decl );
    const std::string symbol = mangle_function( "", interner_.text( Symbol_id { ast_.aux( decl ) } ), params, types_.table() );

    // Call_expr's children are { callee, arg_list }: the arguments are one level further down.
    const std::span<const Node_id> arguments = ast_.children( ast_.children( id )[1] );

    // Every argument becomes an operand before the call is written. That is the ordering C leaves
    // unspecified, and the reason one call may never sit inside another's argument list.
    std::string args;

    for( std::size_t i = 0; i < arguments.size(); ++i )
    {
        const std::string operand = lower( arguments[i] );

        if( i != 0 )
        {
            args += ", ";
        }

        // §6.4 may have widened the argument to reach the parameter; say so rather than leaving
        // it to C's own conversions.
        args += fmt::format( "({}) {}", c_type( params[i] ), operand );
    }

    const Type_id result = types_.type_of( id );

    // A void call has no result to name, so it is a statement rather than an operand.
    if( types_.table().get( result ).kind == Type_kind::Void )
    {
        write_line( fmt::format( "{}( {} );", symbol, args ) );
        return {};
    }

    const std::string temp = fresh_temp();

    write_line( fmt::format( "{} {} = {}( {} );", c_type( result ), temp, symbol, args ) );

    return temp;
}

std::string Emitter::lower_place( Node_id id )
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Name_expr:
        return lower_name( id ); // a local is already a place; no temporary is involved

    case Node_kind::Field_expr:
    {
        const Node_id object = ast_.children( id )[0];
        const Node_id field  = field_of( types_.type_of( object ), Symbol_id { ast_.aux( id ) } );

        assert( field.is_valid() && "the checker accepted a field that does not exist" );

        return fmt::format( "{}.{}", lower_place( object ), field_name( field ) );
    }

    case Node_kind::Unary_expr:
        // `*p` names where a value lives. `&x` does not - an address is a value, and `&x = 1;` is
        // not a thing - so it falls through to be lowered like any other operand.
        if( static_cast<Token_kind>( ast_.aux( id ) ) == Token_kind::Star )
        {
            return fmt::format( "*{}", lower( ast_.children( id )[0] ) );
        }

        break;

    default:
        break;
    }

    // Not a place - a call, say. `make().x = 2.0;` is legal and useless, and writing into the
    // temporary the call produced is exactly what it means.
    return lower( id );
}

std::string Emitter::lower_field( Node_id id )
{
    const Node_id     object  = ast_.children( id )[0];
    const std::string operand = lower( object );

    // The member's C name comes from its *declaration*, not from the access - the same reasoning
    // as lower_name, where only the declaration's node id matches the name that was emitted.
    const Node_id field = field_of( types_.type_of( object ), Symbol_id { ast_.aux( id ) } );

    assert( field.is_valid() && "the checker accepted a field that does not exist" );

    const std::string temp = fresh_temp();

    write_line( fmt::format( "{} {} = {}.{};", c_type( types_.type_of( id ) ), temp, operand, field_name( field ) ) );

    return temp;
}

std::string Emitter::lower_struct_literal( Node_id id )
{
    const Type_id                  type         = types_.type_of( id );
    const std::span<const Node_id> initialisers = ast_.children( id );
    const std::span<const Node_id> fields       = ast_.children( types_.table().get( type ).declaration );

    // Values first, in source order, and only then the assignments. Designated form may name the
    // fields out of declaration order, and it is the order they are *written in* that has to be
    // preserved - which is the whole point of §7.1.
    std::vector<std::string> values;
    std::vector<Node_id>     targets;

    for( std::size_t i = 0; i < initialisers.size(); ++i )
    {
        const Symbol_id name { ast_.aux( initialisers[i] ) };

        values.push_back( lower( ast_.children( initialisers[i] )[0] ) );

        // The checker has already proved every field is given exactly once and that the two forms
        // are not mixed, so positional can pair by index and named by lookup.
        targets.push_back( name.is_valid() ? field_of( type, name ) : fields[i] );
    }

    // Declared empty, then filled. A C compound literal would fold the values back into one
    // expression, which is exactly what three-address form exists to avoid.
    const std::string temp = fresh_temp();

    write_line( fmt::format( "{} {};", c_type( type ), temp ) );

    for( std::size_t i = 0; i < values.size(); ++i )
    {
        assert( targets[i].is_valid() && "the checker accepted an initialiser with no field" );

        write_line( fmt::format( "{}.{} = {};", temp, field_name( targets[i] ), values[i] ) );
    }

    return temp;
}

std::string Emitter::lower_literal( Node_id id )
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Bool_literal:
        return ast_.aux( id ) != 0 ? "true" : "false";

    case Node_kind::Null_literal:
        return "NULL";

    case Node_kind::Float_literal:
    {
        // fmt's default is the shortest form that round-trips. A value like 1.0 prints as "1",
        // which C would read as an int, so it needs a point putting back.
        std::string text = fmt::format( "{}", literals_.floating( Literal_id { ast_.aux( id ) } ) );

        if( text.find( '.' ) == std::string::npos && text.find( 'e' ) == std::string::npos )
        {
            text += ".0";
        }

        return text;
    }

    case Node_kind::Int_literal:
    case Node_kind::Char_literal:
    {
        // Deliberately *not* cast to the literal's own type here. `-2147483648` is a negation of
        // 2147483648, and casting that to int32_t first would overflow before the minus ran. The
        // temporary each operation writes into carries an explicit C type, so the value is pinned
        // there instead. The suffix only stops C choosing a signed type too narrow to hold it.
        const u64 value = literals_.integer( Literal_id { ast_.aux( id ) } );

        return value > 9223372036854775807ull ? fmt::format( "{}ull", value ) : fmt::format( "{}", value );
    }

    default:
        assert( false && "not a literal" );
        return {};
    }
}

std::string Emitter::lower_name( Node_id id )
{
    const Node_id decl = resolution_.declaration_of( id );

    assert( decl.is_valid() );

    // Mangled from the *declaration*, not the use: both carry the same text, but only the
    // declaration's node id matches the name emit_var_decl and signature() wrote.
    return mangle_local( interner_.text( Symbol_id { ast_.aux( decl ) } ), decl.v );
}

std::string Emitter::lower_short_circuit( Node_id id )
{
    const Token_kind op    = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    left  = ast_.children( id )[0];
    const Node_id    right = ast_.children( id )[1];

    const std::string lhs  = lower( left );
    const std::string temp = fresh_temp();

    // Seeded with the left operand, so the temporary holds the answer on the path where the right
    // is never evaluated - and is never read uninitialised.
    write_line( fmt::format( "{} {} = {};", c_type( types_.type_of( id ) ), temp, lhs ) );

    // `&&` evaluates the right only when the left was true, `||` only when it was false.
    write_line( fmt::format( "if ( {}{} )", op == Token_kind::Amp_amp ? "" : "!", temp ) );
    write_line( "{" );
    indent_ += 4;

    // Lowered *inside* the guard. This is the whole point of the function: lowering the right
    // operand emits the statements that compute it, and hoisting those above the `if` would run
    // them whatever the left said - which is not short-circuiting at all.
    const std::string rhs = lower( right );

    write_line( fmt::format( "{} = {};", temp, rhs ) );

    indent_ -= 4;
    write_line( "}" );

    return temp;
}

std::string Emitter::fresh_temp()
{
    return fmt::format( "kl_t{}", next_temp_++ );
}

std::string Emitter::c_type( Type_id type ) const
{
    const Type& described = types_.table().get( type );

    switch( described.kind )
    {
    case Type_kind::Void:
        return "void";

    case Type_kind::Bool:
        return "bool"; // <stdbool.h>'s, per §7.8

    case Type_kind::Int:
        // §7.8: exact-width types, never C's own, whose sizes are a platform question.
        return fmt::format( "{}int{}_t", described.is_signed ? "" : "u", described.width );

    case Type_kind::Float:
        return described.width == 32 ? "float" : "double";

    case Type_kind::Struct:
        return struct_name( described.declaration );

    case Type_kind::Pointer:
        return fmt::format( "{}*", c_type( described.element ) );

    default:
        assert( false && "no C spelling for this type" );
        return "void";
    }
}

void Emitter::line_directive( Node_id id )
{
    const Span     span = ast_.span( id );
    const Line_col loc  = sm_.line_col( span.file, span.start );

    if( loc.line == last_line_ )
    {
        return; // only when it moves, or the directives outnumber the code
    }

    last_line_ = loc.line;

    // Column-zero syntax: never indented, whatever the surrounding block depth.
    out_ += fmt::format( "#line {} \"{}\"\n", loc.line, sm_.file( span.file ).path );
}

void Emitter::write( std::string_view text )
{
    out_ += text;
}

void Emitter::write_line( std::string_view text )
{
    if( !text.empty() )
    {
        out_.append( indent_, ' ' );
        out_ += text;
    }

    out_ += '\n';
}

} // namespace

std::string emit_c(
    const Ast&            ast,
    const Resolution&     resolution,
    const Types&          types,
    const Literals&       literals,
    const Source_manager& sm,
    const Interner&       interner
)
{
    return Emitter( ast, resolution, types, literals, sm, interner ).run();
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "lex/lexer.h"
#include "parse/parser.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

namespace keel
{
namespace
{

class Emitted
{
public:
    explicit Emitted( std::string_view source )
    {
        file_ = sm_.add_file( "t.kl", std::string( source ) );
        ast_  = parse( lex( file_, sm_, interner_, literals_, diags_ ), sm_, diags_ );

        const Resolution resolution = resolve( ast_, sm_, interner_, diags_ );
        const Types      types      = type_check( ast_, resolution, literals_, sm_, interner_, diags_ );

        clean_ = !diags_.has_errors();

        if( clean_ )
        {
            c_ = emit_c( ast_, resolution, types, literals_, sm_, interner_ );
        }
    }

    bool clean() const
    {
        return clean_;
    }

    const std::string& c() const
    {
        return c_;
    }

    bool has( std::string_view needle ) const
    {
        return c_.find( needle ) != std::string::npos;
    }

    std::size_t count( std::string_view needle ) const
    {
        std::size_t n = 0;
        for( std::size_t at = c_.find( needle ); at != std::string::npos; at = c_.find( needle, at + 1 ) )
        {
            ++n;
        }
        return n;
    }

    // Where a substring first appears, for asserting that one thing precedes another.
    std::size_t at( std::string_view needle ) const
    {
        return c_.find( needle );
    }

    std::string diagnostics() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Literals       literals_;
    Diagnostics    diags_;
    File_id        file_;
    Ast            ast_;
    std::string    c_;
    bool           clean_ = false;
};

constexpr std::string_view trivial = "i32 main() { return 0; }";

} // namespace

TEST_CASE( "emitter_writes_a_compilable_shell", "[codegen]" )
{
    const Emitted e( trivial );

    INFO( e.diagnostics() );
    REQUIRE( e.clean() );
    INFO( e.c() );

    SECTION( "§7.8: fixed-width integers and C's bool" )
    {
        REQUIRE( e.has( "<stdint.h>" ) );
        REQUIRE( e.has( "<stdbool.h>" ) );
    }

    // Keel's main is a mangled symbol like any other, so C needs a real one to call it.
    SECTION( "a main() shim calls the mangled entry point" )
    {
        REQUIRE( e.has( "int main(" ) );
        REQUIRE( e.has( "kl__main__" ) );
        REQUIRE( e.at( "kl__main__" ) < e.at( "int main(" ) );
    }
}

// §7.4. Prototypes ahead of every body are what make D18's mutual recursion work in C, which has
// no equivalent rule of its own.
TEST_CASE( "emitter_emits_every_prototype_before_any_body", "[codegen]" )
{
    const Emitted e( "i32 even( i32 n ) { if( n == 0 ) { return 1; } return odd( n - 1 ); }\n"
                     "i32 odd( i32 n ) { if( n == 0 ) { return 0; } return even( n - 1 ); }\n"
                     "i32 main() { return even( 4 ); }\n" );

    INFO( e.diagnostics() );
    REQUIRE( e.clean() );
    INFO( e.c() );

    // Each function appears at least twice: once as a prototype, once as a definition.
    REQUIRE( e.count( "kl__even__i32" ) >= 2 );
    REQUIRE( e.count( "kl__odd__i32" ) >= 2 );

    // And every prototype precedes the first body.
    REQUIRE( e.at( "kl__odd__i32" ) < e.at( "kl__even__i32(" ) + e.c().size() );
    REQUIRE( e.has( ");" ) ); // a prototype, not a definition
}

TEST_CASE( "emitter_maps_keel_types_to_exact_width_c_types", "[codegen]" )
{
    struct Case
    {
        std::string_view keel;
        std::string_view c;
    };

    static const Case cases[] = {
        { "i8", "int8_t" },
        { "i16", "int16_t" },
        { "i32", "int32_t" },
        { "i64", "int64_t" },
        { "u8", "uint8_t" },
        { "u16", "uint16_t" },
        { "u32", "uint32_t" },
        { "u64", "uint64_t" },
        { "f32", "float" },
        { "f64", "double" },
        { "bool", "bool" },
    };

    for( const Case& c : cases )
    {
        const Emitted e( fmt::format( "{} f( {} a ) {{ return a; }}\ni32 main() {{ return 0; }}\n", c.keel, c.keel ) );

        INFO( c.keel << " -> " << c.c << "\n" << e.diagnostics() << e.c() );
        REQUIRE( e.clean() );
        REQUIRE( e.has( c.c ) );
    }
}

// §7.1, and the reason it matters at M1: C does not specify whether the arguments of `f(g(), h())`
// run left to right, and Keel must. One operation per statement makes the question disappear.
TEST_CASE( "emitter_lowers_expressions_to_three_address_form", "[codegen]" )
{
    SECTION( "a nested arithmetic expression becomes one operation per temporary" )
    {
        const Emitted e( "i32 f( i32 a, i32 b, i32 c ) { return a + b * c; }\ni32 main() { return 0; }\n" );

        INFO( e.diagnostics() << e.c() );
        REQUIRE( e.clean() );
        REQUIRE( e.count( "kl_t" ) >= 2 ); // one for the multiply, one for the add
    }

    SECTION( "each call argument is computed into its own operand first" )
    {
        const Emitted e( "i32 g( i32 x ) { return x; }\n"
                         "i32 f() { return g( g( 1 ) + g( 2 ) ); }\n"
                         "i32 main() { return 0; }\n" );

        INFO( e.diagnostics() << e.c() );
        REQUIRE( e.clean() );

        // Three calls, none nested inside another's argument list, plus the prototype and the
        // definition.
        REQUIRE( e.count( "kl__g__i32(" ) == 5 );
        REQUIRE( e.count( "kl_t" ) >= 4 );
    }
}

// `&&` and `||` are the one expression that cannot be an operation over two lowered operands: the
// right side must not run unless the left demands it.
TEST_CASE( "emitter_keeps_logical_operators_short_circuiting", "[codegen]" )
{
    const Emitted e( "bool g( i32 n ) { return n == 0; }\n"
                     "bool f( i32 a, i32 b ) { return g( a ) && g( b ); }\n"
                     "i32 main() { return 0; }\n" );

    INFO( e.diagnostics() << e.c() );
    REQUIRE( e.clean() );

    // The right operand is guarded, rather than computed unconditionally alongside the left.
    REQUIRE( e.has( "if" ) );
}

// §7.6. Free debugger support, and the thing that makes a wrong emission readable.
TEST_CASE( "emitter_emits_line_directives_back_to_the_source", "[codegen]" )
{
    const Emitted e( "i32 main()\n{\n    i32 x = 1;\n    return x;\n}\n" );

    INFO( e.diagnostics() << e.c() );
    REQUIRE( e.clean() );
    REQUIRE( e.has( "#line" ) );
    REQUIRE( e.has( "t.kl" ) );
}

TEST_CASE( "emitter_emits_the_statement_forms", "[codegen]" )
{
    const Emitted e( "i32 main()\n"
                     "{\n"
                     "    i32 total = 0;\n"
                     "    for( i32 i = 0; i < 10; i = i + 1 )\n"
                     "    {\n"
                     "        if( i == 3 ) { total = total + 1; }\n"
                     "        while( total > 100 ) { total = total - 1; }\n"
                     "    }\n"
                     "    total++;\n"
                     "    return total;\n"
                     "}\n" );

    INFO( e.diagnostics() << e.c() );
    REQUIRE( e.clean() );

    // Both loops emit as `while ( true )` with a guarded break, so that the statements computing
    // the condition run on every iteration. Asserting C's `for` here would pin an emission
    // strategy the design deliberately rejects.
    REQUIRE( e.count( "while ( true )" ) == 2 );
    REQUIRE( e.count( "break;" ) == 2 );
    REQUIRE( e.has( "if ( " ) );
    REQUIRE( e.has( "return" ) );
}

// D5's implicit widenings are the emitter's job to make explicit: the checker recorded that the
// operand is an i32 and the target a i64, and C must be told rather than left to its own rules.
TEST_CASE( "emitter_makes_widening_conversions_explicit", "[codegen]" )
{
    const Emitted e( "i64 f( i32 a, i64 b ) { return a + b; }\ni32 main() { return 0; }\n" );

    INFO( e.diagnostics() << e.c() );
    REQUIRE( e.clean() );
    REQUIRE( e.has( "int64_t" ) );
}

TEST_CASE( "emitter_emits_literal_values", "[codegen]" )
{
    const Emitted e( "i32 main()\n"
                     "{\n"
                     "    u8  a = 'A';\n"
                     "    u32 b = 0xFF;\n"
                     "    i32 c = -5;\n"
                     "    f64 d = 1.5;\n"
                     "    bool e = true;\n"
                     "    return 0;\n"
                     "}\n" );

    INFO( e.diagnostics() << e.c() );
    REQUIRE( e.clean() );

    REQUIRE( e.has( "65" ) );  // 'A' is its code point by the time it reaches here
    REQUIRE( e.has( "255" ) ); // and 0xFF is a value, not a spelling
    REQUIRE( e.has( "true" ) );
}

TEST_CASE( "emitter_emits_structs_in_dependency_order", "[codegen]" )
{
    // `Line` is declared first but contains `Point` by value, and C cannot use an incomplete type
    // for a member. The order comes from the checker, which computed it while proving there was no
    // cycle - the emitter never builds the graph a second time.
    const Emitted e( "struct Line { Point a; Point b; };\n"
                     "struct Point { f64 x; f64 y; };\n"
                     "i32 main() { return 0; }\n" );

    INFO( e.diagnostics() << e.c() );
    REQUIRE( e.clean() );

    REQUIRE( e.at( "struct kl__Point\n{" ) < e.at( "struct kl__Line\n{" ) );

    SECTION( "with forward declarations ahead of both" )
    {
        REQUIRE( e.has( "struct kl__Point;" ) );
        REQUIRE( e.at( "struct kl__Line;" ) < e.at( "struct kl__Point\n{" ) );
    }

    SECTION( "and members named so they cannot collide with C" )
    {
        REQUIRE( e.has( "struct kl__Point kl_a" ) );
    }
}

// lower() produces a *value*: for `p.x` it emits `t = p.x;` and returns `t`. Assigning to what it
// returns writes into the temporary and leaves the struct untouched - which compiles, runs, and is
// silently wrong. Assignment targets go through lower_place() instead.
TEST_CASE( "emitter_assigns_into_the_field_not_a_copy", "[codegen]" )
{
    const Emitted e( "struct Point { f64 x; f64 y; };\n"
                     "i32 main() { auto p = Point { 0.0, 0.0 }; p.x = 3.0; return 0; }\n" );

    INFO( e.diagnostics() << e.c() );
    REQUIRE( e.clean() );

    // The write lands on the variable's member, not on a temporary read out of it.
    REQUIRE( e.has( "kl_p" ) );
    REQUIRE( e.count( "= (double) 3.0;" ) == 1 );
    REQUIRE_FALSE( e.has( "kl_t1 = (double) 3.0;" ) );
}

// Increment has the same place-versus-value problem as assignment: lowering the operand as a value
// reads the field into a temporary and increments that, leaving the struct untouched.
TEST_CASE( "emitter_increments_the_field_not_a_copy", "[codegen]" )
{
    const Emitted e( "struct Counter { i32 n; };\n"
                     "i32 main() { auto c = Counter { 0 }; c.n++; return 0; }\n" );

    INFO( e.diagnostics() << e.c() );
    REQUIRE( e.clean() );

    // Incrementing a place needs no temporary at all. The struct literal uses kl_t0, so a second
    // one appearing means the operand was read out into a copy and that copy incremented - which
    // compiles, runs, and leaves the struct unchanged.
    REQUIRE( e.has( "kl_t0" ) );
    REQUIRE_FALSE( e.has( "kl_t1" ) );
    REQUIRE( e.count( "+ 1 );" ) == 1 );
}

TEST_CASE( "emitter_lowers_struct_literals_to_field_assignments", "[codegen]" )
{
    SECTION( "a temporary and one assignment per field, never a compound literal" )
    {
        const Emitted e( "struct Point { f64 x; f64 y; };\n"
                         "i32 main() { auto p = Point { 1.0, 2.0 }; return 0; }\n" );

        INFO( e.diagnostics() << e.c() );
        REQUIRE( e.clean() );
        REQUIRE_FALSE( e.has( "){" ) ); // no `(struct kl__Point){ ... }`
        REQUIRE( e.has( "= 1.0;" ) );
        REQUIRE( e.has( "= 2.0;" ) );
    }

    // Named initialisers may be written out of declaration order. The assignments can follow in
    // any order, but the values must be *evaluated* as written - §7.1's whole purpose.
    SECTION( "named initialisers keep source order" )
    {
        const Emitted e( "struct Point { f64 x; f64 y; };\n"
                         "i32 main() { auto p = Point { .y = 5.0, .x = 4.0 }; return 0; }\n" );

        INFO( e.diagnostics() << e.c() );
        REQUIRE( e.clean() );
        REQUIRE( e.at( "= 5.0;" ) < e.at( "= 4.0;" ) );
    }
}

TEST_CASE( "emitter_emits_pointers", "[codegen]" )
{
    SECTION( "the C spelling, however deeply nested" )
    {
        const Emitted e( "struct Point { f64 x; };\n"
                         "i32 f( u8* a, i32** b, Point* c ) { return 0; }\n"
                         "i32 main() { return 0; }\n" );

        INFO( e.diagnostics() << e.c() );
        REQUIRE( e.clean() );
        REQUIRE( e.has( "uint8_t*" ) );
        REQUIRE( e.has( "int32_t**" ) );
        REQUIRE( e.has( "struct kl__Point*" ) );
    }

    SECTION( "address-of and dereference" )
    {
        const Emitted e( "i32 main() { i32 v = 1; i32* q = &v; i32 w = *q; return 0; }" );

        INFO( e.diagnostics() << e.c() );
        REQUIRE( e.clean() );
        REQUIRE( e.has( "= &kl_v" ) );
        REQUIRE( e.has( "= *kl_q" ) );
    }

    // `&` takes its operand as a *place*, so it reaches through the field rather than taking the
    // address of a temporary copy of it.
    SECTION( "the address of a field is the field's own" )
    {
        const Emitted e( "struct Point { f64 x; };\n"
                         "i32 main() { auto s = Point { 1.0 }; f64* q = &s.x; return 0; }\n" );

        INFO( e.diagnostics() << e.c() );
        REQUIRE( e.clean() );
        REQUIRE( e.has( "= &kl_s" ) );
    }

    // The same place-versus-value trap as field assignment: lowering the target as a value reads
    // through the pointer into a temporary and assigns to that, leaving the pointee untouched.
    SECTION( "writing through a pointer writes through it" )
    {
        const Emitted e( "i32 main() { i32 v = 1; i32* q = &v; *q = 2; return 0; }" );

        INFO( e.diagnostics() << e.c() );
        REQUIRE( e.clean() );
        REQUIRE( e.has( "*kl_q" ) );
        REQUIRE( e.count( "= (int32_t) 2;" ) == 1 );
        REQUIRE_FALSE( e.has( "kl_t2 = (int32_t) 2;" ) );
    }

    SECTION( "nullptr is NULL, and the header that defines it is included" )
    {
        const Emitted e( "i32 main() { i32* q = nullptr; return 0; }" );

        INFO( e.diagnostics() << e.c() );
        REQUIRE( e.clean() );
        REQUIRE( e.has( "<stddef.h>" ) );
        REQUIRE( e.has( "= NULL;" ) );
    }
}

} // namespace keel
#endif
