// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/solve_linear_maps.h"
#include <algorithm>
#include <map>
#include <optional>
#include <utility>
#include "linear/circuit_cache.h"
#include "linear/lower_circuit.h"
#include "linear/recognise.h"
#include "linear/sums.h"

namespace keel
{
namespace
{
class Places
{
public:
    Places( Function& func, const Type_table& types, Literal_pool& literals, Span span )
        : func_( func ),
          literals_( literals ),
          index_type_( types.integer( 64, false ) ),
          span_( span )
    {
    }

    // No place for the input fixed at 1.
    Value_place place( const Value_path& v )
    {
        if( !v.real.local.is_valid() )
        {
            return {};
        }
        return { place( v.real ), v.imaginary.local.is_valid() ? place( v.imaginary ) : Place {} };
    }

    // The arrays holding a contiguous run of values, value j element j of each part, the real part alone when `real`;
    // nothing when they don't.
    std::optional<Value_place> arrays( const std::vector<Value_path>& v, bool real = false )
    {
        const auto run = [&]( auto part ) -> std::optional<Param_path>
        {
            Param_path whole = part( v[0] );
            if( !whole.local.is_valid() || whole.steps.empty() )
            {
                return std::nullopt;
            }
            whole.steps.pop_back();
            for( u64 j = 0; j < v.size(); ++j )
            {
                const Param_path& p = part( v[j] );
                if( p.local != whole.local || p.through != whole.through || p.steps.size() != whole.steps.size() + 1 ||
                    !p.steps.back().index || p.steps.back().element != j ||
                    !std::equal(
                        whole.steps.begin(),
                        whole.steps.end(),
                        p.steps.begin(),
                        []( const Step& a, const Step& b )
                        { return a.index == b.index && a.element == b.element && a.field == b.field; }
                    ) )
                {
                    return std::nullopt;
                }
            }
            return whole;
        };
        const std::optional<Param_path> re = run( []( const Value_path& x ) { return x.real; } );
        if( real && re && std::ranges::none_of( v, []( const Value_path& x ) { return x.imaginary.local.is_valid(); } ) )
        {
            return Value_place { place( *re ), {} };
        }
        const std::optional<Param_path> im = run( []( const Value_path& x ) { return x.imaginary; } );
        if( !re || !im )
        {
            return std::nullopt;
        }
        return Value_place { place( *re ), place( *im ) };
    }

    // The index locals, each assigned its constant.
    std::vector<Statement> take_prologue()
    {
        return std::move( prologue_ );
    }

private:
    Place place( const Param_path& p )
    {
        Place at { .local = p.local };
        if( p.through )
        {
            at = projected( func_, at, Projection { .kind = Projection_kind::Deref } );
        }
        for( const Step& s : p.steps )
        {
            at = projected(
                func_,
                at,
                s.index ? Projection { .kind = Projection_kind::Index, .index = index( s.element ) }
                        : Projection { .kind = Projection_kind::Field, .field = s.field }
            );
        }
        return at;
    }

    Local_id index( u64 element )
    {
        const auto [it, fresh] = indices_.try_emplace( element );
        if( fresh )
        {
            it->second = func_.locals.push_back( Local { .type = index_type_, .span = span_ } );
            prologue_.push_back( Statement {
                .span  = span_,
                .place = Place { .local = it->second },
                .value = func_.add_rvalue( use( constant( literals_.add_integer( element ), index_type_ ) ) )
            } );
        }
        return it->second;
    }

    Function&               func_;
    Literal_pool&           literals_;
    Type_id                 index_type_;
    Span                    span_;
    std::map<u64, Local_id> indices_;
    std::vector<Statement>  prologue_;
};

// The span the body's first block ends at, for the statements that replace it.
Span body_span( const Function& func )
{
    for( const Statement& s : func.statements )
    {
        if( s.span.is_valid() )
        {
            return s.span;
        }
    }
    return func.blocks[k_entry_block].terminator.span;
}

// `body` in place of every block.
void replace_body( Function& func, std::vector<Statement> body, Span span )
{
    const u32 count = static_cast<u32>( body.size() );
    func.statements = Statements( std::move( body ) );
    func.blocks     = {};
    func.blocks.push_back( Block {
        .first_statement = 0, .statement_count = count, .terminator = { .kind = Terminator_kind::Return, .span = span }
    } );
    func.compact_rvalues();
}

// The body replaced by `circuit` writing each output its node, when that costs less than the body
// ran; whether it did. The comparison counts every multiply, fused or not.
bool replace_with(
    Function&                      func,
    const Circuit&                 circuit,
    const Field&                   field,
    const std::vector<u32>&        nodes,
    const std::vector<Value_path>& in,
    const std::vector<Value_path>& out,
    Type_id                        type,
    Cost                           input,
    const Type_table&              types,
    Literal_pool&                  literals,
    const std::vector<Row>&        rows = {}
)
{
    if( circuit.cost() >= input )
    {
        return false;
    }
    const Span               span = body_span( func );
    Places                   places( func, types, literals, span );
    std::vector<Value_place> inputs, outputs;
    for( const Value_path& v : in )
    {
        inputs.push_back( places.place( v ) );
    }
    for( const Value_path& v : out )
    {
        outputs.push_back( places.place( v ) );
    }
    std::vector<Statement> body = places.take_prologue();
    for( Statement& s : lower_circuit( func, circuit, field, nodes, inputs, outputs, type, literals, span, rows ) )
    {
        body.push_back( std::move( s ) );
    }
    // A read body's only effects are its outputs, so the circuit replaces all of it.
    replace_body( func, std::move( body ), span );
    return true;
}

// keelc's function computing `k` alone by `program`, `void( const f64[n]* re, const f64[n]* im, f64[m]* yr,
// f64[m]* yi )`, its parts called through `called`.
Function planned_function(
    const Kernel& k, const Loop_program& program, Type_table& types, Literal_pool& literals, Span span, const Called& called
)
{
    Function f;
    f.type_arguments  = kernel_arguments( types, k );
    f.parameter_count = 4;
    f.planned         = true;
    f.real_arithmetic = true;
    f.locals.push_back( Local { .type = types.builtin( Type_kind::Void ), .span = span } );
    for( u32 p = 0; p < 4; ++p )
    {
        f.references.push_back( f.locals.push_back( Local { .type = kernel_parameter( types, k, p ), .span = span } ) );
    }
    const auto through = [&]( u32 p )
    { return projected( f, Place { .local = Local_id { p } }, Projection { .kind = Projection_kind::Deref } ); };
    lower_program(
        f, program, { through( 1 ), through( 2 ) }, { through( 3 ), through( 4 ) }, {}, types, literals, span, called
    );
    return f;
}
} // namespace

Map_outcome solve_linear_maps(
    Function&                    func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    Type_table&                  types,
    Literal_pool&                literals,
    const std::filesystem::path& cache,
    const Solve_options&         options,
    const Global_constants&      globals,
    Planned_kernels*             kernels
)
{
    // Past what solve takes whole, the loops are read as a sum rather than run.
    if( const std::optional<Read_kernel> read = read_kernel( func, functions, purity, types, literals, globals );
        read && read->sum.n > Plan_options {}.leaf_max )
    {
        const Plan_options plan { .solve = options, .cache = cache, .calls = kernels != nullptr };
        // Arrays in and out take the plan as loops, when it has any.
        const Span span   = body_span( func );
        Called     called = [&]( const Kernel& k, const Loop_program& body )
        {
            if( kernels->made.insert( k ).second )
            {
                kernels->add( planned_function( k, body, types, literals, span, called ) );
            }
        };
        Places                           places( func, types, literals, span );
        const std::optional<Value_place> x = places.arrays( read->inputs, read->sum.real ), y = places.arrays( read->outputs );
        if( const std::optional<Loop_program> program = x && y ? plan_loops( read->sum, plan ) : std::nullopt;
            program && ( program->loops > 0 || !program->calls.empty() ) )
        {
            if( program->cost >= read->input )
            {
                return { .cost = read->input };
            }
            lower_program( func, *program, *x, *y, places.take_prologue(), types, literals, span, called );
            return { .solved = true, .cost = program->cost };
        }
        if( const std::optional<Planned> planned = plan_sum( read->sum, plan ) )
        {
            const Field complex( Ring_kind::Complex, 4 );
            const bool  solved = replace_with(
                func,
                planned->circuit,
                complex,
                planned->outputs,
                read->inputs,
                read->outputs,
                types.floating( 64 ),
                read->input,
                types,
                literals
            );
            return { .solved = solved, .cost = solved ? planned->circuit.cost() : read->input };
        }
    }

    std::variant<Recognised, Unrecognised> attempt = recognise( func, functions, purity, types, literals, globals );
    if( Unrecognised* u = std::get_if<Unrecognised>( &attempt ) )
    {
        return { .ending = *u };
    }

    Recognised&            map    = std::get<Recognised>( attempt );
    std::optional<Circuit> found  = cache.empty() ? std::nullopt : cached_circuit( cache, map.field, map.rows, options );
    const bool             cached = found.has_value();
    if( !found )
    {
        found = solve( map.field, map.rows, options ).circuit;
        if( !cache.empty() )
        {
            cache_circuit( cache, map.field, map.rows, options, *found );
        }
    }
    for( u32 j = 0; j < map.inputs.size(); ++j )
    {
        if( !map.inputs[j].real.local.is_valid() )
        {
            found = found->bound( j );
        }
    }
    const bool solved = replace_with(
        func,
        *found,
        map.field,
        found->nodes( map.rows ),
        map.inputs,
        map.outputs,
        map.type,
        map.input,
        types,
        literals,
        map.rows
    );
    return { .solved = solved, .cached = cached, .ending = std::nullopt, .cost = solved ? found->cost() : map.input };
}
} // namespace keel
