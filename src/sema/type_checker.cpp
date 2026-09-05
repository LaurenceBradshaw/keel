#include "sema/type_checker.h"
#include <fmt/format.h>
#include "lex/token.h"

#include <algorithm>
#include <unordered_set>

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

// What `cast` and `wrap` each mean for a pair of type kinds. They are not one operator with two
// spellings: `cast` preserves the value, `wrap` keeps the low bits. A conversion that is neither
// of those things belongs to neither operator.
//
// Absent pairs are refused, and the omissions are the point: float to int has no one obvious
// rounding, int to bool says less than `x != 0` does, and nothing converts to or from a struct.
enum class Conversion : u8
{
    None,
    Cast_only, // modular arithmetic has no meaning here
    Both,
    Unsafe, // a real conversion, but one that needs a gate Keel does not have yet
};

struct Conversion_rule
{
    Type_kind  from;
    Type_kind  to;
    Conversion allows;
};

constexpr Conversion_rule conversion_rules[] = {
    { Type_kind::Int, Type_kind::Int, Conversion::Both },
    { Type_kind::Int, Type_kind::Float, Conversion::Cast_only },
    { Type_kind::Float, Type_kind::Float, Conversion::Cast_only },
    { Type_kind::Bool, Type_kind::Int, Conversion::Cast_only },
    { Type_kind::Pointer, Type_kind::Pointer, Conversion::Unsafe },
};

// Remove when the KIR can emit the check. `cast` is defined to test the value at run time and
// nothing can do that yet, so a narrowing `cast` would silently truncate - which is `wrap`'s
// behaviour wearing `cast`'s name. Deleting this and the one branch that reads it is the whole
// change; no program that compiles today changes meaning when it goes.
constexpr bool k_narrowing_cast_needs_a_run_time_check = true;

Conversion conversion_for( Type_kind from, Type_kind to )
{
    for( const Conversion_rule& rule : conversion_rules )
    {
        if( rule.from == from && rule.to == to )
        {
            return rule.allows;
        }
    }

    return Conversion::None;
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
    void declare_signatures_struct_decls();
    void declare_signatures_field_decls();
    void declare_signatures_function_decls();

    void order_structs();
    bool contains_itself( Node_id decl, std::vector<Node_id>& path );

    // The main entry points for visiting and inferring types.
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

    Node_id find_field( Type_id type, Symbol_id name ) const;
    Type_id infer_field( Node_id id );

    // A composite constructor, not a value literal: its type is fixed by its name rather than
    // adopted from context, which is why it has no place in infer_literal.
    Type_id infer_struct_literal( Node_id id );

    Type_id infer_cast( Node_id id );

    // Walks an expression only for the errors inside it, in a context that has already failed.
    void absorb( Node_id id );

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
    std::vector<Type_id> types_;        // sized node_count(), invalid-filled, like bindings_ in Resolver
    std::vector<Node_id> struct_order_; // dependencies first; also the "already proved acyclic" set
    Type_id              current_return_;
};

Types Checker::run()
{
    types_.assign( ast_.node_count(), Type_id {} );
    declare_signatures();
    visit( ast_.root() );
    return Types( std::move( table_ ), std::move( types_ ), std::move( struct_order_ ) );
}

void Checker::declare_signatures()
{
    declare_signatures_struct_decls();
    declare_signatures_field_decls();
    order_structs();
    declare_signatures_function_decls();
}

void Checker::declare_signatures_struct_decls()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Struct_decl )
        {
            continue;
        }

        std::string_view name = interner_.text( Symbol_id { ast_.aux( child ) } );
        record( child, table_.structure( child, name ) );
    }
}

void Checker::declare_signatures_field_decls()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Struct_decl )
        {
            continue;
        }

        for( Node_id field : ast_.children( child ) )
        {
            if( ast_.kind( field ) != Node_kind::Field_decl )
            {
                continue;
            }

            record( field, type_of_annotation( ast_.children( field )[0] ) );
        }
    }
}

void Checker::declare_signatures_function_decls()
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

void Checker::order_structs()
{
    std::vector<Node_id> cycle_reported {};

    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Struct_decl )
        {
            continue;
        }

        bool already_reported = std::find( cycle_reported.begin(), cycle_reported.end(), child ) != cycle_reported.end();
        if( already_reported )
        {
            continue;
        }

        std::vector<Node_id> path;

        if( contains_itself( child, path ) )
        {
            // The route, so the message names how it closes rather than only that it does.
            std::string route;

            for( const Node_id node : path )
            {
                if( !route.empty() )
                {
                    route += " -> ";
                }

                route += interner_.text( Symbol_id { ast_.aux( node ) } );
            }

            error_at(
                ast_.span( child ),
                fmt::format( "`{}` contains itself, so it has no size", interner_.text( Symbol_id { ast_.aux( child ) } ) ),
                route
            );

            // Every struct on the cycle is reported by this one message. Without marking them all,
            // `A -> B -> A` is found again from B and reported twice for one mistake.
            for( const Node_id node : path )
            {
                cycle_reported.push_back( node );
            }
        }
    }
}

bool Checker::contains_itself( Node_id decl, std::vector<Node_id>& path )
{
    // Being in the order *is* being done: a struct lands there only once every struct it contains
    // has, so membership and "proved acyclic" are the same fact. A linear scan is right here -
    // this is the number of structs in one file.
    if( std::find( struct_order_.begin(), struct_order_.end(), decl ) != struct_order_.end() )
    {
        return false;
    }

    path.push_back( decl );

    for( Node_id field : ast_.children( decl ) )
    {
        if( ast_.kind( field ) != Node_kind::Field_decl )
        {
            continue;
        }

        // Read back what the field pass recorded. Calling type_of_annotation again would report
        // every unknown type and D1 suggestion a second time.
        const Type_id field_type = types_[field.v];

        // A pointer to a struct is finite, so only by-value containment counts - which is what
        // makes `struct Node { Node* next; }` legal once pointers arrive at M3.
        if( !field_type.is_valid() || table_.is_error( field_type ) || !table_.is_struct( field_type ) )
        {
            continue;
        }

        const Node_id field_decl = table_.get( field_type ).declaration;
        if( !field_decl.is_valid() || ast_.kind( field_decl ) != Node_kind::Struct_decl )
        {
            continue;
        }

        if( std::find( path.begin(), path.end(), field_decl ) != path.end() )
        {
            path.push_back( field_decl );
            return true;
        }

        if( contains_itself( field_decl, path ) )
        {
            return true;
        }
    }

    // The DFS post-order: every struct after everything it contains, which is exactly the order C
    // needs for by-value members. A struct on a cycle never reaches here, and never needs to - the
    // driver stops before emission when anything reported.
    struct_order_.push_back( decl );
    path.pop_back();
    return false;
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
    // A field is always assignable. `make().x = 2.0;` writes into a temporary and is therefore
    // useless, but it is legal C++ - a member of a class prvalue is an xvalue - and rejecting it
    // would need a notion of value categories that v0 does not otherwise have. D15 does not cover
    // it either: that rule asks whether an expression *kind* has an effect, and an assignment
    // always does. Pointless-but-harmless belongs to a future warning, not to the type checker.
    if( ast_.kind( id ) == Node_kind::Field_expr )
    {
        return true; // whether the object has fields at all is infer_field's question
    }

    if( ast_.kind( id ) == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( id ) ) == Token_kind::Star )
    {
        return true; // `*ptr = 42;` writes through the pointer. Whether ptr *is* one is infer_unary's question
    }

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
        absorb( value );
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

        absorb( target );
        absorb( value );
        return;
    }

    const Type_id target_type = infer( target );

    if( table_.is_error( target_type ) )
    {
        absorb( value );
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
    case Node_kind::Null_literal:
        return infer_literal( id );

    case Node_kind::String_literal:
        // Lexed and parsed, but a string has no type until there is a String, which is M7.
        // Silence here would make it look accepted.
        error_at( ast_.span( id ), "string literals are not supported yet" );
        return record( id, table_.builtin( Type_kind::Error ) );

    case Node_kind::Field_expr:
        return infer_field( id );

    case Node_kind::Struct_literal:
        return infer_struct_literal( id );

    case Node_kind::Cast_expr:
        return infer_cast( id );

    case Node_kind::Marker_expr:
        error_at( ast_.span( id ), "this expression is not supported yet" );
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
    case Node_kind::Null_literal:
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

    case Node_kind::Null_literal:
        error_at( ast_.span( id ), "cannot infer type of `nullptr`" );
        return record( id, table_.builtin( Type_kind::Error ) );

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
    {
        if( !table_.is_float( expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format( "expected `{}`, but got a floating-point literal", table_.name( expected ) ),
                table_.is_integer( expected ) ? "a fractional value cannot be an integer" : std::string {}
            );

            return expected;
        }

        const Literal_id value = Literal_id { ast_.aux( literal ) };

        // A literal the lexer could not scan has no value recorded. It reported there.
        if( value.is_valid() && !table_.fits_float( literals_.floating( value ), expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format(
                    "`{}{}` does not fit in `{}`",
                    literal == id ? "" : "-", // the sign lives in the Unary_expr, not in the value
                    literals_.floating( value ),
                    table_.name( expected )
                )
            );

            return expected;
        }

        break;
    }

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
    case Node_kind::Null_literal:
        if( !table_.is_pointer( expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format( "expected `{}`, but got a null literal", table_.name( expected ) ),
                "a null literal can only be assigned to a pointer"
            );

            return expected;
        }

        break;

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

        // A failed operand gives nothing to adopt, and check() absorbs an error expectation - so
        // the literal is carried along rather than asked to invent a type it has no basis for.
        const Type_id adopted = check( literal_side, known );

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

    // Pointer equality, which is how a null check is written. Handled before the rule table, whose
    // operand classes are all numeric or bool. Ordering is deliberately absent: comparing pointers
    // into different allocations is meaningless, and §6.4 has no row for them.
    if( table_.is_pointer( lhs_type ) || table_.is_pointer( rhs_type ) )
    {
        if( op != Token_kind::Equal_equal && op != Token_kind::Bang_equal )
        {
            return reject( {} );
        }

        if( lhs_type != rhs_type )
        {
            return reject( "only pointers of the same type can be compared" );
        }

        return record( id, bool_type );
    }

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

    // Neither of these is in the rule table: that table assumes the result type is the operand's,
    // and both of these change it.
    if( op == Token_kind::Amp )
    {
        // The operand must be somewhere a value lives. `&f()` names the address of a temporary
        // that is about to vanish, and `&1` names nothing at all. Asked after infer() so that
        // errors inside the operand are reported first.
        if( !is_assignable( ast_.children( id )[0] ) )
        {
            error_at( ast_.span( id ), "cannot take the address of this expression", "it does not name a variable" );

            return record( id, error );
        }

        return record( id, table_.pointer_to( operand_type ) );
    }

    if( op == Token_kind::Star )
    {
        if( !table_.is_pointer( operand_type ) )
        {
            error_at( ast_.span( id ), fmt::format( "`{}` cannot be dereferenced", table_.name( operand_type ) ) );

            return record( id, error );
        }

        return record( id, table_.get( operand_type ).element );
    }

    const Unary_rule* rule = unary_rule_for( op );

    if( rule == nullptr )
    {
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

Node_id Checker::find_field( Type_id type, Symbol_id name ) const
{
    const Node_id decl = table_.get( type ).declaration;

    if( !decl.is_valid() )
    {
        return Node_id();
    }

    for( const Node_id field : ast_.children( decl ) )
    {
        if( ast_.aux( field ) == name.v )
        {
            return field;
        }
    }

    return Node_id();
}

Type_id Checker::infer_field( Node_id id )
{
    const Node_id base      = ast_.children( id )[0];
    const Type_id base_type = infer( base );

    if( table_.is_error( base_type ) )
    {
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    if( !table_.is_struct( base_type ) )
    {
        error_at( ast_.span( base ), fmt::format( "`{}` has no fields", table_.name( base_type ) ) );
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    const Node_id decl = find_field( base_type, Symbol_id { ast_.aux( id ) } );
    if( !decl.is_valid() )
    {
        error_at(
            ast_.span( id ),
            fmt::format( "`{}` has no field `{}`", table_.name( base_type ), interner_.text( Symbol_id { ast_.aux( id ) } ) )
        );

        return record( id, table_.builtin( Type_kind::Error ) );
    }

    return record( id, types_[decl.v] );
}

Type_id Checker::infer_struct_literal( Node_id id )
{
    const Type_id error = table_.builtin( Type_kind::Error );

    // The resolver already bound the type name, so there is no scope lookup here - and if it
    // failed, it reported. Saying so again is the cascade the error type exists to prevent.
    const Node_id decl = resolution_.declaration_of( id );

    if( !decl.is_valid() || ast_.kind( decl ) != Node_kind::Struct_decl )
    {
        for( const Node_id init : ast_.children( id ) )
        {
            infer( ast_.children( init )[0] ); // type the values anyway
        }

        return record( id, error );
    }

    const std::span<const Node_id> initialisers = ast_.children( id );
    const std::span<const Node_id> fields       = ast_.children( decl );
    const std::string_view         struct_name  = interner_.text( Symbol_id { ast_.aux( decl ) } );
    const Type_id                  result       = types_[decl.v];

    // One convention per literal, as C++20 requires. Two in the same literal is a reader's
    // problem rather than a parser's.
    std::size_t named = 0;

    for( const Node_id init : initialisers )
    {
        named += Symbol_id { ast_.aux( init ) }.is_valid() ? 1 : 0;
    }

    if( named != 0 && named != initialisers.size() )
    {
        error_at(
            ast_.span( id ),
            "an initialiser list is either all positional or all named",
            "give every field a name, or none of them"
        );
    }

    if( named == 0 )
    {
        if( initialisers.size() != fields.size() )
        {
            error_at(
                ast_.span( id ),
                fmt::format(
                    "`{}` has {} field{}, but {} {} given",
                    struct_name,
                    fields.size(),
                    fields.size() == 1 ? "" : "s",
                    initialisers.size(),
                    initialisers.size() == 1 ? "was" : "were"
                )
            );
        }

        // The pairs that line up are checked even when the count is wrong: one missing value
        // should not hide a type error in the others.
        const std::size_t shared = std::min( initialisers.size(), fields.size() );

        for( std::size_t i = 0; i < shared; ++i )
        {
            // check, never infer - `Point { 1, 2 }` has to let those literals become f64.
            check( ast_.children( initialisers[i] )[0], types_[fields[i].v] );
        }

        for( std::size_t i = shared; i < initialisers.size(); ++i )
        {
            infer( ast_.children( initialisers[i] )[0] );
        }

        return record( id, result );
    }

    std::unordered_map<u32, Node_id> seen;

    for( const Node_id init : initialisers )
    {
        const Symbol_id name { ast_.aux( init ) };
        const Node_id   value = ast_.children( init )[0];

        if( !name.is_valid() )
        {
            absorb( value ); // the mixing error above already covered this one
            continue;
        }

        const Node_id field = find_field( result, name );

        if( !field.is_valid() )
        {
            error_at( ast_.span( init ), fmt::format( "`{}` has no field `{}`", struct_name, interner_.text( name ) ) );
            absorb( value );
            continue;
        }

        if( !seen.try_emplace( name.v, init ).second )
        {
            error_at( ast_.span( init ), fmt::format( "field `{}` is given twice", interner_.text( name ) ) );
        }

        check( value, types_[field.v] );
    }

    // Every field that is missing, not just the first: a struct that gained three fields should
    // say so once rather than over three compiles.
    for( const Node_id field : fields )
    {
        const Symbol_id name { ast_.aux( field ) };

        if( name.is_valid() && seen.find( name.v ) == seen.end() )
        {
            error_at( ast_.span( id ), fmt::format( "field `{}` is not initialised", interner_.text( name ) ) );
        }
    }

    return record( id, result );
}

Type_id Checker::infer_cast( Node_id id )
{
    const bool    is_cast = static_cast<Keyword>( ast_.aux( id ) ) == Keyword::Cast;
    const Type_id target  = type_of_annotation( ast_.children( id )[0] );
    const Node_id operand = ast_.children( id )[1];
    const Type_id error   = table_.builtin( Type_kind::Error );

    // A literal has a value and no type, so `cast` is the context that gives it one:
    // `cast<u8>( 300 )` is the ordinary out-of-range error rather than a conversion, and
    // `cast<f32>( 1 )` is simply an f32 literal. `wrap` must not do this - accepting a value the
    // target cannot hold is the entire point of it.
    const bool from_literal = is_cast && !table_.is_error( target ) && is_literal_expression( operand );

    const Type_id value = from_literal ? check( operand, target ) : infer( operand );

    if( table_.is_error( target ) || table_.is_error( value ) )
    {
        return record( id, error );
    }

    // check() has already ruled on the pair, and reported if it was wrong.
    if( from_literal )
    {
        return record( id, target );
    }

    const std::string_view name = is_cast ? "cast" : "wrap";

    const auto reject = [&]( std::string message, std::string help = {} )
    {
        error_at( ast_.span( id ), std::move( message ), std::move( help ) );

        return record( id, error );
    };

    switch( conversion_for( table_.get( value ).kind, table_.get( target ).kind ) )
    {
    case Conversion::None:
    {
        std::string help;

        if( table_.is_float( value ) && table_.is_integer( target ) )
        {
            help = "rounding is not implied; this needs an explicit rounding function";
        }
        else if( table_.is_integer( value ) && target == table_.builtin( Type_kind::Bool ) )
        {
            help = "compare it instead, as in `x != 0`";
        }

        return reject(
            fmt::format( "`{}` cannot convert `{}` to `{}`", name, table_.name( value ), table_.name( target ) ),
            std::move( help )
        );
    }

    case Conversion::Unsafe:
        return reject( "converting between pointer types is not supported yet" );

    case Conversion::Cast_only:
        if( !is_cast )
        {
            return reject(
                fmt::format( "`wrap` cannot convert `{}` to `{}`", table_.name( value ), table_.name( target ) ),
                "`wrap` keeps the low bits of an integer; use `cast` here"
            );
        }

        break;

    case Conversion::Both:
        // Narrowing is the one place the two operators disagree, and the check that makes `cast`
        // safe there does not exist yet.
        if( k_narrowing_cast_needs_a_run_time_check && is_cast && !table_.holds( value, target ) )
        {
            return reject(
                fmt::format( "`cast` cannot narrow `{}` to `{}` yet", table_.name( value ), table_.name( target ) ),
                fmt::format( "the run-time check is unimplemented; `wrap<{}>` truncates instead", table_.name( target ) )
            );
        }

        break;
    }

    return record( id, target );
}

Type_id Checker::check( Node_id id, Type_id expected )
{
    if( !id.is_valid() )
    {
        return expected;
    }

    if( table_.is_error( expected ) )
    {
        absorb( id );
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
    case Node_kind::Null_literal:
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
        // Check for user-defined types first
        const Node_id decl = resolution_.declaration_of( id );

        if( decl.is_valid() )
        {
            if( ast_.kind( decl ) != Node_kind::Struct_decl )
            {
                // Resolved to a function or variable of the same name.
                error_at(
                    ast_.span( id ), fmt::format( "`{}` is not a type", interner_.text( Symbol_id { ast_.aux( id ) } ) )
                );

                return table_.builtin( Type_kind::Error );
            }

            return types_[decl.v];
        }

        // Check for built-in types next

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

    case Node_kind::Pointer_type:
        return table_.pointer_to( type_of_annotation( ast_.children( id )[0] ) );

    case Node_kind::Generic_type:
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

// A literal is skipped: there is nothing inside one to be wrong, and its only complaint is that
// nothing told it what type to be - which is exactly what the error that got us here already
// explains. Inferring it anyway is how one mistake produces two diagnostics.
void Checker::absorb( Node_id id )
{
    if( id.is_valid() && !is_literal_expression( id ) )
    {
        infer( id );
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

TEST_CASE( "type_checker_types_struct_declarations", "[sema][types]" )
{
    SECTION( "a struct is a type, and a field carries its own" )
    {
        const Typed p( "struct Point { f64 x; u32 n; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Struct_decl, 0 ) ) == "Point" );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 0 ) ) == "f64" );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 1 ) ) == "u32" );
    }

    // The field pass has to reach into each struct: fields are children of the Struct_decl, not of
    // the root, so a loop over the root's children finds none of them and a bad field type becomes
    // invisible.
    SECTION( "an unknown field type is reported" )
    {
        const Typed p( "struct Point { Widget w; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "unknown type `Widget`" ) != std::string::npos );
    }

    SECTION( "D1 applies to a field type too" )
    {
        const Typed p( "struct Point { double x; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "f64" ) != std::string::npos );
    }

    // Struct types must all exist before any field type resolves: `Line` names `Point`, which is
    // declared below it.
    SECTION( "a field may name a struct declared later" )
    {
        const Typed p( "struct Line { Point a; Point b; };\nstruct Point { f64 x; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a struct is usable in a signature" )
    {
        const Typed p( "struct Point { f64 x; };\nPoint make( Point p ) { return p; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The resolver binds the name; if it bound to something that is not a struct, that is not a
    // type however good the spelling looks.
    SECTION( "a function name is not a type" )
    {
        const Typed p( "i32 Point() { return 0; }\ni32 f( Point p ) { return 0; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// A struct containing itself by value has infinite size. C's error for this is incomprehensible,
// and the emitter's topological sort would not terminate.
TEST_CASE( "type_checker_rejects_recursive_structs", "[sema][types]" )
{
    SECTION( "directly" )
    {
        const Typed p( "struct Node { Node next; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    // Exactly one message: every struct on the cycle is marked when it is found, or the same cycle
    // is rediscovered from B and reported twice for one mistake.
    SECTION( "and through another struct, reported once" )
    {
        const Typed p( "struct A { B b; };\nstruct B { A a; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a three-struct cycle is also one message" )
    {
        const Typed p( "struct A { B b; };\nstruct B { C c; };\nstruct C { A a; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Containment is what makes it infinite. A chain that does not close is fine however deep.
    SECTION( "a deep chain that does not close is fine" )
    {
        const Typed p( "struct A { f64 x; };\nstruct B { A a; };\nstruct C { B b; };\n"
                       "struct D { C c; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a struct used twice is not a cycle" )
    {
        const Typed p( "struct Point { f64 x; };\nstruct Line { Point a; Point b; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_types_field_access", "[sema][types]" )
{
    SECTION( "a field access has the field's type" )
    {
        const Typed p( "struct Point { f64 x; u32 n; };\n"
                       "f64 get( Point p ) { return p.x; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 0 ) ) == "f64" );
    }

    SECTION( "and is checked against its context" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "u32 get( Point p ) { return p.x; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "an unknown field is reported" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "f64 get( Point p ) { return p.z; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "z" ) != std::string::npos );
    }

    SECTION( "so is a field on something that is not a struct" )
    {
        const Typed p( "i32 f( i32 n ) { return n.x; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "access chains" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "struct Line { Point a; };\n"
                       "f64 get( Line l ) { return l.a.x; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        // The inner access is built first, so nth() sees `l.a` before `l.a.x`.
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 0 ) ) == "Point" );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 1 ) ) == "f64" );
    }

    // One diagnostic for one mistake: an object that failed to type has nothing to look a field up
    // in, and saying so again would be the cascade the error type exists to prevent.
    SECTION( "a field on an unresolved name reports once" )
    {
        const Typed p( "i32 main() { auto v = missing.x; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 ); // the resolver already reported the name
    }

    // Useless - the temporary is discarded - but legal, as it is in C++. Rejecting it would need
    // value categories, which v0 does not have.
    SECTION( "a field of a temporary is still assignable" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "Point make() { return Point { 1.0 }; }\n"
                       "i32 main() { make().x = 2.0; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a field is assignable" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "f64 bump( Point p ) { p.x = 1.0; return p.x; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_types_struct_literals", "[sema][types]" )
{
    constexpr std::string_view point = "struct Point { f64 x; f64 y; };\n";

    SECTION( "positional initialisers match the fields in order" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { 1.0, 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Struct_literal, 0 ) ) == "Point" );
    }

    SECTION( "designated initialisers match by name" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { .x = 1.0, .y = 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Struct_literal, 0 ) ) == "Point" );
    }

    // The values are *checked*, not inferred, so a literal adopts the field's type. Inferring
    // would settle `1` on i32 and then refuse to store it in an f64 field.
    SECTION( "a value adopts the field's type" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { 1, 2 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "f64" );
    }

    SECTION( "a value that does not fit the field is rejected" )
    {
        const Typed p( "struct Small { u8 v; };\ni32 main() { auto q = Small { 300 }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and so is one of the wrong type" )
    {
        const Typed p( std::string( point ) + "i32 main() { bool b = true; auto q = Point { b, 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "too few and too many both report" )
    {
        for( const char* tail :
             { "i32 main() { auto q = Point { 1.0 }; return 0; }", "i32 main() { auto q = Point { 1.0, 2.0, 3.0 }; return 0; }"
             } )
        {
            const Typed p( std::string( point ) + tail );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() >= 1 );
        }
    }

    SECTION( "an unknown field name is reported" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { .z = 1.0, .y = 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "a repeated field name is reported" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { .x = 1.0, .x = 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    // Every field must be given, so adding one to a struct errors at each construction site rather
    // than silently zeroing.
    SECTION( "a missing field is reported" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { .x = 1.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    // As C++20 forbids. Two conventions in one literal is a reader's problem, not a parser's.
    SECTION( "positional and designated may not be mixed" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { 1.0, .y = 2.0 }; return 0; }" );

        INFO( p.rendered() );

        // The message, not just a count: with the mixing rule removed the literal is read as
        // named, `x` goes uninitialised, and a different error keeps the count above zero.
        REQUIRE( p.rendered().find( "all positional or all named" ) != std::string::npos );
    }

    SECTION( "an empty struct takes an empty literal" )
    {
        const Typed p( "struct Empty { };\ni32 main() { auto q = Empty { }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "literals nest" )
    {
        const Typed p( "struct Point { f64 x; };\nstruct Line { Point a; Point b; };\n"
                       "i32 main() { auto l = Line { Point { 1.0 }, Point { 2.0 } }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and can be passed, returned and assigned" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "Point echo( Point p ) { return p; }\n"
                       "i32 main() { auto a = Point { 1.0 }; auto b = echo( a ); a = b; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "but there is no arithmetic on one" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "i32 main() { auto a = Point { 1.0 }; auto b = Point { 2.0 }; auto c = a + b; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// The same rule integer literals get: a value is measured against the type it is being given.
TEST_CASE( "type_checker_range_checks_float_literals", "[sema][types]" )
{
    SECTION( "a value beyond f32's range is rejected" )
    {
        const Typed p( "i32 main() { f32 x = 1e40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit in `f32`" ) != std::string::npos );
    }

    // The sign lives in the Unary_expr, not in the recorded value, so the message has to put it
    // back or it names a number the author did not write.
    SECTION( "and the message keeps the sign" )
    {
        const Typed p( "i32 main() { f32 x = -1e40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`-1e+40`" ) != std::string::npos );
    }

    SECTION( "a value at the edge is accepted" )
    {
        const Typed p( "i32 main() { f32 x = 3.4e38; f32 y = -3.4e38; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an inexact value is accepted - range is not representability" )
    {
        const Typed p( "i32 main() { f32 x = 0.1; f32 y = 1e-50; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "f64 takes anything the lexer let through" )
    {
        const Typed p( "i32 main() { f64 x = 1e300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Reported by the lexer, which is the only place the digits still exist. Sema records no value
    // for it and must not report a second time.
    SECTION( "a value beyond f64 reports exactly once, from the lexer" )
    {
        const Typed p( "i32 main() { f64 x = 1e400; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.rendered().find( "out of range" ) != std::string::npos );
    }

    // This regressed when fits_float replaced the is_float() guard: every float-into-integer
    // assignment silently compiled.
    SECTION( "a float literal still cannot be given to an integer" )
    {
        const Typed p( "i32 main() { i32 x = 1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "floating-point literal" ) != std::string::npos );
        REQUIRE( p.rendered().find( "cannot be an integer" ) != std::string::npos );
    }

    SECTION( "nor to a bool" )
    {
        const Typed p( "i32 main() { bool b = 1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// Parsed, but with no meaning until M3 gives types destructors and M4 gives them references.
// Silence here would make them look accepted.
TEST_CASE( "type_checker_rejects_passing_markers_for_now", "[sema][types]" )
{
    for( const char* source : {
             "i32 g( i32 a ) { return a; }\ni32 main() { i32 b = 1; return g( move b ); }\n",
             "i32 g( i32 a ) { return a; }\ni32 main() { i32 b = 1; return g( ref b ); }\n",
             "i32 g( i32 a ) { return a; }\ni32 main() { i32 b = 1; return g( out b ); }\n",
         } )
    {
        const Typed p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "not supported yet" ) != std::string::npos );
    }
}

TEST_CASE( "type_checker_types_pointers", "[sema][types]" )
{
    SECTION( "an annotation, an address and a dereference" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32* q = &v; i32 w = *q; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 1 ) ) == "i32*" );
        REQUIRE( p.type_name( p.nth( Node_kind::Unary_expr, 0 ) ) == "i32*" ); // &v
        REQUIRE( p.type_name( p.nth( Node_kind::Unary_expr, 1 ) ) == "i32" );  // *q
    }

    SECTION( "pointers nest" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32* q = &v; i32** r = &q; i32 w = **r; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 2 ) ) == "i32**" );
    }

    SECTION( "and point at structs" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "i32 main() { auto s = Point { 1.0 }; Point* q = &s; f64 v = (*q).x; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The address of a temporary names storage that is about to vanish, and a literal has no
    // storage at all.
    SECTION( "the address of something that is not a place is rejected" )
    {
        for( const char* tail : { "i32 v = &f(); return 0;", "i32 v = &1; return 0;", "i32 v = &( 1 + 2 ); return 0;" } )
        {
            const Typed p( std::string( "i32 f() { return 1; }\ni32 main() { " ) + tail + " }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() >= 1 );
            REQUIRE( p.rendered().find( "cannot take the address" ) != std::string::npos );
        }
    }

    SECTION( "the address of a field is fine - a field is a place" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "i32 main() { auto s = Point { 1.0 }; f64* q = &s.x; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "dereferencing something that is not a pointer is rejected" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32 w = *v; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot be dereferenced" ) != std::string::npos );
    }

    SECTION( "writing through a pointer" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32* q = &v; *q = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Identity only: no void*, no conversion between pointee types.
    SECTION( "pointer types do not convert" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32* q = &v; u8* r = q; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "nor to or from an integer" )
    {
        const Typed a( "i32 main() { i32 v = 1; i32* q = &v; i64 n = q; return 0; }" );
        const Typed b( "i32 main() { i64 n = 1; i32* q = n; return 0; }" );

        INFO( a.rendered() << b.rendered() );
        REQUIRE( a.errors() == 1 );
        REQUIRE( b.errors() == 1 );
    }

    // D27. Arithmetic belongs to a many-item pointer, which v0 does not have.
    SECTION( "there is no pointer arithmetic" )
    {
        for( const char* tail : { "i32* r = q + 1;", "i32* r = q - 1;", "i32 n = q * 2;" } )
        {
            const Typed p( std::string( "i32 main() { i32 v = 1; i32* q = &v; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() >= 1 );
        }
    }

    SECTION( "a pointer can be passed and returned" )
    {
        const Typed p( "i32 read( i32* q ) { return *q; }\n"
                       "i32 main() { i32 v = 1; return read( &v ); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D26: a literal whose type comes from context, exactly like an integer literal.
TEST_CASE( "type_checker_types_nullptr", "[sema][types]" )
{
    SECTION( "it adopts the annotated pointer type" )
    {
        const Typed p( "i32 main() { i32* q = nullptr; u8* r = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Null_literal, 0 ) ) == "i32*" );
        REQUIRE( p.type_name( p.nth( Node_kind::Null_literal, 1 ) ) == "u8*" );
    }

    SECTION( "with no context there is nothing to adopt" )
    {
        const Typed p( "i32 main() { auto q = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and it is not a value of any other type" )
    {
        for( const char* tail : { "i32 v = nullptr;", "bool b = nullptr;", "f64 d = nullptr;" } )
        {
            const Typed p( std::string( "i32 main() { " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    SECTION( "it can be passed to a pointer parameter" )
    {
        const Typed p( "i32 take( i32* q ) { return 0; }\ni32 main() { return take( nullptr ); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The conversion table. `cast` preserves the value and `wrap` keeps the low bits, so the two
// accept genuinely different sets - a cell that both allow, or that neither does, is the
// interesting part rather than an accident.
TEST_CASE( "type_checker_allows_the_conversions_in_the_table", "[sema][types][cast]" )
{
    SECTION( "cast widens an integer" )
    {
        const Typed p( "i32 main() { i32 x = 1; i64 y = cast<i64>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Cast_expr, 0 ) ) == "i64" );
    }

    SECTION( "cast converts an integer to a float" )
    {
        const Typed p( "i32 main() { i32 x = 1; f32 y = cast<f32>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Precision loss, but a float that cannot hold the value gives an infinity rather than a
    // plausible wrong number, so this is not the case narrowing is held back for.
    SECTION( "cast narrows a float" )
    {
        const Typed p( "i32 main() { f64 d = 1.5; f32 y = cast<f32>( d ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "cast converts a bool to an integer" )
    {
        const Typed p( "i32 main() { bool b = true; i32 y = cast<i32>( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "wrap narrows an integer" )
    {
        const Typed p( "i32 main() { i32 x = 300; u8 y = wrap<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Cast_expr, 0 ) ) == "u8" );
    }

    // Losing the sign is exactly what wrap is for, and exactly what cast must refuse.
    SECTION( "wrap reinterprets the sign" )
    {
        const Typed p( "i32 main() { i32 x = 0 - 1; u32 y = wrap<u32>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "wrap widens too, and simply never wraps" )
    {
        const Typed p( "i32 main() { i32 x = 1; i64 y = wrap<i64>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_rejects_the_conversions_outside_the_table", "[sema][types][cast]" )
{
    // No one obvious rounding, so neither operator picks one.
    SECTION( "neither converts a float to an integer" )
    {
        for( const char* tail : { "i32 y = cast<i32>( d );", "i32 y = wrap<i32>( d );" } )
        {
            const Typed p( std::string( "i32 main() { f64 d = 1.5; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "rounding" ) != std::string::npos );
        }
    }

    // `x != 0` says it better, and modular arithmetic down to one bit says something else again.
    SECTION( "neither converts an integer to a bool" )
    {
        for( const char* tail : { "bool y = cast<bool>( x );", "bool y = wrap<bool>( x );" } )
        {
            const Typed p( std::string( "i32 main() { i32 x = 1; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    SECTION( "wrap refuses anything that is not integer to integer" )
    {
        for( const char* source :
             { "i32 main() { i32 x = 1; f32 y = wrap<f32>( x ); return 0; }",
               "i32 main() { bool b = true; i32 y = wrap<i32>( b ); return 0; }",
               "i32 main() { f64 d = 1.5; f32 y = wrap<f32>( d ); return 0; }" } )
        {
            const Typed p( source );

            INFO( source << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "`wrap` cannot convert" ) != std::string::npos );
        }
    }

    SECTION( "neither touches a struct" )
    {
        for( const char* tail : { "i32 y = cast<i32>( p );", "i32 y = wrap<i32>( p );" } )
        {
            const Typed p( std::string( "struct P { i32 x; };\ni32 main() { P p = P { 1 }; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    SECTION( "converting to a struct is refused too" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { i32 x = 1; P y = cast<P>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // A real conversion, held back for the unsafe gate rather than rejected as nonsense - so it
    // gets its own message.
    SECTION( "a pointer conversion is not supported yet" )
    {
        const Typed p( "i32 main() { i32 x = 1; i32* q = &x; u8* r = cast<u8*>( q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "not supported yet" ) != std::string::npos );
    }

    SECTION( "an unknown target type is reported once" )
    {
        const Typed p( "i32 main() { i32 x = 1; return cast<Nope>( x ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// Narrowing is the one cell where the two operators disagree, and `cast` is defined to check the
// value at run time. Nothing can do that yet, so it is refused rather than silently truncating -
// which would be `wrap`'s behaviour under `cast`'s name. Delete these when the check exists.
TEST_CASE( "type_checker_holds_back_a_narrowing_cast", "[sema][types][cast]" )
{
    SECTION( "narrowing the width" )
    {
        const Typed p( "i32 main() { i32 x = 300; u8 y = cast<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot narrow" ) != std::string::npos );
        REQUIRE( p.rendered().find( "wrap<u8>" ) != std::string::npos );
    }

    // u32 cannot hold a negative i32, so this narrows even though the widths match.
    SECTION( "changing the signedness" )
    {
        const Typed p( "i32 main() { i32 x = 1; u32 y = cast<u32>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot narrow" ) != std::string::npos );
    }

    SECTION( "but wrap says the same thing and is allowed" )
    {
        const Typed p( "i32 main() { i32 x = 300; u8 y = wrap<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A literal has a value and no type, so `cast` is the context that gives it one rather than a
// conversion applied after the fact. `wrap` must not do this: taking a value the target cannot
// hold is the whole point of it.
TEST_CASE( "type_checker_gives_a_literal_its_type_from_a_cast", "[sema][types][cast]" )
{
    SECTION( "an in-range literal simply becomes the target type" )
    {
        const Typed p( "i32 main() { u8 y = cast<u8>( 200 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u8" );
    }

    // Not "cast cannot narrow": there is nothing to narrow, the literal never had a wider type.
    SECTION( "an out-of-range literal is a range error" )
    {
        const Typed p( "i32 main() { u8 y = cast<u8>( 300 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit" ) != std::string::npos );
    }

    SECTION( "an integer literal can be cast to a float" )
    {
        const Typed p( "i32 main() { f32 y = cast<f32>( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "f32" );
    }

    SECTION( "a negated literal keeps the asymmetric range" )
    {
        const Typed p( "i32 main() { i8 y = cast<i8>( -128 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The literal is inferred first, then wrapped - so this is 44, not an error.
    SECTION( "wrap takes the literal at its default type instead" )
    {
        const Typed p( "i32 main() { u8 y = wrap<u8>( 300 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "i32" );
    }

    SECTION( "a float literal cannot be cast to an integer either" )
    {
        const Typed p( "i32 main() { i32 y = cast<i32>( 1.5 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// The result has a type of its own, so it composes like any other expression - and must not be
// mistaken for a literal by the code that gives literals their type from context.
TEST_CASE( "type_checker_uses_a_conversion_as_an_ordinary_expression", "[sema][types][cast]" )
{
    SECTION( "as an operand" )
    {
        const Typed p( "i32 main() { i32 x = 1; i64 y = cast<i64>( x ) + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "i64" );
    }

    SECTION( "as an argument" )
    {
        const Typed p( "i32 take( u8 v ) { return 0; }\n"
                       "i32 main() { i32 x = 300; return take( wrap<u8>( x ) ); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The result is a value, not a place, so it cannot be assigned through.
    SECTION( "but it is not assignable" )
    {
        const Typed p( "i32 main() { i32 x = 1; cast<i64>( x ) = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot assign to this expression" ) != std::string::npos );
    }

    SECTION( "the result type is what the annotation says, not the operand's" )
    {
        const Typed p( "i32 main() { i32 x = 1; u8 y = wrap<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Cast_expr, 0 ) ) == "u8" );
    }
}

// One mistake, one diagnostic. A literal has a value and no type, so in a context that has already
// failed there is nothing for it to adopt from - and asking it anyway makes it report that, on top
// of the error that is the actual cause. `nullptr` is the one that shows this, because it is the
// only literal with no default type to fall back on.
TEST_CASE( "type_checker_absorbs_a_literal_beside_a_failed_operand", "[sema][types]" )
{
    SECTION( "comparison, literal on the right" )
    {
        const Typed p( "i32 main() { if ( nope() != nullptr ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
        REQUIRE( p.rendered().find( "`nope` is not declared" ) != std::string::npos );
    }

    // The operand order must not change which diagnostics appear.
    SECTION( "comparison, literal on the left" )
    {
        const Typed p( "i32 main() { if ( nullptr != nope() ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
    }

    SECTION( "assigning to something that is not a place" )
    {
        const Typed p( "i32* get( i32* q ) { return q; }\n"
                       "i32 main() { i32 x = 1; get( &x ) = nullptr; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot assign to this expression" ) != std::string::npos );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
    }

    SECTION( "assigning to a name that does not resolve" )
    {
        const Typed p( "i32 main() { nope = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
    }

    SECTION( "returning a value from a void function" )
    {
        const Typed p( "void f() { return nullptr; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
    }

    SECTION( "an initialiser for a field that does not exist" )
    {
        const Typed p( "struct P { i32* q; };\ni32 main() { auto p = P { .bad = nullptr }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );

        // Two errors, but two *facts*: the name is wrong, and so the real field went uninitialised.
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "has no field `bad`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "field `q` is not initialised" ) != std::string::npos );
    }
}

// The other half of absorbing: a literal that genuinely has nothing to adopt from still has to say
// so, and one that is simply out of range must not be waved through. Absorbing too eagerly would
// silence both.
TEST_CASE( "type_checker_still_reports_a_literal_with_no_context", "[sema][types]" )
{
    SECTION( "auto has nothing to give a null literal" )
    {
        const Typed p( "i32 main() { auto p = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot infer type of `nullptr`" ) != std::string::npos );
    }

    SECTION( "a null literal against a real type is still wrong" )
    {
        const Typed p( "i32 main() { i32 v = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "range checking survives" )
    {
        const Typed p( "i32 main() { u8 x = 300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit" ) != std::string::npos );
    }

    SECTION( "a literal target is still not assignable" )
    {
        const Typed p( "i32 main() { 1 = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot assign to this expression" ) != std::string::npos );
    }

    // The reason infer_binary pushes the expectation at all: without it these compare a u32 or a u8
    // against an i32, which §6.4 rejects and no suffix can write around.
    SECTION( "and a literal beside a good operand still adopts from it" )
    {
        const Typed a( "i32 main() { u32 bits = 3; if ( bits != 0 ) { return 1; } return 0; }" );
        const Typed b( "i32 main() { u8 i = 97 + 1; return 0; }" );

        INFO( a.rendered() << b.rendered() );
        REQUIRE( a.clean() );
        REQUIRE( b.clean() );
    }
}

} // namespace keel
#endif
