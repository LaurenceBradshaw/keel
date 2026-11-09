// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/conformances.h"
#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include "sema/type_checker.h"

namespace keel
{

namespace
{

Type_id recorded_type( std::span<const Type_id> recorded, Node_id node )
{
    return node.v < recorded.size() ? recorded[node.v] : Type_id {};
}

// What a requirement of `interface` reads as on `type`: `self` is `type`, and the interface's
// parameters are the instance's arguments.
Bindings requirement_bindings( Type_table& table, Type_id interface, Type_id type )
{
    Bindings bindings = table.instance_bindings( interface );
    bindings.emplace( table.self_of( table.get( interface ).declaration ).v, type );
    return bindings;
}

// The parameters past the receiver, and the return, each with its mode, reading the requirement's
// `self` as the implementer. The receivers differ by construction, and constness is asked separately.
bool same_signature(
    const Ast&               ast,
    std::span<const Type_id> recorded,
    Node_id                  requirement,
    Node_id                  method,
    Type_table&              table,
    const Bindings&          self
)
{
    const std::span<const Node_id> wanted = ast.explicit_params( requirement );
    const std::span<const Node_id> given  = ast.explicit_params( method );

    if( wanted.size() != given.size() ||
        table.substitute( recorded_type( recorded, requirement ), self ) != recorded_type( recorded, method ) ||
        ast.is_ref_parameter( requirement ) != ast.is_ref_parameter( method ) )
    {
        return false;
    }

    for( std::size_t i = 0; i < wanted.size(); ++i )
    {
        if( table.substitute( recorded_type( recorded, wanted[i] ), self ) != recorded_type( recorded, given[i] ) ||
            parameter_mode_of( ast, wanted[i] ) != parameter_mode_of( ast, given[i] ) )
        {
            return false;
        }
    }

    return true;
}

// Nothing when the implementer answers the requirement with `found`; otherwise the problem with
// the nearest candidate. An overload of the name that matches wins over one that does not.
std::optional<Conformance_problem> answer(
    const Ast&               ast,
    std::span<const Type_id> recorded,
    Node_id                  implementer,
    Node_id                  requirement,
    Node_id&                 found,
    Type_table&              table,
    const Bindings&          self
)
{
    found = Node_id {};

    Conformance_problem nearest = Conformance_problem::Missing;

    for( const Node_id member : ast.members( implementer ) )
    {
        if( ast.kind( member ) != Node_kind::Method_decl || ast.name( member ) != ast.name( requirement ) )
        {
            continue;
        }

        const bool explicit_params_ref_error = std::any_of(
            ast.explicit_params( member ).begin(),
            ast.explicit_params( member ).end(),
            [&]( Node_id param ) { return table.references_error( recorded_type( recorded, param ) ); }
        );

        if( table.references_error( recorded_type( recorded, member ) ) || explicit_params_ref_error )
        {
            found = member;
            return std::nullopt;
        }

        if( !found.is_valid() )
        {
            found = member;
        }

        if( !same_signature( ast, recorded, requirement, member, table, self ) )
        {
            nearest = nearest == Conformance_problem::Missing ? Conformance_problem::Mismatched : nearest;
            continue;
        }

        found = member;

        if( ast.is_static_method( member ) && !ast.is_static_method( requirement ) )
        {
            return Conformance_problem::Static;
        }

        if( !ast.is_static_method( member ) && ast.is_static_method( requirement ) )
        {
            return Conformance_problem::Not_static;
        }

        if( ast.own_type_param_list( member ).is_valid() )
        {
            return Conformance_problem::Generic;
        }

        if( ast.is_const_method( requirement ) && !ast.is_const_method( member ) )
        {
            return Conformance_problem::Not_const;
        }

        if( ast.is_located( member ) )
        {
            return Conformance_problem::Located;
        }

        if( ast.required_params( member ) != ast.explicit_params( member ).size() )
        {
            return Conformance_problem::Defaulted;
        }

        if( ast.access( member ) == Access::Private )
        {
            return Conformance_problem::Private;
        }

        return std::nullopt;
    }

    return nearest;
}

// Whether `conformed` stands for `type`: itself, or the generic declaration `type` instantiates.
bool conforms_as( const Type_table& table, Type_id conformed, Type_id type )
{
    if( conformed == type )
    {
        return true;
    }

    const bool declared = table.is_struct( type ) || table.is_enum( type );
    return declared && ( table.is_struct( conformed ) || table.is_enum( conformed ) ) &&
           table.get( conformed ).declaration == table.get( type ).declaration;
}

// What `type`'s declaration says it implements, `declared`, is `interface` at this instance.
bool declares_as( const Type_table& table, Type_id declared, Type_id type, Type_id interface )
{
    return table.substitutes_to( declared, table.instance_bindings( type ), interface );
}

// A number gets all three. `bool`, a payload-free enum and a pointer get equality and a hash, but
// no order. An interface pointer is two words, and gets nothing yet. An array is a
// `fixed_memory` of its element.
bool supplied( const Type_table& table, Type_id type, Type_id interface, bool payload )
{
    if( !interface.is_valid() )
    {
        return false;
    }

    if( table.is_array( type ) && table.is_interface( interface ) &&
        table.get( interface ).declaration == table.prelude().fixed_memory )
    {
        return table.get( interface ).arguments.size() == 1 && table.get( interface ).arguments[0] == table.get( type ).element;
    }

    const bool number = table.is_integer( type ) || table.is_float( type );

    if( interface == table.declared( table.prelude().comparable ) )
    {
        return number;
    }

    if( interface != table.declared( table.prelude().equatable ) && interface != table.declared( table.prelude().hashable ) )
    {
        return false;
    }

    return number || type == table.builtin( Type_kind::Bool ) || ( table.is_enum( type ) && !payload ) ||
           ( ( table.is_pointer( type ) || table.is_many_pointer( type ) ) && !table.is_interface_pointer( type ) );
}

} // namespace

void Conformance_model::declare( const Type_table& table, Node_id implementer, Type_id type, Type_id interface )
{
    std::vector<Declared>& implemented = declared_[table.get( type ).declaration.v];
    if( std::ranges::none_of( implemented, [&]( const Declared& each ) { return each.interface == interface; } ) )
    {
        implemented.push_back( { implementer, interface } );
    }
}

void Conformance_model::declare_block( Node_id block, Type_id pattern, Type_id interface )
{
    blocks_.push_back( { block, pattern, interface } );
}

void Conformance_model::declare_promised( Type_id parameter, std::span<const Type_id> interfaces )
{
    promised_[parameter.v].assign( interfaces.begin(), interfaces.end() );
}

void Conformance_model::declare_payload_enum( Node_id declaration )
{
    payload_enums_.push_back( declaration );
}

bool Conformance_model::conforms( const Type_table& table, Type_id type, Type_id interface ) const
{
    if( table.references_error( type ) )
    {
        return true;
    }

    if( table.is_parameter( type ) )
    {
        const auto found = promised_.find( type.v );
        return found != promised_.end() && std::ranges::find( found->second, interface ) != found->second.end();
    }

    return declares( table, type, interface ) || ( kind_supplies( table, type, interface ) &&
                                                   ( !table.is_prelude_interface( interface ) ||
                                                     !declares( table, type, table.declared( table.prelude().equatable ) ) ) );
}

// Terminates: each binding is smaller than `type`, since a pattern is never a bare parameter.
std::optional<Bindings> Conformance_model::covers( const Type_table& table, Type_id pattern, Type_id type ) const
{
    Bindings bindings;

    if( !table.deduce( pattern, type, bindings ) || !table.substitutes_to( pattern, bindings, type ) )
    {
        return std::nullopt;
    }

    for( const auto& [parameter, bound] : bindings )
    {
        const auto promised = promised_.find( parameter );

        if( promised == promised_.end() )
        {
            continue;
        }

        for( const Type_id interface : promised->second )
        {
            if( !conforms( table, bound, interface ) )
            {
                return std::nullopt;
            }
        }
    }

    return bindings;
}

std::optional<Conformance_model::Unmet_bound>
Conformance_model::unmet_bound( const Type_table& table, Type_id type, Type_id interface ) const
{
    for( const Block& block : blocks_ )
    {
        Bindings bindings;

        if( table.get( block.interface ).declaration != table.get( interface ).declaration ||
            !table.deduce( block.pattern, type, bindings ) || !table.substitutes_to( block.pattern, bindings, type ) ||
            !table.substitutes_to( block.interface, bindings, interface ) )
        {
            continue;
        }

        for( const auto& [parameter, bound] : bindings )
        {
            const auto promised = promised_.find( parameter );

            for( const Type_id each : promised == promised_.end() ? std::vector<Type_id> {} : promised->second )
            {
                if( !conforms( table, bound, each ) )
                {
                    return Unmet_bound { block.node, Type_id { parameter }, each, bound };
                }
            }
        }
    }

    return std::nullopt;
}

std::optional<Bindings>
Conformance_model::bindings_at( const Type_table& table, const Conformance& conformance, Type_id type ) const
{
    if( conformance.block )
    {
        return covers( table, conformance.type, type );
    }

    return conforms_as( table, conformance.type, type ) ? std::optional( table.instance_bindings( type ) ) : std::nullopt;
}

void Conformance_model::declare_refinement( const Type_table& table, Type_id interface, Type_id refined )
{
    refinements_[table.get( interface ).declaration.v].push_back( refined );
}

void Conformance_model::declare_bound_only( Node_id interface, Node_id method )
{
    bound_only_.emplace( interface.v, method );
}

void Conformance_model::declare_cyclic( Node_id interface )
{
    cyclic_.push_back( interface );
}

bool Conformance_model::reaches_cyclic( const Type_table& table, Node_id interface ) const
{
    return std::ranges::any_of(
        cyclic_, [&]( Node_id cyclic ) { return cyclic == interface || refines_declaration( table, interface, cyclic ); }
    );
}

Node_id Conformance_model::implementer_of( const Type_table& table, Type_id type, Type_id interface ) const
{
    if( !type.is_valid() || !table.is_interface( interface ) )
    {
        return Node_id {};
    }

    if( table.is_struct( type ) || table.is_enum( type ) )
    {
        if( const auto found = declared_.find( table.get( type ).declaration.v ); found != declared_.end() )
        {
            for( const Declared& each : found->second )
            {
                if( declares_as( table, each.interface, type, interface ) )
                {
                    return each.implementer;
                }
            }
        }
    }

    for( const Block& block : blocks_ )
    {
        if( table.get( block.interface ).declaration != table.get( interface ).declaration )
        {
            continue;
        }

        if( const std::optional<Bindings> bindings = covers( table, block.pattern, type );
            bindings && table.substitutes_to( block.interface, *bindings, interface ) )
        {
            return block.node;
        }
    }

    return Node_id {};
}

bool Conformance_model::declares( const Type_table& table, Type_id type, Type_id interface ) const
{
    return implementer_of( table, type, interface ).is_valid();
}

// A type's own `==` would disagree with the compiler's hash, so declaring `equatable` drops all three.
bool Conformance_model::supplies( const Ast& ast, const Type_table& table, Type_id type, Type_id interface ) const
{
    return supplied_by_kind( ast, table, type, interface ) && !declares( table, type, interface ) &&
           ( !table.is_prelude_interface( interface ) || !declares( table, type, table.declared( table.prelude().equatable ) )
           );
}

// supplied_by_kind without the tree: payloads as signatures recorded them.
bool Conformance_model::kind_supplies( const Type_table& table, Type_id type, Type_id interface ) const
{
    const bool payload =
        table.is_enum( type ) && std::ranges::find( payload_enums_, table.get( type ).declaration ) != payload_enums_.end();
    return supplied( table, type, interface, payload );
}

bool Conformance_model::supplied_by_kind( const Ast& ast, const Type_table& table, Type_id type, Type_id interface )
{
    return supplied( table, type, interface, table.is_enum( type ) && ast.enum_has_payload( table.get( type ).declaration ) );
}

Type_id Conformance_model::interface_for( const Type_table& table, Token_kind op )
{
    switch( op )
    {
    case Token_kind::Equal_equal:
    case Token_kind::Bang_equal:
        return table.declared( table.prelude().equatable );
    case Token_kind::Less:
    case Token_kind::Greater:
    case Token_kind::Less_equal:
    case Token_kind::Greater_equal:
    case Token_kind::Less_equal_greater:
        return table.declared( table.prelude().comparable );

    default:
        return Type_id {};
    }
}

bool Conformance_model::holds( const Type_table& table, Type_id from, Type_id to ) const
{
    if( table.references_error( from ) || table.references_error( to ) || from == to )
    {
        return true;
    }

    const Type& f = table.get( from );
    const Type& t = table.get( to );

    // An implementer reaches its interface only as a borrow, since no `Shape` is ever held by value.
    if( t.kind == Type_kind::Interface )
    {
        return declares( table, from, to );
    }

    if( table.is_box( from ) && table.is_box( to ) && table.is_interface( t.arguments[0] ) )
    {
        return declares( table, f.arguments[0], t.arguments[0] );
    }

    if( t.kind == Type_kind::Pointer && f.kind == Type_kind::Pointer && table.is_interface( t.element ) )
    {
        return declares( table, f.element, t.element ) && ( !f.const_element || t.const_element );
    }

    return table.holds( from, to );
}

std::vector<Type_id> Conformance_model::instances_of( Type_table& table, Type_id type, Node_id interface ) const
{
    std::vector<Type_id> found;
    const auto           add = [&]( Type_id each )
    {
        if( table.is_interface( each ) && table.get( each ).declaration == interface &&
            std::ranges::find( found, each ) == found.end() )
        {
            found.push_back( each );
        }
    };

    if( table.is_parameter( type ) )
    {
        if( const auto promised = promised_.find( type.v ); promised != promised_.end() )
        {
            std::ranges::for_each( promised->second, add );
        }

        return found;
    }

    if( table.is_struct( type ) || table.is_enum( type ) )
    {
        if( const auto declared = declared_.find( table.get( type ).declaration.v ); declared != declared_.end() )
        {
            for( const Declared& each : declared->second )
            {
                add( table.substitute( each.interface, table.instance_bindings( type ) ) );
            }
        }
    }

    for( const Block& block : blocks_ )
    {
        if( table.get( block.interface ).declaration != interface )
        {
            continue;
        }

        if( const std::optional<Bindings> bindings = covers( table, block.pattern, type ) )
        {
            add( table.substitute( block.interface, *bindings ) );
        }
    }

    if( table.is_array( type ) && interface == table.prelude().fixed_memory )
    {
        add( table.interface( interface, std::array { table.get( type ).element }, "fixed_memory" ) );
    }

    return found;
}

std::vector<Type_id> Conformance_model::refinements( Type_table& table, Type_id interface ) const
{
    if( !interface.is_valid() || !table.is_interface( interface ) )
    {
        return {};
    }

    const auto found = refinements_.find( table.get( interface ).declaration.v );
    if( found == refinements_.end() )
    {
        return {};
    }

    const Bindings       bindings = table.instance_bindings( interface );
    std::vector<Type_id> result;

    for( const Type_id refined : found->second )
    {
        result.push_back( table.substitute( refined, bindings ) );
    }

    return result;
}

// Recursion is safe: a refinement closing a cycle is refused before it is declared.
bool Conformance_model::refines( Type_table& table, Type_id interface, Type_id other ) const
{
    for( const Type_id refined : refinements( table, interface ) )
    {
        if( refined == other || refines( table, refined, other ) )
        {
            return true;
        }
    }

    return false;
}

// By declaration, as written, so it terminates on a cycle `refines` would follow forever.
bool Conformance_model::refines_declaration( const Type_table& table, Node_id interface, Node_id other ) const
{
    const auto found = refinements_.find( interface.v );
    if( found == refinements_.end() )
    {
        return false;
    }

    for( const Type_id refined : found->second )
    {
        const Node_id declaration = table.get( refined ).declaration;
        if( declaration == other || refines_declaration( table, declaration, other ) )
        {
            return true;
        }
    }

    return false;
}

Node_id Conformance_model::bound_only_method( const Type_table& table, Type_id interface ) const
{
    if( !interface.is_valid() || !table.is_interface( interface ) )
    {
        return Node_id {};
    }

    const auto found = bound_only_.find( table.get( interface ).declaration.v );
    return found == bound_only_.end() ? Node_id {} : found->second;
}

std::span<const Conformance> Conformance_model::all() const
{
    assert( completed_ && "the method map is read only once every signature is typed" );
    return conformances_;
}

std::span<const Conformance_error> Conformance_model::errors() const
{
    assert( completed_ && "the method map is read only once every signature is typed" );
    return errors_;
}

const Conformance* Conformance_model::find( Node_id implementer, Type_id interface ) const
{
    for( const Conformance& conformance : all() )
    {
        if( conformance.implementer == implementer && conformance.interface == interface )
        {
            return &conformance;
        }
    }

    return nullptr;
}

const Conformance* Conformance_model::of( const Type_table& table, Type_id type, Type_id interface ) const
{
    for( const Conformance& conformance : all() )
    {
        if( const std::optional<Bindings> bindings = bindings_at( table, conformance, type );
            bindings && table.substitutes_to( conformance.interface, *bindings, interface ) )
        {
            return &conformance;
        }
    }

    return nullptr;
}

Node_id Conformance_model::method_for( const Type_table& table, Type_id type, Type_id interface, Node_id requirement ) const
{
    for( const Conformance& conformance : all() )
    {
        const std::optional<Bindings> bindings = bindings_at( table, conformance, type );
        if( !bindings || !table.substitutes_to( conformance.interface, *bindings, interface ) )
        {
            continue;
        }

        for( std::size_t i = 0; i < conformance.requirements.size(); ++i )
        {
            if( conformance.requirements[i] == requirement )
            {
                return conformance.methods[i];
            }
        }
    }

    return Node_id {};
}

Node_id Conformance_model::block_declaring( const Ast& ast, const Type_table& table, Type_id type, Symbol_id name ) const
{
    const std::vector<Node_id> blocks = blocks_declaring( ast, table, type, name );
    return blocks.empty() ? Node_id {} : blocks.front();
}

std::vector<Node_id>
Conformance_model::blocks_declaring( const Ast& ast, const Type_table& table, Type_id type, Symbol_id name ) const
{
    std::vector<Node_id> blocks;

    for( const Conformance& conformance : all() )
    {
        if( !conformance.block || !bindings_at( table, conformance, type ) )
        {
            continue;
        }

        for( const Node_id member : ast.members( conformance.implementer ) )
        {
            if( ast.kind( member ) == Node_kind::Method_decl && ast.name( member ) == name )
            {
                blocks.push_back( conformance.implementer );
                break;
            }
        }
    }

    return blocks;
}

// That `type` is an `interface`, written at `at`, whose methods are `implementer`'s members.
void Conformance_model::conform(
    const Ast&               ast,
    Type_table&              table,
    std::span<const Type_id> recorded,
    Node_id                  implementer,
    Type_id                  interface,
    Span                     at,
    Type_id                  type
)
{
    if( const Conformance* first = of( table, type, interface ) )
    {
        errors_.push_back(
            { .problem     = Conformance_problem::Repeated,
              .at          = at,
              .implementer = implementer,
              .interface   = interface,
              .found       = first->implementer,
              .self        = type }
        );
        return;
    }

    // Two that could meet at one instance, whatever their `where` says.
    for( const Conformance& earlier : conformances_ )
    {
        if( ( earlier.block || ast.kind( implementer ) == Node_kind::Implements_decl ) &&
            table.unifies( earlier.type, type, earlier.interface, interface ) )
        {
            errors_.push_back(
                { .problem     = Conformance_problem::Overlapping,
                  .at          = at,
                  .implementer = implementer,
                  .interface   = interface,
                  .found       = earlier.implementer,
                  .self        = type,
                  .refined     = earlier.type }
            );
            return;
        }
    }

    const Node_id  declaration = table.get( interface ).declaration;
    const Bindings self        = requirement_bindings( table, interface, type );

    Conformance conformance {
        .implementer  = implementer,
        .type         = type,
        .block        = ast.kind( implementer ) == Node_kind::Implements_decl,
        .interface    = interface,
        .requirements = {},
        .methods      = {}
    };

    for( const Node_id requirement : ast.members( declaration ) )
    {
        // A generic requirement was refused where it was declared; nothing answers one.
        const bool refused = ast.own_type_param_list( requirement ).is_valid();

        Node_id                                  found;
        const std::optional<Conformance_problem> problem =
            answer( ast, recorded, implementer, requirement, found, table, self );

        conformance.requirements.push_back( requirement );
        conformance.methods.push_back( problem ? Node_id {} : found );

        if( problem && !refused )
        {
            errors_.push_back( {
                .problem     = *problem,
                .at          = at,
                .implementer = implementer,
                .interface   = interface,
                .requirement = requirement,
                .found       = found,
                .self        = type,
            } );
        }
    }

    conformances_.push_back( std::move( conformance ) );
}

// Each refinement `interface` implies and nothing answers yet, checked against the same members.
void Conformance_model::conform_implied(
    const Ast&               ast,
    Type_table&              table,
    std::span<const Type_id> recorded,
    Node_id                  implementer,
    Type_id                  interface,
    Span                     at,
    Type_id                  type
)
{
    for( const Type_id refined : refinements( table, interface ) )
    {
        if( of( table, type, refined ) || supplied_by_kind( ast, table, type, refined ) )
        {
            continue;
        }

        // A block answering it under a narrower `where` would overlap an implied one.
        if( unmet_bound( table, type, refined ) )
        {
            errors_.push_back( {
                .problem     = Conformance_problem::Unrefined,
                .at          = at,
                .implementer = implementer,
                .interface   = interface,
                .self        = type,
                .refined     = refined,
            } );
            continue;
        }

        conform( ast, table, recorded, implementer, refined, at, type );
        conform_implied( ast, table, recorded, implementer, refined, at, type );
    }
}

// A block's method no conformance of the block requires, written or implied. A refused block's
// methods were reported with it.
void Conformance_model::check_requested( const Ast& ast, Node_id block, Type_id interface, Type_id type )
{
    if( std::ranges::none_of( conformances_, [&]( const Conformance& each ) { return each.implementer == block; } ) )
    {
        return;
    }

    for( const Node_id member : ast.members( block ) )
    {
        const bool requested = std::ranges::any_of(
            conformances_,
            [&]( const Conformance& conformance )
            {
                return conformance.implementer == block &&
                       std::ranges::any_of(
                           conformance.requirements,
                           [&]( Node_id requirement ) { return ast.name( requirement ) == ast.name( member ); }
                       );
            }
        );

        if( ast.kind( member ) == Node_kind::Method_decl && !requested )
        {
            errors_.push_back( {
                .problem     = Conformance_problem::Unrequested,
                .at          = ast.name_span( member ),
                .implementer = block,
                .interface   = interface,
                .found       = member,
                .self        = type,
            } );
        }
    }
}

// Written conformances first, so one written anywhere answers before an implied one would.
void Conformance_model::complete( const Ast& ast, Type_table& table, std::span<const Type_id> recorded )
{
    assert( !completed_ && "conformances are completed once" );
    completed_ = true; // conform's own `of` reads what is built so far

    struct Written
    {
        Node_id implementer;
        Type_id interface;
        Span    at;
        Type_id type;
    };
    std::vector<Written> written;

    for( const Node_id decl : ast.declarations( ast.root() ) )
    {
        const bool block = ast.kind( decl ) == Node_kind::Implements_decl;
        if( !block && !is_aggregate( ast.kind( decl ) ) )
        {
            continue;
        }

        const Type_id type = recorded_type( recorded, decl );

        // A refused block conforms nothing.
        if( block && ( !type.is_valid() || table.is_error( type ) ) )
        {
            continue;
        }

        for( const Node_id each : ast.implemented( decl ) )
        {
            const Type_id interface = recorded_type( recorded, each );
            if( interface.is_valid() && table.is_interface( interface ) ) // otherwise reported where it was resolved
            {
                written.push_back( { decl, interface, ast.span( each ), type } );
                conform( ast, table, recorded, decl, interface, ast.span( each ), type );
            }
        }
    }

    for( const Written& each : written )
    {
        conform_implied( ast, table, recorded, each.implementer, each.interface, each.at, each.type );
    }

    for( const Written& each : written )
    {
        if( ast.kind( each.implementer ) == Node_kind::Implements_decl )
        {
            check_requested( ast, each.implementer, each.interface, each.type );
        }
    }
}

} // namespace keel

namespace keel::sema
{

namespace
{

// The requirement as the implementer must write it, with `self` replaced by the implementer's type.
std::string substituted_requirement(
    const Ast&               ast,
    const Interner&          interner,
    Type_table&              table,
    std::span<const Type_id> recorded,
    const Conformance_error& error
)
{
    const Bindings self = requirement_bindings( table, error.interface, error.self );

    std::string text = fmt::format(
        "{}{}{} {}(",
        ast.is_static_method( error.requirement ) ? "static " : "",
        param_spelling( parameter_mode_of( ast, error.requirement ) ),
        table.name( table.substitute( recorded_type( recorded, error.requirement ), self ) ),
        interner.text( ast.name( error.requirement ) )
    );

    const std::span<const Node_id> params = ast.explicit_params( error.requirement );
    for( std::size_t i = 0; i < params.size(); ++i )
    {
        text += fmt::format(
            "{}{}{} {}",
            i == 0 ? "" : ", ",
            param_spelling( parameter_mode_of( ast, params[i] ) ),
            table.name( table.substitute( recorded_type( recorded, params[i] ), self ) ),
            interner.text( ast.name( params[i] ) )
        );
    }

    text += ")";
    return ast.is_const_method( error.requirement ) ? text + " const" : text;
}

} // namespace

void report_conformance_errors(
    const Ast&               ast,
    const Interner&          interner,
    Type_table&              table,
    std::span<const Type_id> recorded,
    const Conformance_model& model,
    Reporter&                reporter
)
{
    for( const Conformance_error& error : model.errors() )
    {
        const bool             in_block  = ast.kind( error.implementer ) == Node_kind::Implements_decl;
        const std::string_view interface = table.name( error.interface );
        // A block's type as written after `for`; an aggregate's by its name, whatever its parameters.
        const Type&            self = table.get( error.self );
        const std::string_view type = in_block && ( !self.arguments.empty() || self.element.is_valid() )
                                          ? table.name( error.self )
                                          : table.base_name( error.self );
        const bool blocks = in_block && error.found.is_valid() && ast.kind( error.found ) == Node_kind::Implements_decl;

        if( error.problem == Conformance_problem::Repeated )
        {
            // Two blocks may come from two packages, each owning an argument.
            reporter.error_at(
                error.at,
                fmt::format( "`{}` already implements `{}`", type, interface ),
                blocks ? fmt::format( "the block at {} implements it first", reporter.position( ast.name_span( error.found ) ) )
                       : ""
            );
            continue;
        }

        if( error.problem == Conformance_problem::Overlapping )
        {
            const std::string_view earlier = table.name( error.refined );
            const std::string      first   = reporter.position( ast.name_span( error.found ) );
            reporter.error_at(
                error.at,
                type == earlier
                    ? fmt::format( "two blocks for `{}` both implement `{}`", type, interface )
                    : fmt::format( "`{}` could meet `{}` at one type, and both implement `{}`", type, earlier, interface ),
                blocks ? fmt::format(
                             "the block at {} implements it first; a `where` does not keep blocks apart, so narrow one's type",
                             first
                         )
                       : fmt::format( "the declaration at {} implements it first; narrow one so no type matches both", first )
            );
            continue;
        }

        if( error.problem == Conformance_problem::Unrequested )
        {
            reporter.error_at(
                error.at,
                fmt::format( "`{}` has no method `{}`", interface, interner.text( ast.name( error.found ) ) ),
                "a block holds only the methods of the interface it implements"
            );
            continue;
        }

        if( error.problem == Conformance_problem::Unrefined )
        {
            const std::string_view refined = table.name( error.refined );
            const auto             unmet   = model.unmet_bound( table, error.self, error.refined );
            const std::string      help    = fmt::format(
                "the block at {} makes `{}` implement `{}` only where `{} : {}`; {}",
                reporter.position( ast.name_span( unmet->block ) ),
                table.name( recorded_type( recorded, unmet->block ) ),
                refined,
                table.name( unmet->parameter ),
                table.name( unmet->interface ),
                table.is_parameter( unmet->argument ) ? fmt::format(
                                                            "add `{}` to this block's `where` clause for `{}`",
                                                            table.name( unmet->interface ),
                                                            table.name( unmet->argument )
                                                        )
                                                              : fmt::format( "which `{}` is not", table.name( unmet->argument ) )
            );

            reporter.error_at(
                error.at,
                fmt::format( "`{}` implements `{}` but not `{}`, which `{}` requires", type, interface, refined, interface ),
                help
            );
            continue;
        }

        const std::string_view method = interner.text( ast.name( error.requirement ) );

        switch( error.problem )
        {
        case Conformance_problem::Missing:
            reporter.error_at(
                error.at,
                fmt::format( "`{}` implements `{}` but has no method `{}`", type, interface, method ),
                in_block
                    ? fmt::format( "add `{}` to the block", substituted_requirement( ast, interner, table, recorded, error ) )
                    : fmt::format( "add `{}` to `{}`", substituted_requirement( ast, interner, table, recorded, error ), type )
            );
            break;
        case Conformance_problem::Mismatched:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` does not match the one `{}` declares", type, method, interface ),
                fmt::format( "`{}` wants `{}`", interface, substituted_requirement( ast, interner, table, recorded, error ) )
            );
            break;
        case Conformance_problem::Not_const:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` must be `const`, as `{}` declares it", type, method, interface ),
                model.bound_only_method( table, error.interface ).is_valid()
                    ? fmt::format( "a `const ref T` with `T : {}` may call it", interface )
                    : fmt::format( "a caller holding a `const ref {}` may call it", interface )
            );
            break;
        case Conformance_problem::Static:
            reporter.error_at(
                error.at, fmt::format( "`{}`'s `{}` is `static`, but `{}` calls it on an object", type, method, interface )
            );
            break;
        case Conformance_problem::Not_static:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` must be `static`, as `{}` declares it", type, method, interface ),
                fmt::format( "it is called as `{}::{}(...)`, with no object", type, method )
            );
            break;
        case Conformance_problem::Generic:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` has type parameters, so it cannot answer `{}`'s", type, method, interface ),
                "a call through the interface reaches one compiled method, which cannot be every instance"
            );
            break;
        case Conformance_problem::Located:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` cannot be `@located`, since it answers `{}`", type, method, interface ),
                fmt::format( 
                    "a call through `{0}` passes no location; move the `@located` work into a method `{0}` "
                    "does not declare", 
                    interface 
                )
            );
            break;
        case Conformance_problem::Defaulted:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` cannot have a default, since it answers `{}`", type, method, interface ),
                fmt::format( "a call through `{}` would not see it; pass the value at each call instead", interface )
            );
            break;
        case Conformance_problem::Private:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` is private, but `{}` makes it callable by anyone", type, method, interface ),
                fmt::format( "make `{}` public", method )
            );
            break;
        case Conformance_problem::Repeated:
        case Conformance_problem::Overlapping:
        case Conformance_problem::Unrequested:
        case Conformance_problem::Unrefined:
            break;
        }
    }
}

} // namespace keel::sema
#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// Keyed by declaration, so every instance of a generic implements what the declaration says.
TEST_CASE( "conformance_model_substitutes_a_refinement_per_instance", "[sema][type][interfaces]" )
{
    Type_table        table;
    Conformance_model model;

    const Type_id i32     = table.integer( 32, true );
    const Type_id i64     = table.integer( 64, true );
    const Type_id t       = table.parameter( Node_id { 6 }, "T" );
    const Type_id u       = table.parameter( Node_id { 8 }, "U" );
    const Type_id sink    = table.interface( Node_id { 5 }, std::array { t }, "sink" );
    const Type_id counted = table.interface( Node_id { 7 }, std::array { u }, "counted" );
    const Type_id shape   = table.interface( Node_id { 9 }, {}, "Shape" );

    model.declare_refinement( table, counted, table.interface( Node_id { 5 }, std::array { u }, "sink" ) );

    const Type_id counted_i32 = table.interface( Node_id { 7 }, std::array { i32 }, "counted" );
    const Type_id sink_i32    = table.interface( Node_id { 5 }, std::array { i32 }, "sink" );
    const Type_id sink_i64    = table.interface( Node_id { 5 }, std::array { i64 }, "sink" );

    REQUIRE( table.instance_bindings( counted_i32 ) == Bindings { { u.v, i32 } } );
    REQUIRE( table.instance_bindings( shape ).empty() );
    REQUIRE( table.instance_bindings( sink ) == Bindings { { t.v, t } } ); // the open form binds itself

    REQUIRE( model.refinements( table, counted_i32 ) == std::vector { sink_i32 } );
    REQUIRE( model.refines( table, counted_i32, sink_i32 ) );
    REQUIRE_FALSE( model.refines( table, counted_i32, sink_i64 ) );

    SECTION( "by declaration, whatever the arguments" )
    {
        REQUIRE( model.refines_declaration( table, Node_id { 7 }, Node_id { 5 } ) );
        REQUIRE_FALSE( model.refines_declaration( table, Node_id { 5 }, Node_id { 7 } ) );
        REQUIRE_FALSE( model.refines_declaration( table, Node_id { 9 }, Node_id { 5 } ) );
    }
}

TEST_CASE( "conformance_model_records_what_implements_an_interface", "[sema][type][interfaces]" )
{
    Type_table        table;
    Conformance_model model;

    const Type_id i32   = table.integer( 32, true );
    const Type_id f64   = table.floating( 64 );
    const Type_id shape = table.interface( Node_id { 5 }, {}, "Shape" );
    const Type_id drawn = table.interface( Node_id { 7 }, {}, "Drawn" );
    const Type_id point = table.structure( Node_id { 3 }, {}, "Point" );
    const Type_id other = table.structure( Node_id { 4 }, {}, "Other" );

    model.declare( table, Node_id { 3 }, point, shape );
    model.declare( table, Node_id { 3 }, point, shape );
    model.declare( table, Node_id { 11 }, table.structure( Node_id { 11 }, std::array { i32 }, "Box" ), drawn );

    REQUIRE( model.declares( table, point, shape ) );
    REQUIRE_FALSE( model.declares( table, point, drawn ) );
    REQUIRE_FALSE( model.declares( table, other, shape ) );

    SECTION( "every instance of a generic" )
    {
        REQUIRE( model.declares( table, table.structure( Node_id { 11 }, std::array { i32 }, "Box" ), drawn ) );
        REQUIRE( model.declares( table, table.structure( Node_id { 11 }, std::array { f64 }, "Box" ), drawn ) );
    }

    SECTION( "and nothing that is not an aggregate" )
    {
        REQUIRE_FALSE( model.declares( table, shape, shape ) );
        REQUIRE_FALSE( model.declares( table, table.pointer_to( point ), shape ) );
        REQUIRE_FALSE( model.declares( table, i32, shape ) );
        REQUIRE_FALSE( model.declares( table, Type_id {}, shape ) );
    }
}

// A generic implementer declares in its open form, so each instance implements its own instance.
TEST_CASE( "conformance_model_reads_a_generic_implementers_interface_per_instance", "[sema][type][interfaces]" )
{
    Type_table        table;
    Conformance_model model;

    const Type_id i32       = table.integer( 32, true );
    const Type_id i64       = table.integer( 64, true );
    const Type_id t         = table.parameter( Node_id { 12 }, "T" );
    const Type_id sink      = table.interface( Node_id { 5 }, std::array { t }, "sink" );
    const Type_id last      = table.structure( Node_id { 11 }, std::array { t }, "last" );
    const Type_id last_i32  = table.structure( Node_id { 11 }, std::array { i32 }, "last" );
    const Type_id sink_i32  = table.interface( Node_id { 5 }, std::array { i32 }, "sink" );
    const Type_id sink_i64  = table.interface( Node_id { 5 }, std::array { i64 }, "sink" );
    const Type_id sink_last = table.interface( Node_id { 5 }, std::array { table.pointer_to( last_i32 ) }, "sink" );

    model.declare( table, Node_id { 11 }, last, sink );

    REQUIRE( table.instance_bindings( last_i32 ) == Bindings { { t.v, i32 } } );
    REQUIRE( table.instance_bindings( table.pointer_to( last_i32 ) ).empty() );

    REQUIRE( model.declares( table, last_i32, sink_i32 ) );
    REQUIRE_FALSE( model.declares( table, last_i32, sink_i64 ) );
    REQUIRE( model.declares( table, last, sink ) );

    SECTION( "matched structurally, without interning" )
    {
        const Bindings bindings { { t.v, i32 } };

        REQUIRE( table.substitutes_to( table.pointer_to( t ), bindings, table.pointer_to( i32 ) ) );
        REQUIRE_FALSE( table.substitutes_to( table.pointer_to( t ), bindings, table.pointer_to( i64 ) ) );
        REQUIRE_FALSE( table.substitutes_to( table.pointer_to( t ), bindings, table.pointer_to( i32, true ) ) );
        REQUIRE_FALSE( table.substitutes_to( sink, bindings, sink_last ) );
    }

    SECTION( "an open form has no vtable" )
    {
        REQUIRE( table.interface_types() == std::vector { sink_i32, sink_i64, sink_last } );
    }
}

// Only as a borrow or a pointer: the implementer is still the object, so `const` may be added, never dropped.
TEST_CASE( "conformance_model_holds_an_implementer_as_its_interface", "[sema][type][interfaces]" )
{
    Type_table        table;
    Conformance_model model;

    const Type_id shape = table.interface( Node_id { 5 }, {}, "Shape" );
    const Type_id point = table.structure( Node_id { 3 }, {}, "Point" );
    const Type_id other = table.structure( Node_id { 4 }, {}, "Other" );

    model.declare( table, Node_id { 3 }, point, shape );

    REQUIRE( model.holds( table, point, shape ) );
    REQUIRE( model.holds( table, table.pointer_to( point ), table.pointer_to( shape ) ) );
    REQUIRE( model.holds( table, table.pointer_to( point ), table.pointer_to( shape, true ) ) );
    REQUIRE( model.holds( table, table.pointer_to( point, true ), table.pointer_to( shape, true ) ) );

    REQUIRE_FALSE( model.holds( table, table.pointer_to( point, true ), table.pointer_to( shape ) ) );
    REQUIRE_FALSE( model.holds( table, other, shape ) );
    REQUIRE_FALSE( model.holds( table, table.pointer_to( other ), table.pointer_to( shape ) ) );
    REQUIRE_FALSE( model.holds( table, shape, point ) );
    REQUIRE_FALSE( model.holds( table, table.pointer_to( shape ), table.pointer_to( point ) ) );
    REQUIRE_FALSE(
        model.holds( table, table.pointer_to( table.pointer_to( point ) ), table.pointer_to( table.pointer_to( shape ) ) )
    );
    REQUIRE_FALSE( model.holds( table, table.many_pointer_to( point ), table.pointer_to( shape ) ) );
}

// Only the declared box, only towards an interface, and only from an implementer.
TEST_CASE( "conformance_model_holds_a_box_of_an_implementer_as_a_box_of_its_interface", "[sema][type][interfaces]" )
{
    Type_table        table;
    Conformance_model model;

    const Type_id shape = table.interface( Node_id { 5 }, {}, "Shape" );
    const Type_id point = table.structure( Node_id { 3 }, {}, "Point" );
    const Type_id other = table.structure( Node_id { 4 }, {}, "Other" );

    model.declare( table, Node_id { 3 }, point, shape );

    const auto boxed = [&]( Type_id held ) { return table.structure( Node_id { 9 }, std::array { held }, "box" ); };

    SECTION( "before the box is declared, it is any generic" )
    {
        REQUIRE_FALSE( table.is_box( boxed( point ) ) );
        REQUIRE_FALSE( model.holds( table, boxed( point ), boxed( shape ) ) );
    }

    Prelude_items prelude;
    prelude.box = Node_id { 9 };
    table.set_prelude( prelude );

    REQUIRE( table.is_box( boxed( point ) ) );
    REQUIRE( table.is_box( boxed( shape ) ) );
    REQUIRE_FALSE( table.is_box( point ) );
    REQUIRE_FALSE( table.is_box( table.pointer_to( boxed( point ) ) ) );

    REQUIRE( model.holds( table, boxed( point ), boxed( shape ) ) );

    REQUIRE_FALSE( model.holds( table, boxed( other ), boxed( shape ) ) );
    REQUIRE_FALSE( model.holds( table, boxed( shape ), boxed( point ) ) );
    REQUIRE_FALSE( model.holds( table, boxed( point ), boxed( other ) ) );
    REQUIRE_FALSE( model.holds(
        table,
        table.structure( Node_id { 10 }, std::array { point }, "Holder" ),
        table.structure( Node_id { 10 }, std::array { shape }, "Holder" )
    ) );
}

TEST_CASE( "conformances_record_the_method_answering_each_requirement", "[sema][interfaces]" )
{
    const Typed p(
        "interface Shape { f64 area() const; void grow( f64 by ); };\n"
        "interface Named { i32 id() const; };\n"
        "struct Sq implements Shape, Named {\n"
        "    f64 s;\n"
        "    void grow( f32 by ) { }\n"
        "    void grow( f64 by ) { }\n"
        "    i32 id() const { return 1; }\n"
        "    f64 area() const { return s * s; }\n"
        "};\n"
        "struct Box<T> implements Shape where T : copyable { T v; f64 area() const { return 0.0; } void grow( f64 by ) { } };\n"
        "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Ast&               ast          = p.ast();
    const Conformance_model& conformances = p.types().conformances();
    const Node_id            shape_decl   = p.nth( Node_kind::Interface_decl, 0 );
    const Node_id            named_decl   = p.nth( Node_kind::Interface_decl, 1 );
    const Node_id            square       = p.nth( Node_kind::Struct_decl, 0 );
    const Node_id            box          = p.nth( Node_kind::Struct_decl, 1 );
    const Type_id            shape        = p.types().type_of( shape_decl );
    const Type_id            named        = p.types().type_of( named_decl );

    REQUIRE( conformances.all().size() == 7 ); // and the prelude's four
    REQUIRE( conformances.errors().empty() );

    const Conformance* sq_shape = conformances.find( square, shape );
    REQUIRE( sq_shape != nullptr );
    REQUIRE( conformances.find( square, named ) != nullptr );
    REQUIRE( conformances.find( box, shape ) != nullptr ); // keyed by the declaration, not an instance
    REQUIRE( conformances.find( box, named ) == nullptr );

    // In the interface's order, whatever order the implementer declares them in.
    const std::span<const Node_id> requirements = ast.members( shape_decl );
    REQUIRE( sq_shape->requirements == std::vector<Node_id>( requirements.begin(), requirements.end() ) );
    REQUIRE( sq_shape->methods.size() == 2 );

    const std::span<const Node_id> members = ast.members( square );
    REQUIRE( sq_shape->methods[0] == members[4] ); // area
    REQUIRE( sq_shape->methods[1] == members[2] ); // the `grow` that matches, not the first of the name

    const Type_table& table    = p.types().table();
    const Type_id     sq_type  = p.types().type_of( square );
    const Type_id     box_type = p.types().type_of( box );

    REQUIRE( conformances.of( table, sq_type, shape ) == sq_shape );
    REQUIRE( conformances.of( table, box_type, named ) == nullptr );
    REQUIRE( conformances.method_for( table, sq_type, shape, requirements[1] ) == members[2] );
    REQUIRE( conformances.method_for( table, sq_type, named, ast.members( named_decl )[0] ) == members[3] );
    REQUIRE( conformances.method_for( table, box_type, shape, requirements[0] ) == ast.members( box )[1] );
    REQUIRE_FALSE( conformances.method_for( table, sq_type, shape, square ).is_valid() );
}

// Each at the interface as written after `implements`, with the requirement quoted in the help.
TEST_CASE( "conformances_report_each_problem", "[sema][interfaces]" )
{
    struct Case
    {
        const char*         members;
        Conformance_problem problem;
        const char*         message;
        const char*         help;
    };

    const Case cases[] = {
        { "f64 area() const { return s; }",
          Conformance_problem::Missing,
          "`Sq` implements `Shape` but has no method `grow`",
          "add `void grow(f64 by)` to `Sq`" },
        { "f64 area() const { return s; } void grow( f32 by ) { }",
          Conformance_problem::Mismatched,
          "`Sq`'s `grow` does not match the one `Shape` declares",
          "`Shape` wants `void grow(f64 by)`" },
        { "f64 area() const { return s; } void grow( const ref f64 by ) { }",
          Conformance_problem::Mismatched,
          "`Sq`'s `grow` does not match the one `Shape` declares",
          "`Shape` wants `void grow(f64 by)`" },
        { "f64 area() const { return s; } void grow() { }",
          Conformance_problem::Mismatched,
          "`Sq`'s `grow` does not match the one `Shape` declares",
          "`Shape` wants `void grow(f64 by)`" },
        { "f32 area() const { return 0.0; } void grow( f64 by ) { }",
          Conformance_problem::Mismatched,
          "`Sq`'s `area` does not match the one `Shape` declares",
          "`Shape` wants `f64 area() const`" },
        { "f64 area() { return s; } void grow( f64 by ) { }",
          Conformance_problem::Not_const,
          "`Sq`'s `area` must be `const`, as `Shape` declares it",
          "a caller holding a `const ref Shape` may call it" },
        { "static f64 area() { return 0.0; } void grow( f64 by ) { }",
          Conformance_problem::Static,
          "`Sq`'s `area` is `static`, but `Shape` calls it on an object",
          "" },
        { "f64 area() const { return s; } void grow<T>( f64 by ) { }",
          Conformance_problem::Generic,
          "`Sq`'s `grow` has type parameters, so it cannot answer `Shape`'s",
          "a call through the interface reaches one compiled method" },
        { "f64 area() const { return s; } private void grow( f64 by ) { }",
          Conformance_problem::Private,
          "`Sq`'s `grow` is private, but `Shape` makes it callable by anyone",
          "make `grow` public" },
        { "f64 area() const { return s; } @located void grow( f64 by ) { }",
          Conformance_problem::Located,
          "`Sq`'s `grow` cannot be `@located`, since it answers `Shape`",
          "a call through `Shape` passes no location" },
        { "f64 area() const { return s; } void grow( f64 by = 1.0 ) { }",
          Conformance_problem::Defaulted,
          "`Sq`'s `grow` cannot have a default, since it answers `Shape`",
          "a call through `Shape` would not see it" },
    };

    for( const Case& c : cases )
    {
        const std::string source = std::string( "interface Shape { f64 area() const; void grow( f64 by ); };\n" ) +
                                   "class Sq implements Shape { f64 s; " + c.members + " };\ni32 main() { return 0; }";
        const Typed p( source );

        INFO( c.members << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( c.message ) != std::string::npos );
        REQUIRE( p.rendered().find( c.help ) != std::string::npos );

        const std::span<const Conformance_error> errors = p.types().conformances().errors();
        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].problem == c.problem );
        REQUIRE( errors[0].at.start == source.find( "Shape {", source.find( "implements" ) ) );
        REQUIRE( errors[0].at.len() == 5 );

        // Recorded, so a later stage can still walk every requirement, with nothing where none answers.
        const Conformance* conformance = p.types().conformances().find( errors[0].implementer, errors[0].interface );
        REQUIRE( conformance != nullptr );
        REQUIRE( std::ranges::count_if( conformance->methods, []( Node_id m ) { return !m.is_valid(); } ) == 1 );
    }

    SECTION( "a const method answers one that is not" )
    {
        const Typed p( "interface Shape { void grow( f64 by ); };\n"
                       "struct Sq implements Shape { f64 s; void grow( f64 by ) const { } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a const ref return is part of the signature" )
    {
        const Typed p( "interface Shape { const ref f64 side() const; };\n"
                       "struct Sq implements Shape { f64 s; f64 side() const { return s; } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Sq`'s `side` does not match the one `Shape` declares" ) != std::string::npos );
    }

    SECTION( "every requirement unanswered is reported" )
    {
        const Typed p( "interface Shape { f64 area() const; void grow( f64 by ); };\n"
                       "struct Sq implements Shape { f64 s; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }
}

TEST_CASE( "conformances_refuse_a_repeated_interface", "[sema][interfaces]" )
{
    const std::string source = "interface Shape { f64 area() const; };\n"
                               "struct Sq implements Shape, Shape { f64 s; f64 area() const { return s; } };\n"
                               "i32 main() { return 0; }";
    const Typed       p( source );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "`Sq` already implements `Shape`" ) != std::string::npos );

    const Conformance_model& conformances = p.types().conformances();
    REQUIRE( conformances.all().size() == 5 ); // and the prelude's four
    REQUIRE( conformances.errors()[0].at.start == source.find( "Shape {", source.find( "implements" ) ) ); // the second
}

TEST_CASE( "conformances_answer_each_instance_with_its_own_method", "[sema][interfaces][generic_interfaces]" )
{
    const Typed p( "interface sink<T> { void put( T value ); u64 count() const; };\n"
                   "struct log implements sink<i64>, sink<bool> {\n"
                   "    i32 v;\n"
                   "    void put( i64 value ) { }\n"
                   "    void put( bool value ) { }\n"
                   "    u64 count() const { return 0; }\n"
                   "};\n"
                   "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Ast&                     ast          = p.ast();
    const Conformance_model&       conformances = p.types().conformances();
    const Type_table&              table        = p.types().table();
    const Node_id                  log          = p.nth( Node_kind::Struct_decl, 0 );
    const Type_id                  log_type     = p.types().type_of( log );
    const Type_id                  sink_i64     = p.types().type_of( ast.implemented( log )[0] );
    const Type_id                  sink_bool    = p.types().type_of( ast.implemented( log )[1] );
    const std::span<const Node_id> requirements = ast.members( p.nth( Node_kind::Interface_decl, 0 ) );
    const std::span<const Node_id> members      = ast.members( log );

    REQUIRE( sink_i64 != sink_bool );
    REQUIRE( conformances.find( log, sink_i64 ) != nullptr );
    REQUIRE( conformances.find( log, sink_bool ) != nullptr );

    REQUIRE( conformances.method_for( table, log_type, sink_i64, requirements[0] ) == members[1] );
    REQUIRE( conformances.method_for( table, log_type, sink_bool, requirements[0] ) == members[2] );

    // A requirement that does not mention `T` is answered by one method for both.
    REQUIRE( conformances.method_for( table, log_type, sink_i64, requirements[1] ) == members[3] );
    REQUIRE( conformances.method_for( table, log_type, sink_bool, requirements[1] ) == members[3] );
}

TEST_CASE( "conformances_substitute_a_refinement_per_instance", "[sema][interfaces][generic_interfaces]" )
{
    const Typed p(
        "interface sink<T> { void put( T value ); };\n"
        "interface counted<T> implements sink<T> { u64 count() const; };\n"
        "struct met implements counted<i32>, sink<i32> { i32 v; void put( i32 value ) { } u64 count() const { return 0; } };\n"
        "struct unmet implements counted<i32>, sink<i64> { i32 v; void put( i64 value ) { } u64 count() const { return 0; } "
        "};\n"
        "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "`unmet`'s `put` does not match the one `sink<i32>` declares" ) != std::string::npos );

    const Conformance_error& error = p.types().conformances().errors()[0];
    REQUIRE( error.problem == Conformance_problem::Mismatched );
    REQUIRE( error.implementer == p.nth( Node_kind::Struct_decl, 1 ) );
}

TEST_CASE( "conformances_quote_a_static_requirement_as_static", "[sema][interfaces]" )
{
    const Typed p( "interface Zero { static self zero( i32 n ); };\n"
                   "struct Sq implements Zero { f64 s; };\n"
                   "struct Bad implements Zero { f64 s; static Bad zero() { return Bad { 0.0 }; } };\n"
                   "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "add `static Sq zero(i32 n)` to `Sq`" ) != std::string::npos );
    REQUIRE( p.rendered().find( "`Zero` wants `static Bad zero(i32 n)`" ) != std::string::npos );
}

// Each was refused already, so the conformance stage adds nothing to it.
TEST_CASE( "conformances_do_not_cascade_from_an_earlier_error", "[sema][interfaces]" )
{
    const char* cases[] = {
        "interface Shape { T pick<T>( T a ); };\nstruct Sq implements Shape { f64 s; };",
        "struct Sq implements i32 { f64 s; };",
        "struct Sq implements Nope { f64 s; };",
    };

    for( const char* c : cases )
    {
        const Typed p( std::string( c ) + "\ni32 main() { return 0; }" );

        INFO( c << "\n" << p.rendered() );
        const std::string rendered = p.rendered();
        std::size_t       reported = 0;
        for( std::size_t at = rendered.find( "error" ); at != std::string::npos; at = rendered.find( "error", at + 1 ) )
        {
            ++reported;
        }

        REQUIRE( reported == 1 ); // the resolver's or the signatures', whichever refused it
        REQUIRE( p.types().conformances().errors().empty() );
    }
}

TEST_CASE( "conformances_substitute_the_implementer_for_self", "[sema][interfaces][self]" )
{
    const std::string same =
        "interface Same { bool same( const ref self other ) const; self larger( const ref self other ) const; };\n";

    SECTION( "written as the implementer's name" )
    {
        const Typed p(
            same + "struct P implements Same { i32 v;\n"
                   "    bool same( const ref P other ) const { return v == other.v; }\n"
                   "    P larger( const ref P other ) const { return P { v + other.v }; }\n"
                   "};\n"
                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.types().conformances().all().size() == 5 ); // and the prelude's four
    }

    SECTION( "a generic implementer answers with its open form" )
    {
        const Typed p(
            same + "struct B<T> implements Same where T : copyable { T v;\n"
                   "    bool same( const ref B<T> other ) const { return false; }\n"
                   "    B<T> larger( const ref B<T> other ) const { return B { other.v }; }\n"
                   "};\n"
                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Refused where it is written, so the conformance does not report the method a second time.
    SECTION( "the implementer may not write `self`" )
    {
        const Typed p(
            same + "struct P implements Same { i32 v;\n"
                   "    bool same( const ref self other ) const { return true; }\n"
                   "    P larger( const ref P other ) const { return P { v }; }\n"
                   "};\n"
                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "write `P`, the type's own name" ) != std::string::npos );
        REQUIRE( p.types().conformances().errors().empty() );
    }

    SECTION( "a missing method's help is the requirement as the implementer must write it" )
    {
        const Typed p(
            same + "struct P implements Same { i32 v; P larger( const ref P other ) const { return P { v }; } };\n"
                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "add `bool same(const ref P other) const` to `P`" ) != std::string::npos );
    }

    SECTION( "a mismatch quotes the requirement as the implementer must write it" )
    {
        const Typed p(
            same + "struct P implements Same { i32 v;\n"
                   "    bool same( const ref i32 other ) const { return true; }\n"
                   "    P larger( const ref P other ) const { return P { v }; }\n"
                   "};\n"
                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Same` wants `bool same(const ref P other) const`" ) != std::string::npos );

        const std::span<const Conformance_error> errors = p.types().conformances().errors();
        REQUIRE( errors.size() == 1 );
        REQUIRE( p.types().table().name( errors[0].self ) == "P" );
    }

    SECTION( "another implementer's type does not answer for `self`" )
    {
        const Typed p(
            same + "struct Q { i32 v; };\n"
                   "struct P implements Same { i32 v;\n"
                   "    bool same( const ref Q other ) const { return true; }\n"
                   "    P larger( const ref P other ) const { return P { v }; }\n"
                   "};\n"
                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`P`'s `same` does not match the one `Same` declares" ) != std::string::npos );
    }

    SECTION( "a method that is not `const` points the caller at the bound" )
    {
        const Typed p( "interface Same { bool same( const ref self other ) const; };\n"
                       "struct P implements Same { i32 v; bool same( const ref P other ) { return true; } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a `const ref T` with `T : Same` may call it" ) != std::string::npos );
    }
}

// A block conforms the type after `for`: its methods answer, and the type's own do not.
TEST_CASE( "conformances_check_an_implements_block", "[sema][interfaces][implements_for]" )
{
    const std::string hashable = "interface Hashable { u64 hash() const; };\n";

    // The struct is another package's, since a package's own declares what it implements.
    SECTION( "a built-in, a struct and an enum, each answered by its block" )
    {
        Typed p(
            { { "main.kl",
                "import kl::point;\n" + hashable +
                    "enum Colour { Red, Green };\n"
                    "implements Hashable for i32 { u64 hash() const { return 1; } };\n"
                    "implements Hashable for kl::P { u64 hash() const { return wrap<u64>( this.v ); } };\n"
                    "implements Hashable for Colour { u64 hash() const { return 3; } };\n"
                    "i32 main() { return 0; }\n" },
              { "kl/point.kl", "struct P { i32 v; };\n" } },
            prelude_source()
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Type_id interface = p.types().type_of( p.nth( Node_kind::Interface_decl, 0 ) );
        const char*   types[]   = { "i32", "kl::P", "Colour" };

        REQUIRE( p.types().conformances().all().size() == 7 ); // and the prelude's four

        for( std::size_t i = 0; i < 3; ++i )
        {
            const Node_id block = p.nth( Node_kind::Implements_decl, i );

            INFO( types[i] );
            REQUIRE( p.type_name( block ) == types[i] ); // the block's node records the type it is for
            REQUIRE( p.types().conformances().declares( p.types().table(), p.types().type_of( block ), interface ) );

            const Conformance* conformance = p.types().conformances().find( block, interface );
            REQUIRE( conformance != nullptr );
            REQUIRE( conformance->methods.size() == 1 );
            REQUIRE( conformance->methods[0] == p.ast().members( block )[0] );
        }
    }

    // `P`'s own `hash` is a method of `P`, not an answer: only an `implements` on `P` makes it one.
    SECTION( "the type's own method of the name does not answer" )
    {
        const Typed p(
            { { "main.kl", "import kl::point;\n" + hashable + "implements Hashable for kl::P { };\ni32 main() { return 0; }\n"
              },
              { "kl/point.kl", "struct P { i32 v; u64 hash() const { return 1; } };\n" } },
            prelude_source()
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`P` implements `Hashable` but has no method `hash`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "add `u64 hash() const` to the block" ) != std::string::npos );
    }

    SECTION( "a method the interface does not declare, at its name" )
    {
        const Typed p(
            hashable + "implements Hashable for i32 { u64 hash() const { return 1; } void reset() { } };\n"
                       "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Hashable` has no method `reset`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "a block holds only the methods of the interface it implements" ) != std::string::npos );
        REQUIRE( p.rendered().find( ":2:" ) != std::string::npos ); // at `reset`, on the block's line
    }

    SECTION( "a mismatch names the type, not the block" )
    {
        const Typed p(
            hashable + "implements Hashable for i32 { u32 hash() const { return 1; } };\n"
                       "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`i32`'s `hash` does not match the one `Hashable` declares" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`Hashable` wants `u64 hash() const`" ) != std::string::npos );
    }

    SECTION( "a type conforms once, whichever way it says so" )
    {
        const char* sources[] = {
            "implements Hashable for i32 { u64 hash() const { return 1; } };\n"
            "implements Hashable for i32 { u64 hash() const { return 2; } };\n",
            "enum Colour { Red }; implements Hashable for Colour { u64 hash() const { return 1; } };\n"
            "implements Hashable for Colour { u64 hash() const { return 2; } };\n",
        };

        for( const char* source : sources )
        {
            const Typed p( hashable + source + "i32 main() { return 0; }" );

            INFO( p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "already implements `Hashable`" ) != std::string::npos );
            REQUIRE( p.rendered().find( ":3:" ) != std::string::npos ); // at the second
        }
    }

    // A struct's own `implements` and a block for it are in two packages, which import each other.
    SECTION( "a struct's `implements` and the interface's block for it" )
    {
        const Typed p(
            { { "main.kl", "import kl::hash;\ni32 main() { return 0; }\n" },
              { "kl/hash.kl",
                "import geo::point;\n"
                "interface Hashable { u64 hash() const; };\n"
                "implements Hashable for geo::Q { u64 hash() const { return 2; } };\n" },
              { "geo/point.kl",
                "import kl::hash;\nstruct Q implements kl::Hashable { i32 v; u64 hash() const { return 1; } };\n" } },
            prelude_source()
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Q` already implements `Hashable`" ) != std::string::npos );
    }

    // A refused block conforms nothing, so nothing is said about its methods either.
    SECTION( "a block whose type is refused is not checked" )
    {
        const Typed p(
            hashable + "implements Hashable for fn( i32 ) -> i32 { };\n"
                       "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot implement an interface through a block" ) != std::string::npos );
        REQUIRE( p.types().conformances().all().size() == 4 ); // the prelude's four
    }
}

} // namespace keel
#endif
