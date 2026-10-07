// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/places.h"
#include <fmt/format.h>
#include "sema/type_checker.h"

namespace keel
{
namespace sema
{

bool Places::is_assignable( Node_id id ) const
{
    // A call returning `const ref` names something that was alive at the call site, so it is a
    // place - and by §8's argument it outlives any binding declared there. An ordinary call is not:
    // its result has no scope of its own, which is the temporaries hole §8 records and which this
    // deliberately leaves shut.
    if( ast_.kind( id ) == Node_kind::Call_expr )
    {
        // The callable the checker chose, rather than the name the resolver bound: a method call's
        // callee is a Field_expr, and an overloaded call's name is only the first candidate. Either
        // way the question below is the same one - does this callable hand back a binding.
        return call_returns_a_binding( id );
    }

    // A field is always a place, including a field of a temporary - reading one is an ordinary
    // read. Whether it may be *written* is check_writable's question, which asks what the place is
    // rooted in and refuses `make().x = 2.0;` there. No value categories were needed for it.
    if( ast_.kind( id ) == Node_kind::Field_expr )
    {
        return true; // whether the object has fields at all is infer_field's question
    }

    if( ast_.kind( id ) == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( id ) ) == Token_kind::Star )
    {
        return true; // `*ptr = 42;` writes through the pointer. Whether ptr *is* one is infer_unary's question
    }

    if( ast_.kind( id ) == Node_kind::Index_expr )
    {
        return true; // `arr[7] = 42;` writes through the offset. Whether arr *is* one is infer_index's question
    }

    // A name, bare or through its package (`kl::count`); nothing else is bound.
    const Node_id decl = resolution_.declaration_of( id );
    if( !decl.is_valid() )
    {
        return false;
    }

    // C++ forbids it, and `this` is an ordinary parameter here - which is what makes everything
    // else free, and is exactly why this one case has to be written down.
    if( ast_.kind( decl ) == Node_kind::Param_decl && Symbol_id { ast_.aux( decl ) } == Interner::keyword( Keyword::This ) )
    {
        return false;
    }

    // A pattern binding names a place like the rest. Whether it may be *written* is
    // check_writable's question, and it answers no - which is the same split `const` uses.
    return ast_.kind( decl ) == Node_kind::Var_decl || ast_.kind( decl ) == Node_kind::Param_decl ||
           ast_.kind( decl ) == Node_kind::Field_decl || ast_.kind( decl ) == Node_kind::Binding_decl;
}

// The address is recorded on the annotation exactly when a declaration is a borrow - the same
// invariant is_borrowed_binding reads, asked from inside the checker where `Types` is not built
// yet. Answers for a parameter as readily as for a function: children[0] is the annotation for
// both, so "returns a binding" and "is a borrow" are one question asked of different nodes.
bool Places::returns_a_binding( Node_id decl ) const
{
    if( !decl.is_valid() )
    {
        return false;
    }

    const Node_id annotation = ast_.child( decl, 0 );

    return annotation.is_valid() && types_.type_of( annotation ).is_valid();
}

bool Places::call_returns_a_binding( Node_id call ) const
{
    const Node_id method = callees_.callee_of( call );
    const Node_id callee = method.is_valid() ? method : resolution_.declaration_of( ast_.child( call, 0 ) );

    // A callable declaration answers from its own return, and nothing below may be asked of one:
    // `fn() -> const ref i32 maker()` hands back a signature *by value*, and reading that
    // signature's return mode would make the call a place when it is a temporary.
    if( callee.is_valid() &&
        ( ast_.kind( callee ) == Node_kind::Function_decl || ast_.kind( callee ) == Node_kind::Method_decl ) )
    {
        return returns_a_binding( callee );
    }

    // A call through a variable has no such declaration, so the signature the variable holds is what
    // answers - taken off the declaration rather than off the callee expression, because
    // visit_assign asks this before the expression has been typed at all.
    const Type_id signature = callee.is_valid() ? types_.type_of( callee ) : types_.type_of( ast_.child( call, 0 ) );

    return signature.is_valid() && table_.is_function( signature ) &&
           table_.get( signature ).return_mode == Param_mode::Const_ref;
}

// D31/D32: what may be written. A `const` declaration says so itself; a borrow is read-only because
// someone else owns it - different reasons, so different messages and different fixes.
bool Places::check_writable( Node_id target, Node_id current_function, bool replaces )
{
    // Above place_root, which resolves only a `Name_expr`: a call root reaches it as an invalid id
    // and the guard below reads that as permission. `returns_a_binding` rather than
    // `is_const_binding` - `const P f()` returns a const *value*, not a reference.
    const Node_id source = place_source( target );

    if( source.is_valid() )
    {
        // A conditional materialises its result even when both arms are places, and a literal has
        // no storage at all - neither has a declaration, which is why they are named here directly.
        bool temporary = ast_.kind( source ) == Node_kind::Conditional_expr || ast_.kind( source ) == Node_kind::Struct_literal;

        if( ast_.kind( source ) == Node_kind::Call_expr )
        {
            const Node_id method = callees_.callee_of( source );
            const Node_id callee = method.is_valid() ? method : resolution_.declaration_of( ast_.child( source, 0 ) );

            if( call_returns_a_binding( source ) )
            {
                reporter_.error_at(
                    ast_.span( target ),
                    fmt::format(
                        "`{}` returns a const ref, so it cannot be modified",
                        interner_.text( Symbol_id { ast_.aux( ast_.child( source, 0 ) ) } )
                    ),
                    "a returned reference is always read-only"
                );

                return false;
            }

            // A constructor lands here too, since its callee is the aggregate rather than a
            // function. An unresolved one stays quiet: the name was already reported.
            temporary = callee.is_valid();
        }

        if( temporary )
        {
            // The source rather than the target: there is no declaration to name, so the span is
            // what says where the value came from.
            reporter_.error_at(
                ast_.span( source ),
                "this value is a temporary, so writing to it has no effect",
                "assign it to a variable and modify that"
            );

            return false;
        }
    }

    if( !is_read_only( target, current_function ) )
    {
        return true;
    }

    const Node_id pointer = through_const_pointer( target );
    if( pointer.is_valid() && ast_.kind( pointer ) == Node_kind::Index_expr )
    {
        reporter_.error_at(
            ast_.span( target ),
            fmt::format(
                "`{}`'s `operator[]` only reads, so this cannot be modified",
                table_.name( types_.type_of( ast_.child( pointer, 0 ) ) )
            ),
            "it returns a pointer to `const`"
        );

        return false;
    }

    if( pointer.is_valid() )
    {
        reporter_.error_at(
            ast_.span( target ),
            fmt::format( "this is reached through a `{}`, so it cannot be modified", table_.name( types_.type_of( pointer ) ) ),
            "a pointer to `const` only reads what it points at"
        );

        return false;
    }

    const Node_id field = const_field( target );
    if( field.is_valid() )
    {
        if( replaces && initialises_const_field( target, current_function ) )
        {
            return true;
        }

        const bool own_constructor = ast_.kind( current_function ) == Node_kind::Constructor_decl &&
                                     place_root( target, current_function ) == receiver_of( current_function );

        reporter_.error_at(
            ast_.span( target ),
            fmt::format(
                "`{}` is a `const` field, so it cannot be modified", interner_.text( Symbol_id { ast_.aux( field ) } )
            ),
            own_constructor ? "a constructor assigns it once, whole, with `=`" : "only a constructor assigns it"
        );

        return false;
    }

    const Node_id root = place_root( target, current_function );

    // D7: a pattern binding is read-only. The payload is copied today, so this refuses nothing
    // observable - which is the point, because it becomes a borrow once payloads can own.
    if( root.is_valid() && ast_.kind( root ) == Node_kind::Binding_decl )
    {
        reporter_.error_at(
            ast_.span( target ),
            fmt::format(
                "`{}` is bound by a pattern, so it cannot be modified", interner_.text( Symbol_id { ast_.aux( root ) } )
            ),
            "it names part of the value being matched, which the `switch` does not own"
        );

        return false;
    }

    if( is_const_binding( ast_, root ) )
    {
        // The receiver reads differently from any other const binding: the `const` that made it one
        // is written on the *method*, so naming the variable would point at a word the author never
        // wrote. `this` is also not what they typed - a bare field name is what got here.
        if( root == receiver_of( current_function ) )
        {
            reporter_.error_at(
                ast_.span( target ),
                "a `const` method cannot modify its object",
                fmt::format(
                    "remove `const` from `{}` to let it", interner_.text( Symbol_id { ast_.aux( current_function ) } )
                )
            );

            return false;
        }

        reporter_.error_at(
            ast_.span( target ),
            fmt::format( "`{}` is `const`", interner_.text( Symbol_id { ast_.aux( root ) } ) ),
            "remove `const` to modify it"
        );

        return false;
    }

    if( is_borrow_binding( root ) )
    {
        const std::string_view name = interner_.text( Symbol_id { ast_.aux( root ) } );

        reporter_.error_at(
            ast_.span( target ),
            fmt::format( "`{}` is borrowed, so it cannot be modified", name ),
            fmt::format( "take it as `ref {} {}` to modify it", table_.name( types_.type_of( root ) ), name )
        );

        return false;
    }

    return true;
}

// Whether a place may be read and not written: through a pointer to `const` or a `const` field, from a `const ref` return, or
// rooted in a `const` binding, a pattern binding or a read-only borrow. A temporary is check_writable's alone.
bool Places::is_read_only( Node_id target, Node_id current_function ) const
{
    if( through_const_pointer( target ).is_valid() || const_field( target ).is_valid() )
    {
        return true;
    }

    const Node_id source = place_source( target );
    if( source.is_valid() && ast_.kind( source ) == Node_kind::Call_expr && call_returns_a_binding( source ) )
    {
        return true;
    }

    const Node_id root = place_root( target, current_function );
    if( root.is_valid() &&
        ( ast_.kind( root ) == Node_kind::Binding_decl || is_borrow_binding( root ) || is_const_binding( ast_, root ) ) )
    {
        return true;
    }

    return false;
}

Node_id Places::through_const_pointer( Node_id place ) const
{
    while( true )
    {
        Node_id         pointer {};
        const Node_kind kind = ast_.kind( place );

        if( kind == Node_kind::Field_expr )
        {
            const Node_id object      = ast_.child( place, 0 );
            const Type_id object_type = types_.type_of( object );
            if( object_type.is_valid() && !table_.is_pointer( object_type ) )
            {
                place = object;
                continue;
            }
            else
            {
                // D22's auto deref
                pointer = object;
            }
        }

        if( kind == Node_kind::Index_expr )
        {
            const Node_id method = callees_.callee_of( place );

            if( method.is_valid() )
            {
                if( table_.points_to_const( types_.type_of( method ) ) )
                {
                    return place;
                }

                place = ast_.child( place, 0 );
                continue;
            }

            pointer = ast_.child( place, 0 );
        }

        if( kind == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( place ) ) == Token_kind::Star )
        {
            pointer = ast_.child( place, 0 );
        }

        if( pointer.is_valid() && table_.points_to_const( types_.type_of( pointer ) ) )
        {
            return pointer;
        }

        return Node_id {};
    }
}

Node_id Places::const_field( Node_id place ) const
{
    if( ast_.kind( place ) == Node_kind::Field_expr )
    {
        const Type_id object_type = types_.type_of( ast_.child( place, 0 ) );

        if( !object_type.is_valid() || table_.is_error( object_type ) )
        {
            return Node_id {};
        }

        const bool    through   = table_.is_pointer( object_type );
        const Type_id aggregate = through ? table_.get( object_type ).element : object_type;

        if( table_.is_struct( aggregate ) )
        {
            const std::span<const Node_id> members = ast_.members( table_.get( aggregate ).declaration );
            for( const Node_id member : members )
            {
                if( ast_.kind( member ) == Node_kind::Field_decl && ast_.aux( member ) == ast_.aux( place ) &&
                    is_const_field( ast_, member ) )
                {
                    return member;
                }
            }
        }

        if( through )
        {
            return Node_id();
        }

        place = ast_.child( place, 0 );
    }

    const Node_id decl = resolution_.declaration_of( place );
    if( ast_.kind( place ) == Node_kind::Name_expr && ast_.kind( decl ) == Node_kind::Field_decl &&
        is_const_field( ast_, decl ) )
    {
        return decl;
    }

    return Node_id {};
}

bool Places::initialises_const_field( Node_id place, Node_id current_function ) const
{
    if( ast_.kind( current_function ) != Node_kind::Constructor_decl )
    {
        return false;
    }

    if( ast_.kind( place ) == Node_kind::Name_expr )
    {
        return is_const_field( ast_, resolution_.declaration_of( place ) );
    }

    if( ast_.kind( place ) == Node_kind::Field_expr )
    {
        const Node_id object = ast_.child( place, 0 );

        if( ast_.kind( object ) == Node_kind::Name_expr &&
            resolution_.declaration_of( object ) == receiver_of( current_function ) )
        {
            return true;
        }
    }

    return false;
}

// Bare and owning: the read-only borrow. `move` is not one - the callee owns what it was given -
// and `ref` is the mutable borrow, which every question here is asked in contrast to.
bool Places::is_borrow_binding( Node_id decl ) const
{
    return decl.is_valid() && ast_.kind( decl ) == Node_kind::Param_decl && parameter_mode( ast_, decl ) == Keyword::Count &&
           !bounds_.satisfies( types_.type_of( decl ), Bound::Copyable );
}

Node_id Places::receiver_of( Node_id function ) const
{
    if( !has_receiver( ast_, function ) )
    {
        return Node_id {};
    }

    const std::span<const Node_id> params = ast_.children( ast_.child( function, 1 ) );

    return params.empty() ? Node_id {} : params[0];
}

Node_id Places::place_root( Node_id id, Node_id current_function ) const
{
    id = place_source( id );

    // place_source bails on a pointer projection, and every question below reads the node.
    if( !id.is_valid() )
    {
        return Node_id {};
    }

    // A name, bare or through its package (`kl::count`); nothing else is bound.
    const Node_id decl = resolution_.declaration_of( id );

    // A bare field name is `this.field` written implicitly (D22), so what it is rooted in is the
    // **receiver**, not the field. Without this a `const` method could write its own object: the
    // field is not a binding, so every question below would answer no and nothing would object.
    if( decl.is_valid() && ast_.kind( decl ) == Node_kind::Field_decl )
    {
        return receiver_of( current_function );
    }

    return decl;
}

Node_id Places::place_source( Node_id id ) const
{
    while( ast_.kind( id ) == Node_kind::Field_expr || is_operator_index( id ) )
    {
        const Node_id object = ast_.child( id, 0 );

        // `v[i]` is rooted in `v`: it may be written exactly when `v` may.
        if( ast_.kind( id ) == Node_kind::Index_expr )
        {
            id = object;
            continue;
        }

        // D22 reaches through a pointer, and past one the borrow says nothing: what a pointer
        // points at was never part of the object that was lent. The same shallowness `const` has
        // in C++, arrived at the same way - the pointer is what is borrowed, not the pointee.
        if( table_.is_pointer( types_.type_of( object ) ) )
        {
            return Node_id {};
        }

        id = object;
    }

    return id;
}

bool Places::is_operator_index( Node_id id ) const
{
    return ast_.kind( id ) == Node_kind::Index_expr && callees_.callee_of( id ).is_valid();
}

// The `v[i]` a place is reached through, which lasts only as long as its expression.
Node_id Places::operator_projection( Node_id place ) const
{
    while( ast_.kind( place ) == Node_kind::Field_expr && !table_.is_pointer( types_.type_of( ast_.child( place, 0 ) ) ) )
    {
        place = ast_.child( place, 0 );
    }

    return is_operator_index( place ) ? place : Node_id {};
}

Node_id Places::dying_storage( Node_id place, Node_id current_function ) const
{
    Node_id root = place_root( place, current_function );

    while( root.is_valid() && ast_.kind( root ) == Node_kind::Var_decl && returns_a_binding( root ) )
    {
        root = place_root( ast_.child( root, 1 ), current_function );
    }

    if( !root.is_valid() )
    {
        return Node_id {};
    }

    if( ast_.kind( root ) == Node_kind::Binding_decl )
    {
        return root;
    }

    if( ast_.kind( root ) == Node_kind::Param_decl && !returns_a_binding( root ) )
    {
        return root;
    }

    if( ast_.kind( root ) == Node_kind::Var_decl && ast_.span( current_function ).contains( ast_.span( root ) ) )
    {
        return root;
    }

    return Node_id {};
}

void Places::check_owning_source( Node_id value, Type_id type )
{
    if( !value.is_valid() || bounds_.satisfies( type, Bound::Copyable ) || ast_.kind( value ) == Node_kind::Marker_expr )
    {
        return;
    }

    // A temporary needs no marker: nothing else owns it and no variable is left behind to wonder
    // about. is_assignable is the "names a place" test that separates the two.
    if( !is_assignable( value ) )
    {
        return;
    }

    if( is_operator_index( value ) )
    {
        reporter_.error_at(
            ast_.span( value ),
            "an owning value is transferred, not copied",
            "it stays in its container; take it out through one of the container's methods"
        );
        return;
    }

    reporter_.error_at(
        ast_.span( value ),
        "an owning value is transferred, not copied",
        fmt::format( "write `move {}`", reporter_.text( ast_.span( value ) ) )
    );
}

bool Places::check_owning_return( Node_id value, Type_id type )
{
    if( !value.is_valid() || bounds_.satisfies( type, Bound::Copyable ) || ast_.kind( value ) == Node_kind::Marker_expr )
    {
        return false;
    }

    if( !is_assignable( value ) )
    {
        return false;
    }

    const Node_id decl = resolution_.declaration_of( value );
    if( ast_.kind( value ) == Node_kind::Name_expr &&
        ( ast_.kind( decl ) == Node_kind::Var_decl || ast_.kind( decl ) == Node_kind::Param_decl ||
          ast_.kind( decl ) == Node_kind::Binding_decl ) )
    {
        return false;
    }

    const Span  span    = ast_.span( value );
    std::string message = "an owning value is transferred, not copied";

    if( ast_.kind( value ) == Node_kind::Index_expr && !is_operator_index( value ) )
    {
        reporter_.error_at( span, message, fmt::format( "write `move {}`", reporter_.text( span ) ) );
    }
    else
    {
        reporter_.error_at( span, message, fmt::format( "`{}` still belongs to what holds it", reporter_.text( span ) ) );
    }

    return true;
}

void Places::record_borrowed_parameters()
{
    // Flat rather than over root's children: parameters live under functions, constructors and
    // destructors alike, and a walk that names its containers can miss one. The cost is the
    // orphans of a failed declaration, which the skip below tolerates.
    for( u32 i = 0; i < ast_.node_count(); ++i )
    {
        Node_id param { i };

        if( ast_.kind( param ) != Node_kind::Param_decl )
        {
            continue;
        }

        const Type_id type = types_.type_of( param );

        // Untyped: an orphan of a failed declaration, per the note above. It is not part of the
        // program, and pointer_to() on an invalid type asserts rather than degrading.
        if( !type.is_valid() )
        {
            continue;
        }

        const Keyword mode = parameter_mode( ast_, param );

        // D31's two borrows, plus `out` for the same reason `ref` has. A bare parameter of an
        // owning type is the read-only borrow, forced rather than chosen: a class cannot be
        // copied. `move` is neither - the callee owns it and destroys it, so it travels by value.
        const bool by_address = mode == Keyword::Ref || mode == Keyword::Out ||
                                ( mode == Keyword::Count && !bounds_.satisfies( type, Bound::Copyable ) );

        if( !by_address )
        {
            continue;
        }

        record_binding_address( ast_.child( param, 0 ), type );
    }
}

void Places::record_binding_address( Node_id annotation, Type_id type )
{
    types_.record( annotation, table_.pointer_to( type ) );
}

} // namespace sema
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// D31: an owning value is transferred, not copied, and the transfer is written down - in an
// initialiser and an assignment as much as at a call. Without this the lowerer moves it anyway,
// which frees exactly once but does it silently, and a silent transfer is the one thing D2 exists
// to prevent.
TEST_CASE( "type_checker_requires_move_when_copying_an_owning_value", "[sema][move]" )
{
    constexpr std::string_view owning = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n";

    SECTION( "an initialiser from a named variable" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); B c = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "write `move a`" ) != std::string::npos );
    }

    SECTION( "an assignment from a named variable" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); B c = B( 2 ); c = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "with the marker it is accepted" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); B c = move a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A temporary has no other owner, so handing it over is the only thing that can happen to it -
    // and there is no variable left behind for a reader to wonder about.
    SECTION( "a temporary needs no marker" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); a = B( 2 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and `auto` is checked the same way" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); auto c = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // Only a local is dying at a `return`, so only a local needs no marker there.
    SECTION( "a return through a pointer" )
    {
        const Typed p( std::string( owning ) + "B first( B* p ) { return *p; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "an owning value is transferred, not copied" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`*p` still belongs to what holds it" ) != std::string::npos );
    }

    SECTION( "a return of a field" )
    {
        const Typed p(
            std::string( owning ) + "class H { B kept; H() { kept = B( 1 ); } public B leak() const { return kept; } };\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`kept` still belongs to what holds it" ) != std::string::npos );
    }

    SECTION( "a return of a raw slot asks for the marker" )
    {
        const Typed p( std::string( owning ) + "B take( B[*] data ) { unsafe { return data[0]; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "write `move data[0]`" ) != std::string::npos );
    }

    SECTION( "and a generic one is checked once, for any `T`" )
    {
        const Typed p( "T first<T>( T* p ) { return *p; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a return of a local needs no marker" )
    {
        const Typed p( std::string( owning ) + "B make() { B b = B( 1 ); return b; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "nor does a return of a pointee that copies freely" )
    {
        const Typed p( "i32 first( i32* p ) { return *p; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Nothing changes for a type that owns nothing: copying one is what it is for.
    SECTION( "a non-owning value copies freely" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { P a = P { 1 }; P c = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// PLAN D31. A bare parameter of an owning type is a *read-only* borrow: the caller still owns it
// and still destroys it, so the callee may look and must not touch. It is forced rather than
// chosen - a class cannot be copied while §6.6 keeps copy constructors out of v0, so borrowing is
// the only meaning a bare argument has left.
TEST_CASE( "type_checker_makes_a_bare_owning_parameter_read_only", "[sema][borrow]" )
{
    constexpr std::string_view owning = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n";

    SECTION( "a field of it cannot be assigned" )
    {
        const Typed p( std::string( owning ) + "u64 f( B b ) { b.n = 5; return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` is borrowed, so it cannot be modified" ) != std::string::npos );
        REQUIRE( p.rendered().find( "take it as `ref B b` to modify it" ) != std::string::npos );
    }

    SECTION( "nor compound-assigned" )
    {
        const Typed p( std::string( owning ) + "u64 f( B b ) { b.n += 5; return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Its own case because `++` reaches assignability by a different route, and a `u64` field
    // typechecks perfectly well - nothing else would have stopped it.
    SECTION( "nor incremented" )
    {
        const Typed p( std::string( owning ) + "u64 f( B b ) { b.n++; return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and the binding itself cannot be assigned" )
    {
        const Typed p( std::string( owning ) + "u64 f( B b ) { b = B( 2 ); return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "is borrowed" ) != std::string::npos );
    }

    SECTION( "reading it is what it is for" )
    {
        const Typed p( std::string( owning ) + "u64 f( B b ) { return b.n; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The walk is a loop, not one step: a path of any depth still roots in the borrow.
    SECTION( "a nested field is refused too" )
    {
        const Typed p( "struct P { i32 x; };\nclass W { public P p; ~W() { } };\n"
                       "i32 f( W w ) { w.p.x = 5; return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`w` is borrowed" ) != std::string::npos );
    }

    // Shallow, the way `const` is in C++, and for the same reason: what a pointer points at was
    // never part of the object that was lent. The borrow covers the pointer, not the pointee.
    SECTION( "but writing through a pointer it holds is allowed" )
    {
        const Typed p( "class B { public i32* q; ~B() { } };\n"
                       "i32 f( B b ) { *b.q = 5; return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "including through D22's implicit reach" )
    {
        const Typed p( "struct P { i32 x; };\nclass B { public P* q; ~B() { } };\n"
                       "i32 f( B b ) { b.q.x = 5; return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A local owns what it holds, so none of this touches it. Without this case the rule could be
    // refusing every write to an owning type and the section above would not notice.
    SECTION( "and a local of the same type is unaffected" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); a.n = 5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Three ways to give away what you were lent, and each has its own reason to be refused. Returning
// one is the sharpest: `B pass( B b ) { return b; }` freed one resource twice before this existed.
TEST_CASE( "type_checker_refuses_to_transfer_a_borrow", "[sema][borrow]" )
{
    constexpr std::string_view owning = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n";

    SECTION( "it cannot be moved" )
    {
        const Typed p(
            std::string( owning ) + "void consume( move B b ) { }\nvoid f( B b ) { consume( move b ); }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot move out of a borrow" ) != std::string::npos );
        REQUIRE( p.rendered().find( "take it as `move B b` to own it" ) != std::string::npos );
    }

    SECTION( "it cannot be lent mutably" )
    {
        const Typed p(
            std::string( owning ) + "void grow( ref B b ) { b.n = 1; }\nvoid f( B b ) { grow( ref b ); }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` is borrowed" ) != std::string::npos );
    }

    SECTION( "and it cannot be returned" )
    {
        const Typed p( std::string( owning ) + "B pass( B b ) { return b; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot return a borrowed value" ) != std::string::npos );
        REQUIRE( p.rendered().find( "the caller still owns it" ) != std::string::npos );
    }

    // The gate is that the *return type* owns something. Reading a value out of a borrow and
    // returning that copies nothing anyone else holds.
    SECTION( "though a value read out of it can be" )
    {
        const Typed p( std::string( owning ) + "u64 f( B b ) { return b.n; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Lending on what you were lent needs no marker and no copy - the address is simply forwarded.
    SECTION( "and it can be passed on as a borrow" )
    {
        const Typed p(
            std::string( owning ) + "u64 peek( B b ) { return b.n; }\nu64 f( B b ) { return peek( b ); }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A `ref` parameter is the mutable borrow, so it is the contrast that gives the bare one meaning.
TEST_CASE( "type_checker_leaves_a_ref_parameter_writable", "[sema][borrow]" )
{
    const Typed p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                   "void grow( ref B b ) { b.n = b.n + 1; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// PLAN D32's shape applied to const: `ref` is a binding mode, `const` is a binding property. Both
// are about the name rather than the data, which is what lets this reuse the read-only machinery
// the borrow rule already has instead of putting constness into the type.
TEST_CASE( "type_checker_enforces_const_on_a_binding", "[sema][const]" )
{
    SECTION( "a local cannot be assigned" )
    {
        const Typed p( "i32 main() { const i32 k = 1; k = 2; return k; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`k` is `const`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "remove `const` to modify it" ) != std::string::npos );
    }

    // Its own route to assignability, so its own case - exactly as the borrow rule needed.
    SECTION( "nor incremented" )
    {
        const Typed p( "i32 main() { const i32 k = 1; k++; return k; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "nor compound-assigned" )
    {
        const Typed p( "i32 main() { const i32 k = 1; k += 2; return k; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a parameter cannot be assigned" )
    {
        const Typed p( "i32 f( const i32 n ) { n = 2; return n; }\ni32 main() { return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`n` is `const`" ) != std::string::npos );
    }

    SECTION( "and neither can a global" )
    {
        const Typed p( "const i32 g = 1;\ni32 main() { g = 2; return g; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Initialising is not modifying, and reading is what a const is for. Without these the rule
    // could be refusing every mention of the name and no section above would notice.
    SECTION( "but it may be initialised and read" )
    {
        const Typed p( "i32 main() { const i32 k = 1; i32 y = k + 1; return y; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a non-const local of the same type is unaffected" )
    {
        const Typed p( "i32 main() { i32 k = 1; k = 2; return k; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The same walk the borrow rule uses: a field path of any depth roots in the declaration, and the
// walk stops at a pointer because what a pointer points at was never named by this declaration.
// That is C++'s shallow const, reached from bindings rather than inherited.
TEST_CASE( "type_checker_reaches_const_through_fields", "[sema][const]" )
{
    SECTION( "a field of a const struct cannot be written" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { const P p = P { 1 }; p.x = 5; return p.x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`p` is `const`" ) != std::string::npos );
    }

    SECTION( "nor a nested one" )
    {
        const Typed p( "struct Inner { i32 x; };\nstruct Outer { Inner i; };\n"
                       "i32 main() { const Outer o = Outer { Inner { 1 } }; o.i.x = 5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`o` is `const`" ) != std::string::npos );
    }

    SECTION( "but the walk stops at a pointer it holds" )
    {
        const Typed p( "struct P { i32 x; };\nstruct H { P* q; };\n"
                       "i32 main() { P v = P { 1 }; const H h = H { &v }; h.q.x = 5; return v.x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// PLAN §8. A reference may be returned only as a `const ref` derived from one of the function's own
// reference parameters. That makes dangling representable but rejected rather than impossible, and
// the check is syntactic with no dataflow: trace the returned expression to its root and require a
// parameter that travels by address.
//
// Why no lifetimes are needed: a `ref` binding is initialised at its declaration and never
// reseated, so everything nameable at a call site outlives any binding declared there. It does not
// matter which parameter the result came from, because all of them outlive it - which is the
// question Rust answers with lifetime parameters and Keel does not have to ask.
TEST_CASE( "type_checker_accepts_a_const_ref_return_from_a_parameter", "[sema][escape]" )
{
    SECTION( "from a const ref parameter" )
    {
        const Typed p( "const ref i32 pick( const ref i32 a, const ref i32 b ) { return a; }\n"
                       "i32 main() { i32 x = 1; i32 y = 2; return pick( x, y ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Which one it came from is exactly what does not matter, and a second parameter is the case
    // that would need a lifetime parameter in Rust.
    SECTION( "or from either of two" )
    {
        const Typed p( "const ref i32 pick( const ref i32 a, const ref i32 b ) { if( a > b ) { return a; } return b; }\n"
                       "i32 main() { i32 x = 1; i32 y = 2; return pick( x, y ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "from a mutable ref parameter" )
    {
        const Typed p( "const ref i32 look( ref i32 a ) { return a; }\n"
                       "i32 main() { i32 x = 1; return look( ref x ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "through a field of one" )
    {
        const Typed p( "struct P { i32 x; };\nconst ref i32 get( const ref P p ) { return p.x; }\n"
                       "i32 main() { P v = P { 1 }; return get( v ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A bare parameter of an owning type is already a read-only borrow, so it is a reference
    // parameter for this rule too - the caller owns the object and outlives the call.
    SECTION( "and from a bare owning parameter, which is one" )
    {
        const Typed p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                       "const ref u64 peek( B b ) { return b.n; }\n"
                       "i32 main() { B a = B( 1 ); return wrap<i32>( peek( a ) ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_refuses_an_escaping_reference", "[sema][escape]" )
{
    SECTION( "a local dies when the function returns" )
    {
        const Typed p( "const ref i32 bad() { i32 v = 1; return v; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must borrow from a parameter" ) != std::string::npos );
    }

    // A by-value parameter is the callee's own copy, so it dies with the frame exactly as a local
    // does. This is the case that reads as safe and is not.
    SECTION( "and so does a by-value parameter" )
    {
        const Typed p( "const ref i32 bad( i32 n ) { return n; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must borrow from a parameter" ) != std::string::npos );
    }

    SECTION( "a temporary has no scope to outlive the call" )
    {
        const Typed p( "i32 make() { return 1; }\nconst ref i32 bad() { return make(); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a field of a local is still local" )
    {
        const Typed p( "struct P { i32 x; };\nconst ref i32 bad() { P v = P { 1 }; return v.x; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Conservative and deliberately so: the rule traces the returned *expression*, and a local
    // binding is not a parameter however safe its referent happens to be. Relaxing this is
    // additive, so it waits for a program that wants it.
    SECTION( "and a local ref binding is not a parameter, even when its referent would be safe" )
    {
        const Typed p( "const ref i32 bad( const ref i32 a ) { const ref i32 r = a; return r; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// Only `const ref` may be returned. A mutable one would let a caller write through a reference it
// never asked for, and §8 gives up reseating in exchange for needing no lifetimes - not mutability
// through a returned binding.
TEST_CASE( "type_checker_refuses_a_mutable_ref_return", "[sema][escape]" )
{
    const Typed p( "ref i32 bad( ref i32 a ) { return a; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE_FALSE( p.clean() );
    REQUIRE( p.rendered().find( "only a `const ref` may be returned" ) != std::string::npos );
}

// The result names something that was alive at the call site, so it outlives any binding declared
// there - the same argument that makes the whole rule work, applied at the caller.
TEST_CASE( "type_checker_binds_the_result_of_a_const_ref_return", "[sema][escape]" )
{
    constexpr std::string_view pick = "const ref i32 pick( const ref i32 a ) { return a; }\n";

    SECTION( "it may be bound" )
    {
        const Typed p( std::string( pick ) + "i32 main() { i32 x = 1; const ref i32 r = pick( x ); return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and it may be copied" )
    {
        const Typed p( std::string( pick ) + "i32 main() { i32 x = 1; i32 v = pick( x ); return v; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // It is a `const ref`, so the binding it makes is read-only like any other.
    SECTION( "but not written through" )
    {
        const Typed p( std::string( pick ) + "i32 main() { i32 x = 1; const ref i32 r = pick( x ); r = 5; return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`r` is `const`" ) != std::string::npos );
    }

    // The contrast that keeps the temporaries hole shut: an ordinary call returns a value with no
    // scope of its own, and binding to one is still refused.
    SECTION( "and an ordinary call is still not a place" )
    {
        const Typed p( "i32 make() { return 1; }\ni32 main() { const ref i32 r = make(); return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "needs a variable to bind to" ) != std::string::npos );
    }
}

// Every claim above, asked of a call through a variable rather than through a name. The question a
// place asks is "does this callable hand back a binding", and for an indirect call the only thing
// that can answer is the signature the variable holds - there is no declaration behind it.
TEST_CASE( "type_checker_binds_the_result_of_a_const_ref_return_through_a_pointer", "[sema][escape][m7][mode]" )
{
    constexpr std::string_view pick = "const ref i32 pick( const ref i32 a ) { return a; }\n";
    constexpr std::string_view held = "fn( const ref i32 ) -> const ref i32 p = &pick; ";

    SECTION( "it may be bound" )
    {
        const Typed p(
            std::string( pick ) + "i32 main() { " + std::string( held ) + "i32 x = 1; const ref i32 r = p( x ); return r; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and it may be copied" )
    {
        const Typed p( std::string( pick ) + "i32 main() { " + std::string( held ) + "i32 x = 1; i32 v = p( x ); return v; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // No call result is writable, and the message names the variable the call went through because
    // that is the whole of what the call site wrote.
    SECTION( "but the result is not written through" )
    {
        const Typed p( std::string( pick ) + "i32 main() { " + std::string( held ) + "i32 x = 1; p( x ) = 5; return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`p` returns a const ref, so it cannot be modified" ) != std::string::npos );
    }

    // A function whose *return type* is a borrow-returning signature. The signature says
    // `const ref`, the call hands one back by value, and asking the wrong one of those two makes a
    // temporary into a place.
    SECTION( "and a function returning such a signature is not one" )
    {
        const Typed p( "const ref i32 pick( const ref i32 a ) { return a; }\n"
                       "fn( const ref i32 ) -> const ref i32 maker() { return &pick; }\n"
                       "i32 main() { const ref fn( const ref i32 ) -> const ref i32 r = maker(); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "needs a variable to bind to" ) != std::string::npos );
    }

    // The contrast, one step over: a pointer to a function returning a *value* hands back nothing
    // with a scope of its own, so binding to it is refused exactly as an ordinary call is.
    SECTION( "and a pointer to a value-returning function is still not a place" )
    {
        const Typed p( "i32 make( i32 a ) { return a; }\n"
                       "i32 main() { fn( i32 ) -> i32 p = &make; const ref i32 r = p( 1 ); return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "needs a variable to bind to" ) != std::string::npos );
    }
}

// PLAN D32. `ref` is a binding mode, not a type: the declaration keeps `T`, so every use inside the
// body is an ordinary `T`. The address it travels as goes on the annotation, which nothing else
// records a type on - and which is what lowering and the emitter read back.
TEST_CASE( "type_checker_types_a_ref_parameter_as_its_referent", "[sema][ref]" )
{
    const Typed p( "void bump( ref i32 n ) { n = n + 1; }\ni32 main() { i32 x = 1; bump( ref x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    // An i32 and not a pointer - `n = n + 1` above would not have typed otherwise.
    REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 0 ) ) == "i32" );
    REQUIRE( p.type_name( p.nth( Node_kind::Mode_type, 0 ) ) == "i32*" );
}

// The same mechanism reaches a class, which is what makes it worth having: a borrow is the only
// thing §6.6 leaves available for passing one without giving it away.
TEST_CASE( "type_checker_lends_a_class_by_ref", "[sema][ref]" )
{
    const Typed p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                   "void grow( ref B b ) { b.n = b.n + 1; }\n"
                   "i32 main() { B a = B( 1 ); grow( ref a ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.type_name( p.nth( Node_kind::Mode_type, 0 ) ) == "B*" );
}

// A `move` parameter is not a borrow: the callee owns what it was given, so it is mutable, movable
// and dropped there. Its own case because the first version of this rule classified one as a
// borrow, which silently took the drop flag off every conditionally-moved local.
TEST_CASE( "type_checker_leaves_a_move_parameter_owned", "[sema][borrow]" )
{
    constexpr std::string_view owning = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n";

    SECTION( "it can be modified" )
    {
        const Typed p( std::string( owning ) + "void own( move B b ) { b.n = 99; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "it can be moved on" )
    {
        const Typed p(
            std::string( owning ) + "void take( move B b ) { }\nvoid own( move B b ) { take( move b ); }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and it can be returned" )
    {
        const Typed p( std::string( owning ) + "B own( move B b ) { return b; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The receiver is a pointer rather than a borrow binding, so writing through it is untouched -
// which is what keeps every destructor written so far compiling.
TEST_CASE( "type_checker_leaves_the_receiver_alone", "[sema][borrow]" )
{
    const Typed p( "class C { u64 n; C( u64 x ) { n = x; } ~C() { n = 0; } };\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// A bare parameter is a borrow when its type owns, and `Wrap<Tracked>` owns only through its
// argument - so the borrow has to be decided per instance, or the callee gets a copy it frees.
TEST_CASE( "type_checker_borrows_an_instance_that_owns_through_its_argument", "[sema][borrow][generic]" )
{
    const Typed p( "class Tracked { public i32 n; Tracked( i32 m ) { n = m; } ~Tracked() { } };\n"
                   "class Wrap<T> { public T v; Wrap( move T x ) { v = move x; } };\n"
                   "void consume( move Wrap<Tracked> w ) { }\n"
                   "void f( Wrap<Tracked> w ) { consume( move w ); }\n"
                   "i32 main() { return 0; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "cannot move out of a borrow" ) != std::string::npos );
}

// A constructor's parameters are declared by a pass that runs *before* field types are recorded,
// which is exactly why the borrow rule is a pass of its own. Without that they would never be borrows.
TEST_CASE( "type_checker_borrows_in_a_member_function_too", "[sema][borrow]" )
{
    constexpr std::string_view owning = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n";

    SECTION( "a constructor may read one" )
    {
        const Typed p(
            std::string( owning ) + "class W { u64 m; W( B b ) { m = b.n; } ~W() { } };\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and may not write one" )
    {
        const Typed p(
            std::string( owning ) + "class W { u64 m; W( B b ) { b.n = 1; m = 0; } ~W() { } };\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` is borrowed" ) != std::string::npos );
    }
}

// PLAN D31/D32. `const ref T` is the read-only borrow: it travels by address like `ref`, and is
// unwritable like `const`. Neither half is new - both predicates already existed - and putting them
// together is what finally makes passing a large struct read-only without copying expressible.
//
// D31 said `const T&` did not survive "because a bare argument already means it". That is true of a
// `class` and false of a `struct`, where bare is a copy - so this is a genuinely new parameter form
// rather than a second spelling of one.
TEST_CASE( "type_checker_accepts_a_const_ref_parameter", "[sema][constref]" )
{
    constexpr std::string_view point = "struct P { i32 x; };\n";

    SECTION( "it is typed as its referent" )
    {
        const Typed p( std::string( point ) + "i32 peek( const ref P p ) { return p.x; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        // A P and not a pointer, or `p.x` above would have needed D22's reach-through.
        REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 0 ) ) == "P" );

        // The address goes on the *outermost* node of the annotation, which for `const ref` is the
        // Const_type - that is the node is_borrowed_binding reads, and putting it on the Mode_type
        // underneath would leave the binding looking like an ordinary parameter.
        REQUIRE( p.type_name( p.nth( Node_kind::Const_type, 0 ) ) == "P*" );
    }

    SECTION( "and it may not be written through" )
    {
        const Typed p( std::string( point ) + "i32 peek( const ref P p ) { p.x = 5; return p.x; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`p` is `const`" ) != std::string::npos );
    }
}

// record_borrowed_parameters walks the node array rather than the tree, so it sees nodes nothing
// references. A declaration whose name failed to parse becomes an Error node, but the parameters
// and the synthesised receiver built before that point stay in the array untyped - and
// `pointer_to` on an invalid type asserts rather than degrading. Every one of these aborted the
// compiler.
TEST_CASE( "type_checker_ignores_the_orphans_of_a_failed_declaration", "[sema][types][recovery]" )
{
    SECTION( "a method named by a keyword" )
    {
        // The receiver is a synthesised `ref C` parameter, so it is by-address and reaches the
        // walk even though the Method_decl was never built.
        const Typed p( "class C { i32 x; i32 case() { return x; } };" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "every keyword, in that position" )
    {
        for( const char* keyword :
             { "this",   "if",   "return", "switch", "case",   "default",  "break",  "continue", "while", "for",    "else",
               "struct", "enum", "class",  "const",  "auto",   "unsafe",   "extern", "alloc",    "free",  "move",   "out",
               "ref",    "cast", "wrap",   "new",    "delete", "template", "import", "true",     "false", "nullptr" } )
        {
            const std::string source = std::string( "class C { i32 x; i32 " ) + keyword + "() { return x; } };";
            const Typed       p( source );

            INFO( source << "\n" << p.rendered() );
            REQUIRE_FALSE( p.clean() );
        }
    }

    SECTION( "a free function named by a keyword, with a ref parameter" )
    {
        const Typed p( "i32 case( ref i32 a ) { return a; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "with an out parameter" )
    {
        const Typed p( "void case( out i32 a ) { a = 1; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a method named by a keyword with a ref parameter" )
    {
        const Typed p( "class C { i32 x; i32 case( ref i32 a ) { return a; } };" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a valid declaration beside a failed one is still checked" )
    {
        // The orphans must be skipped without the walk losing the real parameters around them.
        const Typed p( "class C { i32 x; i32 case() { return x; } i32 get( ref i32 a ) { return a + x; } };" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "expected an identifier" ) != std::string::npos );
    }
}

// `const` attaches to the name, so `i32* const p` - a const *binding* holding a pointer - is
// expressible and means exactly what C++ means by it.
TEST_CASE( "type_checker_gives_a_const_pointer_its_c_meaning", "[sema][const]" )
{
    SECTION( "the pointer cannot be reseated" )
    {
        const Typed p( "i32 main() { i32 y = 1; i32 z = 2; i32* const q = &y; q = &z; return y; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`q` is `const`" ) != std::string::npos );
    }

    SECTION( "but writing through it is allowed" )
    {
        const Typed p( "i32 main() { i32 y = 1; i32* const q = &y; *q = 5; return y; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// PLAN §6.7. A place rooted in a call reached check_writable as an invalid `Node_id`, because
// place_root resolved only a `Name_expr` - and the guard that treats an unrooted place as writable
// then skipped every rule below it. Two different bugs shared that one line.
TEST_CASE( "type_checker_refuses_a_write_through_a_returned_reference", "[sema][constref]" )
{
    constexpr std::string_view lend = "struct P { i32 t; };\n"
                                      "const ref P by_ref( const ref P p ) { return p; }\n";

    // Not merely useless: this one compiled to a store and changed the object.
    SECTION( "a field of one cannot be assigned" )
    {
        const Typed p( std::string( lend ) + "i32 main() { P a = P { 1 }; by_ref( a ).t = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`by_ref` returns a const ref" ) != std::string::npos );
    }

    // A `const` method's return let a caller write the object the method could not touch.
    SECTION( "a method's return is the same rule" )
    {
        const Typed p( "struct P { i32 t; };\n"
                       "struct H { P inner; const ref P get() const { return inner; } };\n"
                       "i32 main() { H h = H { P { 1 } }; h.get().t = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`get` returns a const ref" ) != std::string::npos );
    }

    SECTION( "reading through one stays legal" )
    {
        const Typed p( std::string( lend ) + "i32 main() { P a = P { 1 }; return by_ref( a ).t; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // What a pointer points at was never part of the call's result, so this is an ordinary write.
    // The bare `p.t` spelling reaches through the pointer, which is the form the walk actually sees.
    SECTION( "a pointer's referent is not covered" )
    {
        const Typed p( "struct P { i32 t; };\n"
                       "P* by_pointer( P* p ) { return p; }\n"
                       "i32 main() { P a = P { 1 }; by_pointer( &a ).t = 3; return a.t; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// PLAN §6.7. The other half: a value with no storage of its own. This replaces a case that asserted
// the opposite - it held that refusing this needed value categories v0 does not have, and naming
// what the place came from turned out to be enough.
TEST_CASE( "type_checker_refuses_a_write_to_a_temporary", "[sema][places]" )
{
    constexpr std::string_view make = "struct P { i32 t; };\nP make() { return P { 1 }; }\n";

    SECTION( "a field of a by-value call cannot be assigned" )
    {
        const Typed p( std::string( make ) + "i32 main() { make().t = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "this value is a temporary" ) != std::string::npos );
    }

    // Both arms are places and the result still is not - the conditional materialises a value.
    SECTION( "a conditional's result is one too" )
    {
        const Typed p( "struct P { i32 t; };\n"
                       "i32 main() { P a = P { 1 }; P b = P { 2 }; ( true ? a : b ).t = 3; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "this value is a temporary" ) != std::string::npos );
    }

    // This one reached the lowerer and aborted, so refusing it here removes a crash rather than a
    // silently accepted program.
    SECTION( "a struct literal is one, and used to abort" )
    {
        const Typed p( "struct P { i32 t; };\ni32 main() { P { 1 }.t = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "this value is a temporary" ) != std::string::npos );
    }

    // The discriminator is returns_a_binding, not is_const_binding: a `const` by-value return is a
    // temporary like any other, and keying on constness would call it a reference.
    SECTION( "a const by-value return is a temporary, not a reference" )
    {
        const Typed p( "struct P { i32 t; };\n"
                       "const P make() { return P { 1 }; }\n"
                       "i32 main() { make().t = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "this value is a temporary" ) != std::string::npos );
    }

    // M7 slice 3. The callee is a variable, and a variable's child 0 is its type annotation - so
    // asking whether it returns a binding answers yes and names a reason that does not exist.
    SECTION( "a call through a function-typed variable is one too" )
    {
        const Typed p( std::string( make ) + "i32 main() { fn() -> P f = &make; f().t = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "this value is a temporary" ) != std::string::npos );
    }

    // Reading is what a temporary is for, and refusing it would take the conditional's own tests.
    SECTION( "reading through one stays legal" )
    {
        const Typed p( std::string( make ) + "i32 main() { return make().t; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A `T[*]` binding's `const` is the pointer's, exactly as on `T*`: it cannot be reseated, and what
// it points at stays writable - a read-only buffer is `String`'s decision.
TEST_CASE( "type_checker_gives_a_const_many_item_pointer_its_c_meaning", "[sema][const][many]" )
{
    SECTION( "the pointer cannot be reseated" )
    {
        const Typed p( "i32 main() { i32[*] const q = nullptr; q = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`q` is `const`" ) != std::string::npos );
    }

    SECTION( "but its elements can be written" )
    {
        const Typed p( "i32 main() { unsafe { i32[*] const q = alloc<i32>( 1 ); q[ 0 ] = 5; free( q ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "through a const ref parameter too, since the pointer is what is borrowed" )
    {
        const Typed p( "void set( const ref i32[*] q ) { unsafe { q[ 0 ] = 1; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "while a const ref parameter's pointer is still not reseated" )
    {
        const Typed p( "void set( const ref i32[*] q ) { q = nullptr; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a const struct's field of this type is the same" )
    {
        const Typed p( "struct B { i32[*] data; };\n"
                       "void set( const ref B b ) { unsafe { b.data[ 0 ] = 1; } }\n"
                       "void reseat( const ref B b ) { b.data = nullptr; }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 ); // the reseat, not the element write
    }
}

namespace
{
constexpr std::string_view pointees = "struct P { i32 x; };\n"
                                      "class C { C() { n = 1; } public void bump() { n += 1; } "
                                      "public i32 peek() const { return n; } private i32 n; };\n"
                                      "void set( ref i32 v ) { v = 1; }\n";

std::string in_main( std::string_view body )
{
    return fmt::format( "{}i32 main() {{ i32 y = 0; P p = P {{ 1 }}; C c = C(); {} return 0; }}", pointees, body );
}
} // namespace

// A pointer to const reads what it points at and nothing writes through it: every way of naming the
// pointee is a place check_writable refuses, the same way it refuses a `const` binding.
TEST_CASE( "type_checker_refuses_writes_through_a_pointer_to_const", "[sema][const]" )
{
    struct Case
    {
        const char* body;
        const char* through; // the pointer type the message names
    };

    for( const Case c : {
             Case { "const i32* q = &y; *q = 2;", "const i32*" },
             Case { "const P* q = &p; q.x = 2;", "const P*" },
             Case { "const P* q = &p; ( *q ).x = 2;", "const P*" },
             Case { "const i32[*] q = nullptr; unsafe { q[ 0 ] = 1; }", "const i32[*]" },
             Case { "const P[*] q = nullptr; unsafe { q[ 0 ].x = 1; }", "const P[*]" },
             Case { "const i32* q = &y; ( *q )++;", "const i32*" },
             Case { "const i32* q = &y; set( ref *q );", "const i32*" },
         } )
    {
        const Typed p( in_main( c.body ) );

        INFO( c.body << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE(
            p.rendered().find( fmt::format( "this is reached through a `{}`, so it cannot be modified", c.through ) ) !=
            std::string::npos
        );
        REQUIRE( p.rendered().find( "a pointer to `const` only reads what it points at" ) != std::string::npos );
    }
}

TEST_CASE( "type_checker_reads_through_a_pointer_to_const", "[sema][const]" )
{
    for( const char* body : {
             "const i32* q = &y; i32 z = *q + 1;",
             "const P* q = &p; i32 z = q.x + ( *q ).x;",
             "const C* q = &c; i32 z = q.peek();",
             "i32* w = &y; const i32* q = w;", // const is added implicitly
             "i32* w = &y; const i32* q = &y; bool b = q == w && w == q;",
             "const i32* q = nullptr; bool b = q == nullptr;",
             "const void* q = nullptr;",
             "i32* w = &y; const i32* const* q = nullptr; i32* const* r = &w;",
             "const i32* q = &y; unsafe { i32* w = cast<i32*>( q ); *w = 1; }", // dropping it is unsafe
         } )
    {
        const Typed p( in_main( body ) );

        INFO( body << "\n" << p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_never_drops_const_implicitly", "[sema][const]" )
{
    SECTION( "into a plain pointer" )
    {
        const Typed p( in_main( "const i32* q = &y; i32* w = q;" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "expected `i32*`, but got `const i32*`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "a pointer to `const` never converts back to one that writes" ) != std::string::npos );
    }

    // `T**` to `const T**` would let a `const T*` be stored where a `T*` is read back.
    SECTION( "nor adds it two levels down" )
    {
        const Typed p( in_main( "i32** raw = nullptr; const i32** q = raw;" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "expected `const i32**`, but got `i32**`" ) != std::string::npos );
    }

    SECTION( "a cast that drops it needs an unsafe block" )
    {
        const Typed p( in_main( "const i32* q = &y; i32* w = cast<i32*>( q );" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "needs an `unsafe` block" ) != std::string::npos );
    }
}

TEST_CASE( "type_checker_calls_only_const_methods_through_a_pointer_to_const", "[sema][const]" )
{
    const Typed p( in_main( "const C* q = &c; q.bump();" ) );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "`bump` may modify its object, which is reached through a `const C*`" ) != std::string::npos );
    REQUIRE( p.rendered().find( "a pointer to `const` can call only `const` methods" ) != std::string::npos );
}

// Reported after the block is counted, so the block is not also called pointless.
TEST_CASE( "type_checker_refuses_to_free_a_pointer_to_const", "[sema][const]" )
{
    const Typed p( in_main( "const i32[*] q = nullptr; unsafe { free( q ); }" ) );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "`free` cannot release a `const i32[*]`, which points to `const`" ) != std::string::npos );
    REQUIRE( p.rendered().find( "if it came from `alloc`, `cast<i32[*]>` it back first" ) != std::string::npos );
}

// The hole this closes: `&` on a read-only place made a pointer that wrote through it.
TEST_CASE( "type_checker_takes_the_address_of_a_read_only_place_as_a_pointer_to_const", "[sema][const]" )
{
    SECTION( "each is a pointer to const" )
    {
        struct Case
        {
            const char* program;
            std::size_t q; // which Var_decl is `q`
        };

        for( const Case c : {
                 Case { "i32 f() { const i32 x = 1; auto q = &x; return 0; }", 1 },
                 Case { "i32 f( const ref i32 r ) { auto q = &r; return 0; }", 0 },
                 Case { "struct S { i32 x; };\ni32 f() { const S s = S { 1 }; auto q = &s.x; return 0; }", 1 },
                 Case { "class C { C() { n = 1; } i32 f() const { auto q = &n; return 0; } private i32 n; };", 0 },
                 Case { "class B { public i32 n; B() { n = 1; } ~B() { } };\ni32 f( B b ) { auto q = &b.n; return 0; }", 0 },
                 Case {
                     "enum Shape { Circle( i32 r ), Dot };\n"
                     "i32 f( Shape s ) { switch( s ) { case Shape::Circle( r ): { auto q = &r; return 0; } default: return 0; "
                     "} }",
                     0
                 },
                 Case {
                     "const ref i32 h( const ref i32 a ) { return a; }\ni32 f() { i32 y = 0; auto q = &h( y ); return 0; }", 1
                 },
                 Case { "class C { public const i32 n; C() { n = 1; } };\ni32 f() { C c = C(); auto q = &c.n; return 0; }", 1 },
                 Case { "class C { const i32 n; C() { n = 1; auto q = &n; } };", 0 },
             } )
        {
            const Typed p( std::string( c.program ) + "\ni32 main() { return 0; }" );

            INFO( c.program << "\n" << p.rendered() );
            REQUIRE( p.clean() );
            REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, c.q ) ) == "const i32*" );
        }
    }

    SECTION( "and none of them converts to a pointer that writes" )
    {
        for( const char* program : {
                 "i32 f() { const i32 x = 1; i32* q = &x; *q = 2; return x; }",
                 "i32 f( const ref i32 r ) { i32* q = &r; *q = 5; return r; }",
                 "struct S { i32 x; };\ni32 f() { const S s = S { 1 }; i32* q = &s.x; *q = 9; return s.x; }",
                 "class C { C() { n = 1; } i32 f() const { i32* q = &n; *q = 7; return n; } private i32 n; };",
             } )
        {
            const Typed p( std::string( program ) + "\ni32 main() { return 0; }" );

            INFO( program << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "expected `i32*`, but got `const i32*`" ) != std::string::npos );
        }
    }

    SECTION( "a writable place's address still writes" )
    {
        const Typed p( "i32 f( ref i32 r ) { i32 y = 0; i32* a = &y; i32* b = &r; return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A `const` field is assigned by its own constructor, whole, and by nothing else.
TEST_CASE( "type_checker_lets_a_constructor_assign_its_const_field", "[sema][const][field]" )
{
    for( const char* program : {
             "class C { const i32 n; C() { n = 1; } };",
             "class C { const i32 n; C() { this.n = 1; } };",
             "class C { public u8[*] const p; C() { unsafe { p = alloc<u8>( 1 ); } } ~C() { unsafe { free( p ); } } };",
             "class C { public const u8[*] const p; C( const u8[*] q ) { p = q; } };",
             "class Box<T> where T : Copyable { public const T v; Box( T x ) { v = x; } };\n"
             "i32 f() { Box<i32> b = Box<i32>( 1 ); return b.v; }",
             // Read like any field, from any method.
             "class C { const i32 n; C() { n = 1; } i32 get() const { return n; } i32 twice() { return n + this.n; } };",
             // A struct's comes from its literal, and the struct is still replaced whole.
             "struct P { const i32 x; i32 y; };\ni32 f() { P p = P { 1, 2 }; p.y = 3; p = P { 4, 5 }; return p.x; }",
         } )
    {
        const Typed p( std::string( program ) + "\ni32 main() { return 0; }" );

        INFO( program << "\n" << p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_refuses_writes_to_a_const_field", "[sema][const][field]" )
{
    struct Case
    {
        const char* program;
        const char* help;
    };

    constexpr const char* outside = "only a constructor assigns it";
    constexpr const char* inside  = "a constructor assigns it once, whole, with `=`";

    for( const Case c : {
             Case { "class C { const i32 n; C() { n = 1; } void set() { n = 2; } };", outside },
             Case { "class C { const i32 n; C() { n = 1; } void bump() { this.n++; } };", outside },
             Case { "class C { const i32 n; C() { n = 1; } ~C() { n = 0; } };", outside },
             Case { "class C { public const i32 n; C() { n = 1; } };\nvoid f( ref C c ) { c.n += 1; }", outside },
             Case { "class C { public const i32 n; C() { n = 1; } };\nvoid f( C* c ) { c.n = 2; }", outside },
             Case { "struct P { const i32 x; };\nvoid f( ref P p ) { p.x = 2; }", outside },
             Case { "struct P { const i32 x; };\nstruct Q { P p; };\nvoid f( ref Q q ) { q.p.x = 2; }", outside },
             // Another object of its own class is not the one being built.
             Case { "class C { const i32 n; C() { n = 1; } C( ref C o ) { n = 1; o.n = 2; } };", outside },
             Case { "class C { const i32 n; C() { n = 1; n += 1; } };", inside },
             Case { "struct P { i32 x; };\nclass C { const P p; C() { p = P { 1 }; p.x = 2; } };", inside },
             Case { "void set( ref i32 a ) { a = 1; }\nclass C { const i32 n; C() { set( ref n ); } };", inside },
             // `out` too: only `=` sets one, so every address of it only reads.
             Case { "void init( out i32 a ) { a = 1; }\nclass C { const i32 n; C() { init( out n ); } };", inside },
             Case {
                 "class In { i32 v; In() { v = 0; } void poke() { v = 1; } };\n"
                 "class C { const In i; C() { i = In(); i.poke(); } };",
                 inside
             },
         } )
    {
        const Typed p( std::string( c.program ) + "\ni32 main() { return 0; }" );

        INFO( c.program << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "is a `const` field, so it cannot be modified" ) != std::string::npos );
        REQUIRE( p.rendered().find( c.help ) != std::string::npos );
    }
}

// An element is a place: it can be written, have its address taken and be passed by `ref`.
TEST_CASE( "type_checker_treats_an_element_as_a_place", "[sema][places][many]" )
{
    constexpr std::string_view head = "struct P { i32 x; };\n"
                                      "void bump( ref i32 v ) { v = v + 1; }\n"
                                      "void fill( out i32 v ) { v = 9; }\n"
                                      "i32 main() { i32[*] many = nullptr; P[*] points = nullptr; "
                                      "unsafe { many = alloc<i32>( 2 ); points = alloc<P>( 2 ); } ";
    constexpr std::string_view tail = " unsafe { free( many ); free( points ); } return 0; }";

    for( const char* body : {
             "unsafe { many[ 0 ] = 1; }",
             "unsafe { points[ 1 ].x = 1; }",
             "unsafe { bump( ref many[ 0 ] ); }",
             "unsafe { fill( out many[ 1 ] ); }",
             "unsafe { bump( ref points[ 0 ].x ); }",
             "unsafe { ref i32 r = many[ 0 ]; r = 4; }",
             "unsafe { i32* e = &points[ 1 ].x; *e = 3; }",
         } )
    {
        const Typed p( std::string( head ) + body + std::string( tail ) );

        INFO( body << "\n" << p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an offset is a value, not a place" )
    {
        const Typed p( std::string( head ) + "unsafe { many + 1 = nullptr; }" + std::string( tail ) );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// D37 says writing through a pointer never drops what was there, because what was there may never
// have been a value - `alloc` hands back raw memory. An element is the same, so an owning element is
// allowed; destroying one in place, and moving one out, are `Vector`'s questions.
TEST_CASE( "type_checker_leaves_owning_elements_raw", "[sema][places][many]" )
{
    constexpr std::string_view head = "class C { i32 n; C( i32 v ) { n = v; } ~C() { } };\n"
                                      "i32 main() { C[*] cs = nullptr; unsafe { cs = alloc<C>( 2 ); } ";
    constexpr std::string_view tail = " unsafe { free( cs ); } return 0; }";

    SECTION( "a temporary is written into a slot" )
    {
        const Typed p( std::string( head ) + "unsafe { cs[ 0 ] = C( 1 ); }" + std::string( tail ) );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a named value is moved into one" )
    {
        const Typed p( std::string( head ) + "C c = C( 2 ); unsafe { cs[ 1 ] = move c; }" + std::string( tail ) );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and not copied" )
    {
        const Typed p( std::string( head ) + "C c = C( 2 ); unsafe { cs[ 1 ] = c; }" + std::string( tail ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "an owning value is transferred, not copied" ) != std::string::npos );
    }

    SECTION( "reading one out would copy it, which is refused" )
    {
        const Typed p( std::string( head ) + "C c = C( 0 ); unsafe { c = cs[ 0 ]; }" + std::string( tail ) );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "an owning value is transferred, not copied" ) != std::string::npos );
    }

    SECTION( "borrowing one is fine" )
    {
        const Typed p( "class C { i32 n; C( i32 v ) { n = v; } ~C() { } i32 get() const { return n; } };\n"
                       "i32 peek( const ref C c ) { return c.get(); }\n"
                       "i32 main() { C[*] cs = nullptr; i32 r = 0; unsafe { cs = alloc<C>( 1 ); cs[ 0 ] = C( 3 ); "
                       "r = peek( cs[ 0 ] ) + cs[ 0 ].get(); free( cs ); } return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

} // namespace keel
#endif
