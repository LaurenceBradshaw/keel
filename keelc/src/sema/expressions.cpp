#include "sema/expressions.h"

#include <fmt/format.h>

#include <optional>
#include <unordered_map>

// The expression walk: the `infer`/`check` dispatch and one member per node kind.

namespace keel
{

namespace sema
{

Type_id Expressions::infer( Node_id id )
{
    if( !id.is_valid() )
    {
        return Type_id {};
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Error:
        return types_.record( id, table_.builtin( Type_kind::Error ) );

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
        return literals_.infer_literal( id );

    case Node_kind::String_literal:
        // Lexed and parsed, but a string has no type until there is a String, which is M7.
        // Silence here would make it look accepted.
        reporter_.error_at( ast_.span( id ), "string literals are not supported yet" );
        return types_.record( id, table_.builtin( Type_kind::Error ) );

    case Node_kind::Field_expr:
        return infer_field( id );

    case Node_kind::Path_expr:
        return infer_path( id );

    case Node_kind::Struct_literal:
        return infer_struct_literal( id );

    case Node_kind::Cast_expr:
        return infer_cast( id );

    case Node_kind::Alloc_expr:
        return infer_alloc( id );

    case Node_kind::Free_expr:
        return infer_free( id );

    case Node_kind::Marker_expr:
        return infer_marker( id );

    case Node_kind::Conditional_expr:
        return infer_conditional( id );

    default:
        // Every expression not yet given a case of its own - the literals, chiefly, which cannot
        // be typed until their values survive lexing. Children are still typed, so a mistake
        // inside one is not swallowed by the parent being unsupported.
        for( const Node_id child : ast_.children( id ) )
        {
            infer( child );
        }

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }
}

Type_id Expressions::infer_name( Node_id id )
{
    const Node_id decl = resolution_.declaration_of( id );

    if( !decl.is_valid() )
    {
        return types_.record( id, table_.builtin( Type_kind::Error ) ); // the resolver already said so
    }

    if( ast_.kind( decl ) == Node_kind::Function_decl )
    {
        // types_.type_of( decl ) holds the function's *return* type, so without this `i32 x = f;` would
        // quietly succeed whenever f happens to return an i32.
        reporter_.error_at(
            ast_.span( id ), fmt::format( "`{}` is a function, not a value", interner_.text( Symbol_id { ast_.aux( id ) } ) )
        );

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    return types_.record( id, types_.type_of( decl ) );
}

// D26: a literal has a value rather than a type, and context is what gives it one - so where the
// context is the thing being chosen, a literal narrows the set to a family and no further. A struct
// literal takes its instance from the expectation in the same way, so it says nothing at all.
Argument_shape Expressions::argument_shape( Node_id argument )
{
    Argument_shape shape;

    Node_id value = argument;

    if( ast_.kind( argument ) == Node_kind::Marker_expr )
    {
        shape.marker = static_cast<Keyword>( ast_.aux( argument ) );
        value        = ast_.child( argument, 0 );
    }

    if( literals_.is_literal_expression( value ) )
    {
        const Node_id inner = ast_.kind( value ) == Node_kind::Unary_expr ? ast_.child( value, 0 ) : value;

        switch( ast_.kind( inner ) )
        {
        case Node_kind::Int_literal:
        case Node_kind::Char_literal:
            shape.kind = Argument_kind::Integer;
            return shape;

        case Node_kind::Float_literal:
            shape.kind = Argument_kind::Floating;
            return shape;

        case Node_kind::Bool_literal:
            // `true` has one type, so it is as informative as a variable - it just has not been
            // recorded yet, because Literals::check_literal is what records a literal.
            shape.kind = Argument_kind::Typed;
            shape.type = table_.builtin( Type_kind::Bool );
            return shape;

        default:
            return shape; // `nullptr`, which every pointer accepts
        }
    }

    if( ast_.kind( value ) == Node_kind::Struct_literal )
    {
        return shape;
    }

    shape.kind     = Argument_kind::Typed;
    shape.type     = infer( argument );
    shape.recorded = true;

    return shape;
}

Type_id Expressions::infer_call( Node_id id )
{
    const Node_id callee = ast_.child( id, 0 );
    const Node_id args   = ast_.child( id, 1 );

    // D7: `Shape::Circle( 1.0 )` constructs a variant. Handled before the ordinary call path
    // because the callee is a Path_expr rather than a name, and because what it checks the
    // arguments against is a *payload* rather than a parameter list.
    if( ast_.kind( callee ) == Node_kind::Path_expr )
    {
        return infer_variant_construction( id );
    }

    // `p.area()` - a method call. The receiver is the Field_expr's object, and the callable is
    // found on its type, which is the same lookup infer_field does for a field name.
    if( ast_.kind( callee ) == Node_kind::Field_expr )
    {
        return infer_method_call( id );
    }

    // v0 has no function pointers, so anything but a plain name in call position has no
    // declaration to find.
    const Node_id decl = ast_.kind( callee ) == Node_kind::Name_expr ? resolution_.declaration_of( callee ) : Node_id {};

    // Taken and cleared, because the expectation belongs to this call and to nothing inside it:
    // in `f( g() )` the type wanted of `f` says nothing about what `g` should produce. What it
    // does say is what a type parameter appearing only in the return type must be.
    const Type_id expectation = expected_;
    expected_                 = Type_id {};

    // What choosing between candidates already had to type. Empty until it does, which is every
    // call with one candidate - so the ordinary path types each argument exactly once, as before.
    std::vector<Argument_shape> shapes;

    // Even on a failed call the arguments must be typed, or later passes meet untyped nodes and a
    // genuine mistake inside one goes unreported. Typed *once*: an argument reported twice is one
    // mistake told twice.
    const auto type_the_arguments_anyway = [&]()
    {
        const std::span<const Node_id> list = ast_.children( args );

        for( std::size_t i = 0; i < list.size(); ++i )
        {
            if( i < shapes.size() && shapes[i].recorded )
            {
                continue;
            }

            infer( list[i] );
        }
    };

    if( !decl.is_valid() )
    {
        // An unknown name was already reported by the resolver; say nothing twice.
        if( ast_.kind( callee ) != Node_kind::Name_expr )
        {
            reporter_.error_at( ast_.span( callee ), "this expression is not callable" );
        }

        type_the_arguments_anyway();
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    const Node_id          type_args = ast_.child( id, 2 );
    const std::string_view name      = interner_.text( Symbol_id { ast_.aux( callee ) } );

    // What the arguments are checked against, what the call produces, and how many leading
    // parameters are not the author's to supply. For a plain function all three are the obvious
    // answers; a construction is where they come apart.
    Type_id result          = types_.type_of( decl );
    u32     implicit_params = 0;

    std::vector<Node_id> candidates;

    if( is_aggregate( ast_.kind( decl ) ) )
    {
        // `Buffer( 16 )` names a type, not a function: the arguments belong to its constructors,
        // but the result is the type itself - a constructor returns nothing and writes through
        // `this`. They share the type's name rather than a scope, so they have no chain to walk.
        for( const Node_id member : ast_.members( decl ) )
        {
            if( ast_.kind( member ) == Node_kind::Constructor_decl )
            {
                candidates.push_back( member );
            }
        }

        if( candidates.empty() )
        {
            reporter_.error_at(
                ast_.span( callee ),
                fmt::format( "`{}` has no constructor", name ),
                fmt::format( "build it from a literal: `{} {{ ... }}`", name )
            );
            type_the_arguments_anyway();
            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }

        // The receiver is an ordinary first parameter, so skipping it here is what stops every
        // call site owing an extra argument.
        implicit_params = 1;
    }
    else if( ast_.kind( decl ) == Node_kind::Method_decl )
    {
        // A bare `add( by )` inside a method is `this.add( by )`: the member scope puts a sibling
        // in reach by name, and the receiver is the one this function was given.
        return infer_implicit_method_call( id, decl );
    }
    else if( ast_.kind( decl ) != Node_kind::Function_decl )
    {
        reporter_.error_at( ast_.span( callee ), fmt::format( "`{}` is not callable", name ) );
        type_the_arguments_anyway();
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }
    else
    {
        for( Node_id candidate = decl; candidate.is_valid(); candidate = resolution_.next_overload( candidate ) )
        {
            candidates.push_back( candidate );
        }
    }

    // Which instance a failed candidate list should be spelled in terms of. A construction has one
    // before anything is deduced, because the expectation already names it; a free function does
    // not, and `f` there is what the author wrote.
    const Type_id constructed = is_aggregate( ast_.kind( decl ) ) ? expectation : Type_id {};

    // Resolved once and read by every candidate: resolving inside the loop would report an unknown
    // type once per candidate. Left empty on the one-candidate path, where the call below fills it.
    std::vector<Type_id> resolved;
    // Parallel to `resolved`, and empty unless the arguments deduced it: which argument settled
    // each type parameter, so a broken bound underlines the argument that chose the type rather
    // than the whole call. Invalid where the expectation is what settled it.
    std::vector<Node_id> bound_by;
    Node_id              callable {};

    if( candidates.size() == 1 )
    {
        callable = candidates.front();

        // A generic call on something that is not generic. After 1b the parser reads `a < b > ( c )`
        // this way, so this is the message that shape produces - it has to name the real problem.
        if( type_args.is_valid() && !is_generic( ast_, callable ) )
        {
            reporter_.error_at( ast_.span( callee ), fmt::format( "`{}` is not a generic", name ) );

            type_the_arguments_anyway();
            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }
    }
    else
    {
        // Two calls because selection is lazy: what the call wrote narrows the set first, and only
        // a set still holding two is worth typing an argument for - which is what cannot be undone.
        const std::vector<Node_id> viable = overloads_.viable_overloads( id, name, candidates, implicit_params, constructed );

        if( viable.empty() )
        {
            type_the_arguments_anyway();
            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }

        if( viable.size() == 1 )
        {
            callable = viable.front();
        }
        else
        {
            for( const Node_id argument : ast_.children( args ) )
            {
                shapes.push_back( argument_shape( argument ) );
            }

            callable = overloads_.select_overload( id, name, viable, implicit_params, constructed, shapes, resolved );

            if( !callable.is_valid() )
            {
                type_the_arguments_anyway();
                return types_.record( id, table_.builtin( Type_kind::Error ) );
            }
        }
    }

    // The chosen callable's return type, not the name's. `decl` is only the first candidate, so
    // reading the result off it hands every call in a set the first one's type.
    if( !is_aggregate( ast_.kind( decl ) ) )
    {
        result = types_.type_of( callable );
    }

    // A generic whose type arguments were not written, which is the ordinary way to call one.
    // *After* the callable is settled rather than inside either branch above: selection reaches an
    // answer by arity alone as readily as by type, and a candidate chosen that way still has its
    // parameters to deduce. One place deduces, whichever way the callable was arrived at.
    // Deduction reports its own failure - which parameter, and why - so there is nothing to add.
    if( !type_args.is_valid() && is_generic( ast_, callable ) )
    {
        if( shapes.empty() )
        {
            for( const Node_id argument : ast_.children( args ) )
            {
                shapes.push_back( argument_shape( argument ) );
            }
        }

        if( !overloads_.deduce_type_arguments(
                callable,
                implicit_params,
                shapes,
                ast_.children( args ),
                result,
                expectation,
                name,
                ast_.span( callee ),
                resolved,
                bound_by
            ) )
        {
            type_the_arguments_anyway();
            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }
    }

    // Recorded for every call, not only an overloaded one: lowering reads the choice rather than
    // making it, and a rule applied in one pass and repeated in another is a rule that drifts.
    callees_.record( id, callable );

    if( is_extern( ast_, callable ) )
    {
        require_unsafe(
            callee,
            fmt::format( "calling `{}` needs an `unsafe` block", name ),
            "it is defined in C, so the compiler cannot check what it does with its arguments"
        );
    }

    Bindings bindings;

    // Written or deduced, it is the same call from here on. Two conditions rather than one because
    // a written list is resolved *inside* this block, so it is still empty when the block is
    // entered - and a deduced one was filled before the callable was even settled.
    if( type_args.is_valid() || !resolved.empty() )
    {
        if( type_args.is_valid() )
        {
            if( !annotations_.resolve_type_arguments( callable, type_args, name, resolved ) )
            {
                type_the_arguments_anyway();
                return types_.record( id, table_.builtin( Type_kind::Error ) );
            }
        }
        else
        {
            // Deduced, so there is no written annotation to count or to underline - but a bound is
            // a promise about the type argument however it was arrived at.
            const std::vector<Node_id> parameters = type_parameters( ast_, ast_.type_param_list( callable ) );

            for( std::size_t i = 0; i < parameters.size() && i < resolved.size(); ++i )
            {
                bounds_.check_bounds(
                    parameters[i],
                    i < bound_by.size() && bound_by[i].is_valid() ? ast_.span( bound_by[i] ) : ast_.span( callee ),
                    resolved[i],
                    name
                );
            }
        }

        bindings = overloads_.type_bindings( callable, resolved );

        // The edge, before `resolved` is consumed. Only from inside a generic: a call in `main` is
        // already an instance rather than a step towards one.
        generic_recursion_.record_call( current_function_, callable, resolved, ast_.span( id ) );

        const std::size_t instance = generic_recursion_.record_instantiation( callable, std::move( resolved ) );

        overloads_.record_instantiation( id, instance );
    }

    // `Overloads` answers what each argument still needs and the walk is done here, because the
    // walk is the one thing a class below may not re-enter.
    for( const Argument_work& work : overloads_.check_call_arguments( id, callable, name, implicit_params, bindings, shapes ) )
    {
        if( work.with_expectation )
        {
            check( work.argument, work.expected );
        }
        else
        {
            infer( work.argument );
        }
    }

    overloads_.check_argument_markers( id, callable, name, implicit_params, bindings );

    // Substituted here rather than where `result` is set: the aggregate path overwrites it with
    // the type being constructed, so doing it earlier would substitute the wrong thing or twice.
    return types_.record( id, table_.substitute( result, bindings ) );
}

Type_id Expressions::infer_method_call( Node_id id )
{
    const Node_id callee = ast_.child( id, 0 );
    const Node_id object = ast_.child( callee, 0 );

    const Type_id result      = infer( object );
    const Type_id object_type = table_.is_pointer( result ) ? table_.get( result ).element : result;

    if( table_.is_error( object_type ) )
    {
        for( const Node_id argument : ast_.children( ast_.child( id, 1 ) ) )
        {
            infer( argument );
        }

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    if( !table_.is_struct( object_type ) )
    {
        reporter_.error_at( ast_.span( object ), fmt::format( "`{}` has no methods", table_.name( object_type ) ) );
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    // The aggregate, not the call: find_method searches a declaration's members, and hands back
    // the first of however many share the name.
    const Node_id first = aggregates_.find_method( table_.get( object_type ).declaration, Symbol_id { ast_.aux( callee ) } );

    if( !first.is_valid() )
    {
        // Check if a field of the same name exists, which is a common mistake when a method is expected. The
        // field's type is not a method, so it cannot be called.
        const Node_id field = aggregates_.find_field( object_type, Symbol_id { ast_.aux( callee ) } );
        if( field.is_valid() )
        {
            reporter_.error_at(
                ast_.span( callee ),
                fmt::format(
                    "`{}` is a field of `{}`, not a method; it cannot be called",
                    interner_.text( Symbol_id { ast_.aux( callee ) } ),
                    table_.name( object_type )
                )
            );

            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }

        reporter_.error_at(
            ast_.span( callee ),
            fmt::format(
                "`{}` has no method `{}`", table_.name( object_type ), interner_.text( Symbol_id { ast_.aux( callee ) } )
            )
        );

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    std::vector<Argument_shape> shapes;

    const std::vector<Node_id> viable = overloads_.viable_methods( id, first, object_type );
    Node_id                    method {};

    if( viable.size() == 1 )
    {
        method = viable.front();
    }
    else if( !viable.empty() )
    {
        for( const Node_id argument : ast_.children( ast_.child( id, 1 ) ) )
        {
            shapes.push_back( argument_shape( argument ) );
        }

        method = overloads_.select_method( id, first, object_type, viable, shapes );
    }

    if( !method.is_valid() )
    {
        for( std::size_t i = 0; i < ast_.children( ast_.child( id, 1 ) ).size(); ++i )
        {
            if( i >= shapes.size() || !shapes[i].recorded )
            {
                infer( ast_.children( ast_.child( id, 1 ) )[i] );
            }
        }

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    callees_.record( id, method );

    // A method without a trailing `const` takes its receiver as `ref T` and may write the object,
    // so calling one needs a receiver that may be written. That is exactly check_writable's
    // question - asked of the object rather than of an assignment - so a `const` local, a
    // read-only borrow and a pattern binding are all refused here by the rules that already refuse
    // them elsewhere, each with its own message.
    if( !is_const_method( ast_, method ) && !places_.check_writable( object, current_function_ ) )
    {
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    return check_method_arguments( id, method, object_type, shapes );
}

// Shared by both call shapes - `p.area()` and a bare `area()` inside a method - because what is
// checked is the same: the parameters from 1, the receiver having been supplied either way.
Type_id
Expressions::check_method_arguments( Node_id id, Node_id method, Type_id receiver, std::span<const Argument_shape> shapes )
{
    const std::span<const Node_id> params    = ast_.children( ast_.child( method, 1 ) ).subspan( 1 );
    const std::span<const Node_id> arguments = ast_.children( ast_.child( id, 1 ) );

    if( params.size() != arguments.size() )
    {
        reporter_.error_at(
            ast_.span( ast_.child( id, 1 ) ),
            fmt::format(
                "`{}` takes {} argument{}, but {} {} given",
                interner_.text( Symbol_id { ast_.aux( method ) } ),
                params.size(),
                params.size() == 1 ? "" : "s",
                arguments.size(),
                arguments.size() == 1 ? "was" : "were"
            )
        );
    }

    // Check the pairs that do line up even when the count is wrong: one missing argument should
    // not hide a type error in the others.
    const std::size_t shared = std::min( params.size(), arguments.size() );

    // The receiver's arguments are the method's: `p.first()` on a `Pair<i32>` is that instance's
    // `first`, and the declaration's `T` means nothing until they are applied. Reading either the
    // parameters or the result without them is how a method returns `T` to a caller expecting i32.
    const Bindings bindings = aggregates_.bindings_of( receiver );

    for( std::size_t i = 0; i < shared; ++i )
    {
        const Node_id param = params[i];
        const Node_id arg   = arguments[i];

        // Already typed by selection, which had to know what it was choosing between.
        if( i >= shapes.size() || !shapes[i].recorded )
        {
            check( arg, table_.substitute( types_.type_of( param ), bindings ) );
        }
    }

    record_method_instantiation( id, method, receiver );

    return types_.record( id, table_.substitute( types_.type_of( method ), bindings ) );
}

// A method call writes no type arguments - the receiver carries them - so the explicit path that
// records an instantiation never runs for one. Without this a method of a generic is checked and
// then emitted by nobody.
void Expressions::record_method_instantiation( Node_id id, Node_id method, Type_id receiver )
{
    if( !receiver.is_valid() || !table_.is_struct( receiver ) )
    {
        return;
    }

    const std::span<const Type_id> arguments = table_.get( receiver ).arguments;

    if( arguments.empty() )
    {
        return;
    }

    const std::vector<Type_id> resolved( arguments.begin(), arguments.end() );

    // The same edge a written call records: from inside a generic this names a template, and the
    // worklist is what turns it into an instance once the enclosing one is known.
    generic_recursion_.record_call( current_function_, method, resolved, ast_.span( id ) );

    overloads_.record_instantiation( id, generic_recursion_.record_instantiation( method, resolved ) );
}

// D29/D32. `add( by )` inside a method: the receiver is the one this function was given, so there
// is no object expression to check - the constness question is asked of the *enclosing* method's
// receiver instead, which is what stops a `const` method calling a mutating sibling.
Type_id Expressions::infer_implicit_method_call( Node_id id, Node_id method )
{
    const Node_id receiver = places_.receiver_of( current_function_ );

    if( !receiver.is_valid() )
    {
        reporter_.error_at(
            ast_.span( ast_.child( id, 0 ) ),
            fmt::format(
                "`{}` is a method, and there is no object here to call it on",
                interner_.text( Symbol_id { ast_.aux( method ) } )
            ),
            "a method can only be called by bare name from inside another method of the same type"
        );

        for( const Node_id argument : ast_.children( ast_.child( id, 1 ) ) )
        {
            infer( argument );
        }

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    std::vector<Argument_shape> shapes;

    const std::vector<Node_id> viable = overloads_.viable_methods( id, method, types_.type_of( receiver ) );
    const Node_id              first  = method;

    method = Node_id {};

    if( viable.size() == 1 )
    {
        method = viable.front();
    }
    else if( !viable.empty() )
    {
        for( const Node_id argument : ast_.children( ast_.child( id, 1 ) ) )
        {
            shapes.push_back( argument_shape( argument ) );
        }

        method = overloads_.select_method( id, first, types_.type_of( receiver ), viable, shapes );
    }

    if( !method.is_valid() )
    {
        for( std::size_t i = 0; i < ast_.children( ast_.child( id, 1 ) ).size(); ++i )
        {
            if( i >= shapes.size() || !shapes[i].recorded )
            {
                infer( ast_.children( ast_.child( id, 1 ) )[i] );
            }
        }

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    if( !is_const_method( ast_, method ) && is_const_binding( ast_, receiver ) )
    {
        reporter_.error_at(
            ast_.span( ast_.child( id, 0 ) ),
            fmt::format(
                "a `const` method cannot call `{}`, which may modify the object",
                interner_.text( Symbol_id { ast_.aux( method ) } )
            ),
            fmt::format(
                "remove `const` from `{}`, or add it to `{}`",
                interner_.text( Symbol_id { ast_.aux( current_function_ ) } ),
                interner_.text( Symbol_id { ast_.aux( method ) } )
            )
        );

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    callees_.record( id, method );

    return check_method_arguments( id, method, types_.type_of( receiver ), shapes );
}

Type_id Expressions::infer_binary( Node_id id )
{
    const Token_kind op    = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    left  = ast_.child( id, 0 );
    const Node_id    right = ast_.child( id, 1 );
    const Type_id    error = table_.builtin( Type_kind::Error );

    // A literal has a value, not a type, so the other operand is what gives it one. Inferring both
    // would settle it on its default first, and `u32 bits; bits != 0` would then compare a u32
    // with an i32 - which §6.4 rejects, and which no suffix exists to write around (D13). That
    // would leave unsigned code very nearly unwritable.
    Type_id lhs_type;
    Type_id rhs_type;

    if( literals_.is_literal_expression( left ) != literals_.is_literal_expression( right ) )
    {
        const bool literal_on_the_left = literals_.is_literal_expression( left );

        const Node_id known_side   = literal_on_the_left ? right : left;
        const Node_id literal_side = literal_on_the_left ? left : right;

        const Type_id known = infer( known_side );

        // D41: a literal the other operand cannot hold is not a type error, because the comparison
        // still has an answer - so it takes a type of its own to be compared in rather than
        // adopting one it never had to fit. Arithmetic keeps the adoption, because there the two
        // really do have to meet somewhere.
        const Type_id standalone = is_comparison( op ) ? literals_.standalone_literal_type( literal_side, known ) : Type_id {};

        // A failed operand gives nothing to adopt, and check() absorbs an error expectation - so
        // the literal is carried along rather than asked to invent a type it has no basis for.
        const Type_id adopted = check( literal_side, standalone.is_valid() ? standalone : known );

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
        return types_.record( id, error );
    }

    const Binary_result result = operators_.result_of_binary( op, lhs_type, rhs_type, current_function_, ast_.span( id ) );

    if( !result.type.is_valid() )
    {
        return types_.record( id, error );
    }

    if( result.compare_constant )
    {
        literals_.warn_if_constant_comparison( id, op, lhs_type, rhs_type );
    }

    // record_constant for every answer, including a generic body's: a division by zero and an
    // out-of-range shift are answerable without knowing `T`, and routing around the check is how a
    // generic body would be the one place they are not caught. Where the answer is `bool` it does
    // nothing, since nothing there can overflow.
    return constant_folder_.record_constant( id, result.type );
}

Type_id Expressions::infer_unary( Node_id id )
{
    const Token_kind op           = static_cast<Token_kind>( ast_.aux( id ) );
    const Type_id    operand_type = infer( ast_.child( id, 0 ) );
    const Type_id    error        = table_.builtin( Type_kind::Error );

    if( table_.is_error( operand_type ) )
    {
        return types_.record( id, error );
    }

    // Neither of these is in the rule table: that table assumes the result type is the operand's,
    // and both of these change it. Address-of is a question about places besides, which is why the
    // two stay here rather than following the other four into Operators.
    if( op == Token_kind::Amp )
    {
        // The operand must be somewhere a value lives. `&f()` names the address of a temporary
        // that is about to vanish, and `&1` names nothing at all. Asked after infer() so that
        // errors inside the operand are reported first.
        if( !places_.is_assignable( ast_.child( id, 0 ) ) )
        {
            reporter_.error_at( ast_.span( id ), "cannot take the address of this expression", "it does not name a variable" );

            return types_.record( id, error );
        }

        return types_.record( id, table_.pointer_to( operand_type ) );
    }

    if( op == Token_kind::Star )
    {
        if( !table_.is_pointer( operand_type ) )
        {
            reporter_.error_at( ast_.span( id ), fmt::format( "`{}` cannot be dereferenced", table_.name( operand_type ) ) );

            return types_.record( id, error );
        }

        return types_.record( id, table_.get( operand_type ).element );
    }

    const Type_id result = operators_.result_of_unary( op, operand_type, ast_.span( id ) );

    if( !result.is_valid() )
    {
        return types_.record( id, error );
    }

    return constant_folder_.record_constant( id, result );
}

// D30: a variant is reached only through its enum - `Colour::Red`, never a bare `Red`. The
// qualifier resolves through the ordinary name path, and the variant is looked up against the
// enum's declaration here, which is exactly what infer_field below does against a struct's.
// D7. `Shape::Circle( 1.0 )`: the variant names the shape of the payload, so the arguments are
// checked against its fields exactly as a call's are checked against a parameter list. The result
// is the *enum*, never the payload - a constructed variant is a Shape, and which one it is is a
// question only `switch` may ask.
Type_id Expressions::infer_variant_construction( Node_id id )
{
    const Node_id                  path      = ast_.child( id, 0 );
    const std::span<const Node_id> arguments = ast_.children( ast_.child( id, 1 ) );

    // Tells infer_path the arguments account for the payload, so the bare path is not incomplete.
    naming_variant_      = true;
    const Type_id result = infer( path );
    naming_variant_      = false;

    if( table_.is_error( result ) )
    {
        for( const Node_id argument : arguments )
        {
            infer( argument );
        }

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    // infer_path recorded the ordinal, which is how the variant is found again without a second
    // name lookup.
    const Node_id                       decl     = table_.get( result ).declaration;
    const std::optional<Constant_value> ordinal  = constant_folder_.value_of( path );
    const std::span<const Node_id>      variants = ast_.variants( decl );

    if( !ordinal || ordinal->magnitude >= variants.size() )
    {
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    const Node_id                  variant = variants[static_cast<std::size_t>( ordinal->magnitude )];
    const std::span<const Node_id> payload = ast_.children( variant );

    if( payload.empty() )
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format( "`{}` carries no payload", interner_.text( Symbol_id { ast_.aux( variant ) } ) ),
            "write it without arguments"
        );
    }
    else if( payload.size() != arguments.size() )
    {
        reporter_.error_at(
            ast_.span( ast_.child( id, 1 ) ),
            fmt::format(
                "`{}` carries {} value{}, but {} {} given",
                interner_.text( Symbol_id { ast_.aux( variant ) } ),
                payload.size(),
                payload.size() == 1 ? "" : "s",
                arguments.size(),
                arguments.size() == 1 ? "was" : "were"
            )
        );
    }

    // The pairs that do line up are checked even when the count is wrong: one missing value should
    // not hide a type error in the others.
    const std::size_t shared = std::min( payload.size(), arguments.size() );

    for( std::size_t i = 0; i < shared; ++i )
    {
        check( arguments[i], aggregates_.field_type( result, payload[i] ) );
    }

    for( std::size_t i = shared; i < arguments.size(); ++i )
    {
        infer( arguments[i] );
    }

    return types_.record( id, result );
}

Type_id Expressions::infer_conditional( Node_id id )
{
    check_condition( ast_.child( id, 0 ) );
    const Type_id then_type = infer( ast_.child( id, 1 ) );
    const Type_id else_type = infer( ast_.child( id, 2 ) );

    if( table_.is_error( then_type ) || table_.is_error( else_type ) )
    {
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    const Type_id result = operators_.result_of_conditional( then_type, else_type, ast_.span( id ) );

    return types_.record( id, result.is_valid() ? result : table_.builtin( Type_kind::Error ) );
}

Type_id Expressions::infer_alloc( Node_id id )
{
    const Type_id element = annotations_.type_of( ast_.child( id, 0 ) );

    if( table_.is_error( element ) )
    {
        return types_.record( id, element );
    }

    if( element == table_.builtin( Type_kind::Void ) )
    {
        reporter_.error_at( ast_.span( id ), "`alloc` needs a type to allocate", "`void` has no size" );
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    require_unsafe(
        id,
        "`alloc` needs an `unsafe` block",
        "it hands back memory that does not hold a value yet, so the pointer's type is a claim rather than a fact"
    );

    return types_.record( id, table_.pointer_to( element ) );
}

Type_id Expressions::infer_free( Node_id id )
{
    const Type_id operand = infer( ast_.child( id, 0 ) );

    if( table_.is_error( operand ) )
    {
        return types_.record( id, operand );
    }

    if( !table_.is_pointer( operand ) )
    {
        reporter_.error_at( ast_.span( id ), fmt::format( "`free` needs a pointer, but got `{}`", table_.name( operand ) ) );
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    require_unsafe( id, "`free` needs an `unsafe` block", "the compiler cannot tell whether anything still points at it" );

    return types_.record( id, table_.builtin( Type_kind::Void ) );
}

void Expressions::require_unsafe( Node_id id, std::string what, std::string why )
{
    if( unsafe_depth_ == 0 )
    {
        reporter_.error_at( ast_.span( id ), what, why );
    }
    else
    {
        unsafe_used_ = true;
    }
}

Type_id Expressions::infer_path( Node_id id )
{
    const Node_id   qualifier = ast_.child( id, 0 );
    const Symbol_id name { ast_.aux( id ) };

    const Node_id decl = ast_.kind( qualifier ) == Node_kind::Name_expr ? resolution_.declaration_of( qualifier ) : Node_id {};

    // An unresolved qualifier was already reported by the resolver; anything else is not a name at
    // all, and `f()::x` deserves its own complaint rather than a second one about the name.
    if( !decl.is_valid() )
    {
        if( ast_.kind( qualifier ) != Node_kind::Name_expr )
        {
            reporter_.error_at( ast_.span( qualifier ), "`::` needs the name of an `enum` on its left" );
        }

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    if( ast_.kind( decl ) != Node_kind::Enum_decl )
    {
        reporter_.error_at(
            ast_.span( qualifier ),
            fmt::format( "`{}` is not an `enum`", interner_.text( Symbol_id { ast_.aux( qualifier ) } ) ),
            "`::` reaches a variant, and only an `enum` has them"
        );

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    const std::span<const Node_id> variants = ast_.variants( decl );

    for( std::size_t i = 0; i < variants.size(); ++i )
    {
        if( Symbol_id { ast_.aux( variants[i] ) } != name )
        {
            continue;
        }

        // The ordinal, recorded where the constant folder already puts values - a variant *is* a
        // constant expression. Lowering needs the number, and aux holds the name.
        constant_folder_.record_value( id, Constant_value { .kind = Constant_value::Kind::Integer, .magnitude = i } );

        // D7: a variant with a payload is not a *value* until it has one. `Shape s = Shape::Circle;`
        // is as incomplete as a struct literal with no fields, and saying so here is better than
        // letting it type as a Shape and produce garbage in the payload.
        //
        // Naming the variant is a different thing from producing one, and the three places that do
        // it set the flag first: a construction supplies the payload, a pattern destructures it,
        // and a bare `case` label ignores it.
        if( !ast_.children( variants[i] ).empty() && !naming_variant_ )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format( "`{}` carries a payload", interner_.text( name ) ),
                fmt::format( "write `{}( ... )` with a value for each field", reporter_.text( ast_.span( id ) ) )
            );

            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }

        // Which instance this path names. The declaration test is what keeps a wrong expectation
        // out: `Opt<i32> x = Plain::One;` falls through to the open form and is refused below by
        // the ordinary mismatch, rather than being quietly retyped to whatever was wanted.
        if( expected_.is_valid() && table_.get( expected_ ).declaration == decl )
        {
            return types_.record( id, expected_ );
        }

        // Nothing named an instance, and a generic declaration's own type is the open form -
        // `Opt<T>`, which is a template rather than anything a value can hold. Left to the mismatch
        // below it would escape through `auto`, which has no expectation to disagree with.
        if( is_generic( ast_, decl ) )
        {
            return types_.record( id, no_instance_named( id, decl ) );
        }

        return types_.record( id, types_.type_of( decl ) );
    }

    reporter_.error_at(
        ast_.span( id ),
        fmt::format( "`{}` has no variant `{}`", interner_.text( Symbol_id { ast_.aux( decl ) } ), interner_.text( name ) )
    );

    return types_.record( id, table_.builtin( Type_kind::Error ) );
}

Type_id Expressions::infer_field( Node_id id )
{
    const Node_id base      = ast_.child( id, 0 );
    const Type_id base_type = infer( base );

    if( table_.is_error( base_type ) )
    {
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    // D22: `.` reaches through a pointer, so a pointer-to-struct base is the struct.
    const Type_id object_type = table_.is_pointer( base_type ) ? table_.get( base_type ).element : base_type;

    if( !table_.is_struct( object_type ) )
    {
        reporter_.error_at( ast_.span( base ), fmt::format( "`{}` has no fields", table_.name( object_type ) ) );
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    const Node_id decl = aggregates_.find_field( object_type, Symbol_id { ast_.aux( id ) } );
    if( !decl.is_valid() )
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format( "`{}` has no field `{}`", table_.name( object_type ), interner_.text( Symbol_id { ast_.aux( id ) } ) )
        );

        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    return types_.record( id, aggregates_.field_type( object_type, decl ) );
}

// Nothing named an instance of a generic declaration, so there is no type here a value can hold.
// Two causes with two different fixes, which is why one message cannot serve both: either nothing
// was expected at all, or what was expected belongs to another declaration entirely.
Type_id Expressions::no_instance_named( Node_id id, Node_id declaration )
{
    const std::string_view name = interner_.text( Symbol_id { ast_.aux( declaration ) } );

    if( expected_.is_valid() )
    {
        // The ordinary mismatch, stated here rather than left to check(): falling through would
        // name the open form - `Box<T>`, a type the author never wrote - on the `got` side.
        reporter_.error_at( ast_.span( id ), fmt::format( "expected `{}`, but got `{}`", table_.name( expected_ ), name ) );
    }
    else
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format( "nothing here says which `{}` this is", name ),
            fmt::format( "write the type where the value lands, as in `{}<i32> x = ...`", name )
        );
    }

    return table_.builtin( Type_kind::Error );
}

Type_id Expressions::infer_struct_literal( Node_id id )
{
    const Type_id error = table_.builtin( Type_kind::Error );

    // The resolver already bound the type name, so there is no scope lookup here - and if it
    // failed, it reported. Saying so again is the cascade the error type exists to prevent.
    const Node_id decl = resolution_.declaration_of( id );

    if( !decl.is_valid() || !is_aggregate( ast_.kind( decl ) ) )
    {
        for( const Node_id init : ast_.children( id ) )
        {
            infer( ast_.child( init, 0 ) ); // type the values anyway
        }

        return types_.record( id, error );
    }

    // D29: a class with a constructor is built by calling it. That is what stops the two
    // initialisation syntaxes competing for one type, and it is the rule the struct-literal-on-a-
    // class exception was standing in for until constructors existed.
    if( aggregates_.find_member( decl, Node_kind::Constructor_decl ).is_valid() )
    {
        const std::string_view name = interner_.text( Symbol_id { ast_.aux( decl ) } );

        reporter_.error_at(
            ast_.span( id ),
            fmt::format( "`{}` has a constructor, so it cannot be built from a literal", name ),
            fmt::format( "write `{}( ... )`", name )
        );

        for( const Node_id init : ast_.children( id ) )
        {
            infer( ast_.child( init, 0 ) );
        }

        // The declared type rather than the error type: the mistake is how it was built, not what
        // it is, so a cascade at the assignment would say nothing new.
        return types_.record( id, types_.type_of( decl ) );
    }

    const std::span<const Node_id> initialisers = ast_.children( id );

    // Fields, not members: a destructor is a child of the declaration too, and counting it would
    // demand an extra initialiser and misalign every positional one after it.
    std::vector<Node_id> fields;

    for( const Node_id member : ast_.members( decl ) )
    {
        if( ast_.kind( member ) == Node_kind::Field_decl )
        {
            fields.push_back( member );
        }
    }

    const std::string_view struct_name = interner_.text( Symbol_id { ast_.aux( decl ) } );

    // Which instance this literal builds. The declaration test is what keeps a wrong expectation
    // out: `Box<i32> x = Plain { 1 };` falls through and is refused by the ordinary mismatch below,
    // rather than being quietly retyped to whatever was wanted.
    const bool names_an_instance = expected_.is_valid() && table_.get( expected_ ).declaration == decl;

    if( !names_an_instance && is_generic( ast_, decl ) )
    {
        // Reported before the values are typed, because typing one runs check(), which is what
        // sets expected_ - and the message depends on it.
        const Type_id poison = no_instance_named( id, decl );

        for( const Node_id init : initialisers )
        {
            infer( ast_.child( init, 0 ) ); // type the values anyway
        }

        return types_.record( id, poison );
    }

    const Type_id result = names_an_instance ? expected_ : types_.type_of( decl );

    // One convention per literal, as C++20 requires. Two in the same literal is a reader's
    // problem rather than a parser's.
    std::size_t named = 0;

    for( const Node_id init : initialisers )
    {
        named += Symbol_id { ast_.aux( init ) }.is_valid() ? 1 : 0;
    }

    if( named != 0 && named != initialisers.size() )
    {
        reporter_.error_at(
            ast_.span( id ),
            "an initialiser list is either all positional or all named",
            "give every field a name, or none of them"
        );
    }

    if( named == 0 )
    {
        if( initialisers.size() != fields.size() )
        {
            reporter_.error_at(
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
            check( ast_.child( initialisers[i], 0 ), aggregates_.field_type( result, fields[i] ) );
        }

        for( std::size_t i = shared; i < initialisers.size(); ++i )
        {
            infer( ast_.child( initialisers[i], 0 ) );
        }

        return types_.record( id, result );
    }

    std::unordered_map<u32, Node_id> seen;

    for( const Node_id init : initialisers )
    {
        const Symbol_id name { ast_.aux( init ) };
        const Node_id   value = ast_.child( init, 0 );

        if( !name.is_valid() )
        {
            absorb( value ); // the mixing error above already covered this one
            continue;
        }

        const Node_id field = aggregates_.find_field( result, name );

        if( !field.is_valid() )
        {
            reporter_.error_at(
                ast_.span( init ), fmt::format( "`{}` has no field `{}`", struct_name, interner_.text( name ) )
            );
            absorb( value );
            continue;
        }

        if( !seen.try_emplace( name.v, init ).second )
        {
            reporter_.error_at( ast_.span( init ), fmt::format( "field `{}` is given twice", interner_.text( name ) ) );
        }

        check( value, aggregates_.field_type( result, field ) );
    }

    // Every field that is missing, not just the first: a struct that gained three fields should
    // say so once rather than over three compiles.
    for( const Node_id field : fields )
    {
        const Symbol_id name { ast_.aux( field ) };

        if( name.is_valid() && seen.find( name.v ) == seen.end() )
        {
            reporter_.error_at( ast_.span( id ), fmt::format( "field `{}` is not initialised", interner_.text( name ) ) );
        }
    }

    return types_.record( id, result );
}

Type_id Expressions::infer_cast( Node_id id )
{
    const bool    is_cast = static_cast<Keyword>( ast_.aux( id ) ) == Keyword::Cast;
    const Type_id target  = annotations_.type_of( ast_.child( id, 0 ) );
    const Node_id operand = ast_.child( id, 1 );
    const Type_id error   = table_.builtin( Type_kind::Error );

    // A literal has a value and no type, so `cast` is the context that gives it one:
    // `cast<u8>( 300 )` is the ordinary out-of-range error rather than a conversion, and
    // `cast<f32>( 1 )` is simply an f32 literal. `wrap` must not do this - accepting a value the
    // target cannot hold is the entire point of it.
    const bool from_literal = is_cast && !table_.is_error( target ) && literals_.is_literal_expression( operand );

    const Type_id value = from_literal ? check( operand, target ) : infer( operand );

    if( table_.is_error( target ) || table_.is_error( value ) )
    {
        return types_.record( id, error );
    }

    // check() has already ruled on the pair, and reported if it was wrong.
    if( from_literal )
    {
        return types_.record( id, target );
    }

    const Conversion_result result = operators_.convert( is_cast, value, target, ast_.span( id ) );

    if( !result.type.is_valid() )
    {
        return types_.record( id, error );
    }

    // D35's gate, which Operators may not hold: unsafe_depth_ is this walk's state.
    if( result.needs_unsafe )
    {
        require_unsafe(
            id,
            fmt::format(
                "converting between `{}` and `{}` needs an `unsafe` block", table_.name( value ), table_.name( target )
            ),
            "the compiler cannot check that the target type describes what is there"
        );

        if( !unsafe_used_ )
        {
            return types_.record( id, error );
        }
    }

    return types_.record( id, result.type );
}

Type_id Expressions::infer_marker( Node_id id )
{
    const Keyword marker  = static_cast<Keyword>( ast_.aux( id ) );
    const Node_id operand = ast_.child( id, 0 );

    // `out` assigns through the place, so it needs one, and one that may be written. Same test as
    // `ref` for the same reason: what may be assigned is exactly what may be lent for assignment.
    if( marker == Keyword::Out )
    {
        const Type_id value = infer( operand );

        if( !places_.is_assignable( operand ) )
        {
            reporter_.error_at( ast_.span( operand ), "`out` needs a variable to assign to" );
            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }

        if( !places_.check_writable( operand, current_function_ ) )
        {
            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }

        return types_.record( id, value );
    }

    // `ref` lends a place the callee may write through, so it needs one that is writable.
    // is_assignable is that question already: what may be assigned is exactly what may be lent, and
    // reusing it is what stops the two drifting apart.
    if( marker == Keyword::Ref )
    {
        const Type_id value = infer( operand );

        if( !places_.is_assignable( operand ) )
        {
            reporter_.error_at( ast_.span( operand ), "`ref` needs a variable to borrow" );
            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }

        // You cannot lend mutably what you hold read-only. check_writable has already named the
        // variable and what to write, so a second sentence would be a second diagnostic for one
        // mistake.
        if( !places_.check_writable( operand, current_function_ ) )
        {
            return types_.record( id, table_.builtin( Type_kind::Error ) );
        }

        return types_.record( id, value );
    }

    const Type_id value = infer( operand );

    // A field on its own is refused rather than lumped in with the rest, because the reason is
    // different and so is the fix: moving one would leave the object partly moved, and its own
    // scope exit would then drop a field that has already gone. Tracking that needs per-field drop
    // flags, so this is a restriction to lift when they exist rather than a rule to keep.
    if( ast_.kind( operand ) == Node_kind::Field_expr )
    {
        reporter_.error_at(
            ast_.span( operand ),
            "a field cannot be moved on its own",
            "moving it would leave the object partly moved - move the whole object instead"
        );
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    // Everything else must be a plain named local or parameter. Deliberately stricter than
    // is_assignable, which also accepts `*p`: that names something this function does not own, so
    // moving out of it leaves a hole nothing tracks. The rule and the analysis then agree by
    // construction - the dataflow tracks whole locals, and nothing else can be moved.
    const Node_id decl = ast_.kind( operand ) == Node_kind::Name_expr ? resolution_.declaration_of( operand ) : Node_id {};
    const bool    named =
        decl.is_valid() && ( ast_.kind( decl ) == Node_kind::Var_decl || ast_.kind( decl ) == Node_kind::Param_decl );

    if( places_.is_borrow_binding( decl ) )
    {
        reporter_.error_at(
            ast_.span( operand ),
            "cannot move out of a borrow",
            fmt::format(
                "take it as `move {} {}` to own it",
                table_.name( types_.type_of( decl ) ),
                interner_.text( Symbol_id { ast_.aux( decl ) } )
            )
        );
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    // Same reason as the field above: `*p` names something this function does not own, so moving
    // out of it leaves a hole nothing tracks.
    if( ast_.kind( operand ) == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( operand ) ) == Token_kind::Star )
    {
        reporter_.error_at( ast_.span( operand ), "a pointee cannot be moved", "move the variable it points into instead" );
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    // A named variable, or a temporary this scope owns. The second is what makes
    // `consume( move Buffer( 16 ) )` expressible: the temporary is built in the caller's frame and
    // handed over, so a transfer genuinely happens and D2 wants it marked - even though no variable
    // is left behind for a use-after-move to catch.
    if( !named && bounds_.satisfies( value, Bound::Copyable ) )
    {
        reporter_.error_at( ast_.span( operand ), "only a variable or an owned temporary can be moved" );
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    return types_.record( id, value );
}

Type_id Expressions::check( Node_id id, Type_id expected )
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
        return literals_.check_literal( id, expected );

    case Node_kind::Unary_expr:
        // `-2147483648` only fits an i32 as a negation, so the expectation goes through the minus.
        if( literals_.is_literal_expression( id ) )
        {
            return literals_.check_literal( id, expected );
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
        const Result_source source = operators_.result_source( static_cast<Token_kind>( ast_.aux( id ) ) );

        if( source == Result_source::Operands && literals_.is_literal_expression( ast_.child( id, 0 ) ) &&
            literals_.is_literal_expression( ast_.child( id, 1 ) ) )
        {
            const std::size_t before = reporter_.error_count();

            check( ast_.child( id, 0 ), expected );
            check( ast_.child( id, 1 ), expected );

            // An operand that did not fit has already been reported, and it is the cause. Folding
            // the whole thing would say the same thing again with a different number: `i8 d = 0 -
            // 200;` would complain about `200` and then about `-200`.
            if( reporter_.error_count() != before )
            {
                return types_.record( id, expected );
            }

            return constant_folder_.record_constant( id, expected );
        }

        // A shift's result type is its *left* operand's (§6.4), so that is the only operand an
        // expectation flows into - the count is a width, not a value in the same type. Without
        // this `u32 d = 1 << 4;` settles the literal on i32 and is then refused for being one.
        if( source == Result_source::Left_operand && literals_.is_literal_expression( ast_.child( id, 0 ) ) )
        {
            const std::size_t before = reporter_.error_count();

            check( ast_.child( id, 0 ), expected );
            infer( ast_.child( id, 1 ) );

            if( reporter_.error_count() != before )
            {
                return types_.record( id, expected );
            }

            return constant_folder_.record_constant( id, expected );
        }

        break;
    }
    case Node_kind::Conditional_expr:
    {
        check_condition( ast_.child( id, 0 ) );
        check( ast_.child( id, 1 ), expected );
        check( ast_.child( id, 2 ), expected );
        return types_.record( id, expected );
    }

    default:
        break;
    }

    expected_            = expected;
    const Type_id actual = infer( id );
    expected_            = Type_id {};

    if( table_.is_error( actual ) )
    {
        return expected;
    }

    if( !table_.holds( actual, expected ) )
    {
        reporter_.error_at(
            ast_.span( id ), fmt::format( "expected `{}`, but got `{}`", table_.name( expected ), table_.name( actual ) )
        );
        return expected;
    }

    return expected;
}

// A literal is skipped: there is nothing inside one to be wrong, and its only complaint is that
// nothing told it what type to be - which is exactly what the error that got us here already
// explains. Inferring it anyway is how one mistake produces two diagnostics.
void Expressions::absorb( Node_id id )
{
    if( id.is_valid() && !literals_.is_literal_expression( id ) )
    {
        infer( id );
    }
}

void Expressions::check_condition( Node_id id )
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
    reporter_.error_at(
        ast_.span( id ),
        fmt::format( "expected `bool`, but got `{}`", table_.name( actual ) ),
        "there is no conversion to `bool`: compare explicitly, as in `x != 0`"
    );
}

// The same flag construction sets, for the two pattern positions a statement reaches: the payload
// is accounted for there, so the path inside it names a variant rather than standing for a value.
Type_id Expressions::infer_in_pattern( Node_id path, Type_id scrutinee )
{
    naming_variant_         = true;
    expected_               = scrutinee;
    const Type_id path_type = infer( path );
    naming_variant_         = false;
    expected_               = Type_id {};

    return path_type;
}

// Saved and restored rather than plainly assigned: M6 brings lambdas, and a nested one would
// otherwise leave the enclosing function's rules reading the wrong declaration.
Node_id Expressions::enter_function( Node_id id )
{
    const Node_id enclosing = current_function_;
    current_function_       = id;

    return enclosing;
}

void Expressions::leave_function( Node_id enclosing )
{
    current_function_ = enclosing;
}

bool Expressions::enter_unsafe( Span at )
{
    if( unsafe_depth_ > 0 )
    {
        reporter_.error_at( at, "an `unsafe` block inside another one", "the enclosing block already permits it" );
    }

    // Saved and returned, not just cleared: the flag answers "did *this* block use its
    // permission", and an inner block must not spend an outer one's.
    const bool enclosing_used = unsafe_used_;

    unsafe_used_ = false;
    unsafe_depth_ += 1;

    return enclosing_used;
}

void Expressions::leave_unsafe( Span at, bool enclosing_used )
{
    unsafe_depth_ -= 1;

    if( !unsafe_used_ )
    {
        reporter_.error_at( at, "an `unsafe` block that does nothing unsafe", "remove `unsafe`" );
    }

    unsafe_used_ = enclosing_used;
}

} // namespace sema
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

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

// PLAN §12. A conditional has two rules and the split between them is the whole decision: an
// expectation reaches both arms, and with none the arms must match exactly. Widening happens after
// a type is settled, never to settle one - the same correction the overloading slice owes.
TEST_CASE( "type_checker_types_conditional_expressions", "[sema][types]" )
{
    SECTION( "with no expectation the arms decide, and they must agree" )
    {
        const Typed p( "i32 main() { i32 a = 1; i32 b = 2; auto c = true ? a : b; return c; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Conditional_expr, 0 ) ) == "i32" );
    }

    SECTION( "arms of different types with no expectation are refused" )
    {
        const Typed p( "i32 main() { i32 a = 1; i64 b = 2; auto c = true ? a : b; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
        REQUIRE( p.rendered().find( "i32" ) != std::string::npos );
        REQUIRE( p.rendered().find( "i64" ) != std::string::npos );
    }

    // The two arms widen to a common type under §6.4 and it still does not settle one. This is the
    // case that separates the decision taken from `Type_table::common`, which would accept it.
    SECTION( "widening does not settle a type between two arms" )
    {
        const Typed p( "i32 main() { u8 a = 1; i64 b = 2; auto c = true ? a : b; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "an expectation reaches both arms" )
    {
        const Typed p( "i32 main() { i64 x = true ? 1 : 2; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Conditional_expr, 0 ) ) == "i64" );
    }

    // Both arms are i32 and the expectation is wider. The arms keep their own type and the
    // conditional takes the expectation, which is what lowering has to insert a conversion for.
    SECTION( "an expectation wider than both arms is the conditional's type" )
    {
        const Typed p( "i32 main() { i32 a = 1; i32 b = 2; i64 x = true ? a : b; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Conditional_expr, 0 ) ) == "i64" );
    }

    SECTION( "an arm that cannot reach the expectation is refused" )
    {
        const Typed p( "i32 main() { i32 x = true ? 1 : false; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "the condition must be a bool" )
    {
        const Typed p( "i32 main() { i32 a = 1; i32 x = a ? 1 : 2; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    // Right-associative, so the else arm is a conditional of its own and the expectation has to
    // reach through it rather than stopping at the outer one.
    SECTION( "an expectation reaches through a nested conditional" )
    {
        const Typed p( "i32 main() { i64 x = true ? 1 : false ? 2 : 3; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Conditional_expr, 0 ) ) == "i64" );
        REQUIRE( p.type_name( p.nth( Node_kind::Conditional_expr, 1 ) ) == "i64" );
    }

    // One error, not two: an arm that failed is the cause, and reporting the disagreement as well
    // would name an error type the author never wrote. This is what the error guard in
    // infer_conditional buys, and the infer path is the only one that can reach it.
    SECTION( "a broken arm does not cascade" )
    {
        const Typed p( "i32 main() { i32 a = 1; auto c = true ? *a : 1; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot be dereferenced" ) != std::string::npos );
    }

    SECTION( "a conditional over owning values is typed" )
    {
        const Typed p( "class Owner { i32 t; Owner( i32 v ) { t = v; } ~Owner() { } };\n"
                       "i32 main() { Owner a = Owner( 1 ); Owner b = Owner( 2 ); "
                       "auto c = true ? move a : move b; return c.t; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Conditional_expr, 0 ) ) == "Owner" );
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

// D35's third and fourth gated operations. `alloc` is gated for a sharper reason than "it can
// fail": it hands back a pointer whose type claims there is a `T` there, and there is not.
TEST_CASE( "type_checker_gates_alloc_and_free_on_unsafe", "[sema][types][alloc]" )
{
    SECTION( "alloc outside an unsafe block" )
    {
        const Typed p( "i32 main() { i32* n = alloc<i32>(); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`alloc` needs an `unsafe` block" ) != std::string::npos );
    }

    SECTION( "free outside an unsafe block" )
    {
        const Typed p( "i32 main() { i32* n = nullptr; free( n ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`free` needs an `unsafe` block" ) != std::string::npos );
    }

    SECTION( "both inside one" )
    {
        const Typed p( "i32 main() { unsafe { i32* n = alloc<i32>(); free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "alloc alone justifies the block" )
    {
        const Typed p( "i32 main() { i32* n = nullptr; unsafe { n = alloc<i32>(); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "free alone justifies the block" )
    {
        const Typed p( "i32 main() { i32* n = nullptr; unsafe { free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the permission does not leak past the block" )
    {
        const Typed p( "i32 main()\n"
                       "{\n"
                       "    i32* a = nullptr;\n"
                       "    unsafe { a = alloc<i32>(); }\n"
                       "    i32* b = alloc<i32>();\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a safe wrapper is the point" )
    {
        const Typed p( "struct N { i32 v; };\n"
                       "N* make() { N* n = nullptr; unsafe { n = alloc<N>(); } return n; }\n"
                       "i32 main() { N* n = make(); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_types_alloc_and_free", "[sema][types][alloc]" )
{
    SECTION( "alloc yields a pointer to its argument" )
    {
        const Typed p( "struct N { i32 v; };\ni32 main() { unsafe { N* n = alloc<N>(); free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Alloc_expr, 0 ) ) == "N*" );
    }

    SECTION( "a builtin element" )
    {
        const Typed p( "i32 main() { unsafe { i32* n = alloc<i32>(); free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Alloc_expr, 0 ) ) == "i32*" );
    }

    SECTION( "a pointer element" )
    {
        const Typed p( "i32 main() { unsafe { i32** n = alloc<i32*>(); free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Alloc_expr, 0 ) ) == "i32**" );
    }

    SECTION( "an enum element" )
    {
        const Typed p( "enum E { A, B };\ni32 main() { unsafe { E* n = alloc<E>(); free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Alloc_expr, 0 ) ) == "E*" );
    }

    SECTION( "free yields void" )
    {
        const Typed p( "i32 main() { i32* n = nullptr; unsafe { free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Free_expr, 0 ) ) == "void" );
    }

    SECTION( "the pointer type is not interchangeable" )
    {
        const Typed p( "struct N { i32 v; };\ni32 main() { unsafe { i32* n = alloc<N>(); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "N*" ) != std::string::npos );
    }

    SECTION( "`void` has no size" )
    {
        const Typed p( "i32 main() { unsafe { i32* n = alloc<void>(); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "needs a type to allocate" ) != std::string::npos );
    }

    SECTION( "an unknown element type is reported, and not twice over as a gate failure" )
    {
        const Typed p( "i32 main() { unsafe { i32* n = alloc<Nope>(); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "needs an `unsafe` block" ) == std::string::npos );
    }

    SECTION( "free needs a pointer" )
    {
        const Typed p( "i32 main() { i32 x = 1; unsafe { free( x ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "`free` needs a pointer, but got `i32`" ) != std::string::npos );
    }

    SECTION( "free of a struct value, not a pointer to one" )
    {
        const Typed p( "struct N { i32 v; };\ni32 main() { N n = N { 1 }; unsafe { free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "needs a pointer" ) != std::string::npos );
    }

    SECTION( "`.` reaches through an allocated pointer, and chains" )
    {
        // The idiom `codegen/alloc_free.kl` is written in: no `(*n).` anywhere in a Keel program.
        const Typed p( "struct N { i32 v; N* next; };\n"
                       "i32 main()\n"
                       "{\n"
                       "    i32 r = 0;\n"
                       "    unsafe\n"
                       "    {\n"
                       "        N* a = alloc<N>();\n"
                       "        N* b = alloc<N>();\n"
                       "        b.v = 3;\n"
                       "        a.next = b;\n"
                       "        r = a.next.v;\n"
                       "        free( a );\n"
                       "        free( b );\n"
                       "    }\n"
                       "    return r;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a file-scope initialiser cannot allocate" )
    {
        // Not a rule of its own: §7 requires a constant expression there, and a call to the
        // allocator is not one. Pinned because the alternative - running it before main - is what
        // a reader might assume.
        const Typed p( "i32* g = alloc<i32>();\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// What `alloc` and `free` do NOT do. Each of these type-checks, and each is a hazard the `unsafe`
// at the call site is standing behind - pinned as accepted rather than as correct, so that the day
// `Owned<T>` makes one of them a compile error the change is visible here.
TEST_CASE( "type_checker_leaves_raw_memory_raw", "[sema][types][alloc]" )
{
    SECTION( "allocating a class runs no constructor" )
    {
        // `alloc<C>()` is memory, not an object: the fields are uninitialised and `C`'s invariant
        // has never held. This is exactly the gap D10's `Owned<T>` exists to close.
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } ~C() { } };\n"
                       "i32 main() { unsafe { C* p = alloc<C>(); free( p ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "freeing runs no destructor" )
    {
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } ~C() { } };\n"
                       "i32 main() { C* p = nullptr; unsafe { free( p ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "freeing a stack address is not caught" )
    {
        const Typed p( "i32 main() { i32 x = 1; unsafe { free( &x ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "freeing twice is not caught" )
    {
        const Typed p( "i32 main() { unsafe { i32* n = alloc<i32>(); free( n ); free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "leaking is not caught" )
    {
        const Typed p( "i32 main() { unsafe { i32* n = alloc<i32>(); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "using after free is not caught" )
    {
        const Typed p( "i32 main() { i32 v = 0; unsafe { i32* n = alloc<i32>(); free( n ); v = *n; } return v; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// §12: "`unsafe` permits operations, it does not disable checks." The same claim D35 makes, tested
// against the two operations this slice added.
TEST_CASE( "type_checker_still_checks_around_alloc", "[sema][types][alloc]" )
{
    SECTION( "the pointer is const-checked" )
    {
        const Typed p( "i32 main() { unsafe { i32* const n = alloc<i32>(); n = nullptr; free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a field written through it is type-checked" )
    {
        const Typed p( "struct N { i32 v; };\n"
                       "i32 main() { unsafe { N* n = alloc<N>(); (*n).v = true; free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and through `.`, which reaches the same field" )
    {
        // D22: `.` is the only member-access operator and reaches through a pointer, so the
        // explicit deref above is a second spelling rather than the required one. Both are checked.
        const Typed p( "struct N { i32 v; };\n"
                       "i32 main() { unsafe { N* n = alloc<N>(); n.v = true; free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "an unknown field is still unknown" )
    {
        const Typed p( "struct N { i32 v; };\n"
                       "i32 main() { unsafe { N* n = alloc<N>(); (*n).nope = 1; free( n ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// The gate, and its one customer. A pointer conversion is a real conversion the compiler cannot
// verify - the target type is an assertion about what is at that address - so it is the operation
// `unsafe` was built to permit. Nothing else is gated yet: raw-pointer dereference stays safe until
// the enumerated list is settled, and `extern` arrives with its own slice.
TEST_CASE( "type_checker_gates_a_pointer_conversion_on_unsafe", "[sema][types][unsafe]" )
{
    SECTION( "refused outside an unsafe block" )
    {
        const Typed p( "i32 main() { i32 x = 1; i32* q = &x; u8* r = cast<u8*>( q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "unsafe" ) != std::string::npos );
    }

    SECTION( "permitted inside one" )
    {
        const Typed p( "i32 main() { i32 x = 1; i32* q = &x; unsafe { u8* r = cast<u8*>( q ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the permission does not leak past the block" )
    {
        const Typed p( "i32 main()\n"
                       "{\n"
                       "    i32 x = 1;\n"
                       "    i32* q = &x;\n"
                       "    unsafe { u8* a = cast<u8*>( q ); }\n"
                       "    u8* b = cast<u8*>( q );\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "the permission does not leak into a later function" )
    {
        const Typed p( "void marked( i32* q ) { unsafe { u8* a = cast<u8*>( q ); } }\n"
                       "i32 main() { i32 x = 1; i32* q = &x; u8* b = cast<u8*>( q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "it reaches nested statements inside the block" )
    {
        const Typed p( "i32 main()\n"
                       "{\n"
                       "    i32 x = 1;\n"
                       "    i32* q = &x;\n"
                       "    unsafe\n"
                       "    {\n"
                       "        if( x == 1 )\n"
                       "        {\n"
                       "            u8* r = cast<u8*>( q );\n"
                       "        }\n"
                       "    }\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "it does not reach a function called from inside it" )
    {
        // The block permits operations written in it, not everything it transitively reaches -
        // otherwise the marker would say nothing about the code the reader is looking at.
        const Typed p( "u8* reinterpret( i32* q ) { return cast<u8*>( q ); }\n"
                       "i32 main()\n"
                       "{\n"
                       "    i32 x = 1;\n"
                       "    i32* q = &x;\n"
                       "    unsafe { u8* a = cast<u8*>( q ); u8* b = reinterpret( q ); }\n"
                       "    return 0;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "unsafe" ) != std::string::npos );
    }
}

// A shift's result type is its *left* operand's (§6.4), so that is where an expectation has to
// flow. Without this `u32 d = 1 << 4;` combines an i32 with a u32 and is refused - the fourth
// instance of inferring where checking belonged.
TEST_CASE( "type_checker_pushes_an_expectation_through_a_shift", "[sema][types][constants]" )
{
    SECTION( "the left operand adopts the target type" )
    {
        const Typed p( "i32 main() { u32 d = 1 << 4; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u32" );
    }

    SECTION( "for narrow types too" )
    {
        const Typed p( "i32 main() { u8 d = 1 << 4; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The count is independent of the shifted type, so it keeps inferring on its own.
    SECTION( "the count is not forced to the same type" )
    {
        const Typed p( "i32 main() { u8 n = 4; u8 d = 1 << n; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a right shift behaves the same" )
    {
        const Typed p( "i32 main() { u32 d = 256 >> 4; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The other half of D29's rule, and the entry that comes out of the debts list: with a constructor
// to build it, the literal form stops being a class's only way in.
TEST_CASE( "type_checker_rejects_a_literal_for_a_constructed_class", "[sema][aggregates]" )
{
    SECTION( "a literal is rejected when the class has a constructor" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer { 16 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and accepted when it has none" )
    {
        const Typed p( "class Handle { u64 value; };\ni32 main() { Handle h = Handle { 1 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a struct is unaffected" )
    {
        const Typed p( "struct Point { i32 x; i32 y; };\ni32 main() { Point q = Point { 1, 2 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D2's marker. It records that the author asked for a transfer; what that *means* is the dataflow's,
// and until that exists a moved-from value is still readable. The keyword is legal on any type -
// D31 makes it an assertion on a struct rather than a transfer - and in any expression position,
// because the same rule governs initialisation and assignment as well as arguments.
TEST_CASE( "type_checker_types_a_move", "[sema][move]" )
{
    SECTION( "the marker has its operand's type" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( move a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Marker_expr, 0 ) ) == "i32" );
    }

    SECTION( "in an initialiser" )
    {
        const Typed p( "i32 main() { i32 a = 1; i32 b = move a; return b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "in an assignment" )
    {
        const Typed p( "i32 main() { i32 a = 1; i32 b = 2; b = move a; return b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A field on its own is refused: moving it would leave the object partly moved, and the
    // object's own scope exit would then drop a field that has already gone. A restriction to lift
    // when per-field drop flags exist, not a rule to keep.
    SECTION( "a field cannot be moved on its own" )
    {
        const Typed p( "struct Point { i32 x; i32 y; };\n"
                       "i32 main() { Point q = Point { 1, 2 }; i32 a = move q.x; return a; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "a field cannot be moved on its own" ) != std::string::npos );
    }

    // Nor through a pointer: `*p` names something this function does not own, so moving out of it
    // leaves a hole nothing tracks.
    SECTION( "nor a pointee" )
    {
        const Typed p( "i32 main() { i32 a = 1; i32* p = &a; i32 b = move *p; return b; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "moving the whole object is how you do it" )
    {
        const Typed p( "struct Point { i32 x; i32 y; };\n"
                       "void f( Point p ) { }\n"
                       "i32 main() { Point q = Point { 1, 2 }; f( move q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Legal on a struct, where it is an assertion rather than a transfer: the bytes are copied and
    // the source is marked dead. One user-visible meaning across both kinds.
    SECTION( "a non-owning type can be moved" )
    {
        const Typed p( "struct Point { i32 x; };\n"
                       "void f( Point p ) { }\n"
                       "i32 main() { Point q = Point { 1 }; f( move q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an owning type can be moved" )
    {
        const Typed p( "class Buffer { u64 len; ~Buffer() { } };\n"
                       "void f( move Buffer b ) { }\n"
                       "i32 main() { Buffer b = Buffer { 1 }; f( move b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// `move` makes its source dead, so it needs a source to kill. is_assignable already answers "is
// this a place", which keeps move and assignment from drifting apart on what counts as one.
TEST_CASE( "type_checker_rejects_moving_a_non_place", "[sema][move]" )
{
    SECTION( "a literal has nowhere to be moved from" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { f( move 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "nor does a call result" )
    {
        const Typed p( "i32 g() { return 1; }\nvoid f( i32 x ) { }\ni32 main() { f( move g() ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "nor an arithmetic expression" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( move ( a + 1 ) ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // A marker annotates a whole argument rather than binding as an operator, so it takes the
    // expression to the boundary and this is `move ( a + 1 )`. Under unary binding it would be
    // `( move a ) + 1`, which passes the *sum* by copy - leaving a `move` at the call site saying
    // nothing about what the callee receives, which is the one thing D2 exists to guarantee.
    SECTION( "a marker takes the whole argument, not just the first operand" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( move a + 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "only a variable or an owned temporary can be moved" ) != std::string::npos );
    }

    // The old reading stays available, and has to be written down.
    SECTION( "parentheses restore it" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( ( move a ) + 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A const binding may not be laundered into a mutable one - and both routes are the same question,
// which is why extending check_writable made them both work with nothing written for either.
TEST_CASE( "type_checker_refuses_to_launder_a_const", "[sema][const]" )
{
    SECTION( "not through a ref binding" )
    {
        const Typed p( "i32 main() { const i32 k = 1; ref i32 r = k; r = 5; return k; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`k` is `const`" ) != std::string::npos );
    }

    SECTION( "and not through a ref argument" )
    {
        const Typed p( "void grow( ref i32 n ) { n = n + 1; }\ni32 main() { const i32 k = 1; grow( ref k ); return k; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`k` is `const`" ) != std::string::npos );
    }
}

TEST_CASE( "type_checker_checks_a_path", "[sema][enum]" )
{
    constexpr std::string_view colour = "enum Colour { Red, Green };\n";

    SECTION( "an unknown variant is refused" )
    {
        const Typed p( std::string( colour ) + "i32 main() { Colour c = Colour::Purple; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Colour` has no variant `Purple`" ) != std::string::npos );
    }

    SECTION( "a qualifier that is not an enum is refused" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { i32 n = P::x; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "is not an `enum`" ) != std::string::npos );
    }

    SECTION( "and `::` needs a name on its left" )
    {
        const Typed p( std::string( colour ) + "i32 main() { i32 n = Colour::Red::Green; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "needs the name of an `enum` on its left" ) != std::string::npos );
    }
}

// The heart of D30: `enum class` semantics without the `class`. Each of these is rejected by a rule
// that already existed - holds() has no branch reaching an integer from an enum, the arithmetic
// table has no row for one, and the variants enter no lexical scope - so the entry is enforced by
// what is absent as much as by what is written.
TEST_CASE( "type_checker_gives_an_enum_enum_class_semantics", "[sema][enum]" )
{
    constexpr std::string_view colour = "enum Colour { Red, Green };\n";

    SECTION( "variants do not leak into the enclosing scope" )
    {
        const Typed p( std::string( colour ) + "i32 main() { Colour c = Red; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Red` is not declared" ) != std::string::npos );
    }

    SECTION( "there is no conversion to an integer" )
    {
        const Typed p( std::string( colour ) + "i32 main() { i32 n = Colour::Red; return n; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "nor from one" )
    {
        const Typed p( std::string( colour ) + "i32 main() { Colour c = 0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "and no arithmetic" )
    {
        const Typed p( std::string( colour ) + "i32 main() { i32 n = Colour::Red + 1; return n; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// PLAN D7. A payload variant is constructed by calling it: `Shape::Circle( 1.0 )`. The variant
// names the shape of the payload, so the call is checked against the payload fields exactly as a
// call is checked against a parameter list.
TEST_CASE( "type_checker_constructs_a_payload_variant", "[sema][payload]" )
{
    constexpr std::string_view shape = "enum Shape { Circle( f64 radius ), Rect( f64 w, f64 h ), Dot };\n";

    SECTION( "with the right arguments" )
    {
        const Typed p( std::string( shape ) + "i32 main() { Shape s = Shape::Circle( 1.0 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and the result is the enum, not the payload" )
    {
        const Typed p( std::string( shape ) + "i32 main() { Shape s = Shape::Rect( 1.0, 2.0 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the wrong count is refused" )
    {
        const Typed p( std::string( shape ) + "i32 main() { Shape s = Shape::Rect( 1.0 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "the wrong type is refused" )
    {
        const Typed p( std::string( shape ) + "i32 main() { Shape s = Shape::Circle( true ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // The two halves of the same rule: a payload variant is not a value, and a payload-free one is
    // not a function.
    SECTION( "a payload variant needs its arguments" )
    {
        const Typed p( std::string( shape ) + "i32 main() { Shape s = Shape::Circle; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Circle` carries a payload" ) != std::string::npos );
    }

    SECTION( "and a payload-free one takes none" )
    {
        const Typed p( std::string( shape ) + "i32 main() { Shape s = Shape::Dot( 1.0 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Dot` carries no payload" ) != std::string::npos );
    }
}

// The flag has to be cleared on the way out, not only set on the way in: it is walk state, and a
// construction earlier in the same body must not leave a later bare name looking complete.
TEST_CASE( "expressions_clear_the_variant_flag_after_a_construction", "[sema][payload]" )
{
    const Typed p( "enum Shape { Square, Circle( f64 r ) };\n"
                   "i32 main() { Shape a = Shape::Circle( 1.0 ); Shape b = Shape::Circle; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "`Circle` carries a payload" ) != std::string::npos );
}

// A pattern's path is given the scrutinee as its expectation, and that is what says which instance
// it names. Without it the binding is left at the declaration's `T` and has no fields to reach.
TEST_CASE( "expressions_name_the_instance_a_pattern_destructures", "[sema][payload][generic]" )
{
    const Typed p( "struct Red { i32 v; };\n"
                   "enum Opt<T> where T : Copyable { None, Some( T v ) };\n"
                   "i32 unwrap( Opt<Red> s ) { switch( s ) { case Opt::Some( v ): return v.v; case Opt::None: return 0; } }\n"
                   "i32 main() { Red r = Red { 1 }; return unwrap( Opt::Some( r ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// The same expectation, at a path with no payload to destructure - the one branch that answers
// "which `Opt`" for a variant nothing else could place.
TEST_CASE( "expressions_take_a_variant_instance_from_the_expectation", "[sema][payload][generic]" )
{
    const Typed p( "struct Red { i32 v; };\n"
                   "enum Opt<T> where T : Copyable { None, Some( T v ) };\n"
                   "i32 main() { Opt<Red> none = Opt::None; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.type_name( p.nth( Node_kind::Path_expr, 0 ) ) == "Opt<Red>" );
}

// D35's flag is saved and restored around an inner block rather than cleared, so an outer block
// that has already spent its permission is not reported as unused once an inner one returns.
TEST_CASE( "expressions_leave_an_outer_unsafe_block_used", "[sema][unsafe]" )
{
    const Typed p( "i32 main() { i32 x = 1; i32* a = &x;\n"
                   "unsafe { u8* p = cast<u8*>( a ); unsafe { u8* q = cast<u8*>( a ); } }\n"
                   "return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "an `unsafe` block inside another one" ) != std::string::npos );
    REQUIRE( p.rendered().find( "does nothing unsafe" ) == std::string::npos );
}

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

// D39's other half: a type may be generic as readily as a function. `Box<i32>` and `Box<f64>` are
// two types from one declaration, and a field written `T` is whichever of them asked. Everything
// here is the type side; nothing emits one yet, so a use reports and these read past that.
TEST_CASE( "type_checker_types_a_generic_aggregate", "[sema][generic][aggregate]" )
{
    SECTION( "the declaration alone is accepted, and emits nothing" )
    {
        // No layout of its own, the way a generic function has no code: what gets ordered and
        // emitted is each instantiation.
        const Typed p( "struct Box<T> where T : Copyable { T v; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a type parameter is in scope inside the declaration and nowhere else" )
    {
        const Typed inside( "struct Box<T> where T : Copyable { T v; };\ni32 main() { return 0; }" );
        const Typed outside( "struct Box<T> where T : Copyable { T v; };\n"
                             "T stray( T a ) { return a; }\n"
                             "i32 main() { return 0; }" );

        INFO( inside.rendered() << outside.rendered() );
        REQUIRE( inside.clean() );
        REQUIRE( outside.rendered().find( "unknown type `T`" ) != std::string::npos );
    }

    SECTION( "two instantiations are two types" )
    {
        const Typed p( "struct Box<T> where T : Copyable { T v; };\n"
                       "i32 main() { Box<i32> a; Box<f64> b; a = b; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "expected `Box<i32>`, but got `Box<f64>`" ) != std::string::npos );
    }

    SECTION( "a field takes the type argument, not the parameter" )
    {
        // The declared type of the field is `T`, which is true of the template and of no value
        // anyone holds - so the diagnostic has to name `i32`, which the author wrote.
        const Typed fits( "struct Box<T> where T : Copyable { T v; };\n"
                          "i32 main() { Box<i32> b; b.v = 1; return b.v; }" );
        const Typed wrong( "struct Box<T> where T : Copyable { T v; };\n"
                           "i32 main() { Box<i32> b; b.v = true; return 0; }" );

        INFO( fits.rendered() << wrong.rendered() );
        REQUIRE( fits.clean() );
        REQUIRE( wrong.rendered().find( "expected `i32`, but got `bool`" ) != std::string::npos );
        REQUIRE( wrong.rendered().find( "`T`" ) == std::string::npos );
    }

    SECTION( "and a second instantiation takes a different one" )
    {
        const Typed p( "struct Box<T> where T : Copyable { T v; };\n"
                       "i32 main() { Box<f64> b; b.v = 1.5; return b.v; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "expected `i32`, but got `f64`" ) != std::string::npos );
    }

    SECTION( "written bare, it says what is missing" )
    {
        // The mirror of a generic call with no type arguments. Inference is a decision of its own
        // and is not taken here, so this names the explicit form rather than guessing.
        const Typed p( "struct Box<T> where T : Copyable { T v; };\ni32 main() { Box b; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Box` is generic, so its type arguments must be written" ) != std::string::npos );
    }

    SECTION( "type arguments on something that takes none" )
    {
        const Typed p( "struct P { i32 v; };\ni32 main() { P<i32> x; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`P` is not a generic" ) != std::string::npos );
    }

    SECTION( "the wrong number, through the same helper a call uses" )
    {
        const Typed many( "struct Box<T> where T : Copyable { T v; };\ni32 main() { Box<i32, f64> b; return 0; }" );
        const Typed few( "struct Pair<T, U> where T : Copyable, where U : Copyable { T a; U b; };\n"
                         "i32 main() { Pair<i32> p; return 0; }" );

        INFO( many.rendered() << few.rendered() );
        REQUIRE( many.rendered().find( "`Box` takes 1 type argument, but 2 were given" ) != std::string::npos );
        REQUIRE( few.rendered().find( "`Pair` takes 2 type arguments, but 1 was given" ) != std::string::npos );
    }

    SECTION( "a bound on the declaration is checked at the annotation" )
    {
        const Typed p( "struct Box<T> where T : Integral { T v; };\ni32 main() { Box<f64> b; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`f64` is not `Integral`, and `Box` requires it of `T`" ) != std::string::npos );
    }

    SECTION( "an unknown type argument is reported once" )
    {
        // And the aggregate is not interned as `Box<<error>>`, which would spell nothing and which
        // every later diagnostic would name.
        const Typed p( "struct Box<T> where T : Copyable { T v; };\ni32 main() { Box<Nope> b; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "unknown type `Nope`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "<error>" ) == std::string::npos );
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

// One mistake, one diagnostic. A literal has a value and no type, so in a context that has already
// failed there is nothing for it to adopt from - and asking it anyway makes it report that, on top
// of the error that is the actual cause. `nullptr` is the one that shows this, because it is the
// only literal with no default type to fall back on.
// D35 deferred "unsafe to call" to this slice, and `extern` is what needed it: the FFI boundary is
// where the assertion belongs, because a C function's contract is a promise rather than a proof.
TEST_CASE( "type_checker_gates_an_extern_call_on_unsafe", "[sema][types][extern]" )
{
    SECTION( "refused outside an unsafe block" )
    {
        const Typed p( "extern i32 abs( i32 v );\ni32 main() { return abs( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`unsafe` block" ) != std::string::npos );
        REQUIRE( p.rendered().find( "abs" ) != std::string::npos );
    }

    SECTION( "permitted inside one" )
    {
        const Typed p( "extern i32 abs( i32 v );\ni32 main() { i32 n = 0; unsafe { n = abs( 1 ); } return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an extern call is enough to justify the block" )
    {
        // The second entry on D35's list, and the one that makes `unsafe_used_` a flag with more
        // than one writer.
        const Typed p( "extern i32 abs( i32 v );\ni32 main() { i32 n = 0; unsafe { n = abs( 1 ); } return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "does nothing unsafe" ) == std::string::npos );
    }

    SECTION( "calling an ordinary function needs nothing" )
    {
        const Typed p( "i32 twice( i32 v ) { return v + v; }\ni32 main() { return twice( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the permission does not leak past the block" )
    {
        const Typed p( "extern i32 abs( i32 v );\n"
                       "i32 main()\n"
                       "{\n"
                       "    i32 n = 0;\n"
                       "    unsafe { n = abs( 1 ); }\n"
                       "    n = abs( 2 );\n"
                       "    return n;\n"
                       "}" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a safe wrapper is the point of the rule" )
    {
        // Being the checked wrapper over an unchecked boundary is what §12 says `Buffer` is for.
        const Typed p( "extern i32 abs( i32 v );\n"
                       "i32 magnitude( i32 v ) { i32 n = 0; unsafe { n = abs( v ); } return n; }\n"
                       "i32 main() { return magnitude( 0 - 3 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "in an argument to another call" )
    {
        const Typed p( "extern i32 abs( i32 v );\n"
                       "i32 twice( i32 v ) { return v + v; }\n"
                       "i32 main() { return twice( abs( 1 ) ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// An extern is checked like any other declaration: it has a signature, and every call is checked
// against it. `unsafe` permits the call; it does not stop the compiler reading the types.
TEST_CASE( "type_checker_checks_calls_to_an_extern", "[sema][types][extern]" )
{
    SECTION( "the argument count" )
    {
        const Typed p( "extern i32 abs( i32 v );\ni32 main() { i32 n = 0; unsafe { n = abs( 1, 2 ); } return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "takes 1 argument" ) != std::string::npos );
    }

    SECTION( "the argument types" )
    {
        const Typed p( "extern i32 abs( i32 v );\n"
                       "i32 main() { f64 d = 1.5; i32 n = 0; unsafe { n = abs( d ); } return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "the return type" )
    {
        const Typed p( "extern i32 abs( i32 v );\ni32 main() { bool b = true; unsafe { b = abs( 1 ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "an unknown parameter type is reported" )
    {
        const Typed p( "extern i32 f( Nope v );\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a pointer signature is typed" )
    {
        const Typed p( "extern u64 length( u8* s );\n"
                       "i32 main() { u8* p = nullptr; u64 n = 0; unsafe { n = length( p ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The modes work on an extern, and two of them are where a Keel guarantee starts resting on a
// promise rather than a proof. Pinned as *accepted* rather than as correct: the call site's
// `unsafe` is what is standing behind them, which is the whole reason the gate is there.
TEST_CASE( "type_checker_accepts_binding_modes_on_an_extern", "[sema][types][extern]" )
{
    SECTION( "a ref parameter" )
    {
        const Typed p( "extern void bump( ref i32 v );\ni32 main() { i32 x = 1; unsafe { bump( ref x ); } return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the marker is still required at the call" )
    {
        const Typed p( "extern void bump( ref i32 v );\ni32 main() { i32 x = 1; unsafe { bump( x ); } return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "an out parameter satisfies definite assignment" )
    {
        // The hole worth knowing about: the obligation to assign is discharged in the callee's
        // body, and an extern has none - so `x` is believed initialised on the C function's word.
        const Typed p( "extern void init( out i32 v );\ni32 main() { i32 x; unsafe { init( out x ); } return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a move parameter is accepted, and the caller's value dies" )
    {
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } ~C() { } };\n"
                       "extern void take( move C c );\n"
                       "i32 main() { C c = C( 1 ); unsafe { take( move c ); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a struct parameter" )
    {
        const Typed p( "struct P { i32 x; };\n"
                       "extern i32 sum( P p );\n"
                       "i32 main() { P p = P { 1 }; i32 n = 0; unsafe { n = sum( p ); } return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "returning void" )
    {
        const Typed p( "extern void flush();\ni32 main() { unsafe { flush(); } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_types_a_constructor_call", "[sema][aggregates]" )
{
    SECTION( "the call has the class's type" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer( 16 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Call_expr, 0 ) ) == "Buffer" );
    }

    // The receiver is a parameter like any other, so forgetting to skip it would demand an extra
    // argument at every call site.
    SECTION( "the receiver is not one of the arguments" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer( 16, 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "too few arguments is an error" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer(); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "an argument is checked against the parameter" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer( nullptr ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a class without a constructor is not callable" )
    {
        const Typed p( "class Handle { u64 value; };\ni32 main() { Handle h = Handle( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

TEST_CASE( "type_checker_requires_a_place_for_a_ref_argument", "[sema][ref]" )
{
    constexpr std::string_view bump = "void bump( ref i32 n ) { n = n + 1; }\n";

    SECTION( "a literal has no place to lend" )
    {
        const Typed p( std::string( bump ) + "i32 main() { bump( ref 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "`ref` needs a variable to borrow" ) != std::string::npos );
    }

    // Unlike a move, a borrow leaves nothing partly gone, so a field needs no restriction of its
    // own - which is the whole reason is_assignable is the test rather than a stricter one.
    SECTION( "a field can be lent" )
    {
        const Typed p(
            "struct P { i32 x; };\n" + std::string( bump ) + "i32 main() { P p = P { 1 }; bump( ref p.x ); return p.x; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and so can a pointee" )
    {
        const Typed p( std::string( bump ) + "i32 main() { i32 x = 1; i32* q = &x; bump( ref *q ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A binding is the caller's object itself, so there is no conversion step for a widened copy to
    // live in. §6.4 would happily widen the argument, and the callee would then write into a
    // temporary nobody reads.
    SECTION( "a ref argument is not widened to reach the parameter" )
    {
        const Typed p( std::string( bump ) + "i32 main() { u8 x = 1; bump( ref x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// The whole of `out` in one program: the callee assigns through the parameter, the call site says
// so, and the definite-assignment pass is satisfied because every path writes it.
TEST_CASE( "type_checker_accepts_out", "[sema][out]" )
{
    const Typed p( "void init( out i32 n ) { n = 1; }\ni32 main() { i32 x = 0; init( out x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// PLAN D31. `out` is the fourth marker: the callee assigns the caller's variable, and both sides
// say so. It travels by address like `ref`, and what separates them is only when it may be read -
// which is assign_check's question, not one the type checker can answer.
TEST_CASE( "type_checker_checks_an_out_argument", "[sema][out]" )
{
    constexpr std::string_view init = "void init( out i32 n ) { n = 1; }\n";

    SECTION( "it needs a variable to assign to" )
    {
        const Typed p( std::string( init ) + "i32 main() { init( out 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`out` needs a variable to assign to" ) != std::string::npos );
    }

    // The callee writes through it, so the same rule that stops `ref` laundering a const stops
    // `out` doing it - one test, reused, which is why check_writable is shared.
    SECTION( "and one that may be written" )
    {
        const Typed p( std::string( init ) + "i32 main() { const i32 k = 1; init( out k ); return k; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`k` is `const`" ) != std::string::npos );
    }

    SECTION( "the marker is required at the call" )
    {
        const Typed p( std::string( init ) + "i32 main() { i32 x = 0; init( x ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "write `out`" ) != std::string::npos );
    }

    SECTION( "a field may be assigned through" )
    {
        const Typed p(
            "struct P { i32 x; };\n" + std::string( init ) + "i32 main() { P v = P { 0 }; init( out v.x ); return v.x; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// An enum is an ordinary type in every position that does not care what it holds.
TEST_CASE( "type_checker_passes_an_enum_like_any_other_type", "[sema][enum]" )
{
    const Typed p( "enum Colour { Red, Green };\n"
                   "Colour flip( Colour c ) { if( c == Colour::Red ) { return Colour::Green; } return Colour::Red; }\n"
                   "i32 main() { Colour c = flip( Colour::Red ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// The call site binds each parameter to the type argument written for it, and every use of the
// parameter's declared type below is of the *substituted* one - so no diagnostic names `T`, which
// the caller never wrote.
TEST_CASE( "type_checker_types_a_generic_call", "[sema][generic]" )
{
    const std::string_view id = "T id<T>( T a ) where T : Copyable { return a; }\n";

    SECTION( "the call takes the argument's type" )
    {
        const Typed p( std::string( id ) + "i32 main() { return id<i32>( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and produces it" )
    {
        const Typed p( std::string( id ) + "i32 main() { f64 x = id<f64>( 1.0 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the result type is substituted, not left as the parameter" )
    {
        const Typed p( std::string( id ) + "i32 main() { bool b = id<i32>( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "expected `bool`, but got `i32`" ) != std::string::npos );
    }

    SECTION( "an argument is checked against the substituted parameter" )
    {
        const Typed p( std::string( id ) + "i32 main() { return id<i32>( true ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "expected `i32`, but got `bool`" ) != std::string::npos );
    }

    SECTION( "one generic at two types in one program" )
    {
        const Typed p( std::string( id ) + "i32 main() { f64 x = id<f64>( 1.0 ); return id<i32>( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "several parameters, bound positionally" )
    {
        const Typed p( "T pick<T, U>( T a, U b ) where T : Copyable { return a; }\n"
                       "i32 main() { f64 d = 2.0; return pick<i32, f64>( 1, d ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a `ref` mismatch names the substituted type, not the parameter" )
    {
        // The borrow check reads the parameter's type in two more places than the argument check
        // does; missing either leaves it comparing against `T` and always firing.
        const Typed p( "void bump<T>( ref T a ) { }\ni32 main() { u8 x = 1; bump<i32>( ref x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "cannot borrow `u8` as `ref i32`" ) != std::string::npos );
    }
}

// PLAN D29. A method is an ordinary function whose parameter 0 is the receiver, so a call is checked
// against the parameters from 1 - the same skip a constructor call already does.
TEST_CASE( "type_checker_calls_a_method", "[sema][method]" )
{
    constexpr std::string_view point = "struct P { i32 x; i32 get() const { return x; } void set( i32 v ) { x = v; } };\n";

    SECTION( "the result is the method's return type" )
    {
        const Typed p( std::string( point ) + "i32 main() { P p = P { 1 }; p.set( 2 ); return p.get(); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // D22 reaches through a pointer for a field, and a method is reached the same way - `q.get()`
    // needs no second rule.
    SECTION( "and it reaches through a pointer" )
    {
        const Typed p( std::string( point ) + "i32 main() { P p = P { 1 }; P* q = &p; return q.get(); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "arguments are checked from parameter 1" )
    {
        const Typed p( std::string( point ) + "i32 main() { P p = P { 1 }; p.set( true ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "and the count excludes the receiver" )
    {
        const Typed p( std::string( point ) + "i32 main() { P p = P { 1 }; p.set( 1, 2 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // The body sees the fields, which is the resolver's member scope - gated on is_function_like,
    // so a method got it by being added to that one predicate.
    SECTION( "the body sees the fields by bare name" )
    {
        const Typed p( "struct P { i32 x; i32 y; i32 sum() const { return x + y; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The body is checked against the method's own return type. It was not, for a while: Method_decl
    // was missing from the checker's visit dispatch, so a body fell to the default child walk and
    // current_return_ was never set - every return in every method went unchecked.
    SECTION( "and against the method's return type" )
    {
        const Typed p( "struct P { i32 x; i32 wrong() const { return true; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "expected `i32`, but got `bool`" ) != std::string::npos );
    }
}

TEST_CASE( "type_checker_reports_a_bad_method_call", "[sema][method]" )
{
    SECTION( "no method of that name" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { P p = P { 1 }; return p.nope(); }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`P` has no method `nope`" ) != std::string::npos );
    }

    // Worth its own message: calling a field is a different mistake from naming one that is not
    // there, and the fix is different too.
    SECTION( "a field called as a method" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { P p = P { 1 }; return p.x(); }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "is a field of `P`, not a method" ) != std::string::npos );
    }

    SECTION( "and a receiver with no methods at all" )
    {
        const Typed p( "i32 main() { i32 n = 1; return n.foo(); }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`i32` has no methods" ) != std::string::npos );
    }

    // A constructor shares the type's name and is reached by calling the type; a destructor is
    // never called at all. Matching either by name here would let `p.P()` resolve.
    SECTION( "a constructor is not reachable by name" )
    {
        const Typed p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                       "i32 main() { B b = B( 1 ); b.B( 2 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "has no method `B`" ) != std::string::npos );
    }
}

// Decision 1, and the whole of what a trailing `const` buys: a method without one takes its
// receiver as `ref T` and may write the object, so calling it needs a receiver that may be written.
// That is check_writable's question asked of the object, so every rule that already refuses a write
// refuses this too, each with its own message.
TEST_CASE( "type_checker_enforces_a_const_method", "[sema][method]" )
{
    constexpr std::string_view owning = "class B { u64 n; B( u64 x ) { n = x; } ~B() { }"
                                        " u64 read() const { return n; } void bump() { n = n + 1; } };\n";

    SECTION( "a mutating method needs a writable receiver" )
    {
        const Typed p( std::string( owning ) + "i32 main() { const B c = B( 1 ); c.bump(); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`c` is `const`" ) != std::string::npos );
    }

    SECTION( "including through a read-only borrow" )
    {
        const Typed p( std::string( owning ) + "u64 f( B b ) { b.bump(); return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` is borrowed" ) != std::string::npos );
    }

    SECTION( "but a `const` method may be called on either" )
    {
        const Typed p(
            std::string( owning ) + "u64 f( B b ) { return b.read(); }\n"
                                    "i32 main() { const B c = B( 1 ); return wrap<i32>( c.read() ); }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The half that makes the receiver's mode mean something inside the body. A bare field name is
    // `this.field` written implicitly, so what it is rooted in is the *receiver* - without that the
    // field is not a binding, every question answers no, and a `const` method writes its own object
    // in silence.
    SECTION( "and a `const` method cannot write its own field" )
    {
        const Typed p( "struct P { i32 x; void bad() const { x = 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a `const` method cannot modify its object" ) != std::string::npos );
        REQUIRE( p.rendered().find( "remove `const` from `bad`" ) != std::string::npos );
    }

    SECTION( "where a non-const one may" )
    {
        const Typed p( "struct P { i32 x; void fine() { x = 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A sibling is callable by bare name: methods join the member scope alongside fields, so
// `add( by )` means `this.add( by )`. C++ does this, and a type whose methods have to qualify each
// other is tiring to write long before it is large.
TEST_CASE( "type_checker_calls_a_sibling_method_by_name", "[sema][method]" )
{
    constexpr std::string_view counter = "struct C { i32 v; i32 read() const { return v; }"
                                         " void add( i32 b ) { v = v + b; } };\n";

    SECTION( "from a mutating method" )
    {
        const Typed p( "struct C { i32 v; void add( i32 b ) { v = v + b; }"
                       " void twice( i32 b ) { add( b ); add( b ); } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and from a `const` one, when the sibling is `const` too" )
    {
        const Typed p( std::string( counter ) + "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The receiver's mode carries through a bare call as it does through a written one: the
    // constness question is asked of the *enclosing* method's receiver, there being no object
    // expression to ask it of.
    SECTION( "but a `const` method cannot call a mutating sibling" )
    {
        const Typed p( "struct C { i32 v; void add( i32 b ) { v = v + b; }"
                       " i32 bad() const { add( 1 ); return v; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a `const` method cannot call `add`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "remove `const` from `bad`, or add it to `add`" ) != std::string::npos );
    }

    SECTION( "arguments are still checked" )
    {
        const Typed p( "struct C { i32 v; void add( i32 b ) { v = v + b; }"
                       " void bad() { add( true ); } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // The scope is the type's, so the name is not in reach from outside it - which is what makes
    // the bare form unambiguous rather than a second way to spell a free function.
    SECTION( "and the name does not escape the type" )
    {
        const Typed p( "struct C { i32 v; i32 read() const { return v; } };\ni32 loose() { return read(); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`read` is not declared" ) != std::string::npos );
    }

    // A method and a field cannot share a name, because both live in the member scope.
    SECTION( "a method colliding with a field is refused" )
    {
        const Typed p( "struct C { i32 v; i32 v() const { return 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// PLAN §8. "A method may return a reference into its object" was fiction until the receiver became
// a binding - `this` was a `T*` passed by value, so it failed the by-address test the rule uses.
// It is true now, and this is what makes it a checked claim.
//
// Four layers had to agree for it to work, and each was silently wrong on its own: the method's
// return annotation had to have its address recorded (only free functions did), a method call had
// to count as a *place* (is_assignable knew only Function_decl), is_const_binding had to answer for
// a Method_decl, and the call had to be typed as the pointer it returns rather than the language
// type - which produced C that assigned an `int*` to an `int`.
TEST_CASE( "type_checker_returns_a_reference_into_the_object", "[sema][method]" )
{
    SECTION( "a `const ref` method is accepted and its result binds" )
    {
        const Typed p( "struct P { i32 x; const ref i32 get() const { return x; } };\n"
                       "i32 main() { P p = P { 7 }; const ref i32 r = p.get(); return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a mutable one is refused, as for a free function" )
    {
        const Typed p( "struct P { i32 x; ref i32 get() { return x; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "only a `const ref` may be returned" ) != std::string::npos );
    }
}

// D29 splits the two kinds at trivial copyability, not at what they may have - so a `class` gets
// methods on the same machinery a `struct` does, alongside the constructor and destructor it may
// also have. Nothing here is a second implementation of anything; this is the case that says so.
TEST_CASE( "type_checker_gives_a_class_methods_too", "[sema][method]" )
{
    constexpr std::string_view owned = "class B { u64 n; B( u64 x ) { n = x; } ~B() { }"
                                       " u64 read() const { return n; }"
                                       " void add( u64 by ) { n = n + by; }"
                                       " void twice( u64 by ) { add( by ); add( by ); } };\n";

    SECTION( "alongside a constructor and destructor" )
    {
        const Typed p( std::string( owned ) + "i32 main() { B b = B( 1 ); b.twice( 10 ); return wrap<i32>( b.read() ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The receiver is a borrow either way, so the rules that apply to a struct's methods apply
    // here unchanged - including the one that matters most for an owning type.
    SECTION( "and the const rule holds on one" )
    {
        const Typed p( std::string( owned ) + "i32 main() { const B b = B( 1 ); b.add( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` is `const`" ) != std::string::npos );
    }
}

// A method on a generic aggregate instantiates it as surely as a written call does, and lowering
// reads the answer rather than working it out again - so the call has to carry the edge.
// The instance a construction is spelled in terms of is the aggregate's own, so a free function's
// set must not be read in the expectation's bindings: `make`'s `T` is not `Box`'s, and binding one
// by the other leaves a parameter unsubstituted that a later substitution asserts on.
TEST_CASE( "expressions_keep_a_free_function_out_of_the_expectation_s_bindings", "[sema][overload][generic]" )
{
    const Typed p( "struct Box<T> where T : Copyable { T v; };\n"
                   "Box<T> make<T>( T a ) where T : Copyable { Box<T> r = Box { a }; return r; }\n"
                   "Box<i32> make( bool a ) { Box<i32> r = Box { 0 }; return r; }\n"
                   "i32 main() { i32 x = 7; Box<i32> b = make( x ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

TEST_CASE( "expressions_record_the_instantiation_a_method_call_chose", "[sema][method][generic]" )
{
    Typed p( "struct Box<T> where T : Copyable { T v; T get( ) { return this.v; } };\n"
             "i32 main() { Box<i32> b = Box { 7 }; return b.get( ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id call = p.nth( Node_kind::Call_expr, 0 );
    REQUIRE( call.is_valid() );
    REQUIRE( p.types().instantiation_of( call ).has_value() );
}

} // namespace keel
#endif
