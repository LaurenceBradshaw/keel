#include "sema/type_checker.h"
#include <fmt/format.h>
#include "lex/token.h"

#include <algorithm>

namespace keel
{

namespace
{

// D1. The rejected C++ spellings are ordinary identifiers to the lexer, so this is the first point
// that knows one was written in type position - which is exactly the argument for putting the
// suggestion here rather than in lex/. Empty when there is nothing to suggest.
//
// Only single-word spellings can appear: `unsigned int` and `long long` are two identifiers and
// fail in the parser long before sema sees them.
std::string_view keel_spelling_for( std::string_view cpp_spelling )
{
    struct Alias
    {
        std::string_view from;
        std::string_view to;
    };

    static constexpr Alias aliases[] = {
        { "int", "i32" },       { "signed", "i32" },   { "unsigned", "u32" },  { "short", "i16" },    { "long", "i64" },
        { "char", "i8" },       { "float", "f32" },    { "double", "f64" },    { "size_t", "u64" },   { "ssize_t", "i64" },
        { "ptrdiff_t", "i64" }, { "intptr_t", "i64" }, { "uintptr_t", "u64" }, { "int8_t", "i8" },    { "int16_t", "i16" },
        { "int32_t", "i32" },   { "int64_t", "i64" },  { "uint8_t", "u8" },    { "uint16_t", "u16" }, { "uint32_t", "u32" },
        { "uint64_t", "u64" },
    };

    for( const Alias& alias : aliases )
    {
        if( alias.from == cpp_spelling )
        {
            return alias.to;
        }
    }

    return {};
}

// What an operator accepts, and where its result type comes from. A table rather than nested
// switches, for the same reason the parser keeps binding_power() as one: these are rules, and
// checking them against PLAN §6.4 and D16 should not mean tracing control flow.
enum class Operands : u8
{
    Numeric,    // integers and floats
    Integer,    // integers only
    Bool,       // bools only
    Comparable, // numeric, or two bools
};

enum class Result : u8
{
    Common, // the §6.4 result of the two operands
    Bool,   // a comparison
    Left,   // the left operand's type - C++ takes no common type for a shift
};

struct Binary_rule
{
    Token_kind kind;
    Operands   operands;
    Result     result;
};

constexpr Binary_rule binary_rules[] = {
    { Token_kind::Plus, Operands::Numeric, Result::Common },
    { Token_kind::Minus, Operands::Numeric, Result::Common },
    { Token_kind::Star, Operands::Numeric, Result::Common },
    { Token_kind::Slash, Operands::Numeric, Result::Common },
    { Token_kind::Percent, Operands::Integer, Result::Common },
    { Token_kind::Amp, Operands::Integer, Result::Common },
    { Token_kind::Pipe, Operands::Integer, Result::Common },
    { Token_kind::Caret, Operands::Integer, Result::Common },
    { Token_kind::Less_less, Operands::Integer, Result::Left },
    { Token_kind::Greater_greater, Operands::Integer, Result::Left },
    { Token_kind::Less, Operands::Numeric, Result::Bool },
    { Token_kind::Less_equal, Operands::Numeric, Result::Bool },
    { Token_kind::Greater, Operands::Numeric, Result::Bool },
    { Token_kind::Greater_equal, Operands::Numeric, Result::Bool },
    { Token_kind::Equal_equal, Operands::Comparable, Result::Bool },
    { Token_kind::Bang_equal, Operands::Comparable, Result::Bool },
    { Token_kind::Amp_amp, Operands::Bool, Result::Bool },
    { Token_kind::Pipe_pipe, Operands::Bool, Result::Bool },
};

struct Unary_rule
{
    Token_kind kind;
    Operands   operands;
    bool       signed_only; // negating an unsigned type has no representable answer
};

constexpr Unary_rule unary_rules[] = {
    { Token_kind::Minus, Operands::Numeric, true },
    { Token_kind::Plus, Operands::Numeric, false },
    { Token_kind::Bang, Operands::Bool, false },
    { Token_kind::Tilde, Operands::Integer, false },
};

const Binary_rule* binary_rule_for( Token_kind kind )
{
    for( const Binary_rule& rule : binary_rules )
    {
        if( rule.kind == kind )
        {
            return &rule;
        }
    }

    return nullptr;
}

const Unary_rule* unary_rule_for( Token_kind kind )
{
    for( const Unary_rule& rule : unary_rules )
    {
        if( rule.kind == kind )
        {
            return &rule;
        }
    }

    return nullptr;
}

// The help line for an operand kind. Empty where the message above it already says enough.
std::string_view operand_requirement( Operands operands )
{
    switch( operands )
    {
    case Operands::Integer:
        return "both operands must be integers";
    case Operands::Bool:
        return "both operands must be `bool`";
    case Operands::Numeric:
    case Operands::Comparable:
        return {};
    }

    return {};
}

class Checker
{
public:
    Checker(
        const Ast& ast, const Interner& interner, const Resolution& resolution, const Literals& literals, Diagnostics& diags
    )
        : ast_( ast ),
          interner_( interner ),
          resolution_( resolution ),
          literals_( literals ),
          diags_( diags )
    {
    }

    Types run();

private:
    // Every top-level signature is typed before any body, so a call can read its callee's
    // parameter and return types straight out of types_. Same reason as the resolver's two-pass
    // file scope: mutual recursion (D18) means `even` needs `odd`'s return type.
    void declare_signatures();

    void    visit( Node_id id ); // statements and declarations
    Type_id infer( Node_id id ); // expression, no expectation

    // One per construct, dispatched from visit and infer - the shape parse_*() uses next door.
    void visit_function( Node_id id );
    void check_condition( Node_id id ); // if/while/for all want the same message
    bool is_assignable( Node_id id ) const;
    void visit_return( Node_id id );
    void visit_var( Node_id id );
    void visit_assign( Node_id id );
    void visit_if( Node_id id );
    void visit_while( Node_id id );
    void visit_for( Node_id id );
    void visit_increment( Node_id id );

    // Int, Float and Bool literals with nothing to give them a type - the fallback defaults.
    // Whether context can give this expression a type, rather than it having one of its own.
    bool is_literal_expression( Node_id id ) const;

    Type_id infer_literal( Node_id id );

    // The bidirectional half (L5): a literal adopts `expected` when its value fits, which is what
    // lets `u32 x = 42;` need no suffix. Also where a unary minus must be pushed through, so that
    // `i32 x = -2147483648;` range-checks as a negative rather than as an out-of-range positive.
    Type_id check_literal( Node_id id, Type_id expected );

    Type_id infer_name( Node_id id );
    Type_id infer_call( Node_id id );
    Type_id infer_binary( Node_id id );
    Type_id infer_unary( Node_id id );

    bool    accepts( Operands operands, Type_id type ) const;
    Type_id check( Node_id id, Type_id expected ); // expression, with one
    Type_id type_of_annotation( Node_id id );      // Named_type/Pointer_type subtree; invalid for auto

    // Writes the node's type and returns it. Every infer branch ends in one of these, so that
    // forgetting to record a type is hard rather than silent.
    Type_id record( Node_id id, Type_id type );

    void error_at( Span span, std::string message, std::string help = {} );

    const Ast&        ast_;
    const Interner&   interner_;
    const Resolution& resolution_;
    const Literals&   literals_;
    Diagnostics&      diags_;

    Type_table           table_;
    std::vector<Type_id> types_; // sized node_count(), invalid-filled, like bindings_ in Resolver
    Type_id              current_return_;
};

Types Checker::run()
{
    types_.assign( ast_.node_count(), Type_id {} );
    declare_signatures();
    visit( ast_.root() );
    return Types( std::move( table_ ), std::move( types_ ) );
}

void Checker::declare_signatures()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Function_decl )
        {
            continue;
        }

        const Node_id return_type_node = ast_.children( child )[0];
        const Type_id return_type      = type_of_annotation( return_type_node );
        record( child, return_type );

        const Node_id param_list = ast_.children( child )[1];
        for( Node_id param : ast_.children( param_list ) )
        {
            if( ast_.kind( param ) != Node_kind::Param_decl )
            {
                continue;
            }

            const Node_id param_type_node = ast_.children( param )[0];
            const Type_id param_type      = type_of_annotation( param_type_node );
            record( param, param_type );
        }

        if( interner_.text( Symbol_id { ast_.aux( child ) } ) == "main" )
        {
            // return type
            if( !table_.is_error( return_type ) && return_type != table_.integer( 32, true ) )
            {
                error_at(
                    ast_.span( return_type_node ),
                    fmt::format( "`main` must return `i32`, but got `{}`", table_.name( return_type ) )
                );
            }

            // parameters
            // Keel currently doesn't support command line args, or arrays,
            // so `main` must be parameterless.
            if( !ast_.children( param_list ).empty() )
            {
                error_at( ast_.span( param_list ), "`main` must not take any parameters" );
            }
        }
    }
}

void Checker::visit( Node_id id )
{
    if( !id.is_valid() )
    {
        return;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Error:
        return;

    case Node_kind::Expr_stmt:
        infer( ast_.children( id )[0] );
        return; // discard the result. D15 already made effectless expressions a parse error

    case Node_kind::Function_decl:
        return visit_function( id );

    case Node_kind::Return_stmt:
        return visit_return( id );

    case Node_kind::Var_decl:
        return visit_var( id );

    case Node_kind::Assign_stmt:
        return visit_assign( id );

    case Node_kind::If_stmt:
        return visit_if( id );

    case Node_kind::While_stmt:
        return visit_while( id );

    case Node_kind::For_stmt:
        return visit_for( id );

    case Node_kind::Increment_stmt:
        return visit_increment( id );

    default:
        // Source_file, Block, and every statement not yet given a case of its own.
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        return;
    }
}

void Checker::visit_function( Node_id id )
{
    // Children are { return_type, param_list, body }. declare_signatures already typed the first
    // two, so only the body is left.
    //
    // Saved and restored rather than plainly assigned: M6 brings lambdas, and a nested one would
    // otherwise leave the enclosing function checking its returns against the wrong type.
    const Type_id enclosing_return = current_return_;

    current_return_ = types_[id.v];
    visit( ast_.children( id )[2] );
    current_return_ = enclosing_return;
}

void Checker::check_condition( Node_id id )
{
    if( !id.is_valid() ) // `for( ; ; )` has no condition, which is not a mistake
    {
        return;
    }

    const Type_id bool_type = table_.builtin( Type_kind::Bool );
    const Type_id actual    = infer( id );

    if( table_.is_error( actual ) || actual == bool_type )
    {
        return;
    }

    // D5's sharpest break from C++ habit, so the message says what to write instead rather than
    // only what is wrong.
    error_at(
        ast_.span( id ),
        fmt::format( "expected `bool`, but got `{}`", table_.name( actual ) ),
        "there is no conversion to `bool`: compare explicitly, as in `x != 0`"
    );
}

bool Checker::is_assignable( Node_id id ) const
{
    const Node_id decl = ast_.kind( id ) == Node_kind::Name_expr ? resolution_.declaration_of( id ) : Node_id {};
    return decl.is_valid() && ( ast_.kind( decl ) == Node_kind::Var_decl || ast_.kind( decl ) == Node_kind::Param_decl );
}

void Checker::visit_return( Node_id id )
{
    const Node_id value     = ast_.children( id )[0];
    const Type_id void_type = table_.builtin( Type_kind::Void );

    if( !value.is_valid() ) // a bare `return;`
    {
        if( current_return_.is_valid() && current_return_ != void_type )
        {
            error_at(
                ast_.span( id ),
                fmt::format( "this function returns `{}`, so `return` needs a value", table_.name( current_return_ ) )
            );
        }

        return;
    }

    if( current_return_ == void_type )
    {
        // Typed anyway: a mistake inside the expression is still a mistake worth reporting.
        infer( value );
        error_at( ast_.span( value ), "a `void` function cannot return a value" );
        return;
    }

    check( value, current_return_ );
}

void Checker::visit_var( Node_id id )
{
    // Children are { type, init }. Either can be invalid: no annotation means `auto`, and no
    // initialiser means the variable is only declared.
    const Node_id annotation = ast_.children( id )[0];
    const Node_id init       = ast_.children( id )[1];

    Type_id type = type_of_annotation( annotation );

    if( type.is_valid() )
    {
        // Annotated. Checking rather than inferring is what lets a literal adopt the declared
        // type, so `u32 x = 42;` needs no suffix. An unknown annotation is the error type, which
        // check() absorbs - one diagnostic, from type_of_annotation.
        if( init.is_valid() )
        {
            check( init, type );
        }
    }
    else if( init.is_valid() )
    {
        type = infer( init ); // `auto`: L6's one form of inference
    }
    else
    {
        error_at( ast_.span( id ), "`auto` needs an initialiser to infer from" );
        type = table_.builtin( Type_kind::Error );
    }

    record( id, type );
}

void Checker::visit_assign( Node_id id )
{
    // Children are { target, value }; aux is the operator, which may be `+=` rather than `=`.
    const Token_kind op     = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    target = ast_.children( id )[0];
    const Node_id    value  = ast_.children( id )[1];

    // Asked before infer(), which would otherwise absorb it: a literal target types as an error
    // today, so `1 = 2;` passed in silence. Parameters are assignable too - L12 is
    // mutable-by-default, and a parameter is a local.
    if( !is_assignable( target ) )
    {
        // A name that does not resolve, or resolves to a function, is already reported by
        // infer_name. Anything else - a literal, a call, an arithmetic expression - has nothing
        // else to report it.
        if( ast_.kind( target ) != Node_kind::Name_expr )
        {
            error_at( ast_.span( target ), "cannot assign to this expression" );
        }

        infer( target );
        infer( value );
        return;
    }

    const Type_id target_type = infer( target );

    if( table_.is_error( target_type ) )
    {
        infer( value ); // absorb, but do not leave the value untyped
        return;
    }

    if( op == Token_kind::Equal )
    {
        check( value, target_type );
        return;
    }

    // A compound assignment is `x = x op y`, so the value is *checked* against the target rather
    // than inferred. Inferring it would settle a literal on its default type first, and `u8 x;
    // x += 3;` would then combine u8 with i32 and have nowhere to put the result.
    //
    // Checking also subsumes the range rule: whatever holds in the target can be combined with it
    // and stored back, and whatever does not is rejected here with a clearer message than
    // "cannot hold the i32 this produces".
    check( value, target_type );

    if( !table_.is_integer( target_type ) && !table_.is_float( target_type ) )
    {
        error_at(
            ast_.span( id ), fmt::format( "no operator `{}` for `{}`", token_kind_spelling( op ), table_.name( target_type ) )
        );
    }
}

void Checker::visit_if( Node_id id )
{
    // Children are { condition, then, else }; else is invalid when there is none, and visit()
    // returns immediately on that.
    check_condition( ast_.children( id )[0] );
    visit( ast_.children( id )[1] );
    visit( ast_.children( id )[2] );
}

void Checker::visit_while( Node_id id )
{
    check_condition( ast_.children( id )[0] );
    visit( ast_.children( id )[1] );
}

void Checker::visit_for( Node_id id )
{
    // Children are { init, condition, update, body }, any of which `for( ; ; )` leaves invalid.
    // init and update are ordinary statements - a declaration, an assignment, an increment - so
    // they go through visit, not infer.
    visit( ast_.children( id )[0] );
    check_condition( ast_.children( id )[1] );
    visit( ast_.children( id )[2] );
    visit( ast_.children( id )[3] );
}

void Checker::visit_increment( Node_id id )
{
    // Children are { operand }; aux is `++` or `--`.
    const Token_kind op      = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    operand = ast_.children( id )[0];

    if( !is_assignable( operand ) )
    {
        // Same split as visit_assign: infer_name already covers an unresolved name and a function.
        if( ast_.kind( operand ) != Node_kind::Name_expr )
        {
            error_at( ast_.span( operand ), fmt::format( "`{}` needs a variable", token_kind_spelling( op ) ) );
        }

        infer( operand );
        return;
    }

    const Type_id operand_type = infer( operand );

    if( table_.is_error( operand_type ) )
    {
        return;
    }

    if( !table_.is_integer( operand_type ) && !table_.is_float( operand_type ) )
    {
        error_at(
            ast_.span( id ), fmt::format( "no operator `{}` for `{}`", token_kind_spelling( op ), table_.name( operand_type ) )
        );
    }
}

Type_id Checker::infer( Node_id id )
{
    if( !id.is_valid() )
    {
        return Type_id {};
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Error:
        return record( id, table_.builtin( Type_kind::Error ) );

    case Node_kind::Name_expr:
        return infer_name( id );

    case Node_kind::Call_expr:
        return infer_call( id );

    case Node_kind::Binary_expr:
        return infer_binary( id );

    case Node_kind::Unary_expr:
        return infer_unary( id );

    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Bool_literal:
    case Node_kind::Char_literal:
        return infer_literal( id );

    case Node_kind::String_literal:
        // Lexed and parsed, but a string has no type until there is a String, which is M7.
        // Silence here would make it look accepted.
        error_at( ast_.span( id ), "string literals are not supported yet" );
        return record( id, table_.builtin( Type_kind::Error ) );

    default:
        // Every expression not yet given a case of its own - the literals, chiefly, which cannot
        // be typed until their values survive lexing. Children are still typed, so a mistake
        // inside one is not swallowed by the parent being unsupported.
        for( const Node_id child : ast_.children( id ) )
        {
            infer( child );
        }

        return record( id, table_.builtin( Type_kind::Error ) );
    }
}

bool Checker::accepts( Operands operands, Type_id type ) const
{
    switch( operands )
    {
    case Operands::Integer:
        return table_.is_integer( type );
    case Operands::Numeric:
        return table_.is_integer( type ) || table_.is_float( type );
    case Operands::Bool:
        return type == table_.builtin( Type_kind::Bool );
    case Operands::Comparable:
        return accepts( Operands::Numeric, type ) || accepts( Operands::Bool, type );
    }

    return false;
}

bool Checker::is_literal_expression( Node_id id ) const
{
    if( !id.is_valid() )
    {
        return false;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Bool_literal:
    case Node_kind::Char_literal:
        return true;

    case Node_kind::Unary_expr:
    {
        // `-5` and `-1.5` are negations of literals, and check_literal pushes the expectation
        // through the minus.
        if( static_cast<Token_kind>( ast_.aux( id ) ) != Token_kind::Minus )
        {
            return false;
        }

        const Node_kind operand = ast_.kind( ast_.children( id )[0] );

        return operand == Node_kind::Int_literal || operand == Node_kind::Float_literal;
    }

    default:
        return false;
    }
}

Type_id Checker::infer_literal( Node_id id )
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
        return record( id, table_.default_integer() );

    case Node_kind::Float_literal:
        return record( id, table_.default_float() );

    case Node_kind::Bool_literal:
        return record( id, table_.builtin( Type_kind::Bool ) );

    case Node_kind::Char_literal:
        return record( id, table_.integer( 8, false ) ); // D20: a code point, so a u8

    default:
        assert( false );
        return record( id, table_.builtin( Type_kind::Error ) );
    }
}

Type_id Checker::check_literal( Node_id id, Type_id expected )
{
    // `-2147483648` parses as a negation of 2147483648, which does not fit an i32 on its own. So
    // the expectation is pushed through the minus and the range check is told the value is
    // negative - exactly the asymmetry Type_table::fits() carries.
    Node_id literal  = id;
    bool    negative = false;

    if( ast_.kind( id ) == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( id ) ) == Token_kind::Minus &&
        is_literal_expression( ast_.children( id )[0] ) )
    {
        literal = ast_.children( id )[0];

        // Only integers have an asymmetric range. A float's negation cannot take it out of range.
        negative = ast_.kind( literal ) == Node_kind::Int_literal;
    }

    switch( ast_.kind( literal ) )
    {
    case Node_kind::Bool_literal:
        if( expected != table_.builtin( Type_kind::Bool ) )
        {
            error_at( ast_.span( id ), fmt::format( "expected `{}`, but got `bool`", table_.name( expected ) ) );
            return expected;
        }

        break;

    case Node_kind::Float_literal:
        if( !table_.is_float( expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format( "expected `{}`, but got a floating-point literal", table_.name( expected ) ),
                table_.is_integer( expected ) ? "a fractional value cannot be an integer" : std::string {}
            );

            return expected;
        }

        break;

    case Node_kind::Char_literal: // a code point is an integer value, measured the same way
    case Node_kind::Int_literal:
    {
        if( !table_.is_integer( expected ) && !table_.is_float( expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format(
                    "expected `{}`, but got {}",
                    table_.name( expected ),
                    ast_.kind( literal ) == Node_kind::Char_literal ? "a character literal" : "an integer literal"
                )
            );

            return expected;
        }

        const Literal_id value = Literal_id { ast_.aux( literal ) };

        // A literal the lexer could not scan - one that overflowed a u64, say - has no value
        // recorded. It was reported there; saying so again here helps nobody.
        if( value.is_valid() && !table_.fits( literals_.integer( value ), negative, expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format(
                    "`{}{}` does not fit in `{}`", negative ? "-" : "", literals_.integer( value ), table_.name( expected )
                )
            );

            return expected;
        }

        break;
    }

    default:
        return expected; // not a literal after all
    }

    // The literal adopts the expected type - the whole point of checking rather than inferring,
    // and what lets `u32 x = 42;` need no suffix. The negation, when there is one, takes it too.
    record( literal, expected );
    return record( id, expected );
}

Type_id Checker::infer_name( Node_id id )
{
    const Node_id decl = resolution_.declaration_of( id );

    if( !decl.is_valid() )
    {
        return record( id, table_.builtin( Type_kind::Error ) ); // the resolver already said so
    }

    if( ast_.kind( decl ) == Node_kind::Function_decl )
    {
        // types_[decl] holds the function's *return* type, so without this `i32 x = f;` would
        // quietly succeed whenever f happens to return an i32.
        error_at(
            ast_.span( id ), fmt::format( "`{}` is a function, not a value", interner_.text( Symbol_id { ast_.aux( id ) } ) )
        );

        return record( id, table_.builtin( Type_kind::Error ) );
    }

    return record( id, types_[decl.v] );
}

Type_id Checker::infer_call( Node_id id )
{
    const Node_id callee = ast_.children( id )[0];
    const Node_id args   = ast_.children( id )[1];

    // v0 has no function pointers, so anything but a plain name in call position has no
    // declaration to find.
    const Node_id decl = ast_.kind( callee ) == Node_kind::Name_expr ? resolution_.declaration_of( callee ) : Node_id {};

    // Even on a failed call the arguments must be typed, or later passes meet untyped nodes and a
    // genuine mistake inside one goes unreported.
    const auto type_the_arguments_anyway = [&]()
    {
        for( const Node_id arg : ast_.children( args ) )
        {
            infer( arg );
        }
    };

    if( !decl.is_valid() )
    {
        // An unknown name was already reported by the resolver; say nothing twice.
        if( ast_.kind( callee ) != Node_kind::Name_expr )
        {
            error_at( ast_.span( callee ), "this expression is not callable" );
        }

        type_the_arguments_anyway();
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    const std::string_view name = interner_.text( Symbol_id { ast_.aux( callee ) } );

    if( ast_.kind( decl ) != Node_kind::Function_decl )
    {
        error_at( ast_.span( callee ), fmt::format( "`{}` is not callable", name ) );
        type_the_arguments_anyway();
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    const std::span<const Node_id> params    = ast_.children( ast_.children( decl )[1] );
    const std::span<const Node_id> arguments = ast_.children( args );

    if( params.size() != arguments.size() )
    {
        error_at(
            ast_.span( args ),
            fmt::format(
                "`{}` takes {} argument{}, but {} {} given",
                name,
                params.size(),
                params.size() == 1 ? "" : "s",
                arguments.size(),
                arguments.size() == 1 ? "was" : "were"
            )
        );
    }

    // Check the pairs that do line up even when the count is wrong: one missing argument should
    // not hide a type error in the others. A parameter whose own type failed to resolve holds an
    // invalid Type_id, which check() absorbs.
    const std::size_t shared = std::min( params.size(), arguments.size() );

    for( std::size_t i = 0; i < shared; ++i )
    {
        check( arguments[i], types_[params[i].v] );
    }

    for( std::size_t i = shared; i < arguments.size(); ++i )
    {
        infer( arguments[i] );
    }

    return record( id, types_[decl.v] );
}

Type_id Checker::infer_binary( Node_id id )
{
    const Token_kind op        = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    left      = ast_.children( id )[0];
    const Node_id    right     = ast_.children( id )[1];
    const Type_id    bool_type = table_.builtin( Type_kind::Bool );
    const Type_id    error     = table_.builtin( Type_kind::Error );

    // A literal has a value, not a type, so the other operand is what gives it one. Inferring both
    // would settle it on its default first, and `u32 bits; bits != 0` would then compare a u32
    // with an i32 - which §6.4 rejects, and which no suffix exists to write around (D13). That
    // would leave unsigned code very nearly unwritable.
    Type_id lhs_type;
    Type_id rhs_type;

    if( is_literal_expression( left ) != is_literal_expression( right ) )
    {
        const bool literal_on_the_left = is_literal_expression( left );

        const Node_id known_side   = literal_on_the_left ? right : left;
        const Node_id literal_side = literal_on_the_left ? left : right;

        const Type_id known = infer( known_side );

        // A failed operand gives nothing to adopt, so the literal falls back to its default.
        const Type_id adopted = table_.is_error( known ) ? infer( literal_side ) : check( literal_side, known );

        lhs_type = literal_on_the_left ? adopted : known;
        rhs_type = literal_on_the_left ? known : adopted;
    }
    else
    {
        lhs_type = infer( left );
        rhs_type = infer( right );
    }

    // An operand that is already wrong was reported where it went wrong. Repeating it here is the
    // cascade the error type exists to prevent - and name() would assert on it besides.
    if( table_.is_error( lhs_type ) || table_.is_error( rhs_type ) )
    {
        return record( id, error );
    }

    const auto reject = [&]( std::string_view help )
    {
        error_at(
            ast_.span( id ),
            fmt::format(
                "no operator `{}` for `{}` and `{}`",
                token_kind_spelling( op ),
                table_.name( lhs_type ),
                table_.name( rhs_type )
            ),
            std::string( help )
        );

        return record( id, error );
    };

    const Binary_rule* rule = binary_rule_for( op );

    if( rule == nullptr )
    {
        error_at( ast_.span( id ), fmt::format( "operator `{}` is not supported yet", token_kind_spelling( op ) ) );
        return record( id, error );
    }

    if( !accepts( rule->operands, lhs_type ) || !accepts( rule->operands, rhs_type ) )
    {
        return reject( operand_requirement( rule->operands ) );
    }

    // Two bools compare and combine directly: §6.4 has no row for bool at all.
    if( lhs_type == bool_type && rhs_type == bool_type )
    {
        return record( id, bool_type );
    }

    if( rule->result == Result::Left )
    {
        return record( id, lhs_type );
    }

    const Type_id common = table_.arithmetic_result( lhs_type, rhs_type );

    if( !common.is_valid() )
    {
        return reject( {} );
    }

    return record( id, rule->result == Result::Bool ? bool_type : common );
}

Type_id Checker::infer_unary( Node_id id )
{
    const Token_kind op           = static_cast<Token_kind>( ast_.aux( id ) );
    const Type_id    operand_type = infer( ast_.children( id )[0] );
    const Type_id    error        = table_.builtin( Type_kind::Error );

    if( table_.is_error( operand_type ) )
    {
        return record( id, error );
    }

    const Unary_rule* rule = unary_rule_for( op );

    if( rule == nullptr )
    {
        // `*` and `&` arrive with pointers, at M2.
        error_at( ast_.span( id ), fmt::format( "unary `{}` is not supported yet", token_kind_spelling( op ) ) );
        return record( id, error );
    }

    const auto reject = [&]( std::string_view help )
    {
        error_at(
            ast_.span( id ),
            fmt::format( "no operator `{}` for `{}`", token_kind_spelling( op ), table_.name( operand_type ) ),
            std::string( help )
        );

        return record( id, error );
    };

    if( !accepts( rule->operands, operand_type ) )
    {
        return reject( operand_requirement( rule->operands ) );
    }

    // C++ answers 4294967295 for `-u32(1)`: a silent wrong answer of exactly the kind D5 removes.
    if( rule->signed_only && table_.is_integer( operand_type ) && !table_.get( operand_type ).is_signed )
    {
        return reject( "negation needs a signed type" );
    }

    return record( id, operand_type );
}

Type_id Checker::check( Node_id id, Type_id expected )
{
    if( !id.is_valid() )
    {
        return expected;
    }

    if( table_.is_error( expected ) )
    {
        infer( id );
        return expected;
    }

    // The bidirectional case: a literal has a value, not a type, so context gives it one. Handled
    // before infer(), which would otherwise settle on the default type first.
    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Bool_literal:
    case Node_kind::Char_literal:
        return check_literal( id, expected );

    case Node_kind::Unary_expr:
        // `-2147483648` only fits an i32 as a negation, so the expectation goes through the minus.
        if( is_literal_expression( id ) )
        {
            return check_literal( id, expected );
        }

        break;

    case Node_kind::Binary_expr:
    {
        // Neither operand has a type of its own, so there is nothing beside them to adopt from -
        // the context is all there is. Without this, `u8 x = 'a' + 1;` combines a u8 with an i32
        // and then has nowhere to put the result.
        //
        // Only for operators whose result comes from their operands: a comparison yields bool
        // whatever it is given, and pushing the expectation into it would be nonsense.
        const Binary_rule* rule = binary_rule_for( static_cast<Token_kind>( ast_.aux( id ) ) );

        if( rule != nullptr && rule->result == Result::Common && is_literal_expression( ast_.children( id )[0] ) &&
            is_literal_expression( ast_.children( id )[1] ) )
        {
            check( ast_.children( id )[0], expected );
            check( ast_.children( id )[1], expected );

            return record( id, expected );
        }

        break;
    }

    default:
        break;
    }

    const Type_id actual = infer( id );
    if( table_.is_error( actual ) )
    {
        return expected;
    }

    if( !table_.holds( actual, expected ) )
    {
        error_at(
            ast_.span( id ), fmt::format( "expected `{}`, but got `{}`", table_.name( expected ), table_.name( actual ) )
        );
        return expected;
    }

    return expected;
}

Type_id Checker::type_of_annotation( Node_id id )
{
    // An invalid Node_id is `auto`, not a mistake - the parser writes one deliberately.
    if( !id.is_valid() )
    {
        return Type_id {};
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Named_type:
    {
        const std::string_view spelling = interner_.text( Symbol_id { ast_.aux( id ) } );
        const Type_id          type     = table_.from_spelling( spelling );

        if( type.is_valid() )
        {
            return type;
        }

        const std::string_view suggestion = keel_spelling_for( spelling );

        error_at(
            ast_.span( id ),
            fmt::format( "unknown type `{}`", spelling ),
            suggestion.empty() ? std::string {} : fmt::format( "Keel spells this `{}`", suggestion )
        );

        return table_.builtin( Type_kind::Error );
    }

    case Node_kind::Const_type:
        return type_of_annotation( ast_.children( id )[0] );

    case Node_kind::Generic_type:
    case Node_kind::Pointer_type:
    case Node_kind::Ref_type:
        // A diagnostic rather than an assert: these parse, so reaching one is bad input, not a
        // broken invariant, and keelc must not abort on a program someone wrote.
        error_at( ast_.span( id ), "this type is not supported yet" );
        return table_.builtin( Type_kind::Error );

    default:
        // Error nodes, and anything the parser puts in type position that is not a type.
        return table_.builtin( Type_kind::Error );
    }
}

Type_id Checker::record( Node_id id, Type_id type )
{
    types_[id.v] = type;
    return type;
}

void Checker::error_at( Span span, std::string message, std::string help )
{
    diags_.error( span, std::move( message ), std::move( help ) );
}

} // namespace

Types type_check(
    const Ast&        ast,
    const Resolution& resolution,
    const Literals&   literals,
    const Source_manager&, // not needed yet; kept so the pass signatures match
    const Interner& interner,
    Diagnostics&    diags
)
{
    return Checker( ast, interner, resolution, literals, diags ).run();
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include "common/interner.h"
#include "lex/lexer.h"
#include "parse/parser.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

namespace keel
{
namespace
{

class Typed
{
public:
    explicit Typed( std::string_view source )
    {
        file_          = sm_.add_file( "t.kl", std::string( source ) );
        ast_           = parse( lex( file_, sm_, interner_, literals_, diags_ ), sm_, diags_ );
        const auto res = resolve( ast_, sm_, interner_, diags_ );
        earlier_       = diags_.error_count();
        types_         = type_check( ast_, res, literals_, sm_, interner_, diags_ );
    }

    // Errors from type checking only, so a fixture with a deliberate parse or name error still
    // says something useful about the types.
    std::size_t errors() const
    {
        return diags_.error_count() - earlier_;
    }

    bool clean() const
    {
        return diags_.error_count() == 0;
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

    // The spelling of a node's type, or "<none>" when it was never typed.
    std::string_view type_name( Node_id id ) const
    {
        const Type_id type = types_.type_of( id );
        return type.is_valid() ? types_.table().name( type ) : "<none>";
    }

    Node_id nth( Node_kind kind, std::size_t index ) const
    {
        for( u32 i = 0; i < ast_.node_count(); ++i )
        {
            const Node_id id { i };
            if( ast_.kind( id ) == kind && index-- == 0 )
            {
                return id;
            }
        }
        return Node_id {};
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Literals       literals_;
    Diagnostics    diags_;
    File_id        file_;
    Ast            ast_;
    Types          types_;
    std::size_t    earlier_ = 0;
};

} // namespace

TEST_CASE( "type_checker_accepts_a_well_typed_program", "[sema][types]" )
{
    const Typed p( "i32 add( i32 a, i32 b )\n"
                   "{\n"
                   "    i32 total = a + b;\n"
                   "    return total;\n"
                   "}\n"
                   "\n"
                   "i32 main()\n"
                   "{\n"
                   "    return add( 1, 2 );\n"
                   "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// The whole point of bidirectional checking (L5): a literal has a value, not a type, until
// something tells it what to be. Neither of these needs a suffix.
TEST_CASE( "type_checker_takes_a_literal_type_from_context", "[sema][types]" )
{
    SECTION( "an annotation supplies it" )
    {
        const Typed p( "i32 main() { u32 x = 42; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u32" );
    }

    SECTION( "so does a wider one" )
    {
        const Typed p( "i32 main() { u64 x = 42; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u64" );
    }

    // Pins the pending D-entry on the default type of an unsuffixed literal.
    SECTION( "with no context it falls back to i32" )
    {
        const Typed p( "i32 main() { auto x = 42; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "i32" );
    }

    SECTION( "a float literal likewise" )
    {
        const Typed p( "i32 main() { f32 x = 1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Float_literal, 0 ) ) == "f32" );
    }
}

TEST_CASE( "type_checker_rejects_a_literal_that_does_not_fit", "[sema][types]" )
{
    static const char* sources[] = {
        "i32 main() { u8 x = 300; return 0; }",
        "i32 main() { i8 x = 200; return 0; }",
        "i32 main() { u32 x = 4294967296; return 0; }",
        "i32 main() { u8 x = -1; return 0; }", // negative never fits an unsigned type
    };

    for( const char* source : sources )
    {
        const Typed p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

TEST_CASE( "type_checker_types_a_name_from_its_declaration", "[sema][types]" )
{
    const Typed p( "i32 f( u16 p )\n"
                   "{\n"
                   "    u64 local = 1;\n"
                   "    return 0;\n"
                   "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 0 ) ) == "u16" );
    REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 0 ) ) == "u64" );
}

// D5 / §6.4 reaching the surface. The table itself is tested in type.cpp; these check that the
// checker consults it, and on the right pair of types.
TEST_CASE( "type_checker_applies_the_conversion_table", "[sema][types]" )
{
    SECTION( "lossless widening needs no cast" )
    {
        const Typed p( "i64 f( i32 a, i64 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "i64" );
    }

    SECTION( "a strictly wider signed type absorbs an unsigned one" )
    {
        const Typed p( "i32 f( i32 a, u8 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "i32" );
    }

    SECTION( "equal-rank mixed signedness is rejected - the case C++ gets wrong" )
    {
        const Typed p( "i64 f( i32 a, u32 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "so is a float that cannot hold the integer exactly" )
    {
        const Typed p( "f64 f( i64 a, f64 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "same-type arithmetic stays in the type" )
    {
        const Typed p( "u8 f( u8 a, u8 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "u8" );
    }
}

// A comparison yields bool, but its operands still have to agree - `i32 < u32` is the original
// bug D5 exists to catch, and it must not slip through just because the result is a bool.
TEST_CASE( "type_checker_types_comparisons_as_bool", "[sema][types]" )
{
    SECTION( "the result is bool, not the operand type" )
    {
        const Typed p( "bool f( i32 a, i32 b ) { return a < b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "bool" );
    }

    SECTION( "the operands are still subject to the table" )
    {
        const Typed p( "bool f( i32 a, u32 b ) { return a < b; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// D5 forbids int -> bool, so C's truthiness is gone. This is the sharpest break from C++ habit.
TEST_CASE( "type_checker_requires_a_bool_condition", "[sema][types]" )
{
    static const char* rejected[] = {
        "i32 f( i32 n ) { if( n ) { return 1; } return 0; }",
        "i32 f( i32 n ) { while( n ) { return 1; } return 0; }",
        "i32 f( i32 n ) { for( ; n; ) { return 1; } return 0; }",
    };

    for( const char* source : rejected )
    {
        const Typed p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    const Typed ok( "i32 f( i32 n ) { if( n != 0 ) { return 1; } return 0; }" );
    INFO( ok.rendered() );
    REQUIRE( ok.clean() );
}

TEST_CASE( "type_checker_checks_return_against_the_signature", "[sema][types]" )
{
    SECTION( "a lossy return is rejected" )
    {
        const Typed p( "u8 f( i32 n ) { return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "a widening return is fine" )
    {
        const Typed p( "i64 f( i32 n ) { return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a literal return takes the signature's type" )
    {
        const Typed p( "u64 f() { return 7; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u64" );
    }
}

TEST_CASE( "type_checker_checks_call_arguments", "[sema][types]" )
{
    SECTION( "a lossy argument is rejected" )
    {
        const Typed p( "i32 g( u8 x ) { return 0; }\ni32 main() { i32 big = 300; return g( big ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "too few arguments" )
    {
        const Typed p( "i32 g( i32 a, i32 b ) { return 0; }\ni32 main() { return g( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "too many arguments" )
    {
        const Typed p( "i32 g( i32 a ) { return 0; }\ni32 main() { return g( 1, 2 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "a call is typed as the return type, and literals adopt the parameter type" )
    {
        const Typed p( "u64 g( u64 x ) { return x; }\ni32 main() { u64 v = g( 7 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Call_expr, 0 ) ) == "u64" );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u64" );
    }
}

// The error type must absorb, the way Node_kind::Error does in the parser: one unresolved name
// produces one diagnostic, not one per enclosing expression.
TEST_CASE( "type_checker_does_not_cascade_from_an_error", "[sema][types]" )
{
    SECTION( "an unknown name is the resolver's error alone" )
    {
        const Typed p( "i32 main() { return unknown + 1 * 2; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "an unknown type is reported once" )
    {
        const Typed p( "i32 main() { Widget w = 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and a parse error does not reach the checker at all" )
    {
        const Typed p( "i32 main() { return 1 +; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

// D1: the rejected C++ spellings are ordinary identifiers to the lexer, and sema is what knows
// they were used in type position.
TEST_CASE( "type_checker_suggests_the_keel_spelling_for_a_c_type", "[sema][types]" )
{
    const Typed p( "i32 main() { int x = 1; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "i32" ) != std::string::npos );
}

TEST_CASE( "type_checker_rejects_a_lossy_assignment", "[sema][types]" )
{
    SECTION( "narrowing" )
    {
        const Typed p( "i32 main() { i64 wide = 1; i32 narrow = wide; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "re-signing" )
    {
        const Typed p( "i32 main() { i32 a = 1; u32 b = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "widening is accepted" )
    {
        const Typed p( "i32 main() { u32 a = 1; i64 b = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an assignment statement is checked like an initialiser" )
    {
        const Typed p( "i32 main() { u8 small = 1; i32 wide = 1; small = wide; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// D1's promise is that a C++ spelling is met with the Keel one, not a bare "unknown type".
TEST_CASE( "d1_suggests_a_keel_spelling_for_every_rejected_c_type", "[sema][types]" )
{
    struct Case
    {
        std::string_view cpp;
        std::string_view keel;
    };

    static const Case cases[] = {
        { "int", "i32" },
        { "unsigned", "u32" },
        { "short", "i16" },
        { "long", "i64" },
        { "char", "i8" },
        { "float", "f32" },
        { "double", "f64" },
        { "size_t", "u64" },
        { "uint8_t", "u8" },
        { "int64_t", "i64" },
    };

    for( const Case& c : cases )
    {
        INFO( c.cpp );
        REQUIRE( keel_spelling_for( c.cpp ) == c.keel );
    }

    SECTION( "and stays quiet when there is nothing to suggest" )
    {
        REQUIRE( keel_spelling_for( "Widget" ).empty() );
        REQUIRE( keel_spelling_for( "" ).empty() );

        // Already Keel spellings: these never reach the suggestion path, but suggesting a type
        // as a replacement for itself would be a bug worth catching if they did.
        REQUIRE( keel_spelling_for( "i32" ).empty() );
        REQUIRE( keel_spelling_for( "bool" ).empty() );
    }
}

// Every other test puts calls on the right of a `return`, which is how an entire category went
// unchecked: visit's default recursed with visit, so a statement-position expression never
// reached infer at all.
TEST_CASE( "type_checker_checks_expressions_in_statement_position", "[sema][types]" )
{
    SECTION( "a call as a statement is checked exactly like one in a return" )
    {
        const Typed as_statement( "i32 f( i32 a ) { return a; }\ni32 main() { f( 1, 2, 3 ); return 0; }\n" );
        const Typed as_value( "i32 f( i32 a ) { return a; }\ni32 main() { return f( 1, 2, 3 ); }\n" );

        INFO( "statement: " << as_statement.rendered() );
        REQUIRE( as_statement.errors() >= 1 );
        REQUIRE( as_statement.errors() == as_value.errors() );
    }

    SECTION( "and so is one inside a loop body" )
    {
        const Typed p( "i32 f( i32 a ) { return a; }\ni32 main() { while( true ) { f( 1, 2 ); } return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

TEST_CASE( "type_checker_checks_increment", "[sema][types]" )
{
    SECTION( "a numeric variable is fine" )
    {
        const Typed p( "i32 main() { i32 i = 0; i++; i--; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a parameter may be incremented" )
    {
        const Typed p( "i32 f( i32 n ) { n++; return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a bool is not numeric" )
    {
        const Typed p( "i32 main() { bool b = true; b++; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Reported before the operand is typed: a literal types as an error today, and absorbing on
    // that would let this pass in silence.
    SECTION( "the operand must be a variable" )
    {
        const Typed p( "i32 main() { 1++; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

TEST_CASE( "type_checker_rejects_assignment_to_a_non_variable", "[sema][types]" )
{
    SECTION( "a literal target" )
    {
        const Typed p( "i32 main() { 1 = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // The three targets below are all unassignable, but two of them are somebody else's error to
    // report. Counting exactly is what keeps the duplicates out.
    SECTION( "a function target reports once, as a function" )
    {
        const Typed p( "i32 f() { return 1; }\ni32 main() { f = 1; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "is a function" ) != std::string::npos );
    }

    SECTION( "an unknown target is the resolver's error alone" )
    {
        const Typed p( "i32 main() { unknown = 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "a parameter is assignable" )
    {
        const Typed p( "i32 f( i32 a ) { a = 1; return a; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D20: a code point is a u8, so it goes through exactly the machinery integer literals do.
TEST_CASE( "type_checker_types_a_character_literal_as_an_integer", "[sema][types]" )
{
    SECTION( "it adopts the annotated type, like any literal" )
    {
        const Typed p( "i32 main() { u8 c = 'A'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Char_literal, 0 ) ) == "u8" );
    }

    SECTION( "a wider integer works too" )
    {
        const Typed p( "i32 main() { u32 c = 'z'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Char_literal, 0 ) ) == "u32" );
    }

    SECTION( "with no context it falls back to u8" )
    {
        const Typed p( "i32 main() { auto c = 'A'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Char_literal, 0 ) ) == "u8" );
    }

    SECTION( "an escape past 127 still fits a u8" )
    {
        const Typed p( "i32 main() { u8 c = '\\xFF'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "but a bool is not an integer" )
    {
        const Typed p( "i32 main() { bool c = 'a'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "character literal" ) != std::string::npos );
    }

    SECTION( "and arithmetic on one is arithmetic on a u8" )
    {
        const Typed p( "u8 f( u8 c ) { return c; }\ni32 main() { u8 x = f( 'a' ); return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A string has no type until String exists (M7), so it must say so rather than vanish.
TEST_CASE( "type_checker_rejects_a_string_literal", "[sema][types]" )
{
    const Typed p( "i32 main() { u8 s = \"hello\"; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "string literals are not supported yet" ) != std::string::npos );
}

// A compound assignment is `x = x op y`, so the value is checked against the target rather than
// inferred. Getting that wrong is invisible until something is emitted: `u8 x; x += 3;` reports
// nothing suspicious, it just settles the literal on i32 and then refuses to store it back.
TEST_CASE( "type_checker_checks_compound_assignment_against_the_target", "[sema][types]" )
{
    SECTION( "a literal adopts the target's type" )
    {
        const Typed p( "i32 main() { u8 x = 1; x += 3; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "including at the edge of the target's range" )
    {
        const Typed p( "i32 main() { u8 x = 1; x += 255; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a value that does not fit the target is still rejected" )
    {
        const Typed p( "i32 main() { u8 x = 1; x += 256; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and so is a wider variable" )
    {
        const Typed p( "i32 main() { u8 x = 1; i32 y = 2; x += y; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "widening the other way is fine" )
    {
        const Typed p( "i32 main() { i64 w = 0; i32 a = 2; w += a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a bool has no arithmetic at all" )
    {
        const Typed p( "i32 main() { bool b = true; b += 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// A literal takes its type from the operand beside it. Without this, `u32 bits; bits != 0` compares
// a u32 with an i32 - which §6.4 rejects, and which no suffix exists to write around (D13). Almost
// no unsigned code would compile.
TEST_CASE( "type_checker_lets_a_literal_adopt_the_other_operand", "[sema][types]" )
{
    SECTION( "comparison against an unsigned variable" )
    {
        const Typed p( "i32 main() { u32 bits = 1; while( bits != 0 ) { bits = bits >> 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "on either side" )
    {
        const Typed p( "i32 main() { u8 b = 1; if( 0 < b ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and in arithmetic, not just comparison" )
    {
        const Typed p( "i32 main() { u64 n = 1; n = n + 1; n = n & 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Adopting is not the same as ignoring: the value still has to fit what it adopted.
    SECTION( "a literal that does not fit the adopted type is still rejected" )
    {
        const Typed p( "i32 main() { u8 b = 1; if( b == 300 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a negative literal cannot adopt an unsigned type" )
    {
        const Typed p( "i32 main() { u32 n = 1; if( n == -1 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Two literals have nothing to adopt from, so both take their defaults.
    SECTION( "two literals still take the default type" )
    {
        const Typed p( "i32 main() { if( 1 < 2 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a mismatch between two real types is still an error" )
    {
        const Typed p( "i32 main() { i32 a = 1; u32 b = 2; if( a < b ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// The same family as adopting from the operand beside it: a literal has a value, not a type, and
// every path that can supply one has to. These two were found by writing a program, not by
// inspecting the checker.
TEST_CASE( "type_checker_pushes_the_expected_type_through_to_literals", "[sema][types]" )
{
    SECTION( "a negated float literal adopts, like a negated integer one" )
    {
        const Typed p( "i32 main() { f32 a = -1.5; f64 b = -1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Float_literal, 0 ) ) == "f32" );
    }

    // Both operands are literals, so there is nothing beside them to adopt from and the context
    // is all there is.
    SECTION( "an operation between two literals takes the context's type" )
    {
        const Typed p( "i32 main() { u8 x = 'a' + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "u8" );
    }

    SECTION( "including when the context is a float" )
    {
        const Typed p( "i32 main() { f32 x = 1 + 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Adopting is not ignoring: each operand still has to fit what it adopted.
    SECTION( "an operand that does not fit the adopted type is rejected" )
    {
        const Typed p( "i32 main() { u8 x = 'a' + 300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // A comparison yields bool whatever it is handed, so pushing the expectation into it would be
    // nonsense - the operands must still meet each other, not the context.
    SECTION( "a comparison does not take the context's type" )
    {
        const Typed p( "i32 main() { bool b = 1 < 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a non-literal operand still forces a real match" )
    {
        const Typed p( "i32 main() { i32 a = 1; u8 x = a + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// C's main returns int and the emitted shim calls it with no arguments, so anything else either
// truncates silently or generates C that will not compile - a cc error pointing at generated code
// rather than a diagnostic pointing at the program.
TEST_CASE( "type_checker_constrains_the_signature_of_main", "[sema][types]" )
{
    SECTION( "i32 and no parameters is the one accepted form" )
    {
        const Typed p( "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "another integer type is rejected" )
    {
        const Typed p( "u64 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`main` must return `i32`" ) != std::string::npos );
    }

    SECTION( "so is void" )
    {
        const Typed p( "void main() { }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "parameters are rejected too - the shim passes none" )
    {
        const Typed p( "i32 main( i32 n ) { return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "parameters" ) != std::string::npos );
    }

    SECTION( "both wrong reports both" )
    {
        const Typed p( "u8 main( i32 n ) { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }

    // The unknown type is reported by type_of_annotation; saying `main` must return i32 on top of
    // that would be two messages for one mistake.
    SECTION( "an unresolved return type reports once, as the unknown type" )
    {
        const Typed p( "Widget main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "unknown type" ) != std::string::npos );
    }

    SECTION( "the rule is about main alone" )
    {
        const Typed p( "u64 helper( i32 a, i32 b ) { return 0; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A program without one is a library. The emitter writes no shim, and nothing here objects.
    SECTION( "no main at all is not an error" )
    {
        const Typed p( "u64 helper() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

} // namespace keel
#endif
