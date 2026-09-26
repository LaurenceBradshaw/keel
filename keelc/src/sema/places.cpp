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
        const Node_id method = callees_.callee_of( id );
        const Node_id callee = method.is_valid() ? method : resolution_.declaration_of( ast_.child( id, 0 ) );

        return callee.is_valid() &&
               ( ast_.kind( callee ) == Node_kind::Function_decl || ast_.kind( callee ) == Node_kind::Method_decl ) &&
               returns_a_binding( callee );
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

    const Node_id decl = ast_.kind( id ) == Node_kind::Name_expr ? resolution_.declaration_of( id ) : Node_id {};
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

// D31/D32: what may be written. A `const` declaration says so itself; a borrow is read-only because
// someone else owns it - different reasons, so different messages and different fixes.
bool Places::check_writable( Node_id target, Node_id current_function )
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

            if( returns_a_binding( callee ) )
            {
                reporter_.error_at(
                    ast_.span( target ),
                    fmt::format(
                        "`{}` returns a const ref, so it cannot be modified", interner_.text( Symbol_id { ast_.aux( callee ) } )
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

    const Node_id root = place_root( target, current_function );

    if( !root.is_valid() )
    {
        return true;
    }

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

    const Node_id decl = ast_.kind( id ) == Node_kind::Name_expr ? resolution_.declaration_of( id ) : Node_id {};

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
    while( ast_.kind( id ) == Node_kind::Field_expr )
    {
        const Node_id object = ast_.child( id, 0 );

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

    reporter_.error_at(
        ast_.span( value ),
        "an owning value is transferred, not copied",
        fmt::format( "write `move {}`", reporter_.text( ast_.span( value ) ) )
    );
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

// A constructor's parameters are declared by a pass that runs *before* compute_owning, which is
// exactly why the borrow rule is a pass of its own. Without that they would never be borrows.
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

// PLAN §12. A place rooted in a call reached check_writable as an invalid `Node_id`, because
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

// PLAN §12. The other half: a value with no storage of its own. This replaces a case that asserted
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

} // namespace keel
#endif
