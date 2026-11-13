// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/solve_linear_maps.h"
#include <map>
#include <utility>
#include "egraph/circuit_cache.h"
#include "egraph/lower_circuit.h"

namespace keel
{
namespace
{
// `b` costs less than `a`: by total, then by multiplies, as solve ranks.
bool cheaper( Cost b, Cost a )
{
    return std::pair { b.adds + b.muls, b.muls } < std::pair { a.adds + a.muls, a.muls };
}

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

    Value_place place( const Value_path& v )
    {
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
} // namespace

Map_outcome solve_linear_maps(
    Function&                    func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    const Type_table&            types,
    Literal_pool&                literals,
    const std::filesystem::path& cache,
    const Solve_options&         options
)
{
    std::variant<Recognised, Unrecognised> attempt = recognise( func, functions, purity, types, literals );
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
    if( !cheaper( found->cost(), map.input ) )
    {
        return { .cached = cached };
    }

    const Span               span = body_span( func );
    Places                   places( func, types, literals, span );
    std::vector<Value_place> inputs, outputs;
    for( const Value_path& v : map.inputs )
    {
        inputs.push_back( places.place( v ) );
    }
    for( const Value_path& v : map.outputs )
    {
        outputs.push_back( places.place( v ) );
    }

    std::vector<Statement> body = places.take_prologue();
    for( Statement& s : lower_circuit( func, *found, map.field, map.rows, inputs, outputs, map.type, literals, span ) )
    {
        body.push_back( std::move( s ) );
    }

    // A recognised body's only effects are its outputs, so the circuit replaces all of it.
    const u32 count = static_cast<u32>( body.size() );
    func.statements = Statements( std::move( body ) );
    func.blocks     = {};
    func.blocks.push_back( Block {
        .first_statement = 0, .statement_count = count, .terminator = { .kind = Terminator_kind::Return, .span = span }
    } );
    func.compact_rvalues();
    return { .solved = true, .cached = cached, .ending = std::nullopt };
}
} // namespace keel
