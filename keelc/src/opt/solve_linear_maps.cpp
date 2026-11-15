// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/solve_linear_maps.h"
#include <map>
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
    const std::vector<Row>&        rows    = {},
    const Callees&                 callees = {}
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
    for( Statement& s : lower_circuit( func, circuit, field, nodes, inputs, outputs, type, literals, span, rows, callees ) )
    {
        body.push_back( std::move( s ) );
    }
    // A read body's only effects are its outputs, so the circuit replaces all of it.
    replace_body( func, std::move( body ), span );
    return true;
}
} // namespace

Map_outcome solve_linear_maps(
    Function&                                              func,
    const std::vector<Function>&                           functions,
    const Purity&                                          purity,
    const Type_table&                                      types,
    Literal_pool&                                          literals,
    const std::filesystem::path&                           cache,
    const Solve_options&                                   options,
    const Global_constants&                                globals,
    const std::function<std::optional<Promised>( u64 n )>& promised
)
{
    // A call's callee, its parameters `f64[N]*`.
    const Callees callees = [&]( u32 f )
    {
        const Function& to = functions[f];
        Callee          callee {
                     .declaration    = to.declaration,
                     .type_arguments = to.type_arguments,
                     .parameters     = {},
                     .array          = {},
                     .index          = types.integer( 64, false ),
                     .none           = types.builtin( Type_kind::Void ),
        };
        for( u32 p = 0; p < 4; ++p )
        {
            callee.parameters[p] = to.locals[Local_id { p + 1 }].type;
        }
        callee.array = types.get( callee.parameters[0] ).element;
        return callee;
    };

    // Past what solve takes whole, the loops are read as a sum rather than run.
    if( const std::optional<Read_kernel> read = read_kernel( func, functions, purity, types, literals, globals );
        read && read->sum.n > Plan_options {}.leaf_max )
    {
        if( const std::optional<Planned> planned =
                plan_sum( read->sum, { .solve = options, .cache = cache, .promised = promised } ) )
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
                literals,
                {},
                callees
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
