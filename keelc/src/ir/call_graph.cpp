// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/call_graph.h"
#include <algorithm>

namespace keel
{

namespace
{

// Which functions each one calls; a call to a body-less extern has no edge. Calls × functions, a map
// by declaration if it shows.
std::vector<std::vector<u32>> call_edges( const std::vector<Function>& functions )
{
    std::vector<std::vector<u32>> edges( functions.size() );
    for( u32 f = 0; f < functions.size(); f++ )
    {
        for( const Statement& statement : functions[f].statements )
        {
            for( u32 c = 0; c < functions.size(); c++ )
            {
                const Function& callee = functions[c];

                if( calls( statement, callee ) )
                {
                    edges[f].push_back( c );
                    break;
                }
            }
        }
    }

    return edges;
}

// `order` is when a function was reached, k_invalid_kir_id until then; `low` the earliest still-open
// function it reaches back to.
struct Walk
{
    const std::vector<std::vector<u32>>& edges;
    std::vector<u32>                     order;
    std::vector<u32>                     low;
    std::vector<bool>                    on_stack;
    std::vector<u32>                     stack;
    u32                                  next = 0;
    std::vector<std::vector<u32>>        groups;
};

// Recursive, so its depth is the longest call chain; an explicit stack if a program overflows it.
void visit( Walk& walk, u32 f )
{
    walk.order[f] = walk.next;
    walk.low[f]   = walk.next;
    walk.next++;
    walk.stack.push_back( f );
    walk.on_stack[f] = true;

    for( u32 c : walk.edges[f] )
    {
        if( walk.order[c] == k_invalid_kir_id )
        {
            visit( walk, c );
            walk.low[f] = std::min( walk.low[f], walk.low[c] );
        }
        else if( walk.on_stack[c] )
        {
            walk.low[f] = std::min( walk.low[f], walk.order[c] );
        }
    }

    if( walk.low[f] == walk.order[f] )
    {
        std::vector<u32> group;
        while( true )
        {
            const u32 popped = walk.stack.back();
            walk.stack.pop_back();
            walk.on_stack[popped] = false;
            group.push_back( popped );

            if( popped == f )
            {
                break;
            }
        }

        walk.groups.push_back( std::move( group ) );
    }
}

} // namespace

std::vector<std::vector<u32>> callees_first( const std::vector<Function>& functions )
{
    const std::vector<std::vector<u32>> edges = call_edges( functions );

    Walk walk {
        .edges    = edges,
        .order    = std::vector<u32>( functions.size(), k_invalid_kir_id ),
        .low      = std::vector<u32>( functions.size(), k_invalid_kir_id ),
        .on_stack = std::vector<bool>( functions.size(), false ),
        .stack    = std::vector<u32> {},
        .next     = 0,
        .groups   = std::vector<std::vector<u32>> {}
    };

    for( u32 f = 0; f < functions.size(); f++ )
    {
        if( walk.order[f] == k_invalid_kir_id )
        {
            visit( walk, f );
        }
    }

    return walk.groups;
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

constexpr std::string_view k_main = "i32 main() { return 0; }\n";

struct Graphed : Compiled
{
    explicit Graphed( std::string_view source )
        : Compiled( std::string( source ) + std::string( k_main ) ),
          groups( callees_first( all ) )
    {
    }

    std::vector<std::vector<u32>> groups;

    // Which group holds the function called `name`, by position in `groups`.
    std::size_t group_of( std::string_view name ) const
    {
        for( std::size_t g = 0; g < groups.size(); ++g )
        {
            for( const u32 f : groups[g] )
            {
                if( interner.text( ast.name( all[f].declaration ) ) == name )
                {
                    return g;
                }
            }
        }

        FAIL( "no function " << name );
        return 0;
    }

    std::size_t group_of_destructor() const
    {
        for( std::size_t g = 0; g < groups.size(); ++g )
        {
            for( const u32 f : groups[g] )
            {
                if( ast.kind( all[f].declaration ) == Node_kind::Destructor_decl )
                {
                    return g;
                }
            }
        }

        FAIL( "no destructor" );
        return 0;
    }
};

} // namespace

TEST_CASE( "callees_first_puts_a_callee_before_its_caller", "[ir][call_graph]" )
{
    const Graphed p( "i32 twice( i32 x ) { return x * 2; }\n"
                     "i32 caller( i32 a ) { return twice( a ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( p.group_of( "twice" ) < p.group_of( "caller" ) );
    REQUIRE( p.groups[p.group_of( "twice" )].size() == 1 );
}

TEST_CASE( "callees_first_groups_two_functions_that_call_each_other", "[ir][call_graph]" )
{
    const Graphed p( "bool even( u32 n ) { if( n == 0 ) { return true; } return odd( n - 1 ); }\n"
                     "bool odd( u32 n ) { if( n == 0 ) { return false; } return even( n - 1 ); }\n"
                     "bool caller() { return even( 4 ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( p.group_of( "even" ) == p.group_of( "odd" ) );
    REQUIRE( p.groups[p.group_of( "even" )].size() == 2 );
    REQUIRE( p.group_of( "even" ) < p.group_of( "caller" ) );
}

// `b`'s drop at the end of `user` is a call to `~Box`.
TEST_CASE( "callees_first_puts_a_destructor_before_its_dropper", "[ir][call_graph]" )
{
    const Graphed p( "class Box { public i32 v; public Box() { v = 0; } ~Box() { } };\n"
                     "i32 user() { Box b = Box(); return b.v; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( p.group_of_destructor() < p.group_of( "user" ) );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
