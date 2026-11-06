// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/size_suffixes.h"
#include <fmt/format.h>
#include <algorithm>
#include <set>

namespace keel::sema
{

namespace
{

bool is_constant_size( const Type_table& table, Type_id id )
{
    return table.is_size( id ) && table.get( id ).width == 0;
}

} // namespace

void Size_suffixes::check(
    std::span<const Generic_call> roots, std::span<const Generic_call> calls, std::span<const Generic_call> implied
)
{
    collect( ast_.root(), Node_id {} );

    if( suffixes_.empty() )
    {
        return;
    }

    std::unordered_map<u32, std::vector<const Generic_call*>> out_of;

    for( const auto edges : { calls, implied } )
    {
        for( const Generic_call& edge : edges )
        {
            out_of[edge.from.v].push_back( &edge );
        }
    }

    // Every edge whose sizes are all written: those out of non-generic code, and `f<4>` inside a
    // generic. Earliest in the source first, so an instance two of them reach names the first.
    std::vector<std::pair<Instance, Span>> starts;

    for( const auto edges : { roots, calls, implied } )
    {
        for( const Generic_call& edge : edges )
        {
            if( std::optional<Instance> instance = reach( edge, {} ) )
            {
                starts.emplace_back( std::move( *instance ), edge.at );
            }
        }
    }

    std::ranges::stable_sort(
        starts, {}, []( const auto& start ) { return std::pair { start.second.file.v, start.second.start }; }
    );

    std::set<std::vector<u32>> seen;

    for( auto& start : starts )
    {
        std::vector<std::pair<Instance, Span>> pending { std::move( start ) };

        while( !pending.empty() )
        {
            auto [instance, origin] = std::move( pending.back() );
            pending.pop_back();

            std::vector<u32> key { instance.declaration.v };
            for( const Type_id size : instance.sizes )
            {
                key.push_back( size.v );
            }

            if( !seen.insert( std::move( key ) ).second || report( instance, origin ) )
            {
                continue;
            }

            const Bindings bindings = bindings_of( instance );

            for( const Generic_call* edge : out_of[instance.declaration.v] )
            {
                if( std::optional<Instance> reached = reach( *edge, bindings ) )
                {
                    const bool writes = std::ranges::any_of(
                        edge->arguments, [&]( Type_id argument ) { return is_constant_size( table_, argument ); }
                    );
                    pending.emplace_back( std::move( *reached ), writes ? edge->at : origin );
                }
            }
        }
    }
}

// Every suffix whose extent is still a computation, under the generic that instantiates it.
void Size_suffixes::collect( Node_id id, Node_id owner )
{
    const Node_kind kind = ast_.kind( id );

    if( is_function_like( kind ) )
    {
        owner = id;
    }
    else if( kind == Node_kind::Field_decl )
    {
        owner = ast_.enclosing_aggregate( id );
    }

    if( owner.is_valid() && id.v < types_.size() && types_[id.v].is_valid() && table_.is_size( types_[id.v] ) &&
        table_.get( types_[id.v] ).width != 0 )
    {
        suffixes_[owner.v].push_back( Suffix { .node = id, .extent = types_[id.v] } );
    }

    for( const Node_id child : ast_.children( id ) )
    {
        if( child.is_valid() )
        {
            collect( child, owner );
        }
    }
}

// The instance an edge makes under its caller's sizes, or none when it takes no size or one is
// still open.
std::optional<Size_suffixes::Instance> Size_suffixes::reach( const Generic_call& edge, const Bindings& caller )
{
    Instance instance { .declaration = edge.to, .sizes = {} };
    bool     sized = false;

    for( const Type_id argument : edge.arguments )
    {
        if( !table_.is_size( argument ) && !table_.is_size_parameter( argument ) )
        {
            instance.sizes.push_back( Type_id {} );
            continue;
        }

        const std::optional<u64> value = table_.evaluate( argument, caller );
        if( !value )
        {
            return std::nullopt;
        }

        instance.sizes.push_back( table_.size( *value ) );
        sized = true;
    }

    return sized ? std::optional { std::move( instance ) } : std::nullopt;
}

Bindings Size_suffixes::bindings_of( const Instance& instance ) const
{
    const std::vector<Node_id> parameters = ast_.type_parameters( ast_.type_param_list( instance.declaration ) );

    Bindings bindings;

    for( std::size_t i = 0; i < parameters.size() && i < instance.sizes.size(); ++i )
    {
        if( instance.sizes[i].is_valid() && parameters[i].v < types_.size() && types_[parameters[i].v].is_valid() )
        {
            bindings.emplace( types_[parameters[i].v].v, instance.sizes[i] );
        }
    }

    return bindings;
}

// Whether the instance holds a bad suffix, reporting the first.
bool Size_suffixes::report( const Instance& instance, Span origin )
{
    const auto found = suffixes_.find( instance.declaration.v );
    if( found == suffixes_.end() )
    {
        return false;
    }

    const Bindings bindings = bindings_of( instance );

    for( const Suffix& suffix : found->second )
    {
        std::vector<Type_id> read;
        parameters_of( suffix.extent, read );

        if( !std::ranges::all_of( read, [&]( Type_id parameter ) { return bindings.contains( parameter.v ); } ) )
        {
            continue; // a method's own size, bound per call rather than per instance
        }

        const std::optional<u64> value = table_.evaluate( suffix.extent, bindings );
        if( value && *value != 0 )
        {
            continue;
        }

        std::string when;

        for( const Type_id parameter : read )
        {
            when += fmt::format(
                "{}`{}` is {}", when.empty() ? "" : " and ", table_.name( parameter ), *table_.evaluate( parameter, bindings )
            );
        }

        // The caller's size to point at is one the failing step reads: a divisor's, for a division.
        const Type_id step    = value ? suffix.extent : failing( suffix.extent, bindings );
        const Size_op op      = Size_op( table_.get( step ).width );
        const bool    divides = !value && ( op == Size_op::Divide || op == Size_op::Remainder );

        std::vector<Type_id> blamed;
        parameters_of( divides ? table_.get( step ).arguments[1] : step, blamed );

        std::vector<u64> values;
        for( const Type_id parameter : blamed )
        {
            values.push_back( *table_.evaluate( parameter, bindings ) );
        }

        const std::string reason = value                     ? "is 0"
                                   : divides                 ? "divides by 0"
                                   : op == Size_op::Subtract ? "is below 0"
                                                             : "overflows a `u64`";

        reporter_.error_at(
            written( origin, values ),
            fmt::format(
                "`{}` makes {}", name_of( instance ), value ? "an array of 0 elements" : "an array whose size " + reason
            ),
            fmt::format(
                "`{}` at {} {} when {}",
                table_.name( suffix.extent ),
                reporter_.position( ast_.span( suffix.node ) ),
                reason,
                when
            )
        );
        return true;
    }

    return false;
}

// `Ring<1>`. A method that shares its aggregate's parameters is named by the aggregate.
std::string Size_suffixes::name_of( const Instance& instance ) const
{
    const Node_id declaration = instance.declaration;
    const Node_id aggregate   = ast_.enclosing_aggregate( declaration );
    const Node_id named = aggregate.is_valid() && !ast_.own_type_param_list( declaration ).is_valid() ? aggregate : declaration;

    const std::vector<Node_id> parameters = ast_.type_parameters( ast_.type_param_list( declaration ) );

    std::string arguments;

    for( std::size_t i = 0; i < parameters.size() && i < instance.sizes.size(); ++i )
    {
        arguments += i == 0 ? "" : ", ";
        arguments += instance.sizes[i].is_valid() ? std::string( table_.name( instance.sizes[i] ) )
                                                  : std::string( interner_.text( ast_.name( parameters[i] ) ) );
    }

    return fmt::format( "{}<{}>", interner_.text( ast_.name( named ) ), arguments );
}

// The size the caller wrote, inside the edge's span: the first whose value the suffix read, else
// the first at all, else the whole edge when the sizes were deduced.
Span Size_suffixes::written( Span origin, std::span<const u64> values ) const
{
    std::optional<Span> any;

    for( u32 i = 0; i < ast_.node_count() && i < types_.size(); ++i )
    {
        const Span span = ast_.span( Node_id { i } );

        if( span.file != origin.file || span.start < origin.start || span.end > origin.end || !types_[i].is_valid() ||
            !is_constant_size( table_, types_[i] ) )
        {
            continue;
        }

        if( std::ranges::find( values, table_.get( types_[i] ).count ) != values.end() )
        {
            return span;
        }

        any = any ? any : span;
    }

    return any.value_or( origin );
}

// The innermost step of a computed size that has no value.
Type_id Size_suffixes::failing( Type_id size, const Bindings& bindings ) const
{
    for( const Type_id operand : table_.get( size ).arguments )
    {
        if( !table_.evaluate( operand, bindings ) )
        {
            return failing( operand, bindings );
        }
    }

    return size;
}

// The size parameters a computed size reads, each once, in the order written.
void Size_suffixes::parameters_of( Type_id size, std::vector<Type_id>& into ) const
{
    const Type& described = table_.get( size );

    if( described.kind == Type_kind::Parameter )
    {
        if( std::ranges::find( into, size ) == into.end() )
        {
            into.push_back( size );
        }
        return;
    }

    if( described.kind == Type_kind::Size && described.width != 0 )
    {
        parameters_of( described.arguments[0], into );
        parameters_of( described.arguments[1], into );
    }
}

} // namespace keel::sema
