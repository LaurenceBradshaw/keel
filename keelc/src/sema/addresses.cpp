// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/addresses.h"
#include <fmt/format.h>
#include "sema/type_checker.h"

namespace keel::sema
{

// `&f` is the one place a function's name is a value. The type is the signature itself rather than
// a pointer to it: `fn( i32 ) -> i32*` already means a function returning a pointer, so there is no
// spelling left for the other reading.
Type_id Addresses::function_address( Node_id id, Node_id declaration, Type_id expectation, Node_id current_function )
{
    const Type_id          error = table_.builtin( Type_kind::Error );
    const std::string_view name  = interner_.text( ast_.name( ast_.operand( id ) ) );

    const Node_id type_args = ast_.type_arg_list( ast_.operand( id ) );

    if( overloads_.next_visible( id, declaration ).is_valid() )
    {
        if( type_args.is_valid() )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format( "`{}` is overloaded, so its type arguments choose nothing", name ),
                "give the generic a name of its own, or drop the type arguments and choose by the signature"
            );

            return types_.record( id, error );
        }

        if( expectation.is_valid() && table_.is_function( expectation ) )
        {
            declaration = overload_for_signature( id, name, declaration, expectation, Bindings {} );

            if( !declaration.is_valid() )
            {
                return types_.record( id, error );
            }
        }
        else
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format( "`{}` is overloaded, so its address names no one function", name ),
                "assigning it to a variable of one signature will choose between them"
            );

            return types_.record( id, error );
        }
    }

    if( ast_.is_generic( declaration ) && !type_args.is_valid() )
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format( "`{}` is generic, so its address names no one function", name ),
            fmt::format( "write its type arguments, as in `&{}<i32>`", name )
        );

        return types_.record( id, error );
    }

    if( !ast_.is_generic( declaration ) && type_args.is_valid() )
    {
        reporter_.error_at(
            ast_.span( type_args ), fmt::format( "`{}` is not generic, so it takes no type arguments", name ), "remove them"
        );

        return types_.record( id, error );
    }

    if( ast_.is_located( declaration ) )
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format( "`{}` is `@located`, so it has no address", name ),
            "a call through a pointer has no caller to pass on; call it directly, or wrap it in a function that is not "
            "`@located`"
        );
        return types_.record( id, error );
    }

    std::vector<Type_id> resolved;
    Bindings             bindings;
    if( type_args.is_valid() )
    {
        bool success = annotations_.resolve_type_arguments(
            ast_.type_parameters( ast_.type_param_list( declaration ) ), type_args, name, resolved
        );
        if( !success )
        {
            return types_.record( id, error );
        }

        bindings = overloads_.type_bindings( declaration, resolved );
    }

    Node_id       refused {};
    const Type_id signature = written_signature( declaration, bindings, refused );
    if( refused.is_valid() )
    {
        const Type_id          type  = types_.type_of( refused );
        const std::string_view spelt = table_.name( type );

        reporter_.error_at(
            ast_.span( id ),
            fmt::format(
                "`{}` takes `{}`, which is not known to be copyable, so its address has no function type yet", name, spelt
            ),
            table_.is_parameter( type )
                ? fmt::format(
                      "add `where {} : copyable` to `{}`, whose parameter is at {}",
                      spelt,
                      name,
                      reporter_.position( ast_.span( refused ) )
                  )
                : fmt::format(
                      "take it as `const ref {}`, so every instance passes it the same way; the parameter is at {}",
                      spelt,
                      reporter_.position( ast_.span( refused ) )
                  )
        );

        return types_.record( id, error );
    }

    // Recorded after the refusal, because a refused address seeds no instance.
    if( type_args.is_valid() )
    {
        overloads_.record_instance( current_function, id, declaration, std::move( resolved ) );
    }

    callees_.record( id, declaration );
    return types_.record( id, signature );
}

Type_id Addresses::method_address( Node_id id, Node_id aggregate, Type_id expectation, Node_id current_function )
{
    const Node_id          path  = ast_.operand( id );
    const Symbol_id        name  = ast_.name( path );
    const std::string_view owner = interner_.text( ast_.name( aggregate ) );

    const Node_id first = aggregates_.find_method( aggregate, name );
    if( !first.is_valid() )
    {
        const Node_id field = aggregates_.find_field( types_.type_of( aggregate ), name );
        if( field.is_valid() )
        {
            return field_address( id, aggregate, field, current_function );
        }

        if( aggregates_.names_constructor( aggregate, name ) )
        {
            reporter_.error_at(
                ast_.span( id ),
                "a constructor has no address",
                fmt::format( "take the address of a function that returns `{}( ... )`", owner )
            );

            return types_.poison( id );
        }

        reporter_.error_at( ast_.span( id ), fmt::format( "`{}` has no member `{}`", owner, interner_.text( name ) ) );

        return types_.poison( id );
    }

    if( !ast_.is_visible_from( first, ast_.enclosing_aggregate( current_function ) ) )
    {
        aggregates_.report_private( id, first );

        return types_.poison( id );
    }

    const Type_id qualified_type = annotations_.qualifier_type( path, aggregate );
    if( table_.is_error( qualified_type ) )
    {
        return types_.poison( id );
    }

    const Bindings bindings = aggregates_.bindings_of( qualified_type );

    Node_id method;
    if( !overloads_.next_overload( first ).is_valid() )
    {
        method = first;
    }
    else if( expectation.is_valid() && table_.is_function( expectation ) )
    {
        method = overload_for_signature( id, interner_.text( name ), first, expectation, bindings );

        if( !method.is_valid() )
        {
            return types_.poison( id );
        }
    }
    else
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format( "`{}` is overloaded, so its address names no one function", interner_.text( name ) ),
            "assigning it to a variable of one signature will choose between them"
        );

        return types_.poison( id );
    }

    if( ast_.is_located( method ) )
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format( "`{}` is `@located`, so it has no address", interner_.text( name ) ),
            "a call through a pointer has no caller to pass on; call it directly, or wrap it in a function that is not "
            "`@located`"
        );
        return types_.poison( id );
    }

    if( ast_.own_type_param_list( method ).is_valid() )
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format( "`{}` has type parameters of its own, so its address names no one function", interner_.text( name ) ),
            "wrap the call you want in a function, and take that function's address"
        );

        return types_.poison( id );
    }

    Node_id       refused {};
    const Type_id signature = written_signature( method, bindings, refused );

    if( refused.is_valid() )
    {
        const Type_id          type  = types_.type_of( refused );
        const std::string_view spelt = table_.name( type );

        reporter_.error_at(
            ast_.span( id ),
            fmt::format(
                "`{}` takes `{}`, which is not known to be copyable, so its address has no function type yet",
                interner_.text( name ),
                spelt
            ),
            table_.is_parameter( type )
                ? fmt::format(
                      "add `where {} : copyable` to `{}`; the parameter of `{}` is at {}",
                      spelt,
                      owner,
                      interner_.text( name ),
                      reporter_.position( ast_.span( refused ) )
                  )
                : fmt::format(
                      "take it as `const ref {}`, so every instance passes it the same way; the parameter of `{}` is at {}",
                      spelt,
                      interner_.text( name ),
                      reporter_.position( ast_.span( refused ) )
                  )
        );

        return types_.poison( id );
    }

    overloads_.record_method_instantiation( current_function, id, method, qualified_type );
    callees_.record( id, method );
    return types_.record( id, signature );
}

Type_id Addresses::field_address( Node_id id, Node_id aggregate, Node_id field, Node_id current_function )
{
    if( !ast_.is_visible_from( field, ast_.enclosing_aggregate( current_function ) ) )
    {
        aggregates_.report_private( id, field );

        return types_.poison( id );
    }

    const Node_id path = ast_.operand( id );
    const Type_id type = annotations_.qualifier_type( path, aggregate );
    if( table_.is_error( type ) )
    {
        return types_.poison( id );
    }

    const Type_id field_type = table_.substitute( types_.type_of( field ), aggregates_.bindings_of( type ) );
    if( !bounds_.satisfies( field_type, Bound::Copyable ) )
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format(
                "`{}` is a `{}`, which is not copyable, so reading it through a field type would copy it",
                interner_.text( ast_.name( field ) ),
                table_.name( field_type )
            ),
            "a field type that borrows is not supported yet"
        );

        return types_.poison( id );
    }

    callees_.record( id, field );
    return types_.record( id, table_.field( type, field_type ) );
}

Type_id Addresses::written_signature( Node_id declaration, const Bindings& bindings, Node_id& refused )
{
    refused = Node_id {};
    std::vector<Parameter> parameters;

    for( const Node_id param : ast_.params( declaration ) )
    {
        Parameter parameter;

        parameter.mode = parameter_mode_of( ast_, param );

        const Type_id declared    = types_.type_of( param );
        const Type_id substituted = table_.substitute( declared, bindings );

        // Refused when the declaration passes it by address and this instance's type would not
        if( parameter.mode == Param_mode::Value && !bounds_.satisfies( declared, Bound::Copyable ) &&
            bounds_.satisfies( substituted, Bound::Copyable ) )
        {
            refused = param;
            return Type_id {};
        }

        parameter.type = substituted;
        // The bound is asked of the declared parameter and the spelling of the substituted one: what
        // travels is decided before the instance exists.
        parameters.push_back( parameter );
    }

    return table_.function(
        table_.substitute( types_.type_of( declaration ), bindings ), parameters, parameter_mode_of( ast_, declaration )
    );
}

Node_id Addresses::overload_for_signature(
    Node_id id, std::string_view name, Node_id first, Type_id signature, const Bindings& bindings
)
{
    std::vector<std::string> offered;
    for( Node_id candidate = first; candidate.is_valid(); candidate = overloads_.next_visible( id, candidate ) )
    {
        if( ast_.is_generic( candidate ) && bindings.empty() )
        {
            continue;
        }

        Node_id       refused {};
        const Type_id written = written_signature( candidate, bindings, refused );

        if( !written.is_valid() )
        {
            continue;
        }

        if( written == signature )
        {
            return candidate;
        }

        offered.push_back( fmt::format( "`{}`", table_.name( written ) ) );
    }

    std::string text;

    for( std::size_t i = 0; i < offered.size(); ++i )
    {
        if( i != 0 )
        {
            text += i + 1 == offered.size() ? " and " : ", ";
        }

        text += offered[i];
    }

    reporter_.error_at(
        ast_.span( id ),
        fmt::format( "no overload of `{}` has the type `{}`", name, table_.name( signature ) ),
        offered.empty() ? "none of its overloads can be written as a type" : fmt::format( "its overloads are {}", text )
    );

    return Node_id {};
}

} // namespace keel::sema
