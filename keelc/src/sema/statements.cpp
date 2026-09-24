#include "sema/statements.h"

#include <fmt/format.h>

#include <optional>

#include "lex/token.h"
#include "sema/type_checker.h"

// The statement and declaration walk, one member per construct. statements.h says why this is the
// only class that may visit, and why the loop over a `switch`'s arms is therefore here.

namespace keel
{
namespace sema
{

void Statements::visit( Node_id id )
{
    if( !id.is_valid() )
    {
        return;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Error:
        return;

    case Node_kind::Source_file:
    {
        for( const Node_id child : ast_.children( id ) )
        {
            // A file-scope variable is checked differently from a local: its type came from the
            // signature pass, and its initialiser must be a literal. Everything else - functions
            // above all - takes the ordinary path, which is what this case replaced when it took
            // over from the recursing default.
            if( ast_.kind( child ) == Node_kind::Var_decl )
            {
                visit_global( child );
                continue;
            }

            visit( child );
        }

        return;
    }
    case Node_kind::Block:
        return visit_block( id );

    case Node_kind::Expr_stmt:
        expressions_.infer( ast_.child( id, 0 ) );
        return; // discard the result. D15 already made effectless expressions a parse error

    case Node_kind::Function_decl:
    case Node_kind::Destructor_decl:
    case Node_kind::Constructor_decl:
    case Node_kind::Method_decl:
        return visit_function( id );

    case Node_kind::Return_stmt:
        return visit_return( id );

    case Node_kind::Var_decl:
        return visit_var( id );

    case Node_kind::Assign_stmt:
        return visit_assign( id );

    case Node_kind::If_stmt:
        return visit_if( id );

    case Node_kind::Switch_stmt:
        return visit_switch( id );

    case Node_kind::While_stmt:
        return visit_while( id );

    case Node_kind::For_stmt:
        return visit_for( id );

    case Node_kind::Increment_stmt:
        return visit_increment( id );

    case Node_kind::Break_stmt:
        if( breakable_depth_ == 0 )
        {
            reporter_.error_at(
                ast_.span( id ),
                "`break` outside a loop or switch body",
                "`break` can only appear inside a `while`, `for` or `switch`"
            );
        }
        return;
    case Node_kind::Fallthrough_stmt:
        // Coverage::check_arm_structure runs before an arm's body is visited, so anything unrecorded
        // by now was not written where a `fallthrough` may go.
        if( !coverage_.ruled_fallthrough( id ) )
        {
            reporter_.error_at(
                ast_.span( id ),
                "`fallthrough` must be the last statement of a `case`",
                "it cannot be nested inside a block, a loop or an `if`"
            );
        }
        return;

    case Node_kind::Continue_stmt:
        if( loop_depth_ == 0 )
        {
            reporter_.error_at(
                ast_.span( id ), "`continue` outside a loop body", "`continue` can only appear inside a `while` or `for`"
            );
        }
        return;

    default:
        // Block, and every statement not yet given a case of its own.
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        return;
    }
}

void Statements::visit_function( Node_id id )
{
    // Children are { return_type, param_list, body }. The declaration pass already typed the first
    // two, so only the body is left.
    //
    // Saved and restored rather than plainly assigned: M6 brings lambdas, and a nested one would
    // otherwise leave the enclosing function checking its returns against the wrong type.
    const Type_id enclosing_return   = current_return_;
    const Node_id enclosing_function = expressions_.enter_function( id );

    current_return_ = types_.type_of( id );
    visit( ast_.child( id, 2 ) );
    current_return_ = enclosing_return;
    expressions_.leave_function( enclosing_function );
}

void Statements::visit_return( Node_id id )
{
    const Node_id value     = ast_.child( id, 0 );
    const Type_id void_type = table_.builtin( Type_kind::Void );

    if( !value.is_valid() ) // a bare `return;`
    {
        if( current_return_.is_valid() && current_return_ != void_type )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format( "this function returns `{}`, so `return` needs a value", table_.name( current_return_ ) )
            );
        }

        return;
    }

    if( current_return_ == void_type )
    {
        // Typed anyway: a mistake inside the expression is still a mistake worth reporting.
        expressions_.absorb( value );
        reporter_.error_at( ast_.span( value ), "a `void` function cannot return a value" );
        return;
    }

    expressions_.check( value, current_return_ );

    // §8's non-escaping rule. A `ref` binding is initialised at its declaration and never reseated,
    // so everything nameable at a call site outlives any binding declared there - which is why
    // *which* parameter the result came from does not matter, and why this needs no dataflow.
    // Syntactic: trace the returned expression to its root and require a parameter that travels by
    // address. A bare parameter of an owning type is one, since the caller owns the object.
    if( places_.returns_a_binding( expressions_.current_function() ) )
    {
        const Node_id root = places_.place_root( value, expressions_.current_function() );

        if( !root.is_valid() || ast_.kind( root ) != Node_kind::Param_decl || !places_.returns_a_binding( root ) )
        {
            reporter_.error_at(
                ast_.span( value ),
                "a returned reference must borrow from a parameter",
                "anything else here dies when this function returns"
            );
        }
    }

    // The predicate rather than check_writable: that one reports about *modifying*, and nothing
    // is being modified here. Gated on the return type owning something, because that is the whole
    // hazard - the caller destroys the borrow too, so returning it frees one resource twice.
    if( !bounds_.satisfies( current_return_, Bound::Copyable ) &&
        places_.is_borrow_binding( places_.place_root( value, expressions_.current_function() ) ) )
    {
        reporter_.error_at( ast_.span( value ), "cannot return a borrowed value", "the caller still owns it" );
    }
}

void Statements::visit_var( Node_id id )
{
    // Children are { type, init }. Either can be invalid: no annotation means `auto`, and no
    // initialiser means the variable is only declared.
    const Node_id annotation = ast_.child( id, 0 );
    const Node_id init       = ast_.child( id, 1 );
    const Node_id spelled    = unwrap_const( ast_, annotation );
    const Keyword mode       = spelled.is_valid() && ast_.kind( spelled ) == Node_kind::Mode_type
                                   ? static_cast<Keyword>( ast_.aux( spelled ) )
                                   : Keyword::Count;

    Type_id type = annotations_.type_of( annotation );

    if( type.is_valid() )
    {
        // Annotated. Checking rather than inferring is what lets a literal adopt the declared
        // type, so `u32 x = 42;` needs no suffix. An unknown annotation is the error type, which
        // check() absorbs - one diagnostic, from Annotations::type_of.
        if( init.is_valid() )
        {
            expressions_.check( init, type );
            // Not for a binding: `ref B r = a;` copies nothing and gives nothing away, so D31 has
            // no transfer to ask about. Only the `auto` path below can skip the test outright.
            if( mode != Keyword::Ref )
            {
                places_.check_owning_source( init, type );
            }
        }
    }
    else if( init.is_valid() )
    {
        type = expressions_.infer( init ); // `auto`: L6's one form of inference
        places_.check_owning_source( init, type );
    }
    else
    {
        reporter_.error_at( ast_.span( id ), "`auto` needs an initialiser to infer from" );
        type = table_.builtin( Type_kind::Error );
    }

    // Both are modes a *parameter* carries, and neither says anything a local could mean - so the
    // message is shared and the help is not, because the reason differs: `move` is about who owns
    // the value, `out` about who writes it.
    if( mode == Keyword::Move || mode == Keyword::Out )
    {
        reporter_.error_at(
            ast_.span( annotation ),
            fmt::format( "`{}` is not a binding mode", interner_.text( interner_.keyword( mode ) ) ),
            mode == Keyword::Move ? "a local already owns what it holds; write the type on its own"
                                  : "`out` is how a callee assigns a caller's variable; write the type on its own"
        );
    }
    else if( mode == Keyword::Ref && !table_.is_error( type ) )
    {
        // A binding is initialised at its declaration and never reseated - which is the whole of
        // why returning one needs no lifetimes, so it is not an optional half of the rule.
        if( !init.is_valid() )
        {
            reporter_.error_at( ast_.span( id ), "a `ref` binding must be initialised", "write `ref T r = x;`" );
        }
        // Not a temporary: the referent has to outlive the binding, and everything that names a
        // place was declared before this line and so outlives it. A temporary is the one thing
        // that would not, and refusing it is what keeps the rule free of any analysis.
        else if( !places_.is_assignable( init ) )
        {
            reporter_.error_at( ast_.span( init ), "a `ref` binding needs a variable to bind to" );
        }
        else if( types_.type_of( init ) != type )
        {
            reporter_.error_at(
                ast_.span( init ),
                fmt::format( "cannot borrow `{}` as `ref {}`", table_.name( types_.type_of( init ) ), table_.name( type ) ),
                "a borrow is the variable itself, so its type must match exactly"
            );
        }
        // Only a *mutable* binding would launder a read-only one. A `const ref` onto a const is
        // exactly what the const half buys, so it skips the test rather than failing it.
        else if( is_const_binding( ast_, id ) || places_.check_writable( init, expressions_.current_function() ) )
        {
            places_.record_binding_address( annotation, type );
        }
    }

    types_.record( id, type );
}

void Statements::visit_assign( Node_id id )
{
    // Children are { target, value }; aux is the operator, which may be `+=` rather than `=`.
    const Token_kind op     = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    target = ast_.child( id, 0 );
    const Node_id    value  = ast_.child( id, 1 );

    // Asked before infer(), which would otherwise absorb it: a literal target types as an error
    // today, so `1 = 2;` passed in silence. Parameters are assignable too - L12 is
    // mutable-by-default, and a parameter is a local.
    if( !places_.is_assignable( target ) )
    {
        // A name that does not resolve, or resolves to a function, is already reported by
        // infer_name; a failed subtree was reported by whichever rule produced it. Anything else -
        // a literal, a call, an arithmetic expression - has nothing else to report it.
        if( ast_.kind( target ) != Node_kind::Name_expr && ast_.kind( target ) != Node_kind::Error )
        {
            reporter_.error_at( ast_.span( target ), "cannot assign to this expression" );
        }

        // `this` is the exception to the rule above: it resolves, so infer_name says nothing, and
        // the guard there stays quiet because it is a Name_expr. Nothing else would report it.
        const Node_id decl = resolution_.declaration_of( target );
        if( decl.is_valid() && ast_.kind( decl ) == Node_kind::Param_decl &&
            Symbol_id { ast_.aux( decl ) } == Interner::keyword( Keyword::This ) )
        {
            reporter_.error_at( ast_.span( target ), "`this` is immutable" );
        }

        expressions_.absorb( target );
        expressions_.absorb( value );
        return;
    }

    const Type_id target_type = expressions_.infer( target );

    if( table_.is_error( target_type ) )
    {
        expressions_.absorb( value );
        return;
    }

    // After infer(), not before it: place_root has to ask whether a field's object is a pointer,
    // and nothing has typed it until here.
    if( !places_.check_writable( target, expressions_.current_function() ) )
    {
        expressions_.absorb( value );
        return;
    }

    if( op == Token_kind::Equal )
    {
        expressions_.check( value, target_type );
        places_.check_owning_source( value, target_type );
        return;
    }

    // A compound assignment is `x = x op y`, so the value is *checked* against the target rather
    // than inferred. Inferring it would settle a literal on its default type first, and `u8 x;
    // x += 3;` would then combine u8 with i32 and have nowhere to put the result.
    //
    // Checking also subsumes the range rule: whatever holds in the target can be combined with it
    // and stored back, and whatever does not is rejected here with a clearer message than
    // "cannot hold the i32 this produces".
    expressions_.check( value, target_type );

    if( !table_.is_integer( target_type ) && !table_.is_float( target_type ) )
    {
        reporter_.error_at(
            ast_.span( id ), fmt::format( "no operator `{}` for `{}`", token_kind_spelling( op ), table_.name( target_type ) )
        );
    }
}

// PLAN D7/D30/D34. The scrutinee and the arm bodies; every rule about a label is `Coverage`'s. Only
// the enum-or-number test is here, because it is what decides which of its two strategies runs.
void Statements::visit_switch( Node_id id )
{
    const std::span<const Node_id> children  = ast_.children( id );
    const Node_id                  scrutinee = children[0];
    const Type_id                  type      = expressions_.infer( scrutinee );

    // Every arm body is visited whatever the scrutinee turned out to be: one mistake there should
    // not hide every mistake inside the arms.
    const auto visit_bodies = [&]()
    {
        for( const Node_id arm : children.subspan( 1 ) )
        {
            visit( ast_.children( arm ).back() );
        }
    };

    if( table_.is_error( type ) )
    {
        visit_bodies();
        return;
    }

    const bool numeric = table_.is_integer( type ) || table_.is_float( type );

    if( !table_.is_enum( type ) && !numeric )
    {
        reporter_.error_at(
            ast_.span( scrutinee ),
            fmt::format( "`switch` needs an `enum` or a number, but this is a `{}`", table_.name( type ) )
        );

        visit_bodies();
        return;
    }

    coverage_.check_arm_structure( id );

    // The arm loop is here rather than in Coverage because an arm's labels declare names its body
    // then reads, so the two interleave and only this class may visit.
    Switch_coverage covered = coverage_.begin_switch( id, type );

    breakable_depth_ += 1;
    for( const Node_id arm : children.subspan( 1 ) )
    {
        coverage_.check_arm_labels( arm, covered );
        visit( ast_.children( arm ).back() );
    }
    breakable_depth_ -= 1;

    coverage_.finish_switch( covered );
}

void Statements::visit_if( Node_id id )
{
    // Children are { condition, then, else }; else is invalid when there is none, and visit()
    // returns immediately on that.
    expressions_.check_condition( ast_.child( id, 0 ) );
    visit( ast_.child( id, 1 ) );
    visit( ast_.child( id, 2 ) );
}

void Statements::visit_while( Node_id id )
{
    expressions_.check_condition( ast_.child( id, 0 ) );
    loop_depth_ += 1;
    breakable_depth_ += 1;
    visit( ast_.child( id, 1 ) );
    loop_depth_ -= 1;
    breakable_depth_ -= 1;
}

void Statements::visit_for( Node_id id )
{
    // Children are { init, condition, update, body }, any of which `for( ; ; )` leaves invalid.
    // init and update are ordinary statements - a declaration, an assignment, an increment - so
    // they go through visit, not infer.
    visit( ast_.child( id, 0 ) );
    expressions_.check_condition( ast_.child( id, 1 ) );
    visit( ast_.child( id, 2 ) );
    loop_depth_ += 1;
    breakable_depth_ += 1;
    visit( ast_.child( id, 3 ) );
    loop_depth_ -= 1;
    breakable_depth_ -= 1;
}

void Statements::visit_increment( Node_id id )
{
    // Children are { operand }; aux is `++` or `--`.
    const Token_kind op      = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    operand = ast_.child( id, 0 );

    if( !places_.is_assignable( operand ) )
    {
        // Same split as visit_assign: infer_name already covers an unresolved name and a function.
        if( ast_.kind( operand ) != Node_kind::Name_expr )
        {
            reporter_.error_at( ast_.span( operand ), fmt::format( "`{}` needs a variable", token_kind_spelling( op ) ) );
        }

        expressions_.infer( operand );
        return;
    }

    const Type_id operand_type = expressions_.infer( operand );

    if( table_.is_error( operand_type ) )
    {
        return;
    }

    if( !places_.check_writable( operand, expressions_.current_function() ) )
    {
        return;
    }

    if( !table_.is_integer( operand_type ) && !table_.is_float( operand_type ) )
    {
        reporter_.error_at(
            ast_.span( id ), fmt::format( "no operator `{}` for `{}`", token_kind_spelling( op ), table_.name( operand_type ) )
        );
    }
}

void Statements::visit_global( Node_id id )
{
    const Type_id type = types_.type_of( id );
    const Node_id init = ast_.child( id, 1 );

    if( table_.is_error( type ) )
    {
        return;
    }

    // A struct literal lowers to a temporary and field assignments, and there is nowhere at C file
    // scope to put those. Checked before the initialiser so `Point origin;` is caught too.
    if( table_.is_struct( type ) )
    {
        reporter_.error_at( ast_.span( id ), "a struct cannot be a file-scope variable yet" );
        return;
    }

    if( !init.is_valid() )
    {
        return; // zero, which C guarantees for file-scope storage
    }

    if( !constant_folder_.is_constant_expression( init ) )
    {
        reporter_.error_at(
            ast_.span( init ),
            "a file-scope initialiser must be a constant expression",
            "it may use literals and arithmetic over them, but nothing that has to run"
        );

        return;
    }

    expressions_.check( init, type ); // range checking, and the literal adopts the declared type

    // Evaluated here rather than printed as an expression by the backend. The rule already says
    // this is a constant expression, so the value exists; computing it once is what lets a backend
    // emit a literal instead of re-deriving §6.4's conversions in its own spelling of the tree.
    if( table_.is_float( type ) )
    {
        if( const std::optional<f64> value = constant_folder_.fold_float( init ) )
        {
            constant_folder_.record_value( id, Constant_value { Constant_value::Kind::Float, 0, false, *value } );
        }

        return;
    }

    const Folded value = constant_folder_.fold_integer( init );

    if( value.constant && !value.overflowed )
    {
        constant_folder_.record_value(
            id, Constant_value { Constant_value::Kind::Integer, value.value.magnitude, value.value.negative, 0.0 }
        );
    }
}

void Statements::visit_block( Node_id id )
{
    if( ast_.aux( id ) != 1 )
    {
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        return;
    }

    // Both of the block's own rules are Expressions': the flag they read is the one
    // `require_unsafe` writes, and only the children in between are statements.
    const bool enclosing_used = expressions_.enter_unsafe( ast_.span( id ) );

    for( const Node_id child : ast_.children( id ) )
    {
        visit( child );
    }

    expressions_.leave_unsafe( ast_.span( id ), enclosing_used );
}

} // namespace sema
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

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

// §12: "`unsafe` permits operations, it does not disable checks." Everything the checker does
// outside a block it still does inside one - this is the half of the design most often got wrong.
TEST_CASE( "type_checker_still_checks_inside_an_unsafe_block", "[sema][types][unsafe]" )
{
    SECTION( "types" )
    {
        const Typed p( "i32 main() { i32* q = nullptr; unsafe { u8* r = cast<u8*>( q ); bool b = r; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a conversion the table refuses outright" )
    {
        // Conversion::None is nonsense rather than unchecked, so `unsafe` does not unlock it.
        const Typed p( "i32 main()\n"
                       "{\n"
                       "    f64 d = 1.5;\n"
                       "    i32* q = nullptr;\n"
                       "    unsafe { u8* r = cast<u8*>( q ); i32 x = cast<i32>( d ); }\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "rounding" ) != std::string::npos );
    }

    SECTION( "const" )
    {
        const Typed p( "i32 main()\n"
                       "{\n"
                       "    const i32 x = 1;\n"
                       "    i32* q = nullptr;\n"
                       "    unsafe { u8* r = cast<u8*>( q ); x = 2; }\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "`break` outside a loop" )
    {
        const Typed p( "i32 main()\n"
                       "{\n"
                       "    i32* q = nullptr;\n"
                       "    unsafe { u8* r = cast<u8*>( q ); break; }\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "break" ) != std::string::npos );
    }

    SECTION( "the scope is a real one" )
    {
        const Typed p( "i32 main() { i32* q = nullptr; unsafe { u8* r = cast<u8*>( q ); } return wrap<i32>( r ); }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// An unsafe block over safe code is D15's shape: a construct with no effect. It is also how an
// unsafe region grows quietly wider than the operation it was opened for.
TEST_CASE( "type_checker_rejects_an_unsafe_block_that_needs_nothing", "[sema][types][unsafe]" )
{
    SECTION( "empty" )
    {
        const Typed p( "i32 main() { unsafe { } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "nothing" ) != std::string::npos );
    }

    SECTION( "holding only safe statements" )
    {
        const Typed p( "i32 main() { i32 y = 0; unsafe { y = 1; y = y + 1; } return y; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "an operation in an enclosing block does not excuse it" )
    {
        const Typed p( "i32 main()\n"
                       "{\n"
                       "    i32 x = 1;\n"
                       "    i32* q = &x;\n"
                       "    unsafe { u8* r = cast<u8*>( q ); }\n"
                       "    unsafe { x = 2; }\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "one that is used reports nothing" )
    {
        const Typed p( "i32 main() { i32 x = 1; i32* q = &x; unsafe { u8* r = cast<u8*>( q ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Permission is not cumulative, so a second marker inside the first can only mislead a reader into
// thinking the inner region is the guarded one.
TEST_CASE( "type_checker_rejects_a_nested_unsafe_block", "[sema][types][unsafe]" )
{
    SECTION( "directly nested" )
    {
        const Typed p( "i32 main() { i32 x = 1; i32* q = &x; unsafe { unsafe { u8* r = cast<u8*>( q ); } } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
        REQUIRE( p.rendered().find( "unsafe" ) != std::string::npos );
    }

    SECTION( "nested through an ordinary block" )
    {
        const Typed p( "i32 main()\n"
                       "{\n"
                       "    i32 x = 1;\n"
                       "    i32* q = &x;\n"
                       "    unsafe\n"
                       "    {\n"
                       "        u8* a = cast<u8*>( q );\n"
                       "        { unsafe { u8* b = cast<u8*>( q ); } }\n"
                       "    }\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "two side by side are both fine" )
    {
        const Typed p( "i32 main()\n"
                       "{\n"
                       "    i32 x = 1;\n"
                       "    i32* q = &x;\n"
                       "    unsafe { u8* a = cast<u8*>( q ); }\n"
                       "    unsafe { u8* b = cast<u8*>( q ); }\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "one in a function called from inside another is fine" )
    {
        const Typed p( "void inner( i32* q ) { unsafe { u8* a = cast<u8*>( q ); } }\n"
                       "i32 main() { i32 x = 1; i32* q = &x; unsafe { u8* b = cast<u8*>( q ); inner( q ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// `break` and `continue` are only meaningful inside a loop, and the depth has to be scoped to the
// *body*: visit()'s default recurses into children rather than asserting, so a missing case here
// would let `break;` compile anywhere at all rather than failing loudly.
TEST_CASE( "type_checker_accepts_break_and_continue_inside_a_loop", "[sema][types][loops]" )
{
    SECTION( "directly in a while" )
    {
        const Typed p( "i32 main() { while ( true ) { break; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "directly in a for" )
    {
        const Typed p( "i32 main() { for ( i32 i = 0; i < 3; i++ ) { continue; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The case a naive check misses: the enclosing statement is an if, not a loop.
    SECTION( "nested inside an if inside a loop" )
    {
        const Typed p( "i32 main() { while ( true ) { if ( true ) { break; } } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "in a block inside a loop" )
    {
        const Typed p( "i32 main() { while ( true ) { { continue; } } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "nested loops" )
    {
        const Typed p( "i32 main() { while ( true ) { for ( i32 i = 0; i < 3; i++ ) { continue; } break; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_rejects_break_and_continue_outside_a_loop", "[sema][types][loops]" )
{
    SECTION( "at the top of a function" )
    {
        const Typed b( "i32 main() { break; return 0; }" );
        const Typed c( "i32 main() { continue; return 0; }" );

        INFO( b.rendered() << c.rendered() );
        REQUIRE( b.errors() == 1 );
        REQUIRE( c.errors() == 1 );
        REQUIRE( b.rendered().find( "`break` outside a loop" ) != std::string::npos );
        REQUIRE( c.rendered().find( "`continue` outside a loop" ) != std::string::npos );
    }

    SECTION( "in an if that is not inside a loop" )
    {
        const Typed p( "i32 main() { if ( true ) { break; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "in a bare block" )
    {
        const Typed p( "i32 main() { { break; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // The depth has to come back down, or everything after a loop is wrongly inside one.
    SECTION( "after the loop has closed" )
    {
        const Typed p( "i32 main() { while ( true ) { break; } break; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "after a nested loop has closed" )
    {
        const Typed p( "i32 main() { while ( true ) { for ( i32 i = 0; i < 3; i++ ) { break; } } continue; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Every loop in a chain must restore the depth, not just the last one.
    SECTION( "after several loops in sequence" )
    {
        const Typed p( "i32 main() {\n"
                       "  while ( true ) { break; }\n"
                       "  for ( i32 i = 0; i < 3; i++ ) { break; }\n"
                       "  while ( true ) { break; }\n"
                       "  break;\n"
                       "  return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// A file-scope initialiser must be a literal. Not because anything cannot be computed, but because
// there is no constant folder: the restriction is the honest statement of what the compiler can
// actually evaluate, and widening it later breaks nothing that compiles under it.
//
// It also removes two problems outright. A global cannot name another global, so initialisation
// order never exists as a question; and no global can hold a type with a destructor, so M3 never
// has to sequence global teardown.
TEST_CASE( "type_checker_accepts_a_literal_initialised_global", "[sema][types][globals]" )
{
    SECTION( "an integer, taking its type from the annotation" )
    {
        const Typed p( "u8 small = 200;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u8" );
    }

    SECTION( "the other literal kinds" )
    {
        for( const char* head : { "f64 ratio = 1.5;", "bool ready = true;", "u8 letter = 'a';", "i32* address = nullptr;" } )
        {
            const Typed p( std::string( head ) + "\ni32 main() { return 0; }\n" );

            INFO( head << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    SECTION( "a negated literal" )
    {
        const Typed p( "i32 below = -1;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "with no initialiser at all" )
    {
        const Typed p( "i32 counter;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The type has to be recorded before any body is checked, or a function using a global sees
    // nothing there.
    SECTION( "a function may use it, and gets its type" )
    {
        const Typed p( "u8 small = 200;\nu8 read() { return small; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Name_expr, 0 ) ) == "u8" );
    }

    SECTION( "including one written above it" )
    {
        const Typed p( "u8 read() { return small; }\nu8 small = 200;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "range checking still applies" )
    {
        const Typed p( "u8 small = 300;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit" ) != std::string::npos );
    }

    SECTION( "and so does the type of the literal" )
    {
        const Typed p( "i32 whole = 1.5;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// PLAN D32. A `ref` local is the same binding mode in a second position: the declaration keeps `T`,
// so every use of the name is an ordinary `T`, and the address it holds lives on the annotation.
//
// It needs no analysis to be safe. A binding is initialised at its declaration and never reseated,
// so its referent was declared before it - same scope or an enclosing one - and therefore outlives
// it. The one escape is binding to a temporary, and one rule below refuses that.
TEST_CASE( "type_checker_binds_a_ref_local_to_its_referent", "[sema][binding]" )
{
    const Typed p( "i32 main() { i32 x = 1; ref i32 r = x; r = r + 1; return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    // An i32 and not a pointer, or `r = r + 1` above would not have typed.
    REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 1 ) ) == "i32" );
    REQUIRE( p.type_name( p.nth( Node_kind::Mode_type, 0 ) ) == "i32*" );
}

TEST_CASE( "type_checker_checks_a_ref_binding", "[sema][binding]" )
{
    SECTION( "it must be initialised" )
    {
        const Typed p( "i32 main() { i32 x = 1; ref i32 r; return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must be initialised" ) != std::string::npos );
    }

    // The referent has to outlive the binding. Everything that names a place was declared before
    // this line and so does; a temporary is the one thing that would not.
    SECTION( "a call result is not a place to bind to" )
    {
        const Typed p( "i32 make() { return 1; }\ni32 main() { ref i32 r = make(); return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "needs a variable to bind to" ) != std::string::npos );
    }

    SECTION( "nor is a literal" )
    {
        const Typed p( "i32 main() { ref i32 r = 1; return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Same reason a `ref` argument is not converted: the binding is the variable itself, so there
    // is no conversion step for a widened copy to live in.
    SECTION( "and the type must match exactly" )
    {
        const Typed p( "i32 main() { u8 x = 1; ref i32 r = x; return r; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "cannot borrow `u8` as `ref i32`" ) != std::string::npos );
    }

    SECTION( "`move` is not a binding mode" )
    {
        const Typed p( "i32 main() { move i32 x = 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "not a binding mode" ) != std::string::npos );
    }

    SECTION( "and `out` still waits" )
    {
        const Typed p( "i32 main() { out i32 x = 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// Anything that names a place can be bound, which is the same test `ref` at a call site uses -
// deliberately, so the two cannot drift apart.
TEST_CASE( "type_checker_binds_a_ref_local_to_any_place", "[sema][binding]" )
{
    SECTION( "a field" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { P p = P { 1 }; ref i32 r = p.x; r = 5; return p.x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a pointee" )
    {
        const Typed p( "i32 main() { i32 x = 1; i32* q = &x; ref i32 r = *q; r = 5; return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a parameter" )
    {
        const Typed p( "i32 f( i32 x ) { ref i32 r = x; r = 5; return x; }\ni32 main() { return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A binding onto a binding: `r` is an ordinary `i32` by the time this line reads it, so
    // nothing here is special-cased.
    SECTION( "and another ref binding" )
    {
        const Typed p( "void f( ref i32 n ) { ref i32 r = n; r = 5; }\ni32 main() { i32 x = 1; f( ref x ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The interaction worth pinning: a `ref` binding to an owning local is *not* a transfer, so D31's
// "an owning value is transferred, not copied" must not fire on it. Nothing is copied and nothing
// is given away - which is exactly what a borrow is for.
TEST_CASE( "type_checker_does_not_treat_a_ref_binding_as_a_transfer", "[sema][binding]" )
{
    constexpr std::string_view owning = "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n";

    SECTION( "no `move` is demanded" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); ref B r = a; r.n = 5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // And the contrast that gives it meaning: without the mode, the same line is a copy and D31
    // rejects it.
    SECTION( "where a plain local of the same type still needs one" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); B c = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // A mutable binding onto something held read-only would launder the borrow.
    SECTION( "and a read-only borrow cannot be bound mutably" )
    {
        const Typed p( std::string( owning ) + "i32 f( B b ) { ref B r = b; return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` is borrowed" ) != std::string::npos );
    }
}

TEST_CASE( "type_checker_checks_a_const_ref_binding", "[sema][constref]" )
{
    SECTION( "writing through it is refused" )
    {
        const Typed p( "i32 main() { i32 x = 1; const ref i32 r = x; r = 5; return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`r` is `const`" ) != std::string::npos );
    }

    SECTION( "but reading through it is not" )
    {
        const Typed p( "i32 main() { i32 x = 1; const ref i32 r = x; return r + 1; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The whole point of the const half: a read-only binding may be taken onto something held
    // read-only, where a mutable one may not.
    SECTION( "it may bind to a const" )
    {
        const Typed p( "i32 main() { const i32 k = 1; const ref i32 r = k; return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "where a mutable binding may not" )
    {
        const Typed p( "i32 main() { const i32 k = 1; ref i32 r = k; return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // The referent is unaffected: `const` is a property of *this* name, not of the variable it
    // was taken onto.
    SECTION( "and the referent stays writable in its own right" )
    {
        const Typed p( "i32 main() { i32 x = 1; const ref i32 r = x; x = 5; return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Every rule the mutable binding has still applies - a binding is initialised at its
    // declaration and never bound to a temporary, whichever half of the spelling it carries.
    SECTION( "it must still be initialised" )
    {
        const Typed p( "i32 main() { const ref i32 r; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must be initialised" ) != std::string::npos );
    }

    SECTION( "and still cannot bind to a temporary" )
    {
        const Typed p( "i32 make() { return 1; }\ni32 main() { const ref i32 r = make(); return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "needs a variable to bind to" ) != std::string::npos );
    }
}

// An `out` parameter is written by definition, so it is not a read-only borrow - and the predicate
// that decides that asks for the *mode*, which is why a bare owning parameter and an `out` one do
// not collide despite both travelling by address.
TEST_CASE( "type_checker_lets_a_callee_write_an_out_parameter", "[sema][out]" )
{
    const Typed p( "void init( out i32 n ) { n = 1; n = n + 1; }\ni32 main() { i32 x = 0; init( out x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// A local is not a parameter, so neither parameter mode means anything on one - and the two want
// different helps, because `move` is about who owns the value and `out` about who writes it.
TEST_CASE( "type_checker_refuses_out_on_a_local", "[sema][out]" )
{
    const Typed p( "i32 main() { out i32 x = 1; return x; }" );

    INFO( p.rendered() );
    REQUIRE_FALSE( p.clean() );
    REQUIRE( p.rendered().find( "`out` is not a binding mode" ) != std::string::npos );
    REQUIRE( p.rendered().find( "how a callee assigns a caller's variable" ) != std::string::npos );
}

TEST_CASE( "type_checker_refuses_a_scrutinee_that_is_neither", "[sema][switch]" )
{
    const Typed p( "i32 f( bool b ) { switch( b ) { default: return 1; } }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "`switch` needs an `enum` or a number" ) != std::string::npos );
}

// A mistake in the scrutinee must not hide the mistakes inside the arms - one compile should report
// all of them.
TEST_CASE( "type_checker_checks_arm_bodies_even_when_the_scrutinee_is_wrong", "[sema][switch]" )
{
    const Typed p( "i32 f( bool b ) { switch( b ) { default: i32 x = true; return 1; } }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() >= 2 );
}

TEST_CASE( "type_checker_binds_break_to_a_switch_as_well_as_a_loop", "[sema][switch][arms]" )
{
    const std::string_view e = "enum E { A, B };\n";

    SECTION( "break in a switch with no enclosing loop" )
    {
        const Typed p(
            std::string( e ) + "void f( E x ) { switch( x ) { case E::A: break; default: } }\ni32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "continue in a switch with no enclosing loop is still an error" )
    {
        const Typed p(
            std::string( e ) + "void f( E x ) { switch( x ) { case E::A: continue; default: } }\ni32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`continue` outside a loop" ) != std::string::npos );
    }

    SECTION( "continue in a switch inside a loop is fine" )
    {
        const Typed p(
            std::string( e ) + "void f( E x ) { while( true ) { switch( x ) { case E::A: continue; default: continue; } } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "break outside either is still an error" )
    {
        const Typed p( "void f() { break; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "outside a loop or switch" ) != std::string::npos );
    }
}

// The arm rules only ever see an arm's top-level statements, so a `fallthrough` written anywhere
// else is never ruled on - and the lowerer would reach it with no target. This branch is the one
// that refuses it, and it is here because only this walk knows where a statement actually sits.
TEST_CASE( "type_checker_refuses_a_fallthrough_the_arm_rules_never_reached", "[sema][switch][fallthrough]" )
{
    const std::string_view e = "enum E { A, B };\n";

    SECTION( "nested inside a block" )
    {
        const Typed p(
            std::string( e ) + "void f( E x ) { switch( x ) { case E::A: { fallthrough; } break; default: break; } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must be the last statement of a `case`" ) != std::string::npos );
    }

    SECTION( "nested inside an if" )
    {
        const Typed p(
            std::string( e ) + "void f( E x ) { switch( x ) { case E::A: if ( true ) { fallthrough; } break;"
                               " default: break; } }\ni32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must be the last statement of a `case`" ) != std::string::npos );
    }

    SECTION( "nested inside a loop" )
    {
        const Typed p(
            std::string( e ) + "void f( E x ) { switch( x ) { case E::A: while( true ) { fallthrough; } break;"
                               " default: break; } }\ni32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must be the last statement of a `case`" ) != std::string::npos );
    }

    SECTION( "outside a switch entirely" )
    {
        const Typed p( "void f() { fallthrough; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must be the last statement of a `case`" ) != std::string::npos );
    }
}

} // namespace keel
#endif
