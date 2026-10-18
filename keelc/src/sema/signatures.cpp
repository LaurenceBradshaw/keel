// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/signatures.h"

#include <fmt/format.h>

#include <unordered_set>

// The signature pass: what each kind of top-level declaration contributes, and the rules over an
// aggregate's own members. The overload rules over those members are `Overloads`'; signatures.h says
// what the pass is for and why its order is the way it is.

namespace keel::sema
{

namespace
{

constexpr Member_kind k_member_kinds[] = {
    { Node_kind::Constructor_decl, "constructor", "", "use `class`, or build this from a literal", false },
    { Node_kind::Destructor_decl, "destructor", "~", "use `class` if this type owns a resource", true },
};

bool names_self( const Ast& ast, const Interner& interner, Node_id annotation )
{
    if( !annotation.is_valid() )
    {
        return false;
    }

    if( ast.kind( annotation ) == Node_kind::Named_type && !ast.package( annotation ).is_valid() &&
        interner.text( ast.name( annotation ) ) == "self" )
    {
        return true;
    }

    for( const Node_id child : ast.children( annotation ) )
    {
        if( names_self( ast, interner, child ) )
        {
            return true;
        }
    }

    return false;
}

// Read from the syntax, since a `ref P` may be resolved before `P`'s own methods are typed.
Node_id bound_only_method( const Ast& ast, const Interner& interner, Node_id interface )
{
    for( const Node_id member : ast.members( interface ) )
    {
        if( ast.is_static_method( member ) )
        {
            return member;
        }

        if( names_self( ast, interner, ast.return_type( member ) ) )
        {
            return member;
        }

        for( const Node_id param : ast.explicit_params( member ) )
        {
            if( names_self( ast, interner, ast.annotation( param ) ) )
            {
                return member;
            }
        }
    }

    return Node_id {};
}

} // namespace

void Signatures::declare()
{
    declare_interfaces();
    declare_structs();

    // After the struct pass: a payload field may name a struct or a class. Before everything else,
    // because a parameter, a field or a global may name an enum in turn.
    declare_enums();

    // Once every type exists, since a bound's arguments may name any of them.
    declare_interface_requirements();
    declare_type_bounds();
    declare_payloads();
    declare_implements();
    declare_member_functions();
    declare_fields();
    order_structs();
    bounds_.check_deferred();
    check_aggregate_members();
    check_signatures();
    check_operators();
    check_struct_ownership();
    declare_functions();
    declare_globals();
    places_.record_borrowed_parameters();
    overloads_.check_overload_sets();
}

void Signatures::declare_structs()
{
    for( Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        // Before the type below, because the type *is* its parameters: `Box` on its own is
        // `Box<T>`, the open form D11 checks the body against, and `T` has to exist to name it.
        declare_parameters( ast_.type_param_list( child ) );

        std::vector<Type_id> arguments;

        for( const Node_id type_param : ast_.type_parameters( ast_.type_param_list( child ) ) )
        {
            arguments.push_back( types_.type_of( type_param ) );
        }

        const std::string_view name = interner_.text( ast_.name( child ) );

        types_.record( child, table_.structure( child, arguments, name ) );
    }
}

// First, because anything below may name one.
void Signatures::declare_interfaces()
{
    for( const Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Interface_decl )
        {
            continue;
        }

        const std::string_view name = interner_.text( ast_.name( child ) );
        const Node_id          list = ast_.type_param_list( child );
        declare_parameters( list );

        std::vector<Type_id> arguments;
        for( const Node_id type_param : ast_.type_parameters( list ) )
        {
            arguments.push_back( types_.type_of( type_param ) );
        }

        types_.record( child, table_.interface( child, arguments, name ) );

        if( const Node_id method = bound_only_method( ast_, interner_, child ); method.is_valid() )
        {
            types_.conformances().declare_bound_only( child, method );
        }
    }
}

void Signatures::declare_parameters( Node_id list )
{
    for( const Node_id type_param : ast_.type_parameters( list ) )
    {
        types_.record( type_param, table_.parameter( type_param, interner_.text( ast_.name( type_param ) ) ) );
    }
}

void Signatures::declare_bounds( Node_id declaration, Node_id list )
{
    declare_parameters( list );

    for( const Node_id clause : ast_.where_clauses( list ) )
    {
        if( ast_.kind( clause ) != Node_kind::Where_clause )
        {
            continue;
        }

        for( const Node_id bound : ast_.bounds( clause ) )
        {
            annotations_.record_interface_bound( bound );
        }
    }

    bounds_.declare_type_parameters( declaration, list );
}

// What each interface's `where` and `implements` name, once every type exists.
void Signatures::declare_interface_requirements()
{
    for( const Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) != Node_kind::Interface_decl )
        {
            continue;
        }

        declare_bounds( child, ast_.type_param_list( child ) );
    }

    // What each interface requires, once every interface has its type. A cycle is refused at the
    // name that closes it, by declaration, so `refines` never recurses forever.
    for( const Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) != Node_kind::Interface_decl )
        {
            continue;
        }

        const Type_id interface = types_.type_of( child );

        for( const Node_id written : ast_.implemented( child ) )
        {
            const Type_id refined = annotations_.resolve_interface( written );
            types_.record( written, refined );

            if( !table_.is_interface( refined ) )
            {
                continue;
            }

            if( table_.get( refined ).declaration == child )
            {
                reporter_.error_at(
                    ast_.span( written ), fmt::format( "`{}` cannot require itself", table_.base_name( interface ) )
                );
                continue;
            }

            if( types_.conformances().refines_declaration( table_, table_.get( refined ).declaration, child ) )
            {
                reporter_.error_at(
                    ast_.span( written ),
                    fmt::format(
                        "`{0}` cannot require `{1}`, which already requires `{0}`",
                        table_.base_name( interface ),
                        table_.base_name( refined )
                    )
                );
                continue;
            }

            types_.conformances().declare_refinement( table_, interface, refined );
        }
    }

    // Requiring a bound-only interface makes one bound-only too: what it promises includes the
    // method naming `self`.
    for( const Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) != Node_kind::Interface_decl ||
            types_.conformances().bound_only_method( table_, types_.type_of( child ) ).is_valid() )
        {
            continue;
        }

        if( const Node_id method = required_bound_only( types_.type_of( child ) ); method.is_valid() )
        {
            types_.conformances().declare_bound_only( child, method );
        }
    }
}

// After the requirements, so a bound closes over what each interface requires.
void Signatures::declare_type_bounds()
{
    for( const Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( is_aggregate( ast_.kind( child ) ) || ast_.kind( child ) == Node_kind::Enum_decl )
        {
            declare_bounds( child, ast_.type_param_list( child ) );
        }
    }
}

// Depth-first through what `interface` requires, read from the syntax so declaration order does not matter.
Node_id Signatures::required_bound_only( Type_id interface ) const
{
    for( const Type_id refined : types_.conformances().refinements( table_, interface ) )
    {
        if( const Node_id method = bound_only_method( ast_, interner_, table_.get( refined ).declaration ); method.is_valid() )
        {
            return method;
        }

        if( const Node_id method = required_bound_only( refined ); method.is_valid() )
        {
            return method;
        }
    }

    return Node_id {};
}

// What each aggregate says it implements, once every type exists to be named there by mistake.
void Signatures::declare_implements()
{
    for( const Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) == Node_kind::Implements_decl )
        {
            declare_block( child );
            continue;
        }

        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        for( const Node_id written : ast_.implemented( child ) )
        {
            const Type_id interface = annotations_.resolve_interface( written );
            const Type_id recorded  = types_.record( written, interface );

            if( recorded.is_valid() && table_.is_interface( interface ) )
            {
                types_.conformances().declare( table_, child, types_.type_of( child ), interface );
            }
        }
    }
}

// The type after `for`, what a block may be written for, and where it may be written.
void Signatures::declare_block( Node_id block )
{
    const Node_id written   = ast_.implemented( block )[0];
    const Type_id interface = annotations_.resolve_interface( written );
    types_.record( written, interface );

    const Type_id type = annotations_.resolve_implementer( ast_.implementer( block ) );
    if( table_.is_error( type ) )
    {
        types_.record( block, type );
        return;
    }

    const bool declared = ( table_.is_struct( type ) || table_.is_enum( type ) ) && table_.get( type ).arguments.empty();
    if( !table_.is_builtin( type ) && !declared )
    {
        reporter_.error_at(
            ast_.name_span( block ),
            fmt::format( "`{}` cannot implement an interface through a block", table_.name( type ) ),
            "a block is for a built-in type, or a `struct`, `class` or `enum` with no type parameters"
        );

        types_.record( block, table_.builtin( Type_kind::Error ) );
        return;
    }

    const Node_id declaration = table_.get( type ).declaration;

    // A built-in type belongs to the prelude.
    const Symbol_id here  = resolution_.package_of( ast_.span( block ).file );
    const Symbol_id owner = declared ? resolution_.package_of( ast_.span( declaration ).file ) : resolution_.prelude_package();

    // A package's own struct or class says what it implements where it is declared, so a block for
    // one is only ever the interface's package reaching a type it cannot edit.
    if( table_.is_struct( type ) && here == owner )
    {
        reporter_.error_at(
            ast_.name_span( block ),
            fmt::format( "`{}` is declared in this package, so a block cannot be written for it", table_.name( type ) ),
            fmt::format(
                "write `implements {}` where `{}` is declared", reporter_.text( ast_.span( written ) ), table_.name( type )
            )
        );

        types_.record( block, table_.builtin( Type_kind::Error ) );
        return;
    }

    if( table_.is_interface( interface ) )
    {
        const Symbol_id issuer = resolution_.package_of( ast_.span( table_.get( interface ).declaration ).file );

        // ponytail: an argument counts by its own declaration; `sink<celsius*>` does not make `celsius`'s package an owner.
        const auto owns_argument = [&]( Type_id argument )
        {
            const Node_id declared_at = table_.get( argument ).declaration;
            return declared_at.is_valid() && resolution_.package_of( ast_.span( declared_at ).file ) == here;
        };

        if( here != owner && here != issuer && std::ranges::none_of( table_.get( interface ).arguments, owns_argument ) )
        {
            reporter_.error_at(
                ast_.name_span( block ),
                fmt::format(
                    "a block for `{0}` must be in the package that declares `{0}`{1}",
                    table_.name( type ),
                    table_.get( interface ).arguments.empty()
                        ? fmt::format( " or `{}`", table_.name( interface ) )
                        : fmt::format( ", `{}` or one of its type arguments", table_.name( interface ) )
                ),
                fmt::format(
                    "two packages could otherwise each make `{}` a `{}`", table_.name( type ), table_.name( interface )
                )
            );

            types_.record( block, table_.builtin( Type_kind::Error ) );
            return;
        }
    }

    types_.record( block, type );

    if( table_.is_interface( interface ) )
    {
        types_.conformances().declare( table_, block, type, interface );
    }
}

void Signatures::declare_fields()
{
    for( Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        for( Node_id field : ast_.members( child ) )
        {
            if( ast_.kind( field ) != Node_kind::Field_decl )
            {
                continue;
            }

            const Node_id annotation = ast_.annotation( field );
            const Type_id field_type = annotations_.resolve( annotation );
            annotations_.refuse_void( annotation, field_type, "field", "holds" );
            annotations_.refuse_never( annotation, field_type, "field" );

            types_.record( field, field_type );

            if( ast_.is_generic( child ) )
            {
                generic_recursion_.record_generic_uses( child, field_type, ast_.span( field ) );
            }
        }
    }
}

void Signatures::declare_functions()
{
    for( Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Function_decl )
        {
            continue;
        }

        declare_bounds( child, ast_.type_param_list( child ) );

        const Node_id return_type_node = ast_.return_type( child );
        const Type_id return_type      = annotations_.resolve( return_type_node );
        types_.record( child, return_type );

        const Keyword return_mode = ast_.parameter_mode( child );

        // §8 allows exactly one reference return, and only the read-only one: a mutable one would
        // let a caller write through a reference it never asked for. Any other mode says how an
        // argument travels, and a return is not one.
        if( return_mode != Keyword::Count && !table_.is_error( return_type ) )
        {
            if( return_mode == Keyword::Ref && ast_.is_const_binding( child ) )
            {
                places_.record_binding_address( return_type_node, return_type );
            }
            else if( return_mode == Keyword::Ref )
            {
                reporter_.error_at(
                    ast_.span( return_type_node ),
                    "only a `const ref` may be returned",
                    fmt::format( "write `const ref {}`", table_.name( return_type ) )
                );
            }
            else
            {
                const std::string_view mode_text = interner_.text( Interner::keyword( return_mode ) );

                reporter_.error_at(
                    ast_.span( return_type_node ),
                    fmt::format( "`{}` is not a return mode", mode_text ),
                    fmt::format( "`{}` says how an argument travels, and a return is not an argument", mode_text )
                );
            }
        }

        const Node_id param_list = ast_.param_list( child );
        for( Node_id param : ast_.children( param_list ) )
        {
            if( ast_.kind( param ) != Node_kind::Param_decl )
            {
                continue;
            }

            const Node_id param_type_node = ast_.annotation( param );
            const Type_id param_type      = annotations_.resolve( param_type_node );
            annotations_.refuse_void( bare_type( param_type_node ), param_type, "parameter", "passes" );
            annotations_.refuse_never( bare_type( param_type_node ), param_type, "parameter" );
            types_.record( param, param_type );
        }

        if( interner_.text( ast_.name( child ) ) == "main" )
        {
            // Main may not be marked `extern`
            if( ast_.is_extern( child ) )
            {
                reporter_.error_at( ast_.span( child ), "`main` may not be marked `extern`" );
            }

            // return type
            if( !table_.is_error( return_type ) && return_type != table_.integer( 32, true ) )
            {
                reporter_.error_at(
                    ast_.span( return_type_node ),
                    fmt::format( "`main` must return `i32`, but got `{}`", table_.name( return_type ) )
                );
            }

            // parameters
            // Keel currently doesn't support command line args, or arrays,
            // so `main` must be parameterless.
            if( !ast_.children( param_list ).empty() )
            {
                reporter_.error_at( ast_.span( param_list ), "`main` must not take any parameters" );
            }
        }
    }
}

void Signatures::declare_member_functions()
{
    for( Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( !has_members( ast_.kind( child ) ) )
        {
            continue;
        }

        for( Node_id member : ast_.members( child ) )
        {
            if( !is_function_like( ast_.kind( member ) ) )
            {
                continue;
            }

            declare_bounds( member, ast_.own_type_param_list( member ) );

            // A constructor and a destructor return nothing and have no annotation to read; a
            // method has both. The return type is invalid for the two
            // that have none.
            const Node_id return_type_node = ast_.return_type( member );

            const Type_id return_type =
                return_type_node.is_valid() ? annotations_.resolve( return_type_node ) : table_.builtin( Type_kind::Void );

            types_.record( member, return_type );

            if( ast_.is_generic( child ) && return_type_node.is_valid() && !ast_.own_type_param_list( member ).is_valid() )
            {
                generic_recursion_.record_generic_uses( child, return_type, ast_.span( return_type_node ) );
            }

            // §8 allows exactly one reference return, and only the read-only one - the same rule
            // declare_functions applies to a free function. Without this a method
            // could write `const ref T` and have it silently mean `T`: the recorded address is what
            // every consumer reads to know a binding travels by address, and nothing else sets it.
            if( ast_.is_ref_parameter( member ) && !table_.is_error( return_type ) )
            {
                if( !ast_.is_const_binding( member ) )
                {
                    reporter_.error_at(
                        ast_.span( return_type_node ),
                        "only a `const ref` may be returned",
                        fmt::format( "write `const ref {}`", table_.name( return_type ) )
                    );
                }
                else
                {
                    places_.record_binding_address( return_type_node, return_type );
                }
            }

            for( Node_id param : ast_.params( member ) )
            {
                const bool    is_receiver     = ast_.has_receiver( member ) && param == ast_.params( member )[0];
                const Node_id param_type_node = ast_.annotation( param );
                const Type_id param_type      = is_receiver && ast_.kind( child ) == Node_kind::Interface_decl
                                                    ? annotations_.resolve_receiver( param_type_node )
                                                    : annotations_.resolve( param_type_node );
                annotations_.refuse_void( bare_type( param_type_node ), param_type, "parameter", "passes" );
                annotations_.refuse_never( bare_type( param_type_node ), param_type, "parameter" );
                types_.record( param, param_type );

                if( ast_.is_generic( child ) && !ast_.own_type_param_list( member ).is_valid() )
                {
                    generic_recursion_.record_generic_uses( child, param_type, ast_.span( param_type_node ) );
                }
            }
        }
    }
}

Node_id Signatures::bare_type( Node_id annotation ) const
{
    const Node_id spelled = ast_.unwrap_const( annotation );

    return ast_.kind( spelled ) == Node_kind::Mode_type ? ast_.inner_type( spelled ) : spelled;
}

void Signatures::declare_globals()
{
    for( Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( is_aggregate( ast_.kind( child ) ) )
        {
            for( Node_id member : ast_.members( child ) )
            {
                if( ast_.kind( member ) != Node_kind::Var_decl )
                {
                    continue;
                }

                const Node_id var_type_node = ast_.annotation( member );
                const Type_id var_type      = annotations_.resolve( var_type_node );
                annotations_.refuse_void( var_type_node, var_type, "global", "holds" );
                annotations_.refuse_never( var_type_node, var_type, "global" );
                types_.record( member, var_type );
            }
        }

        if( ast_.kind( child ) != Node_kind::Var_decl )
        {
            continue;
        }

        const Node_id var_type_node = ast_.annotation( child );
        const Type_id var_type      = annotations_.resolve( var_type_node );
        annotations_.refuse_void( var_type_node, var_type, "global", "holds" );
        annotations_.refuse_never( var_type_node, var_type, "global" );
        types_.record( child, var_type );
    }
}

void Signatures::declare_enums()
{
    for( Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Enum_decl )
        {
            continue;
        }

        const std::string_view name = interner_.text( ast_.name( child ) );

        declare_parameters( ast_.type_param_list( child ) );

        std::vector<Type_id> arguments;

        for( const Node_id type_param : ast_.type_parameters( ast_.type_param_list( child ) ) )
        {
            arguments.push_back( types_.type_of( type_param ) );
        }

        // The underlying type is invalid when unwritten. i32 by default, which is what
        // C++ gives a plain enum - D30 changed the semantics of the keyword, not its arithmetic.
        const Node_id annotation = ast_.underlying_type( child );
        Type_id       underlying = annotation.is_valid() ? annotations_.resolve( annotation ) : table_.integer( 32, true );

        // Only an integer can count variants. Absorbed to i32 so the rest of the pass has a type to
        // record; nothing downstream sees it, because a program with an error never reaches the emitter.
        if( !table_.is_integer( underlying ) && !table_.is_error( underlying ) )
        {
            reporter_.error_at(
                ast_.span( annotation ),
                fmt::format( "an `enum` must be backed by an integer, but `{}` is not one", table_.name( underlying ) )
            );

            underlying = table_.integer( 32, true );
        }

        const Type_id type = table_.enumeration( child, arguments, name, underlying );

        types_.record( child, type );

        // Reported per repeat rather than once, the same shape check_aggregate_members uses for a
        // field: three collisions should say so in one compile rather than over three.
        std::unordered_set<u32> seen;

        for( const Node_id variant : ast_.variants( child ) )
        {
            const Symbol_id variant_name = ast_.name( variant );

            if( !seen.insert( variant_name.v ).second )
            {
                reporter_.error_at(
                    ast_.span( variant ), fmt::format( "`{}` already has a variant `{}`", name, interner_.text( variant_name ) )
                );
            }
        }

        // The *enum* type, never the underlying one. D30's "no implicit conversion to an integer"
        // is enforced by this line and by the absence of a conversion rule for Enum anywhere else:
        // if a variant typed as i32, `i32 x = Colour::Red;` would compile and the entry would be a
        // comment rather than a rule.
        for( const Node_id variant : ast_.variants( child ) )
        {
            types_.record( variant, type );
        }
    }
}

// After every enum's type is recorded, since a payload may name an enum declared after its own.
void Signatures::declare_payloads()
{
    for( const Node_id child : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( child ) != Node_kind::Enum_decl )
        {
            continue;
        }

        for( const Node_id variant : ast_.variants( child ) )
        {
            const Symbol_id variant_name = ast_.name( variant );

            // D7: a variant's children are its payload fields, and they are Field_decls - so this
            // is the same recording declare_fields does for a struct.
            std::unordered_set<u32> fields;

            for( const Node_id field : ast_.payload( variant ) )
            {
                const Node_id field_annotation = ast_.annotation( field );
                const Type_id field_type       = annotations_.resolve( field_annotation );
                annotations_.refuse_void( field_annotation, field_type, "field", "holds" );
                annotations_.refuse_never( field_annotation, field_type, "field" );

                types_.record( field, field_type );

                if( ast_.is_generic( child ) )
                {
                    generic_recursion_.record_generic_uses( child, field_type, ast_.span( field ) );
                }

                const Symbol_id field_name = ast_.name( field );

                if( field_name.is_valid() && !fields.insert( field_name.v ).second )
                {
                    reporter_.error_at(
                        ast_.span( field ),
                        fmt::format(
                            "`{}` already has a field `{}`", interner_.text( variant_name ), interner_.text( field_name )
                        )
                    );
                }
            }
        }
    }
}

void Signatures::order_structs()
{
    // D42 stays quiet about an aggregate on a containment cycle: having no size is the more basic
    // half of one mistake.
    for( const Node_id node : aggregates_.order_structs() )
    {
        generic_recursion_.mark_reported( node );
    }
}

void Signatures::check_aggregate_members()
{
    for( const Node_id decl : ast_.declarations( ast_.root() ) )
    {
        if( !is_aggregate( ast_.kind( decl ) ) )
        {
            continue;
        }

        for( const Member_kind& kind : k_member_kinds )
        {
            check_member_kind( decl, kind );
        }
    }
}

void Signatures::check_member_kind( Node_id decl, const Member_kind& kind )
{
    const bool             on_a_struct = ast_.kind( decl ) == Node_kind::Struct_decl;
    const Symbol_id        type_name   = ast_.name( decl );
    const std::string_view type_text   = interner_.text( type_name );

    Node_id first {};

    for( const Node_id member : ast_.members( decl ) )
    {
        if( ast_.kind( member ) != kind.node )
        {
            continue;
        }

        // One mistake, one diagnostic: declaring one of these on a struct is a single decision to
        // reverse, so the name and duplicate rules stay quiet about it.
        if( on_a_struct )
        {
            reporter_.error_at(
                ast_.span( member ), fmt::format( "a struct cannot have a {}", kind.noun ), std::string( kind.remedy )
            );
            return;
        }

        if( first.is_valid() )
        {
            // A destructor takes no parameters, so a second one has nothing to tell it apart. A
            // constructor does, and Overloads::check_overload_sets asks whether they differ.
            if( kind.only_one )
            {
                reporter_.error_at(
                    ast_.span( member ),
                    fmt::format( "`{}` already has a {}", type_text, kind.noun ),
                    reporter_.previous_declaration_note( ast_.span( first ) )
                );
            }

            continue;
        }

        first = member;

        const Symbol_id written = ast_.name( member );

        if( written.is_valid() && written != type_name )
        {
            reporter_.error_at(
                ast_.span( member ),
                fmt::format( "`{}{}` does not name the enclosing type", kind.prefix, interner_.text( written ) ),
                fmt::format( "write `{}{}`", kind.prefix, type_text )
            );
        }
    }
}

// An interface's method is called through a table of the implementers' methods: so it has no body,
// and has no type parameters of its own.
void Signatures::check_signatures()
{
    for( const Node_id decl : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( decl ) != Node_kind::Interface_decl )
        {
            continue;
        }

        for( const Node_id member : ast_.members( decl ) )
        {
            if( ast_.kind( member ) != Node_kind::Method_decl )
            {
                continue;
            }

            if( ast_.body( member ).is_valid() )
            {
                reporter_.error_at(
                    ast_.name_span( member ),
                    fmt::format( "`{}` is a signature, so it cannot have a body", interner_.text( ast_.name( member ) ) ),
                    "write the body in each type that implements the interface"
                );
            }

            if( ast_.own_type_param_list( member ).is_valid() )
            {
                reporter_.error_at(
                    ast_.span( ast_.own_type_param_list( member ) ),
                    "an interface's method cannot have type parameters",
                    "a call through the interface reaches one compiled method, which cannot be every instance"
                );
            }
        }
    }
}

void Signatures::check_struct_ownership()
{
    for( const Node_id decl : ast_.declarations( ast_.root() ) )
    {
        // A struct with a destructor of its own is already reported by check_aggregate_members,
        // and it is one decision to reverse rather than two.
        if( ast_.kind( decl ) == Node_kind::Struct_decl && !ast_.has_destructor( decl ) )
        {
            check_struct_fields_are_not_owning( decl );
        }
    }
}

void Signatures::check_struct_fields_are_not_owning( Node_id decl )
{
    for( const Node_id field : ast_.members( decl ) )
    {
        if( ast_.kind( field ) != Node_kind::Field_decl )
        {
            continue;
        }

        const Type_id field_type = types_.type_of( field );

        if( !field_type.is_valid() || table_.is_error( field_type ) )
        {
            continue;
        }

        // Copyable rather than ownership directly: a field of type `T` has no declaration to
        // look up, and an unbounded one may turn out to own something - which is exactly what a
        // struct may not contain. The promise that rules it out is `copyable`.
        if( bounds_.satisfies( field_type, Bound::Copyable ) )
        {
            continue;
        }

        const Node_id field_decl = table_.get( field_type ).declaration;

        // One per field: each is a separate place the author has to change.
        reporter_.error_at(
            ast_.span( field ),
            fmt::format(
                "a struct cannot contain `{}`, which {}",
                table_.name( field_type ),
                table_.mentions_parameter( field_type )                      ? "may own a resource"
                : field_decl.is_valid() && ast_.has_destructor( field_decl ) ? "has a destructor"
                                                                             : "owns a resource"
            ),
            table_.is_parameter( field_type )
                ? fmt::format(
                      "a struct is copied freely, so promise it can be: `where {} : copyable`", table_.name( field_type )
                  )
                : fmt::format(
                      "a struct is copied freely, so declare `{}` as a class if it owns this",
                      interner_.text( ast_.name( decl ) )
                  )
        );
    }
}

void Signatures::check_operators()
{
    for( const Node_id decl : ast_.declarations( ast_.root() ) )
    {
        if( !has_members( ast_.kind( decl ) ) )
        {
            continue;
        }

        // One slot per name, and one more for `operator-()`, which shares `operator-( x )`'s.
        std::array<Node_id, static_cast<size_t>( Operator_name::Count ) + 1> first {};

        for( const Node_id member : ast_.members( decl ) )
        {
            const Symbol_id name = ast_.name( member );
            if( ast_.kind( member ) != Node_kind::Method_decl || !interner_.is_operator_name( name ) )
            {
                continue;
            }

            const Operator_name op       = static_cast<Operator_name>( name.v - Interner::operator_name( Operator_name {} ).v );
            const bool          negation = op == Operator_name::Minus && ast_.params( member ).size() == 1;
            Node_id& seen = first[negation ? static_cast<size_t>( Operator_name::Count ) : static_cast<size_t>( op )];

            if( seen.is_valid() )
            {
                reporter_.error_at(
                    ast_.name_span( member ),
                    fmt::format( "`{}` already has an `{}`", interner_.text( ast_.name( decl ) ), interner_.text( name ) ),
                    reporter_.previous_declaration_note( ast_.span( seen ) )
                );
                continue;
            }

            seen = member;

            // An operator call writes no type arguments, and its operands are its only arguments.
            if( ast_.own_type_param_list( member ).is_valid() )
            {
                reporter_.error_at(
                    ast_.span( ast_.own_type_param_list( member ) ),
                    "an operator cannot have type parameters of its own",
                    "it is called without any, so nothing could say what they are"
                );
                continue;
            }

            if( ast_.is_static_method( member ) )
            {
                reporter_.error_at(
                    ast_.name_span( member ), "an operator cannot be `static`", "it is called on its left operand"
                );
                continue;
            }

            // These compare with the receiver's type, which conformance checks for `self` and a block.
            const bool aggregate = is_aggregate( ast_.kind( decl ) );

            switch( op )
            {
            case Operator_name::Equal_equal:
                if( aggregate )
                {
                    check_comparison_operator( member, table_.builtin( Type_kind::Bool ) );
                }
                break;

            case Operator_name::Index:
                if( aggregate )
                {
                    check_index_operator( member );
                }
                break;

            case Operator_name::Compare:
                if( aggregate )
                {
                    check_comparison_operator( member, table_.declared( table_.prelude().ordering ) );
                }
                break;

            case Operator_name::Minus:
                if( ast_.params( member ).size() == 2 )
                {
                    check_binary_operator( member );
                }
                else
                {
                    check_unary_operator( member );
                }
                break;

            case Operator_name::Tilde:
                check_unary_operator( member );
                break;

            case Operator_name::Plus_equal:
            case Operator_name::Minus_equal:
            case Operator_name::Star_equal:
            case Operator_name::Slash_equal:
            case Operator_name::Percent_equal:
            case Operator_name::Amp_equal:
            case Operator_name::Pipe_equal:
            case Operator_name::Caret_equal:
                check_compound_operator( member );
                break;

            default:
                check_binary_operator( member );
                break;
            }
        }
    }
}

void Signatures::check_comparison_operator( Node_id decl, Type_id result )
{
    const std::span<const Node_id> params    = ast_.params( decl );
    const Type_id                  receiver  = types_.type_of( params[0] );
    const Type_id                  self_type = table_.is_pointer( receiver ) ? table_.get( receiver ).element : receiver;

    const std::string_view op_name          = interner_.text( ast_.name( decl ) );
    const std::string_view return_type_name = table_.name( result );

    if( params.size() != 2 )
    {
        reporter_.error_at(
            ast_.span( ast_.param_list( decl ) ), fmt::format( "`{}` takes one parameter, the right-hand operand", op_name )
        );
    }
    else if( !table_.is_error( types_.type_of( params[1] ) ) && types_.type_of( params[1] ) != self_type )
    {
        reporter_.error_at(
            ast_.span( params[1] ),
            fmt::format( "`{0}` on `{1}` must take another `{1}`", op_name, table_.name( self_type ) ),
            fmt::format( "write the parameter as `const ref {}`", table_.name( self_type ) )
        );
    }

    if( !table_.is_error( types_.type_of( decl ) ) && types_.type_of( decl ) != result )
    {
        reporter_.error_at(
            ast_.span( ast_.return_type( decl ) ), fmt::format( "`{}` must return `{}`", op_name, return_type_name )
        );
    }

    if( !ast_.is_const_method( decl ) )
    {
        reporter_.error_at(
            ast_.name_span( decl ),
            fmt::format( "`{}` must be `const`", op_name ),
            "write `const` after its parameter list; comparing reads both operands"
        );
    }
}

// The operand of a binary or compound operator: exactly one, passed by value or `const ref`.
void Signatures::check_operand( Node_id decl )
{
    const std::span<const Node_id> params  = ast_.params( decl );
    const std::string_view         op_name = interner_.text( ast_.name( decl ) );

    if( params.size() != 2 )
    {
        reporter_.error_at(
            ast_.span( ast_.param_list( decl ) ), fmt::format( "`{}` takes one parameter, the right-hand operand", op_name )
        );
    }
    else if( ast_.call_marker( params[1] ) != Keyword::Count )
    {
        reporter_.error_at(
            ast_.span( params[1] ),
            fmt::format( "`{}` takes its right operand by value or `const ref`", op_name ),
            fmt::format( "`a {} b` has nowhere to write a `ref`, `out` or `move`", operator_symbol( decl ) )
        );
    }
}

std::string_view Signatures::operator_symbol( Node_id decl ) const
{
    return interner_.text( ast_.name( decl ) ).substr( std::string_view( "operator" ).size() );
}

bool Signatures::returns_void( Node_id decl ) const
{
    return types_.type_of( decl ) == table_.builtin( Type_kind::Void );
}

void Signatures::check_binary_operator( Node_id decl )
{
    const std::string_view op_name = interner_.text( ast_.name( decl ) );
    const std::string_view sym     = operator_symbol( decl );

    check_operand( decl );

    if( returns_void( decl ) )
    {
        reporter_.error_at(
            ast_.span( ast_.return_type( decl ) ),
            fmt::format( "`{}` must return a value", op_name ),
            fmt::format( "`a {0} b` is an expression; to change `a` in place, declare `operator{0}=`", sym )
        );
    }

    if( !ast_.is_const_method( decl ) )
    {
        reporter_.error_at(
            ast_.name_span( decl ),
            fmt::format( "`{}` must be `const`", op_name ),
            fmt::format( "write `const` after its parameter list; `a {} b` reads both operands", sym )
        );
    }
}

void Signatures::check_compound_operator( Node_id decl )
{
    const std::string_view op_name = interner_.text( ast_.name( decl ) );
    const Type_id          result  = types_.type_of( decl );

    check_operand( decl );

    if( !table_.is_error( result ) && !returns_void( decl ) )
    {
        reporter_.error_at(
            ast_.span( ast_.return_type( decl ) ),
            fmt::format( "`{}` must return `void`", op_name ),
            fmt::format( "`a {} b` is a statement and changes `a` in place", operator_symbol( decl ) )
        );
    }

    if( ast_.is_const_method( decl ) )
    {
        reporter_.error_at(
            ast_.name_span( decl ), fmt::format( "`{}` cannot be `const`", op_name ), "it changes its left operand"
        );
    }
}

void Signatures::check_unary_operator( Node_id decl )
{
    const std::span<const Node_id> params  = ast_.params( decl );
    const std::string_view         op_name = interner_.text( ast_.name( decl ) );
    const std::string_view         sym     = operator_symbol( decl );

    if( sym == "-" && params.size() > 2 )
    {
        reporter_.error_at(
            ast_.span( ast_.param_list( decl ) ), "`operator-` takes no parameter, for `-a`, or one, for `a - b`"
        );
    }
    else if( sym == "~" && params.size() > 1 )
    {
        reporter_.error_at(
            ast_.span( ast_.param_list( decl ) ), "`operator~` takes no parameter", "`~a` has one operand, the object"
        );
    }

    if( returns_void( decl ) )
    {
        reporter_.error_at( ast_.span( ast_.return_type( decl ) ), fmt::format( "`{}` must return a value", op_name ) );
    }

    if( !ast_.is_const_method( decl ) )
    {
        reporter_.error_at(
            ast_.name_span( decl ),
            fmt::format( "`{}` must be `const`", op_name ),
            fmt::format( "write `const` after its parameter list; `{}a` reads its operand", sym )
        );
    }
}

void Signatures::check_index_operator( Node_id decl )
{
    const std::span<const Node_id> params = ast_.params( decl );

    if( params.size() != 2 )
    {
        reporter_.error_at( ast_.span( ast_.param_list( decl ) ), "`operator[]` takes one parameter, the index" );
    }
    else if( ast_.call_marker( params[1] ) != Keyword::Count )
    {
        reporter_.error_at(
            ast_.span( params[1] ),
            "`operator[]` takes its index by value or `const ref`",
            "`v[i]` has nowhere to write a `ref`, `out` or `move`"
        );
    }

    const Node_id annotation  = ast_.return_type( decl );
    const Type_id return_type = types_.type_of( decl );

    if( ast_.kind( ast_.unwrap_const( annotation ) ) == Node_kind::Mode_type )
    {
        reporter_.error_at(
            ast_.span( annotation ),
            "`operator[]` returns a pointer, not a reference",
            "`v[i]` is the element it points at; a reference would outlive the expression"
        );
    }
    else if( !table_.is_error( return_type ) && !table_.is_pointer( return_type ) )
    {
        reporter_.error_at(
            ast_.span( annotation ),
            "`operator[]` must return a pointer to the element",
            fmt::format( "return `{}*`, and `v[i]` is the element it points at", table_.name( return_type ) )
        );
    }
    else if( !table_.is_error( return_type ) && table_.get( return_type ).element == table_.builtin( Type_kind::Void ) )
    {
        reporter_.error_at(
            ast_.span( annotation ), "`operator[]` must return a pointer to the element", "a `void*` points at no element"
        );
    }

    if( !ast_.is_const_method( decl ) )
    {
        reporter_.error_at(
            ast_.name_span( decl ),
            "`operator[]` must be `const`",
            "write `const` after its parameter list; a `const` object is indexed too"
        );
    }
}

} // namespace keel::sema

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

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

    // The unknown type is reported by Annotations::resolve; saying `main` must return i32 on top of
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

    // `Box` holds its `T` by value, so a `Ring` in `Box`'s argument is inside `Ring`.
    SECTION( "through a type argument held by value" )
    {
        const Typed p( "struct Box<T> where T : copyable { T v; };\n"
                       "struct Ring<T> where T : copyable { Box<Ring<T>> link; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Ring` contains itself, so it has no size" ) != std::string::npos );
        REQUIRE( p.rendered().find( "Ring -> Box -> Ring" ) != std::string::npos );
    }

    SECTION( "and through a generic that holds it only through another" )
    {
        const Typed p( "struct Box<T> where T : copyable { T v; };\n"
                       "struct Wrap<T> where T : copyable { Box<T> b; };\n"
                       "struct Loop { Wrap<Loop> w; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "Loop -> Wrap -> Loop" ) != std::string::npos );
    }

    // `Pair`'s first argument holds nothing of `Loop`; asking about it must not hide the second.
    SECTION( "and through the second of two parameters" )
    {
        const Typed p( "struct Pair<A, B> where A : copyable, where B : copyable { A a; B b; };\n"
                       "struct Wrap<T> where T : copyable { Pair<i32, T> p; };\n"
                       "struct Loop { Wrap<Loop> w; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "Loop -> Wrap -> Loop" ) != std::string::npos );
    }

    // An instance inside its own argument is a finite type, not a return to a struct being expanded.
    SECTION( "but a generic nested in itself is not a cycle" )
    {
        const Typed p( "struct Box<T> where T : copyable { T v; };\n"
                       "struct Nested { Box<Box<i32>> b; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A tree holding its children through a buffer is the shape `Vector` exists for.
    SECTION( "but an argument held through a pointer is not a cycle" )
    {
        const Typed p( "struct Many<T> where T : copyable { T[*] items; };\n"
                       "struct Hold<T> where T : copyable { T* one; };\n"
                       "struct Tree { Many<Tree> children; Hold<Tree> parent; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// `main` is the one name the backend also generates a shim for, so an extern one would have the
// shim call the symbol it is standing in for - a silent infinite recursion rather than a link error.
TEST_CASE( "type_checker_rejects_an_extern_main", "[sema][types][extern]" )
{
    const Typed p( "extern i32 main();" );

    INFO( p.rendered() );
    REQUIRE_FALSE( p.clean() );
    REQUIRE( p.rendered().find( "`extern`" ) != std::string::npos );
}

// The predicate three passes share. Asked of the wrong kind it must answer false rather than
// assert: lower() puts every function-like node through it, destructors and constructors included.
TEST_CASE( "type_checker_is_extern_reads_the_absent_body", "[sema][types][extern]" )
{
    const Typed p( "extern i32 abs( i32 v );\n"
                   "class C { i32 x; C( i32 v ) { x = v; } ~C() { } i32 get() { return x; } };\n"
                   "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    REQUIRE( p.ast().is_extern( p.nth( Node_kind::Function_decl, 0 ) ) );       // the extern
    REQUIRE_FALSE( p.ast().is_extern( p.nth( Node_kind::Function_decl, 1 ) ) ); // main
    REQUIRE_FALSE( p.ast().is_extern( p.nth( Node_kind::Constructor_decl, 0 ) ) );
    REQUIRE_FALSE( p.ast().is_extern( p.nth( Node_kind::Destructor_decl, 0 ) ) );
    REQUIRE_FALSE( p.ast().is_extern( p.nth( Node_kind::Method_decl, 0 ) ) );
    REQUIRE_FALSE( p.ast().is_extern( p.nth( Node_kind::Class_decl, 0 ) ) );
}

// D29: a class is built by a constructor and a struct from a literal, which is what stops the two
// initialisation syntaxes competing. The parser accepts one on either kind so the message can name
// the fix rather than being a syntax error.
TEST_CASE( "type_checker_checks_constructor_declarations", "[sema][aggregates]" )
{
    SECTION( "a struct with a constructor is rejected" )
    {
        const Typed p( "struct Point { i32 x; Point( i32 a ) { x = a; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a class with one is accepted" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a name that does not match the enclosing type is rejected" )
    {
        const Typed p( "class Buffer { u64 len; Wrong( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    // One mistake, one diagnostic: declaring a constructor on a struct is a single decision to
    // reverse, so a second one says nothing new.
    SECTION( "a struct with two constructors is still one mistake" )
    {
        const Typed p( "struct Point { i32 x; Point( i32 a ) { x = a; } Point( u64 b ) { x = 1; } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "two constructors that differ coexist" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { } Buffer( i32 m ) { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "two constructors with the same parameters are rejected once" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { } Buffer( u64 m ) { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "already declared with these parameters" ) != std::string::npos );
    }

    SECTION( "a constructor beside a destructor is fine" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the body is type checked" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a constructor returns nothing" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { return 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// The field's type is what is under the `const`.
TEST_CASE( "type_checker_accepts_a_const_field", "[sema][const][field]" )
{
    const Typed p( "struct P { const i32 x; const u8[*] const s; };\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 0 ) ) == "i32" );
    REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 1 ) ) == "const u8[*]" );
}

// Assigning an `out` parameter destroys nothing, so an owning one would leak whatever the caller
// was already holding. Refused until the caller emits a drop before the call.
TEST_CASE( "type_checker_refuses_an_owning_out_parameter", "[sema][out]" )
{
    const Typed p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                   "void init( out B b ) { }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE_FALSE( p.clean() );
    REQUIRE( p.rendered().find( "owns a resource" ) != std::string::npos );
}

// PLAN D30. An enum is a type of its own, and a variant has that type - never the underlying
// integer. That one recording is what makes every rejection below fall out of rules that already
// existed rather than needing new ones.
TEST_CASE( "type_checker_types_an_enum_and_its_variants", "[sema][enum]" )
{
    const Typed p( "enum Colour : u8 { Red, Green, Blue };\ni32 main() { Colour c = Colour::Green; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.type_name( p.nth( Node_kind::Enum_decl, 0 ) ) == "Colour" );
    REQUIRE( p.type_name( p.nth( Node_kind::Variant_decl, 0 ) ) == "Colour" );
    REQUIRE( p.type_name( p.nth( Node_kind::Path_expr, 0 ) ) == "Colour" );
}

TEST_CASE( "type_checker_checks_an_enum_declaration", "[sema][enum]" )
{
    SECTION( "the underlying type defaults to i32" )
    {
        const Typed p( "enum Colour { Red };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and must be an integer when written" )
    {
        const Typed p( "enum Colour : bool { Red };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must be backed by an integer" ) != std::string::npos );
    }

    // As anywhere outside an interface; nothing is said about integers on top.
    SECTION( "and is not `self`" )
    {
        const Typed p( "enum Colour : self { Red };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`self` is written only inside an `interface`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "backed by an integer" ) == std::string::npos );
    }

    SECTION( "duplicate variants are refused, once each" )
    {
        const Typed p( "enum Colour { Red, Green, Red, Red };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "already has a variant `Red`" ) != std::string::npos );
    }
}

// Slice 1a of M6: the shape parses and the resolver scopes the parameters, and sema says once that
// nothing further is built yet. Reporting once matters - typing the body would report an unknown
// type for every use of `T`, turning one unimplemented feature into five errors.
// A generic's signature is typed with `T` standing for itself, so the parameters resolve like any
// other named type and nothing in the signature reports as unknown. The body is not checked yet -
// that is D11's definition-checking, and until it lands an instance is checked at its expansion.
// The order of the eleven steps in the declaration pass is this class's own fact. This is the
// edge nothing else rests on: an enum is a type that a field, a parameter, a return type or a
// global may name, so it is declared before any of them resolves. Out of order the annotation
// silently resolves to nothing - no diagnostic at all, and the crash lands in codegen.
TEST_CASE( "type_checker_declares_an_enum_before_anything_that_can_name_one", "[sema][enum][types]" )
{
    SECTION( "a field" )
    {
        const Typed p( "enum Colour { Red, Green };\n"
                       "struct Holder { Colour c; };\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 0 ) ) == "Colour" );
    }

    SECTION( "a method's return type" )
    {
        const Typed p( "enum Colour { Red, Green };\n"
                       "class Box { i32 v; Colour tint() { return Colour::Red; } };\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Method_decl, 0 ) ) == "Colour" );
    }

    SECTION( "a method's parameter" )
    {
        const Typed p( "enum Colour { Red, Green };\n"
                       "class Box { i32 v; void paint( Colour c ) { v = 1; } };\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 1 ) ) == "Colour" );
    }

    SECTION( "a global" )
    {
        // Uninitialised: a variant is not a constant expression, which is a different rule.
        const Typed p( "enum Colour { Red, Green };\n"
                       "Colour background;\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 0 ) ) == "Colour" );
    }

    SECTION( "another enum's payload, declared before it" )
    {
        const Typed p( "enum Shape { Dot, Filled( Colour c ) };\n"
                       "enum Colour { Red, Green };\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 0 ) ) == "Colour" );
    }

    SECTION( "another enum's payload, through a type argument" )
    {
        const Typed p( "enum Shape { Dot, Filled( Wrap<Colour> c ) };\n"
                       "enum Wrap<T> where T : copyable { Held( T v ) };\n"
                       "enum Colour { Red, Green };\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 0 ) ) == "Wrap<Colour>" );
    }
}

TEST_CASE( "type_checker_types_a_generic_signature", "[sema][generic]" )
{
    SECTION( "the declaration alone is accepted" )
    {
        const Typed p( "T id<T>( T a ) where T : copyable { return a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the type parameters are not unknown types" )
    {
        const Typed p( "T twice<T>( T a, T b ) { T c = a; return c; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "unknown type" ) == std::string::npos );
    }

    SECTION( "a broken non-generic beside it is still checked" )
    {
        const Typed p( "T id<T>( T a ) where T : copyable { return a; }\n"
                       "i32 broken() { return true; }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and a plain program is untouched" )
    {
        const Typed p( "i32 f( i32 a ) { return a; }\ni32 main() { return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D26 meeting a type parameter. A literal has a value rather than a type, and context supplies one -
// but a bound supplies a *set* of types, so the question becomes whether the value fits every type
// `T` may turn out to be. Answering it here rather than at an instantiation is what keeps D11 whole:
// no later call site can break a body that already compiled.
// Monomorphisation emits one function per set of type arguments, so it terminates only if that set
// is finite. Forwarding a parameter keeps it finite however deep the recursion runs; building a type
// out of it does not, and the shape is decidable at the definition rather than at whichever
// expansion happens to hit a limit.
// A type parameter or a `where` subject whose name failed to parse must not become a node. The
// parser reports either way; what a nameless node costs is sema, which reads that name to report
// about it and hands an invalid one to the interner - which asserts, so the compiler dies on a file
// it had already diagnosed. Nine spellings, two nodes, and the same reading parse_aggregate_decl
// takes of a declaration with no name.
TEST_CASE( "type_checker_survives_a_nameless_type_parameter_or_clause", "[sema][generic]" )
{
    const auto diagnosed = []( std::string_view source )
    {
        const Typed p( std::string( source ) + "\ni32 main() { return 0; }" );

        // Reaching here at all is the assertion: the failure mode is an abort, not a wrong answer.
        return !p.clean();
    };

    SECTION( "a malformed type parameter list" )
    {
        REQUIRE( diagnosed( "T f<T,>( T a ) { return a; }" ) );
        REQUIRE( diagnosed( "T f<,T>( T a ) { return a; }" ) );
        REQUIRE( diagnosed( "T f<T,,U>( T a ) { return a; }" ) );
        REQUIRE( diagnosed( "T f<if>( T a ) { return a; }" ) );
    }

    SECTION( "a `where` clause with no subject" )
    {
        REQUIRE( diagnosed( "T f<T>( T a ) where { return a; }" ) );
        REQUIRE( diagnosed( "T f<T>( T a ) where : copyable { return a; }" ) );
        REQUIRE( diagnosed( "T f<T>( T a ) where where T : copyable { return a; }" ) );
        REQUIRE( diagnosed( "T f<T>( T a ) where 5 : copyable { return a; }" ) );
        REQUIRE( diagnosed( "T f<T>( T a ) where if : copyable { return a; }" ) );
    }

    SECTION( "a well-formed one still works" )
    {
        // The guard drops the node rather than the feature.
        const Typed p( "T f<T>( T a ) where T : copyable { return a; }\ni32 main() { return f<i32>( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Self-containment is about size, and size is about by-value members - so the existing walk answers
// it for a generic aggregate unchanged, including the shape that grows a type each time round.
TEST_CASE( "type_checker_refuses_a_generic_aggregate_that_contains_itself", "[sema][generic][aggregate]" )
{
    SECTION( "directly" )
    {
        const Typed p( "struct Odd<T> where T : copyable { Odd<T> inner; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Odd` contains itself" ) != std::string::npos );
    }

    SECTION( "through a type built from its own parameter" )
    {
        // Infinite twice over: no size, and no finite set of instantiations either. The size rule
        // catches it first, which is the same answer for a stronger reason.
        const Typed p( "struct Box<T> where T : copyable { T v; };\n"
                       "struct Odd<T> where T : copyable { Odd<Box<T>> inner; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Odd` contains itself" ) != std::string::npos );

        // And only that one: two messages for one mistake sends the author round twice.
        REQUIRE( p.rendered().find( "bigger type argument" ) == std::string::npos );
    }

    SECTION( "a pointer to itself is finite, and is how a list is written" )
    {
        const Typed p( "class Node<T> where T : copyable { Node<T>* next; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "holding a different instantiation is ordinary containment" )
    {
        // A generic holding a *concrete* instantiation of another is a finite thing, unlike the
        // case above: `Holder<i32>` needs `Box<i32>` and `Box<i32>` needs nothing back.
        const Typed p( "struct Box<T> where T : copyable { T v; };\n"
                       "struct Holder<T> where T : copyable { Box<i32> b; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_scopes_type_parameters", "[sema][generic]" )
{
    SECTION( "a duplicate is reported" )
    {
        const Typed p( "T id<T, T>( T a ) { return a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "already declared" ) != std::string::npos );
    }

    SECTION( "they do not escape the declaration" )
    {
        const Typed p( "T id<T>( T a ) where T : copyable { return a; }\nT stray( T a ) { return a; }\ni32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "unknown type `T`" ) != std::string::npos );
    }

    SECTION( "two declarations may reuse the same parameter name" )
    {
        const Typed p( "T one<T>( T a ) where T : copyable { return a; }\n"
                       "T two<T>( T a ) where T : copyable { return a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );

        // Each declaration's `T` is its own type, keyed by its own Type_param_decl.
        REQUIRE( p.clean() );
    }
}

// PLAN §6.7. An aggregate with no fields is refused by the parser, which alone can tell one from
// fields it left out; these are the alternatives that refusal recommends, and they must stay legal.
TEST_CASE( "type_checker_accepts_what_replaces_an_empty_aggregate", "[sema][types]" )
{
    // The alternative the diagnostic names. Conflating "no fields" with "no data" would reject the
    // fix the message recommends, so this is the section that pins the rule's edge.
    SECTION( "an enum with one variant is accepted" )
    {
        const Typed p( "enum Unit { Only };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a payload-free variant is accepted beside one that carries data" )
    {
        const Typed p( "enum Shape { Nothing, Circle( f64 radius ) };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "one field is enough" )
    {
        const Typed p( "struct One { i32 x; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D29 draws the struct/class line at trivial copyability, and a destructor is what breaks it: copy
// plus destructor is a double free. The parser accepts one on either kind so that the message can
// name `class` as the fix rather than being a syntax error.
TEST_CASE( "type_checker_rejects_a_destructor_on_a_struct", "[sema][aggregates]" )
{
    SECTION( "a struct with a destructor is rejected" )
    {
        const Typed p( "struct Point { i32 x; ~Point() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "cannot have a destructor" ) != std::string::npos );
    }

    // One mistake, one diagnostic - the rest of the declaration is well formed and must not be
    // re-reported as a consequence of the destructor.
    SECTION( "and reported once" )
    {
        const Typed p( "struct Point { i32 x; i32 y; ~Point() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a class with one is accepted" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a class without one is accepted" )
    {
        const Typed p( "class Handle { u64 value; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// aux carries what was written after the `~`, so a mismatch is a comparison rather than a parse
// failure - which is what lets the message name the type that was meant.
TEST_CASE( "type_checker_checks_destructor_names", "[sema][aggregates]" )
{
    SECTION( "a name that does not match the enclosing type is rejected" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Wrong() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a matching name is accepted" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The resolver's duplicate-member check skips everything that is not a Field_decl, so a second
    // destructor reaches here unreported and needs its own rule.
    SECTION( "two destructors are rejected once" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }
}

// D2: a type is owning exactly when it has a destructor, directly or through a by-value member.
TEST_CASE( "type_checker_computes_the_owning_query", "[sema][aggregates][owning]" )
{
    const auto owns = []( Typed& p, Type_id type )
    { return instance_owns( p.ast(), p.types().table(), type, p.types().recorded() ); };
    const auto owning = [&]( Typed& p, std::size_t nth_decl )
    { return owns( p, p.types().type_of( p.nth( Node_kind::Class_decl, nth_decl ) ) ); };

    SECTION( "a class with a destructor owns; one without does not" )
    {
        Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                 "class Handle { u64 value; };\n"
                 "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( owning( p, 0 ) );
        REQUIRE_FALSE( owning( p, 1 ) );
    }

    SECTION( "a struct never owns - it cannot hold anything that does" )
    {
        Typed p( "struct Point { i32 x; i32 y; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE_FALSE( owns( p, p.types().type_of( p.nth( Node_kind::Struct_decl, 0 ) ) ) );
    }

    // The transitivity is forced rather than chosen: destroying a Wrapper destroys its Buffer, so
    // there is no way for it not to own.
    SECTION( "owning is transitive through a by-value member" )
    {
        Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                 "class Wrapper { Buffer inner; };\n"
                 "class Outer { Wrapper w; };\n"
                 "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( owning( p, 1 ) );
        REQUIRE( owning( p, 2 ) );
    }

    // An address says nothing about who frees it, so a pointer breaks the chain.
    SECTION( "a pointer to an owning type does not own" )
    {
        Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                 "class Holder { Buffer* p; };\n"
                 "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE_FALSE( owning( p, 1 ) );
    }

    SECTION( "a builtin never owns" )
    {
        Typed p( "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( owns( p, p.types().table().integer( 32, true ) ) );
    }
}

// The rule that makes `struct` mean something: it is legal exactly when D2's owning query says no,
// so the check costs one call rather than any new machinery.
TEST_CASE( "type_checker_rejects_an_owning_member_in_a_struct", "[sema][aggregates][owning]" )
{
    SECTION( "a struct holding a class with a destructor is rejected" )
    {
        const Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer b; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "transitively, through a class that only contains one" )
    {
        const Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                       "class Wrapper { Buffer inner; };\n"
                       "struct Holder { Wrapper w; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a pointer to one is fine - it owns nothing" )
    {
        const Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer* p; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A destructor is a child of the declaration alongside the fields, so anything counting
    // children counts it too - which demanded an extra initialiser and misaligned every positional
    // one after it. The literal form on a class is D29's temporary exception until constructors
    // exist, so it is exactly the path with no other coverage.
    SECTION( "a destructor is not counted as a field by a struct literal" )
    {
        const Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                       "i32 main() { Buffer b = Buffer { nullptr }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a genuinely wrong count still reports the field count, not the member count" )
    {
        const Typed p( "class Buffer { public u8* ptr; public u64 len; ~Buffer() { } };\n"
                       "i32 main() { Buffer b = Buffer { nullptr }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "has 2 fields" ) != std::string::npos );
    }

    SECTION( "a class holding one is fine" )
    {
        const Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                       "class Wrapper { Buffer inner; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a struct holding a non-owning class is fine" )
    {
        const Typed p( "class Handle { u64 value; };\n"
                       "struct Holder { Handle h; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A struct that declares a destructor is already reported for that, and its owning fields
    // are the same decision to reverse rather than a second one.
    SECTION( "a struct with a destructor of its own is reported once" )
    {
        const Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer b; ~Holder() { } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Every field is reported, because each is a separate place the author has to change.
    SECTION( "two owning fields are two diagnostics" )
    {
        const Typed p( "class Buffer { public u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer a; Buffer b; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }
}

// The destructor body is typed like any function body. `this` needs no rule of its own: it is a
// parameter with an annotation, so the ordinary path types it.
TEST_CASE( "type_checker_types_a_destructor_body", "[sema][types][aggregates]" )
{
    SECTION( "a bare field has the field's type" )
    {
        const Typed p( "class Buffer { u8* ptr; u64 len; ~Buffer() { len = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Name_expr, 0 ) ) == "u64" );
    }

    SECTION( "`this` is a reference to the enclosing type" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { this.ptr = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Name_expr, 0 ) ) == "Buffer" );
    }

    // D22 already makes `.` reach through a pointer, so `this.ptr` needs no rule either.
    SECTION( "`this.field` has the field's type" )
    {
        const Typed p( "class Buffer { u64 len; ~Buffer() { this.len = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 0 ) ) == "u64" );
    }

    SECTION( "a field's type is still checked against what is assigned" )
    {
        const Typed p( "class Buffer { u64 len; ~Buffer() { len = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // A class is written through its fields, so its `this` is never assigned whole.
    SECTION( "`this` cannot be assigned" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { this = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`this` cannot be assigned whole in a `class`" ) != std::string::npos );
    }

    SECTION( "a destructor returns nothing" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { return 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a bare `return` is fine" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { return; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "calling a free function from a destructor works" )
    {
        const Typed p( "void release( u8* p ) { }\n"
                       "class Buffer { u8* ptr; ~Buffer() { release( ptr ); } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D2 counts only by-value containment, so what a generic aggregate owns is a question about the
// *field's* type rather than about the declaration - and an unbounded `T` may turn out to own
// something. Nothing new was needed for any of this; it falls out of rules already written.
TEST_CASE( "type_checker_applies_the_owning_rules_to_a_generic_aggregate", "[sema][generic][aggregate]" )
{
    SECTION( "a struct may not hold a `T` that might own something" )
    {
        const Typed p( "struct Pair<T> { T a; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a struct cannot contain `T`, which may own a resource" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`where T : copyable`" ) != std::string::npos );
    }

    SECTION( "the promise that rules it out is `copyable`" )
    {
        const Typed p( "struct Pair<T> where T : copyable { T a; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a class may hold one, because a class is not copied" )
    {
        const Typed p( "class Box<T> { T a; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a pointer to one owns nothing, whatever `T` is" )
    {
        // Only by-value containment counts, which is the existing rule and needed no generic case.
        const Typed p( "struct Ref<T> { T* a; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Ownership is a question about the *instantiation*, not the declaration: `Box<i32>` and
// `Box<Buf>` come from one class and one of them owns.
TEST_CASE( "type_checker_answers_ownership_per_instantiation", "[sema][generic][aggregate]" )
{
    const std::string_view owner = "class Buf\n"
                                   "{\n"
                                   "    i32* p;\n"
                                   "    Buf() { unsafe { p = alloc<i32>(); } }\n"
                                   "    ~Buf() { unsafe { free( p ); } }\n"
                                   "};\n"
                                   "class Box<T> { public T v; };\n";

    SECTION( "an argument that owns something makes the instance own" )
    {
        Typed p( std::string( owner ) + "i32 main() { Box<Buf> b; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Type_id instance = p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE( instance_owns( p.ast(), p.types().table(), instance, p.types().recorded() ) );
    }

    SECTION( "the same declaration at an argument that does not" )
    {
        Typed p( std::string( owner ) + "i32 main() { Box<i32> b; b.v = 1; return b.v; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Type_id instance = p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE_FALSE( instance_owns( p.ast(), p.types().table(), instance, p.types().recorded() ) );
    }

    SECTION( "a pointer to an owning type owns nothing" )
    {
        // D2 counts only by-value containment, here as everywhere else.
        Typed p(
            std::string( owner ) + "class Ref<T> { T* v; };\n"
                                   "i32 main() { Ref<Buf> r; return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Type_id instance = p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE_FALSE( instance_owns( p.ast(), p.types().table(), instance, p.types().recorded() ) );
    }

    SECTION( "a destructor of its own makes every instance own" )
    {
        Typed p( "class Held<T> { T v; ~Held() { } };\n"
                 "i32 main() { Held<i32> h; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Type_id instance = p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE( instance_owns( p.ast(), p.types().table(), instance, p.types().recorded() ) );
    }
}

// D30. An enum owns exactly when a payload does - through any variant, and per instance.
TEST_CASE( "instance_owns_an_enum_through_its_payload", "[sema][payload][move]" )
{
    constexpr std::string_view owning = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n";

    const auto owns = []( Typed& p, Type_id type )
    { return instance_owns( p.ast(), p.types().table(), type, p.types().recorded() ); };

    SECTION( "a payload that owns makes the enum own" )
    {
        Typed p( std::string( owning ) + "enum H { Empty, Full( B value ) };\ni32 main() { H h = H::Empty; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( owns( p, p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) ) ) );
    }

    SECTION( "copyable payloads do not" )
    {
        Typed p( "enum Shape { Circle( f64 r ), Dot };\ni32 main() { Shape s = Shape::Dot; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE_FALSE( owns( p, p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) ) ) );
    }

    SECTION( "a generic one owns at the instances whose argument does" )
    {
        Typed p(
            std::string( owning ) + "enum Option<T> { Some( T value ), None };\n"
                                    "i32 main() { Option<B> a = Option::None; Option<i32> b = Option::None; return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( owns( p, p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) ) ) );
        REQUIRE_FALSE( owns( p, p.types().type_of( p.nth( Node_kind::Var_decl, 1 ) ) ) );
    }

    SECTION( "so a struct may not hold one" )
    {
        Typed p( std::string( owning ) + "enum H { Empty, Full( B value ) };\nstruct S { H h; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a struct cannot contain `H`, which owns a resource" ) != std::string::npos );
    }
}

// D7. A pattern binding borrows its payload in place, and the enum still owns it - so nothing takes
// it out: not a copy, not a `move`, not a `return`.
TEST_CASE( "type_checker_keeps_an_owning_payload_in_its_enum", "[sema][payload][move]" )
{
    constexpr std::string_view held = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                                      "enum H { Full( B b ), Empty };\n";

    const auto arm = [held]( std::string_view body )
    {
        return std::string( held ) + "B f( H h ) { switch( h ) { case H::Full( b ): " + std::string( body ) +
               " case H::Empty: return B( 0 ); } }\ni32 main() { return 0; }";
    };

    SECTION( "a copy" )
    {
        const Typed p( arm( "B c = b; return B( 1 );" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` still belongs to what holds it" ) != std::string::npos );
    }

    SECTION( "a move" )
    {
        const Typed p( arm( "B c = move b; return B( 1 );" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a binding cannot be moved" ) != std::string::npos );
    }

    SECTION( "a return" )
    {
        const Typed p( arm( "return b;" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` still belongs to what holds it" ) != std::string::npos );
    }

    SECTION( "but reading through it is fine" )
    {
        const Typed p( arm( "return B( b.n );" ) );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and the variant takes a named value only by `move`" )
    {
        const Typed p( std::string( held ) + "H g() { B b = B( 1 ); return H::Full( b ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "write `move b`" ) != std::string::npos );
    }
}

// D7. A `switch` over `move h` takes the value, so each binding owns its payload as a local owns its
// value, and nothing is left to hold.
TEST_CASE( "type_checker_lets_a_consuming_switch_take_its_payload", "[sema][payload][move]" )
{
    constexpr std::string_view held = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                                      "enum H { Full( B b ), Empty };\n"
                                      "H make() { return H::Empty; }\n";

    const auto arm = [held]( std::string_view body, std::string_view scrutinee = "move h" )
    {
        return std::string( held ) + "B f( move H h ) { switch( " + std::string( scrutinee ) +
               " ) { case H::Full( b ): " + std::string( body ) +
               " case H::Empty: return B( 0 ); } }\ni32 main() { return 0; }";
    };

    SECTION( "returned as a local is" )
    {
        const Typed p( arm( "return b;" ) );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "moved" )
    {
        const Typed p( arm( "B c = move b; return c;" ) );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "written, as a local may be" )
    {
        const Typed p( arm( "b.n = 2; return b;" ) );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "but copied only by `move`" )
    {
        const Typed p( arm( "B c = b; return c;" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "write `move b`" ) != std::string::npos );
    }

    SECTION( "and the variable it came from may be filled again" )
    {
        const Typed p( arm( "h = H::Empty; return b;" ) );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a temporary is taken by `move` too" )
    {
        const Typed p( arm( "return b;", "move make()" ) );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and without it is only borrowed" )
    {
        const Typed p( arm( "return b;", "make()" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` still belongs to what holds it" ) != std::string::npos );
    }
}

// D51. `void` written where something is declared holds nothing, so it is refused there; a `void`
// that arrived as a type argument is the generic's business and is not.
TEST_CASE( "signatures_refuse_a_written_void", "[sema][void]" )
{
    struct Case
    {
        const char* source;
        const char* message;
    };

    SECTION( "as a parameter, a field, a payload or a global" )
    {
        const Case cases[] = {
            { "void f( void a ) { }", "a parameter cannot be `void`" },
            { "void f( ref void a ) { }", "a parameter cannot be `void`" },
            { "void f( fn( void ) -> i32 p ) { }", "a parameter cannot be `void`" },
            { "struct S { i32 n; void m( const ref void a ) { } };", "a parameter cannot be `void`" },
            { "struct S { i32 n; void v; };", "a field cannot be `void`" },
            { "enum E { a( void v ), b };", "a field cannot be `void`" },
            { "void g;", "a global cannot be `void`" },
            { "struct S { i32 n; static void g; };", "a global cannot be `void`" },
        };

        for( const Case& c : cases )
        {
            const Typed p( std::string( c.source ) + "\ni32 main() { return 0; }" );

            INFO( c.source << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( c.message ) != std::string::npos );
        }
    }

    // Poisoned once refused, so its uses say nothing more.
    SECTION( "and reported once" )
    {
        const Typed p( "void e() { }\nvoid f( void a ) { }\nstruct S { void v; i32 n; };\n"
                       "i32 main() { f( e() ); S s = S { e(), 1 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }

    SECTION( "but not when a type argument made it `void`" )
    {
        const Typed p( "struct Box<T> where T : copyable { T v; };\n"
                       "enum Maybe<T> { some( T v ), none };\n"
                       "T same<T>( T a ) where T : copyable { return a; }\n"
                       "void e() { }\n"
                       "i32 main() { Box<void> b; Maybe<void> m = Maybe<void>::none; same( e() ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.rendered().empty() );
    }

    // `void` behind a pointer is the address of something unnamed, not a value of nothing.
    SECTION( "nor behind a pointer" )
    {
        const Typed p( "struct S { void* p; };\nvoid f( void* p ) { }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "signatures_type_an_interface_and_what_implements_it", "[sema][interfaces]" )
{
    const Typed p(
        "interface Shape { f64 area(); void scale( f64 by ); };\n"
        "struct Sq implements Shape { f64 s; f64 area() { return s * s; } void scale( f64 by ) { } };\n"
        "struct Box<T> implements Shape where T : copyable { T v; f64 area() { return 0.0; } void scale( f64 by ) { } };\n"
        "struct Plain { i32 x; };\n"
        "f64 measure( const ref Shape s, Shape* p ) { return 0.0; }\n"
        "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id interface = p.nth( Node_kind::Interface_decl, 0 );
    const Node_id square    = p.nth( Node_kind::Struct_decl, 0 );
    const Node_id plain     = p.nth( Node_kind::Struct_decl, 2 );

    REQUIRE( p.type_name( interface ) == "Shape" );

    const Type_id     shape = p.types().type_of( interface );
    const Type_table& table = p.types().table();

    REQUIRE( table.is_interface( shape ) );
    REQUIRE( p.types().conformances().declares( table, p.types().type_of( square ), shape ) );
    REQUIRE_FALSE( p.types().conformances().declares( table, p.types().type_of( plain ), shape ) );

    // On the written entry, which is where the conformance stage reads it.
    REQUIRE( p.type_name( p.ast().implemented( square )[0] ) == "Shape" );

    // The interface's methods are typed as an aggregate's are, so they can be compared and called.
    for( const Node_id method : p.ast().members( interface ) )
    {
        REQUIRE( p.type_name( method ) != "<none>" );
    }

    REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 0 ) ) != "<none>" );
}

// A block is for one named type: a built-in, or an aggregate or enum the block can name whole.
TEST_CASE( "signatures_declare_an_implements_block", "[sema][interfaces][implements_for]" )
{
    const std::string hashable = "interface Hashable { u64 hash() const; };\n";

    // A struct or class is another package's: its own would write `implements` where it is declared.
    SECTION( "every built-in type, and a type with no type parameters" )
    {
        const std::string kl = "struct P { i32 v; };\nclass C { i32 v; public C() { v = 0; } };\nenum E { A, B };\n";

        for( const char* type : { "i8", "u64", "f32", "f64", "bool", "Own", "kl::P", "kl::C", "kl::E" } )
        {
            const Typed p(
                { { "main.kl",
                    "import kl::kinds;\n" + hashable + "enum Own { A, B };\nimplements Hashable for " + type +
                        " { u64 hash() const { return 1; } };\ni32 main() { return 0; }\n" },
                  { "kl/kinds.kl", kl } },
                prelude_source()
            );

            INFO( type << "\n" << p.rendered() );
            REQUIRE( p.clean() );
            REQUIRE( p.type_name( p.nth( Node_kind::Implements_decl, 0 ) ) == type );
        }
    }

    // `self` in a method is refused by name; the receiver is the type itself, never an interface.
    SECTION( "each method's receiver is the type" )
    {
        const Typed p( hashable + "implements Hashable for i32 { u64 hash() const { return 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 1 ) ) == "i32" );
    }

    SECTION( "anything else is refused once, at the type" )
    {
        const std::string_view help =
            "a block is for a built-in type, or a `struct`, `class` or `enum` with no type parameters";

        struct Case
        {
            const char* type;
            const char* message;
        };

        const Case cases[] = {
            { "i32*", "`i32*` cannot implement an interface through a block" },
            { "const i32*", "`const i32*` cannot implement an interface through a block" },
            { "i32[*]", "`i32[*]` cannot implement an interface through a block" },
            { "fn( i32 ) -> i32", "`fn( i32 ) -> i32` cannot implement an interface through a block" },
            { "Pair<i32>", "`Pair<i32>` cannot implement an interface through a block" },
            { "Hashable", "`Hashable` cannot implement an interface through a block" },
        };

        for( const Case& c : cases )
        {
            const Typed p(
                hashable + "struct Pair<T> where T : copyable { T a; };\n" + "implements Hashable for " + c.type +
                " { u64 hash() const { return 1; } };\ni32 main() { return 0; }"
            );

            INFO( c.type << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( c.message ) != std::string::npos );
            REQUIRE( p.rendered().find( help ) != std::string::npos );
            REQUIRE( p.rendered().find( "held by value" ) == std::string::npos );
            REQUIRE_FALSE( p.types().conformances().declares(
                p.types().table(),
                p.types().type_of( p.nth( Node_kind::Implements_decl, 0 ) ),
                p.types().type_of( p.nth( Node_kind::Interface_decl, 0 ) )
            ) );
        }
    }

    SECTION( "an unknown type, or a non-interface, as anywhere else" )
    {
        const char* cases[][2] = {
            { "implements Hashable for Nope { };", "unknown type `Nope`" },
            { "implements i32 for u8 { };", "`i32` is not an interface" },
            { "implements Missing for u8 { };", "unknown type `Missing`" },
        };

        for( const auto& c : cases )
        {
            const Typed p( hashable + c[0] + "\ni32 main() { return 0; }" );

            INFO( c[0] << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( c[1] ) != std::string::npos );
        }
    }
}

// The orphan rule: a block is written where its interface or its type is declared, and a built-in
// type is the prelude's. So two packages can never both make one type conform to one interface.
TEST_CASE( "signatures_place_a_block_beside_its_interface_or_its_type", "[sema][interfaces][implements_for][packages]" )
{
    const std::string prelude = std::string( prelude_source() ) + "interface Sized { u64 size() const; };\n";
    const std::string kl      = "interface Measured { f64 measure() const; };\n"
                                "struct Square { f64 side; };\n"
                                "implements Measured for f64 { f64 measure() const { return this; } };\n";

    const std::string_view refused = "must be in the package that declares";

    SECTION( "the interface's package, or the type's, may write it" )
    {
        const Typed p(
            { { "main.kl",
                "import kl::shapes;\n"
                "enum Size { Small, Large };\n"
                "interface Named { i32 id() const; };\n"
                "implements kl::Measured for Size { f64 measure() const { return 1.0; } };\n"
                "implements Named for kl::Square { i32 id() const { return 1; } };\n"
                "implements Named for i32 { i32 id() const { return this; } };\n"
                "implements Sized for Size { u64 size() const { return 1; } };\n"
                "i32 main() { return 0; }\n" },
              { "kl/shapes.kl", kl } },
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Its own struct or class says what it implements where it is declared, so a block for one is
    // only ever the interface's package reaching a type it cannot edit. An enum has no other way.
    SECTION( "nor may it write one for a struct or class of its own" )
    {
        struct Case
        {
            const char* block;
            const char* message;
            const char* help;
        };

        const Case cases[] = {
            { "implements kl::Measured for Rod { f64 measure() const { return this.length; } };",
              "`Rod` is declared in this package, so a block cannot be written for it",
              "write `implements kl::Measured` where `Rod` is declared" },
            { "implements Sized for Gauge { u64 size() const { return 1; } };",
              "`Gauge` is declared in this package, so a block cannot be written for it",
              "write `implements Sized` where `Gauge` is declared" },
        };

        for( const Case& c : cases )
        {
            const Typed p(
                { { "main.kl",
                    std::string( "import kl::shapes;\n"
                                 "struct Rod { f64 length; };\n"
                                 "class Gauge { i32 v; public Gauge() { v = 0; } };\n" ) +
                        c.block + "\ni32 main() { return 0; }\n" },
                  { "kl/shapes.kl", kl } },
                prelude
            );

            INFO( c.block << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( c.message ) != std::string::npos );
            REQUIRE( p.rendered().find( c.help ) != std::string::npos );
            REQUIRE( p.types().conformances().all().size() == 2 ); // kl's own, for f64, and the prelude's `str`
        }
    }

    // A package is all its files, not the one the type is declared in.
    SECTION( "even from another of its files" )
    {
        const Typed p(
            { { "main.kl", "import kl::shapes;\nimport kl::more;\ni32 main() { return 0; }\n" },
              { "kl/shapes.kl", kl },
              { "kl/more.kl",
                "import kl::shapes;\nimplements Measured for Square { f64 measure() const { return this.side; } };\n" } },
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`kl::Square` is declared in this package" ) != std::string::npos );
    }

    SECTION( "a package that declares neither may not" )
    {
        struct Case
        {
            const char* block;
            const char* message;
            const char* help;
        };

        const Case cases[] = {
            { "implements kl::Measured for i32 { f64 measure() const { return 1.0; } };",
              "a block for `i32` must be in the package that declares `i32` or `Measured`",
              "two packages could otherwise each make `i32` a `Measured`" },
            { "implements kl::Measured for kl::Square { f64 measure() const { return 1.0; } };",
              "a block for `kl::Square` must be in the package that declares `kl::Square` or `Measured`",
              "two packages could otherwise each make `kl::Square` a `Measured`" },
            { "implements Sized for i32 { u64 size() const { return 1; } };",
              "a block for `i32` must be in the package that declares `i32` or `Sized`",
              "two packages could otherwise each make `i32` a `Sized`" },
        };

        for( const Case& c : cases )
        {
            const Typed p(
                { { "main.kl", std::string( "import kl::shapes;\n" ) + c.block + "\ni32 main() { return 0; }\n" },
                  { "kl/shapes.kl", kl } },
                prelude
            );

            INFO( c.block << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( c.message ) != std::string::npos );
            REQUIRE( p.rendered().find( c.help ) != std::string::npos );
            REQUIRE( p.types().conformances().all().size() == 2 ); // kl's own, for f64, and the prelude's `str`
        }
    }

    // Built-in types are the prelude's, so another package needs an interface of its own.
    SECTION( "nor may a package give a prelude interface to a built-in" )
    {
        const Typed p(
            { { "main.kl", "import kl::shapes;\ni32 main() { return 0; }\n" },
              { "kl/shapes.kl", kl + "implements Sized for u8 { u64 size() const { return 1; } };\n" } },
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( refused ) != std::string::npos );
    }
}

TEST_CASE( "signatures_refuse_an_interface_held_by_value", "[sema][interfaces]" )
{
    const char* cases[] = {
        "Shape g;",
        "void f( Shape s ) { }",
        "Shape* f( Shape s ) { return null; }",
        "struct Holder { Shape s; };",
        "enum Held { one( Shape s ), none };",
        "void f( const ref Shape s ) { Shape t = s; }",
        "struct Box<T> where T : copyable { T v; };\nvoid f( Box<Shape>* b ) { }",
    };

    for( const char* c : cases )
    {
        const Typed p( std::string( "interface Shape { f64 area(); };\n" ) + c + "\ni32 main() { return 0; }" );

        INFO( c << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Shape` is an interface, so it cannot be held by value" ) != std::string::npos );
    }
}

TEST_CASE( "signatures_refuse_implementing_what_is_not_an_interface", "[sema][interfaces]" )
{
    struct Case
    {
        const char* source;
        const char* message;
    };

    const Case cases[] = {
        { "struct Sq implements i32 { f64 s; };", "`i32` is not an interface" },
        { "struct Plain { i32 x; };\nstruct Sq implements Plain { f64 s; };", "`Plain` is not an interface" },
        { "struct Sq implements Sq { f64 s; };", "`Sq` is not an interface" },
        { "enum E { a };\nclass Sq implements E { f64 s; };", "`E` is not an interface" },
        { "struct Sq implements Nope { f64 s; };", "unknown type" },
    };

    for( const Case& c : cases )
    {
        const Typed p( std::string( c.source ) + "\ni32 main() { return 0; }" );

        INFO( c.source << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( c.message ) != std::string::npos );
    }
}

// Each one is called through a table of the implementers' methods.
TEST_CASE( "signatures_check_an_interface_s_methods", "[sema][interfaces]" )
{
    struct Case
    {
        const char* source;
        const char* message;
    };

    const Case cases[] = {
        { "interface Shape { f64 area() { return 0.0; } };", "`area` is a signature, so it cannot have a body" },
        { "interface Shape { T pick<T>( T a ); };", "an interface's method cannot have type parameters" },
    };

    for( const Case& c : cases )
    {
        const Typed p( std::string( c.source ) + "\ni32 main() { return 0; }" );

        INFO( c.source << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( c.message ) != std::string::npos );
    }

    SECTION( "a plain signature is none of them" )
    {
        const Typed p( "interface Shape { f64 area(); void scale( f64 by ); };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "signatures_refuse_a_refinement_cycle_through_arguments", "[sema][interfaces][generic_interfaces]" )
{
    SECTION( "itself at another argument" )
    {
        const Typed p( "interface loop<T> implements loop<i32> { void f( T v ); };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`loop` cannot require itself" ) != std::string::npos );
    }

    SECTION( "through another interface" )
    {
        const Typed p( "interface a<T> implements b<i32> { void f( T v ); };\n"
                       "interface b<U> implements a<U> { void g( U v ); };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`b` cannot require `a`, which already requires `b`" ) != std::string::npos );
    }
}
} // namespace keel
#endif
