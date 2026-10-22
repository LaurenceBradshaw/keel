// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/thread_jumps.h"
#include <optional>
#include "ir/live.h"
#include "ir/simplify.h"
#include "sema/type.h"

namespace keel
{
namespace
{

bool whole_local( const Operand& operand )
{
    return operand.kind == Operand_kind::Copy && !operand.place.is_global() && operand.place.num_projections == 0;
}

// Whether the local's last write in `block` is a non-zero constant; `nullopt` if it is not a constant.
std::optional<bool>
written( const Function& func, const Block& block, Local_id local, const Type_table& types, const Literal_pool& literals )
{
    for( u32 s = block.statement_count; s-- > 0; )
    {
        const Statement& statement = func.statements[block.first_statement + s];

        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        if( statement.value.kind == Rvalue_kind::Address_of && statement.value.a.place.local == local )
        {
            return std::nullopt;
        }

        if( statement.place.local == local && !statement.place.is_global() )
        {
            if( statement.place.num_projections == 0 && statement.value.kind == Rvalue_kind::Use &&
                statement.value.a.kind == Operand_kind::Constant && statement.value.a.constant.is_valid() &&
                ( types.get( statement.value.a.type ).kind == Type_kind::Int ||
                  types.get( statement.value.a.type ).kind == Type_kind::Bool ) )
            {
                return literals.integer( statement.value.a.constant );
            }
            else
            {
                return std::nullopt;
            }
        }
    }

    return std::nullopt;
}

// Whether `local` is non-zero on `from`'s edge `edge`, `nullopt` if it is not known.
std::optional<bool> on_edge(
    const Function& func, const Block& from, u32 edge, Local_id local, const Type_table& types, const Literal_pool& literals
)
{
    // The branch reads the local after every statement, so its edge decides it.
    if( from.terminator.kind == Terminator_kind::Branch && whole_local( from.terminator.condition ) &&
        from.terminator.condition.place.local == local )
    {
        return edge == 0;
    }

    return written( func, from, local, types, literals );
}

// A block with no statements that branches on a whole local.
bool only_branches( const Block& block )
{
    return block.statement_count == 0 && block.terminator.kind == Terminator_kind::Branch &&
           whole_local( block.terminator.condition );
}

} // namespace

void thread_jumps( Function& func, const Type_table& types, const Literal_pool& literals )
{
    const Local_set addressed = addressed_locals( func );
    bool            changed   = false;

    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        std::vector<u32> edges;
        if( func.blocks[b].terminator.kind == Terminator_kind::Branch )
        {
            edges = { 0, 1 };
        }
        else if( func.blocks[b].terminator.kind == Terminator_kind::Goto )
        {
            edges = { 0 };
        }

        for( u32 i : edges )
        {
            const Block_id target = func.blocks[b].terminator.targets[i];
            const Block&   into   = func.blocks[target.v];

            if( !only_branches( into ) || addressed[into.terminator.condition.place.local.v] )
            {
                continue;
            }

            const Local_id            local = into.terminator.condition.place.local;
            const std::optional<bool> value = on_edge( func, func.blocks[b], i, local, types, literals );

            if( !value )
            {
                continue;
            }

            // A chain of `&&` or `||` shares one flag, and no statement runs between its tests, so each gives
            // the same answer. Bounded, since an empty loop branches back to itself.
            Block_id next = into.terminator.targets[*value ? 0 : 1];
            for( u32 steps = 0; steps < func.blocks.size(); ++steps )
            {
                const Block& on = func.blocks[next.v];
                if( !only_branches( on ) || on.terminator.condition.place.local != local )
                {
                    break;
                }
                next = on.terminator.targets[*value ? 0 : 1];
            }

            func.blocks[b].terminator.targets[i] = next;
            changed                              = true;
        }
    }

    // A join every edge now goes past has no predecessor left.
    if( changed )
    {
        remove_unreachable_blocks( func );
    }
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

// `_0` is the flag every branch tests, `_1` a `bool` nothing knows, `_2` a pointer.
struct Graph
{
    Type_table   types;
    Literal_pool literals;
    Function     func;

    void build( const std::vector<std::pair<std::vector<Statement>, Terminator>>& blocks )
    {
        func.locals = { Local { .type = flag() }, Local { .type = flag() }, Local {} };
        for( const auto& [statements, terminator] : blocks )
        {
            func.blocks.push_back( Block {
                .first_statement = narrow_cast<u32>( func.statements.size() ),
                .statement_count = narrow_cast<u32>( statements.size() ),
                .terminator      = terminator,
            } );
            func.statements.insert( func.statements.end(), statements.begin(), statements.end() );
        }
    }

    Type_id flag() const
    {
        return types.builtin( Type_kind::Bool );
    }

    Operand read( u32 local ) const
    {
        return Operand { .kind = Operand_kind::Copy, .place = Place { .local = Local_id { local } }, .type = flag() };
    }

    Statement set( bool value )
    {
        const Operand constant { .constant = literals.add_integer( value ? 1 : 0 ), .type = flag() };
        return Statement { .place = Place { .local = Local_id { 0 } }, .value = Rvalue { .type = flag(), .a = constant } };
    }

    Statement unknown() const
    {
        return Statement { .place = Place { .local = Local_id { 0 } }, .value = Rvalue { .type = flag(), .a = read( 1 ) } };
    }

    Terminator branch( u32 local, u32 taken, u32 not_taken ) const
    {
        return Terminator {
            .kind      = Terminator_kind::Branch,
            .condition = read( local ),
            .targets   = { Block_id { taken }, Block_id { not_taken } },
        };
    }

    Block_id target( u32 block, u32 edge ) const
    {
        return func.blocks[block].terminator.targets[edge];
    }

    // Whether the edge still reaches a block that branches, as it does before the pass.
    bool kept( u32 block, u32 edge ) const
    {
        return func.blocks[target( block, edge ).v].terminator.kind == Terminator_kind::Branch;
    }

    void run()
    {
        thread_jumps( func, types, literals );
    }
};

Terminator jump( u32 to )
{
    return Terminator { .kind = Terminator_kind::Goto, .targets = { Block_id { to } } };
}

Terminator done()
{
    return Terminator { .kind = Terminator_kind::Return };
}

} // namespace

// `round`'s `&&` once SCCP has folded its second operand: each edge into the join knows the flag.
TEST_CASE( "thread_jumps_threads_a_constant_write", "[opt][threading]" )
{
    Graph g;
    g.build( {
        { {}, g.branch( 1, 1, 2 ) },
        { { g.set( true ) }, jump( 2 ) },
        { {}, g.branch( 0, 3, 4 ) },
        { {}, done() },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.target( 1, 0 ) == Block_id { 3 } );
    REQUIRE( g.target( 0, 1 ) == Block_id { 2 } );
}

TEST_CASE( "thread_jumps_threads_the_edge_a_branch_decides", "[opt][threading]" )
{
    Graph g;
    g.build( {
        { { g.unknown() }, g.branch( 0, 1, 2 ) },
        { { g.unknown() }, jump( 2 ) },
        { {}, g.branch( 0, 3, 4 ) },
        { {}, done() },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.target( 0, 1 ) == Block_id { 4 } );
    REQUIRE( g.target( 1, 0 ) == Block_id { 2 } );
}

// A flag set `true` and then to something unknown: the last write decides, not the first.
TEST_CASE( "thread_jumps_keeps_a_flag_written_after_its_constant", "[opt][threading]" )
{
    Graph g;
    g.build( {
        { { g.set( true ), g.unknown() }, jump( 1 ) },
        { {}, g.branch( 0, 2, 3 ) },
        { {}, done() },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.kept( 0, 0 ) );
}

// Looks like the target, but the join computes before it branches.
TEST_CASE( "thread_jumps_keeps_a_target_with_statements", "[opt][threading]" )
{
    Graph g;
    g.build( {
        { { g.set( true ) }, jump( 1 ) },
        { { Statement { .kind = Statement_kind::Storage_dead, .place = Place { .local = Local_id { 1 } } } },
          g.branch( 0, 2, 3 ) },
        { {}, done() },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.kept( 0, 0 ) );
}

TEST_CASE( "thread_jumps_keeps_an_addressed_local", "[opt][threading]" )
{
    Graph           g;
    const Statement borrow {
        .place = Place { .local = Local_id { 2 } },
        .value = Rvalue { .kind = Rvalue_kind::Address_of, .a = g.read( 0 ) },
    };
    g.build( {
        { { borrow, g.set( true ) }, jump( 1 ) },
        { {}, g.branch( 0, 2, 3 ) },
        { {}, done() },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.kept( 0, 0 ) );
}

// `a || b || c` with one flag: the first `true` goes past every later test of it.
TEST_CASE( "thread_jumps_follows_a_chain", "[opt][threading]" )
{
    Graph g;
    g.build( {
        { { g.unknown() }, g.branch( 0, 2, 1 ) },
        { { g.unknown() }, jump( 2 ) },
        { {}, g.branch( 0, 4, 3 ) },
        { { g.unknown() }, jump( 4 ) },
        { {}, g.branch( 0, 5, 6 ) },
        { {}, done() },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.target( 0, 0 ) == Block_id { 5 } );
    REQUIRE( g.target( 2, 0 ) == Block_id { 5 } );
}

// `while( k ) { }` with `k` true: the edge lands where it started, and the pass still ends.
TEST_CASE( "thread_jumps_stops_at_an_empty_loop", "[opt][threading]" )
{
    Graph g;
    g.build( {
        { { g.set( true ) }, jump( 1 ) },
        { {}, g.branch( 0, 1, 2 ) },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.kept( 0, 0 ) );
}

// The whole release pipeline over `round`'s shape: one test of the flag is left.
TEST_CASE( "thread_jumps_leaves_one_test_of_an_and", "[opt][threading]" )
{
    const Compiled p(
        "bool big( i32 x ) { return x > 10; }\n"
        "i32 f( i32 i ) { if( i % 2 == 0 && big( 20 ) ) { return 1; } return 2; }\n"
        "i32 main() { return 0; }\n",
        Stage::Elaborated,
        Build { .optimise = true }
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    const Function& f = p.functions[1];
    REQUIRE( verify( f ).empty() );
    REQUIRE(
        std::ranges::count_if( f.blocks, []( const Block& b ) { return b.terminator.kind == Terminator_kind::Branch; } ) == 1
    );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
