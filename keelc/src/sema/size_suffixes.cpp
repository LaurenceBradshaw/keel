// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/size_suffixes.h"
#include <fmt/format.h>
#include <fmt/ranges.h>
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

// Whether the edge writes a size of its own, which makes it the origin of what it reaches.
bool writes( const Type_table& table, const Generic_call& edge )
{
    const auto constant = [&]( Type_id argument ) { return is_constant_size( table, argument ) || table.is_dyn( argument ); };
    return std::ranges::any_of(
        edge.arguments,
        [&]( Type_id argument )
        {
            return constant( argument ) ||
                   ( table.is_pack( argument ) && std::ranges::all_of( table.get( argument ).arguments, constant ) );
        }
    );
}

} // namespace

void Size_suffixes::check(
    std::span<const Generic_call> roots, std::span<const Generic_call> calls, std::span<const Generic_call> implied
)
{
    collect( ast_.root(), Node_id {} );

    if( suffixes_.empty() && conditions_.empty() )
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
            if( std::optional<Instance> instance = reach( edge, {}, nullptr ) )
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
                if( std::optional<Instance> reached = reach( *edge, bindings, &instance ) )
                {
                    pending.emplace_back( std::move( *reached ), writes( table_, *edge ) ? edge->at : origin );
                }
            }
        }
    }
}

// Every suffix whose extent is still a computation, under the generic that instantiates it.
void Size_suffixes::collect( Node_id id, Node_id owner, Node_kind parent )
{
    const Node_kind kind = ast_.kind( id );

    // A method's list repeats its aggregate's clauses, which the aggregate's instance checks.
    if( kind == Node_kind::Function_decl || is_aggregate( kind ) || kind == Node_kind::Enum_decl )
    {
        collect_conditions( id );
    }
    if( kind == Node_kind::Size_condition )
    {
        return;
    }

    if( is_function_like( kind ) )
    {
        owner = id;
    }
    else if( kind == Node_kind::Field_decl )
    {
        owner = ast_.enclosing_aggregate( id );
    }

    if( owner.is_valid() && id.v < types_.size() && types_[id.v].is_valid() &&
        ( ( table_.is_size( types_[id.v] ) && table_.get( types_[id.v] ).width != 0 && !table_.is_dyn( types_[id.v] ) ) ||
          table_.is_shape( types_[id.v] ) ||
          ( parent == Node_kind::Array_type && table_.is_size_parameter( types_[id.v] ) &&
            !table_.is_pack_parameter( types_[id.v] ) ) ) )
    {
        suffixes_[owner.v].push_back(
            Suffix { .node = id, .extent = types_[id.v], .argument = parent == Node_kind::Type_arg_list }
        );
    }

    // A type's size argument, which a run-time size reaches only where its parameter is marked.
    const bool lists = kind == Node_kind::Generic_type || kind == Node_kind::Struct_literal || kind == Node_kind::Call_expr ||
                       kind == Node_kind::Path_expr;
    const Node_id named       = !lists                              ? Node_id {}
                                : kind == Node_kind::Generic_type   ? ast_.child( id, 0 )
                                : kind == Node_kind::Struct_literal ? id
                                : kind == Node_kind::Call_expr      ? ast_.callee( id )
                                                                    : ast_.child( id, 0 );
    const Node_id declaration = named.is_valid() ? resolution_.declaration_of( named ) : Node_id {};
    if( owner.is_valid() && declaration.is_valid() && is_aggregate( ast_.kind( declaration ) ) &&
        ast_.type_arg_list( kind == Node_kind::Generic_type || kind == Node_kind::Struct_literal ? id : named ).is_valid() )
    {
        const std::vector<Node_id>     parameters = ast_.type_parameters( ast_.type_param_list( declaration ) );
        const std::span<const Node_id> arguments =
            ast_.type_args( kind == Node_kind::Generic_type || kind == Node_kind::Struct_literal ? id : named );
        for( std::size_t i = 0; i < arguments.size() && !parameters.empty(); ++i )
        {
            const Node_id parameter = parameters[std::min( i, parameters.size() - 1 )];
            const Type_id argument  = arguments[i].v < types_.size() ? types_[arguments[i].v] : Type_id {};
            if( !ast_.takes_dyn( parameter ) && argument.is_valid() &&
                ( table_.is_size_parameter( argument ) || table_.is_shape( argument ) ||
                  ( table_.is_size( argument ) && table_.get( argument ).width != 0 && !table_.is_dyn( argument ) ) ) )
            {
                suffixes_[owner.v].push_back( Suffix {
                    .node = arguments[i], .extent = argument, .argument = true, .takes_dyn = false, .taker = parameter
                } );
            }
        }
    }

    for( const Node_id child : ast_.children( id ) )
    {
        if( child.is_valid() )
        {
            collect( child, owner, kind );
        }
    }
}

void Size_suffixes::collect_conditions( Node_id declaration )
{
    for( const Node_id clause : ast_.where_clauses( ast_.type_param_list( declaration ) ) )
    {
        if( ast_.kind( clause ) != Node_kind::Size_condition )
        {
            continue;
        }

        const Node_id comparison = ast_.child( clause, 0 );
        const Node_id lhs        = ast_.lhs( comparison );
        const Node_id rhs        = ast_.rhs( comparison );

        const auto size = [&]( Node_id side )
        { return side.v < types_.size() && types_[side.v].is_valid() && !table_.is_error( types_[side.v] ); };

        if( size( lhs ) && size( rhs ) )
        {
            conditions_[declaration.v].push_back(
                Condition { .node = clause, .op = ast_.op( comparison ), .lhs = types_[lhs.v], .rhs = types_[rhs.v] }
            );
        }
    }
}

// The instance an edge makes under its caller's sizes, or none when it takes no size or one is
// still open.
std::optional<Size_suffixes::Instance>
Size_suffixes::reach( const Generic_call& edge, const Bindings& bindings, const Instance* caller )
{
    Instance instance { .declaration = edge.to, .sizes = {}, .sources = {}, .types = {} };
    bool     sized = false;

    // A size passed straight through keeps the caller's source, unless this edge is a new origin.
    const std::vector<Node_id> passed = caller && !writes( table_, edge )
                                            ? ast_.type_parameters( ast_.type_param_list( caller->declaration ) )
                                            : std::vector<Node_id> {};

    // A pack passed on, or a shape function's, is its sizes under the caller's bindings: spread,
    // or whole for a callee of several packs.
    std::vector<Type_id> callee;
    for( const Node_id parameter : ast_.type_parameters( ast_.type_param_list( edge.to ) ) )
    {
        callee.push_back( parameter.v < types_.size() ? types_[parameter.v] : Type_id {} );
    }
    const bool whole = table_.keeps_packs( callee );

    std::vector<Type_id> arguments;
    for( const Type_id argument : edge.arguments )
    {
        if( !table_.is_pack_parameter( argument ) && !table_.is_shape( argument ) && !table_.is_pack( argument ) )
        {
            arguments.push_back( argument );
            continue;
        }
        // Constants, or `dyn`, which reaches the callee as itself.
        std::vector<Type_id> read;
        parameters_of( argument, read );
        if( !std::ranges::all_of( read, [&]( Type_id parameter ) { return bindings.contains( parameter.v ); } ) )
        {
            return std::nullopt;
        }
        const Type_id under = table_.substitute( argument, bindings );
        if( !table_.is_pack( under ) ||
            !std::ranges::all_of(
                table_.get( under ).arguments,
                [&]( Type_id size ) { return is_constant_size( table_, size ) || size == table_.dyn(); }
            ) )
        {
            return std::nullopt;
        }
        const std::vector<Type_id> each( table_.get( under ).arguments.begin(), table_.get( under ).arguments.end() );
        if( whole )
        {
            arguments.push_back( table_.pack( each ) );
        }
        else
        {
            arguments.insert( arguments.end(), each.begin(), each.end() );
        }
    }

    for( std::size_t i = 0; i < arguments.size(); ++i )
    {
        const Type_id argument = arguments[i];

        if( table_.is_pack( argument ) )
        {
            instance.sizes.push_back( argument );
            instance.sources.emplace_back();
            instance.types.emplace_back();
            sized = true;
            continue;
        }

        if( !table_.is_size( argument ) && !table_.is_size_parameter( argument ) )
        {
            instance.sizes.push_back( Type_id {} );
            instance.sources.emplace_back();
            instance.types.push_back( argument );
            continue;
        }

        const std::optional<u64> value = table_.evaluate( argument, bindings );
        if( !value && table_.reaches_dyn( argument, bindings ) )
        {
            instance.sizes.push_back( table_.dyn() );
            instance.sources.emplace_back();
            instance.types.emplace_back();
            sized = true;
            continue;
        }
        if( !value )
        {
            return std::nullopt;
        }

        std::optional<Source> source;
        if( is_constant_size( table_, argument ) )
        {
            const auto earlier = std::ranges::count( arguments.begin(), arguments.begin() + std::ptrdiff_t( i ), argument );
            source             = Source { .value = *value, .occurrence = u32( earlier ) };
        }
        for( std::size_t p = 0;
             p < passed.size() && p < caller->sources.size() && !table_.is_pack_parameter( types_[passed[p].v] );
             ++p )
        {
            if( passed[p].v < types_.size() && types_[passed[p].v] == argument )
            {
                source = caller->sources[p];
            }
        }

        instance.sizes.push_back( table_.size( *value ) );
        instance.sources.push_back( source );
        instance.types.emplace_back();
        sized = true;
    }

    return sized ? std::optional { std::move( instance ) } : std::nullopt;
}

Bindings Size_suffixes::bindings_of( const Instance& instance ) const
{
    std::vector<Type_id> parameters;
    for( const Node_id parameter : ast_.type_parameters( ast_.type_param_list( instance.declaration ) ) )
    {
        parameters.push_back( parameter.v < types_.size() ? types_[parameter.v] : Type_id {} );
    }
    if( std::ranges::any_of( parameters, []( Type_id parameter ) { return !parameter.is_valid(); } ) )
    {
        return {};
    }

    // Only the sizes are bound: a type argument stays its parameter.
    std::vector<Type_id> arguments;
    for( std::size_t i = 0; i < instance.sizes.size(); ++i )
    {
        arguments.push_back( instance.sizes[i].is_valid() ? instance.sizes[i] : table_.builtin( Type_kind::Void ) );
    }
    Bindings bindings = table_.bind( parameters, arguments );
    std::erase_if( bindings, [&]( const auto& each ) { return !table_.is_size_parameter( Type_id { each.first } ); } );
    return bindings;
}

// Whether the instance fails a condition or holds a bad suffix, reporting the first.
bool Size_suffixes::report( const Instance& instance, Span origin )
{
    const Bindings bindings = bindings_of( instance );

    if( report_condition( instance, origin, bindings ) )
    {
        return true;
    }

    const auto found = suffixes_.find( instance.declaration.v );
    if( found == suffixes_.end() )
    {
        return false;
    }

    for( const Suffix& suffix : found->second )
    {
        std::vector<Type_id> read;
        parameters_of( suffix.extent, read );

        if( !std::ranges::all_of( read, [&]( Type_id parameter ) { return bindings.contains( parameter.v ); } ) )
        {
            continue; // a method's own size, bound per call rather than per instance
        }

        // A run-time size may be an argument, to a parameter marked for one, but never an extent.
        const bool run_time = runtime( suffix.extent, bindings );
        if( suffix.taker.is_valid() && !run_time )
        {
            continue; // the unmarked parameter's own check; the rest is the argument's other entry
        }
        if( run_time )
        {
            if( suffix.argument && suffix.takes_dyn )
            {
                continue;
            }
            const Node_id     taker = suffix.taker;
            const Node_id     held  = taker.is_valid() ? owner_of_parameter( taker ) : Node_id {};
            const std::string named = held.is_valid() ? std::string( interner_.text( ast_.name( held ) ) ) : std::string();
            const std::string param = taker.is_valid() ? std::string( interner_.text( ast_.name( taker ) ) ) : std::string();
            reporter_.error_at(
                written_dyn( origin ),
                suffix.argument
                    ? fmt::format(
                          "`{}` gives `{}`'s `{}` a run-time size, which it does not take", name_of( instance ), named, param
                      )
                    : fmt::format( "`{}` makes an array whose length is `dyn`", name_of( instance ) ),
                suffix.argument ? fmt::format(
                                      "`{}` at {} is `dyn`; mark it `{}{} {}(dyn)` in `{}`",
                                      table_.name( suffix.extent ),
                                      reporter_.position( ast_.span( suffix.node ) ),
                                      interner_.text( ast_.name( ast_.children( taker )[0] ) ),
                                      table_.is_pack_parameter( types_[taker.v] ) ? "..." : "",
                                      param,
                                      named
                                  )
                                : fmt::format(
                                      "`{}` at {} is `dyn`, and an array's length is a constant",
                                      table_.name( suffix.extent ),
                                      reporter_.position( ast_.span( suffix.node ) )
                                  )
            );
            return true;
        }

        // A size argument may be 0; an array's extent may not.
        const bool               shape = table_.get( suffix.extent ).kind == Type_kind::Pack;
        const std::optional<u64> value = shape ? std::nullopt : table_.evaluate( suffix.extent, bindings );
        if( shape ? computes( suffix.extent, bindings ) : value && ( *value != 0 || suffix.argument ) )
        {
            continue;
        }

        std::string when;

        for( const Type_id parameter : read )
        {
            when += fmt::format(
                "{}`{}` is {}", when.empty() ? "" : " and ", table_.name( parameter ), value_of( parameter, bindings )
            );
        }

        // The caller's size to point at is one the failing step reads: a divisor's, for a division.
        const Type_id step    = value ? suffix.extent : failing( suffix.extent, bindings );
        const Size_op op      = Size_op( table_.get( step ).width );
        const bool    divides = !value && ( op == Size_op::Divide || op == Size_op::Remainder );

        const Span blamed = blame( instance, bindings, divides ? table_.get( step ).arguments[1] : step, origin );

        // A shape function that fails names what it does, whatever holds it.
        if( !value && ( op == Size_op::Broadcast || op == Size_op::Dropped || op == Size_op::Inserted ) )
        {
            reporter_.error_at(
                blamed,
                fmt::format( "`{}` {}", name_of( instance ), shape_failure( op ) ),
                fmt::format(
                    "`{}` at {} fails when {}", table_.name( step ), reporter_.position( ast_.span( suffix.node ) ), when
                )
            );
            return true;
        }

        const std::string reason = value                     ? "is 0"
                                   : divides                 ? "divides by 0"
                                   : op == Size_op::Subtract ? "is below 0"
                                                             : "overflows a `u64`";

        reporter_.error_at(
            blamed,
            fmt::format(
                "`{}` makes {}",
                name_of( instance ),
                suffix.argument || shape ? ( value ? "a size argument of 0" : "a size argument that " + reason )
                : value                  ? "an array of 0 elements"
                                         : "an array whose size " + reason
            ),
            fmt::format(
                "`{}` at {} {}{}",
                table_.name( suffix.extent ),
                reporter_.position( ast_.span( suffix.node ) ),
                reason,
                table_.is_size_parameter( suffix.extent ) ? "" : " when " + when
            )
        );
        return true;
    }

    return false;
}

// D78 (8): a side that cannot be computed fails its condition, as a bad suffix would.
bool Size_suffixes::report_condition( const Instance& instance, Span origin, const Bindings& bindings )
{
    const auto found = conditions_.find( instance.declaration.v );
    if( found == conditions_.end() )
    {
        return false;
    }

    for( const Condition& condition : found->second )
    {
        std::vector<Type_id> read;
        parameters_of( condition.lhs, read );
        parameters_of( condition.rhs, read );

        if( !std::ranges::all_of( read, [&]( Type_id parameter ) { return bindings.contains( parameter.v ); } ) )
        {
            continue;
        }

        // A run-time side is checked where it runs.
        if( runtime( condition.lhs, bindings ) || runtime( condition.rhs, bindings ) )
        {
            continue;
        }

        const std::optional<u64> lhs = table_.evaluate( condition.lhs, bindings );
        const std::optional<u64> rhs = table_.evaluate( condition.rhs, bindings );

        if( lhs && rhs )
        {
            const bool holds = condition.op == Token_kind::Less            ? *lhs < *rhs
                               : condition.op == Token_kind::Less_equal    ? *lhs <= *rhs
                               : condition.op == Token_kind::Greater       ? *lhs > *rhs
                               : condition.op == Token_kind::Greater_equal ? *lhs >= *rhs
                               : condition.op == Token_kind::Equal_equal   ? *lhs == *rhs
                                                                           : *lhs != *rhs;
            if( holds )
            {
                continue;
            }
        }

        std::string when;
        for( const Type_id parameter : read )
        {
            when += fmt::format(
                "{}`{}` is {}", when.empty() ? "" : " and ", table_.name( parameter ), value_of( parameter, bindings )
            );
        }

        const std::string written_condition =
            fmt::format( "{} {} {}", spelled( condition.lhs ), token_kind_spelling( condition.op ), spelled( condition.rhs ) );

        reporter_.error_at(
            blame( instance, bindings, read.empty() ? condition.lhs : read.front(), origin ),
            fmt::format( "`{}` needs `{}`", name_of( instance ), written_condition ),
            fmt::format(
                "the condition at {} {} when {}",
                reporter_.position( ast_.span( condition.node ) ),
                lhs && rhs ? "fails" : "cannot be computed",
                when.empty() ? "nothing varies" : when
            )
        );
        return true;
    }

    return false;
}

// The size the caller wrote for the first parameter `size` reads.
Span Size_suffixes::blame( const Instance& instance, const Bindings& bindings, Type_id size, Span origin ) const
{
    std::vector<Type_id> blamed;
    parameters_of( size, blamed );

    const std::vector<Node_id> parameters = ast_.type_parameters( ast_.type_param_list( instance.declaration ) );

    std::vector<u64>    values;
    std::vector<Source> sources;
    for( const Type_id parameter : blamed )
    {
        if( table_.is_pack_parameter( parameter ) )
        {
            const std::optional<std::vector<u64>> sizes = table_.evaluate_pack( parameter, bindings );
            values.insert( values.end(), sizes->begin(), sizes->end() );
        }
        else
        {
            values.push_back( *table_.evaluate( parameter, bindings ) );
        }

        for( std::size_t p = 0; p < parameters.size() && p < instance.sources.size(); ++p )
        {
            if( parameters[p].v < types_.size() && types_[parameters[p].v] == parameter && instance.sources[p] )
            {
                sources.push_back( *instance.sources[p] );
            }
        }
    }

    return written( origin, values, sources );
}

// The aggregate a type parameter belongs to.
Node_id Size_suffixes::owner_of_parameter( Node_id parameter ) const
{
    for( u32 i = 0; i < ast_.node_count(); ++i )
    {
        const Node_id each { i };
        if( !is_aggregate( ast_.kind( each ) ) )
        {
            continue;
        }
        const std::vector<Node_id> parameters = ast_.type_parameters( ast_.type_param_list( each ) );
        if( std::ranges::find( parameters, parameter ) != parameters.end() )
        {
            return each;
        }
    }
    return Node_id {};
}

// Whether a size or pack comes out `dyn`, or holds one, under `bindings`.
bool Size_suffixes::runtime( Type_id size, const Bindings& bindings ) const
{
    return table_.reaches_dyn( size, bindings );
}

// The `dyn` the caller wrote, else the whole edge, whose sizes came from values.
Span Size_suffixes::written_dyn( Span origin ) const
{
    for( u32 i = 0; i < ast_.node_count(); ++i )
    {
        const Span span = ast_.span( Node_id { i } );
        if( ast_.kind( Node_id { i } ) == Node_kind::Dyn_size && span.file == origin.file && span.start >= origin.start &&
            span.end <= origin.end )
        {
            return span;
        }
    }
    return origin;
}

// `R`, `0` or `(R * C)`, as a clause writes it.
std::string Size_suffixes::spelled( Type_id size ) const
{
    const std::string name( table_.name( size ) );
    return table_.get( size ).kind == Type_kind::Size && is_arithmetic( Size_op( table_.get( size ).width ) )
               ? fmt::format( "({})", name )
               : name;
}

// `2`, or a pack's `2, 3`.
std::string Size_suffixes::value_of( Type_id parameter, const Bindings& bindings ) const
{
    if( !table_.is_pack_parameter( parameter ) )
    {
        return fmt::format( "{}", *table_.evaluate( parameter, bindings ) );
    }
    const std::optional<std::vector<u64>> sizes = table_.evaluate_pack( parameter, bindings );
    return sizes->empty() ? "empty" : fmt::format( "{}", fmt::join( *sizes, ", " ) );
}

// Whether a size, or a pack, has a value.
bool Size_suffixes::computes( Type_id size, const Bindings& bindings ) const
{
    const Type& described = table_.get( size );
    return described.kind == Type_kind::Pack || table_.is_pack_parameter( size )
               ? table_.evaluate_pack( size, bindings ).has_value()
               : table_.evaluate( size, bindings ).has_value();
}

// `Ring<1>`. A method that shares its aggregate's parameters is named by the aggregate.
std::string Size_suffixes::name_of( const Instance& instance ) const
{
    const Node_id declaration = instance.declaration;
    const Node_id aggregate   = ast_.enclosing_aggregate( declaration );
    const Node_id named = aggregate.is_valid() && !ast_.own_type_param_list( declaration ).is_valid() ? aggregate : declaration;

    const std::vector<Node_id> parameters = ast_.type_parameters( ast_.type_param_list( declaration ) );

    std::string arguments;

    for( std::size_t i = 0; i < instance.sizes.size(); ++i )
    {
        arguments += i == 0 ? "" : ", ";
        const Type_id written = instance.sizes[i].is_valid() ? instance.sizes[i] : instance.types[i];
        arguments += written.is_valid() || i >= parameters.size() ? std::string( table_.name( written ) )
                                                                  : std::string( interner_.text( ast_.name( parameters[i] ) ) );
    }

    return fmt::format( "{}<{}>", interner_.text( ast_.name( named ) ), arguments );
}

// The size the caller wrote, inside the edge's span: the one a source names, else the first whose
// value the suffix read, else the first at all, else the whole edge when the sizes were deduced.
Span Size_suffixes::written( Span origin, std::span<const u64> values, std::span<const Source> sources ) const
{
    std::vector<std::pair<Span, u64>> sizes;

    for( u32 i = 0; i < ast_.node_count() && i < types_.size(); ++i )
    {
        const Span span = ast_.span( Node_id { i } );

        if( span.file == origin.file && span.start >= origin.start && span.end <= origin.end && types_[i].is_valid() &&
            is_constant_size( table_, types_[i] ) )
        {
            sizes.emplace_back( span, table_.get( types_[i] ).count );
        }
    }

    std::ranges::sort( sizes, {}, []( const auto& size ) { return size.first.start; } );

    for( const Source& source : sources )
    {
        u32 seen = 0;
        for( const auto& [span, value] : sizes )
        {
            if( value == source.value && seen++ == source.occurrence )
            {
                return span;
            }
        }
    }

    for( const auto& [span, value] : sizes )
    {
        if( std::ranges::find( values, value ) != values.end() )
        {
            return span;
        }
    }

    return sizes.empty() ? origin : sizes.front().first;
}

// The innermost step of a computed size that has no value.
Type_id Size_suffixes::failing( Type_id size, const Bindings& bindings ) const
{
    const Type& described = table_.get( size );
    if( described.kind == Type_kind::Size && described.width == 0 )
    {
        return size;
    }
    for( const Type_id operand : described.arguments )
    {
        if( !computes( operand, bindings ) )
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

    if( ( described.kind == Type_kind::Size && described.width != 0 ) || described.kind == Type_kind::Pack )
    {
        for( const Type_id operand : described.arguments )
        {
            parameters_of( operand, into );
        }
    }
}

} // namespace keel::sema
