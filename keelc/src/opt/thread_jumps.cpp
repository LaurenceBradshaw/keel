// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/thread_jumps.h"
#include <algorithm>
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
    return operand.kind == Operand_kind::Copy && operand.place.is_whole_local();
}

// Whether the local's last write in `block` is a non-zero constant; `nullopt` if it is not a constant.
std::optional<bool>
written( const Function& func, const Block& block, Local_id local, const Type_table& types, const Literal_pool& literals )
{
    for( u32 s = block.statement_count; s-- > 0; )
    {
        const Statement& statement = func.statements_of( block )[s];
        const Rvalue&    value     = func.rvalue( statement );

        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        if( value.kind == Rvalue_kind::Address_of && value.a.place.local == local )
        {
            return std::nullopt;
        }

        if( statement.place.local == local && !statement.place.is_global() )
        {
            if( statement.place.num_projections == 0 && value.kind == Rvalue_kind::Use &&
                value.a.kind == Operand_kind::Constant && value.a.constant.is_valid() &&
                ( types.get( value.a.type ).kind == Type_kind::Int || types.get( value.a.type ).kind == Type_kind::Bool ) )
            {
                return literals.integer( value.a.constant );
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

// The `bool` constant a block's one statement writes to a whole local, before a `goto`.
std::optional<bool> sets_flag( const Function& func, const Block& block, const Type_table& types, const Literal_pool& literals )
{
    if( block.statement_count != 1 || block.terminator.kind != Terminator_kind::Goto )
    {
        return std::nullopt;
    }

    const Statement& statement = func.statements_of( block )[0];
    if( statement.kind != Statement_kind::Assign || statement.place.is_global() || !statement.place.is_whole_local() )
    {
        return std::nullopt;
    }

    const Rvalue& value = func.rvalue( statement );
    if( value.kind != Rvalue_kind::Use || value.a.kind != Operand_kind::Constant || !value.a.constant.is_valid() ||
        types.get( value.a.type ).kind != Type_kind::Bool )
    {
        return std::nullopt;
    }

    return literals.integer( value.a.constant ) != 0;
}

// `x = true` and `x = false` on a branch's two arms, both then to one join: the true arm writes the
// condition, or its negation, or keeps a constant both write, and the branch becomes a `goto` to it.
bool select_flags( Function& func, const Type_table& types, const Literal_pool& literals )
{
    const std::vector<u32> counts = predecessor_counts( func );
    bool                   any    = false;

    for( Block& block : func.blocks )
    {
        Terminator& terminator = block.terminator;
        if( terminator.kind != Terminator_kind::Branch || terminator.condition.kind != Operand_kind::Copy ||
            terminator.targets[0] == terminator.targets[1] || counts[terminator.targets[0].v] != 1 ||
            counts[terminator.targets[1].v] != 1 )
        {
            continue;
        }

        const Block&              taken     = func.blocks[terminator.targets[0]];
        const Block&              not_taken = func.blocks[terminator.targets[1]];
        const std::optional<bool> on        = sets_flag( func, taken, types, literals );
        const std::optional<bool> off       = sets_flag( func, not_taken, types, literals );

        if( !on || !off || taken.terminator.targets[0] != not_taken.terminator.targets[0] )
        {
            continue;
        }

        const Statement& write = func.statements_of( taken )[0];
        if( write.place.local != func.statements_of( not_taken )[0].place.local )
        {
            continue;
        }

        // Arms that agree keep the constant.
        if( *on != *off )
        {
            Rvalue& value = func.rvalue( write );
            value         = *on ? use( terminator.condition ) : unary( Token_kind::Bang, terminator.condition, value.type );
        }

        terminator.kind       = Terminator_kind::Goto;
        terminator.targets[1] = Block_id {};
        terminator.condition  = Operand {};
        any                   = true;
    }

    return any;
}

constexpr u32 k_copied_statements = 2;

// A branch on a whole local after at most `k_copied_statements` scalar writes to whole locals.
bool small_join( const Function& func, const Block& block )
{
    if( block.statement_count == 0 || block.statement_count > k_copied_statements ||
        block.terminator.kind != Terminator_kind::Branch || !whole_local( block.terminator.condition ) )
    {
        return false;
    }

    for( const Statement& statement : func.statements_of( block ) )
    {
        if( statement.kind != Statement_kind::Assign || statement.place.is_global() || !statement.place.is_whole_local() )
        {
            return false;
        }

        const Rvalue& value = func.rvalue( statement );
        if( value.kind != Rvalue_kind::Use && value.kind != Rvalue_kind::Binary && value.kind != Rvalue_kind::Unary &&
            value.kind != Rvalue_kind::Cast )
        {
            return false;
        }

        bool scalar = true;
        for_each_operand(
            value,
            [&]( const Operand& operand )
            { scalar = scalar && ( operand.kind == Operand_kind::Constant || whole_local( operand ) ); }
        );
        if( !scalar )
        {
            return false;
        }
    }

    return true;
}

// A `goto` into a small join whose input the predecessor writes a constant takes a copy of the join,
// so the next round folds its branch on that edge.
bool copy_joins( Function& func, const Type_table& types, const Literal_pool& literals )
{
    std::vector<Block_id> copied( func.blocks.size() );
    bool                  any = false;

    for( const Block_id b : func.blocks.ids() )
    {
        const Block& from = func.blocks[b];
        if( from.terminator.kind != Terminator_kind::Goto || from.terminator.targets[0] == b ||
            !small_join( func, func.blocks[from.terminator.targets[0]] ) )
        {
            continue;
        }

        // Whether each local the join writes is settled by constants on this edge.
        const Block&                           join = func.blocks[from.terminator.targets[0]];
        std::vector<std::pair<Local_id, bool>> wrote;
        const auto                             settled = [&]( Local_id local )
        {
            for( auto it = wrote.rbegin(); it != wrote.rend(); ++it )
            {
                if( it->first == local )
                {
                    return it->second;
                }
            }
            return written( func, from, local, types, literals ).has_value();
        };

        for( const Statement& statement : func.statements_of( join ) )
        {
            bool known = true;
            for_each_operand(
                func.rvalue( statement ),
                [&]( const Operand& operand )
                { known = known && ( operand.kind == Operand_kind::Constant || settled( operand.place.local ) ); }
            );
            wrote.emplace_back( statement.place.local, known );
        }

        if( settled( join.terminator.condition.place.local ) )
        {
            copied[b.v] = from.terminator.targets[0];
            any         = true;
        }
    }

    if( !any )
    {
        return false;
    }

    std::vector<std::vector<Statement>> copies( func.blocks.size() );
    for( const Block_id b : func.blocks.ids() )
    {
        if( copied[b.v].is_valid() )
        {
            for( const Statement& statement : func.statements_of( copied[b.v] ) )
            {
                copies[b.v].push_back( func.clone( statement ) );
            }
        }
    }

    std::vector<Statement> statements;
    statements.reserve( func.statements.size() );

    for( const Block_id b : func.blocks.ids() )
    {
        Block&    block = func.blocks[b];
        const u32 first = narrow_cast<u32>( statements.size() );
        for( Statement& statement : func.statements_of( block ) )
        {
            statements.push_back( std::move( statement ) );
        }
        if( copied[b.v].is_valid() )
        {
            statements.insert(
                statements.end(), std::make_move_iterator( copies[b.v].begin() ), std::make_move_iterator( copies[b.v].end() )
            );
            block.terminator = func.blocks[copied[b.v]].terminator;
        }
        block.first_statement = first;
        block.statement_count = narrow_cast<u32>( statements.size() ) - first;
    }

    func.statements = std::move( statements );
    return true;
}

// `_L`, `_L.f` or `_L.f.tag`: a local's own storage, which only a statement naming it writes.
bool local_part( const Function& func, const Operand& operand )
{
    if( operand.kind != Operand_kind::Copy || operand.place.is_global() || !operand.place.local.is_valid() )
    {
        return false;
    }
    const Run<const Projection> run = func.projections_of( operand.place );
    return std::ranges::all_of(
        run, []( const Projection& p ) { return p.kind == Projection_kind::Field || p.kind == Projection_kind::Tag; }
    );
}

bool same_operand( const Function& func, const Operand& a, const Operand& b, const Literal_pool& literals )
{
    if( a.kind == Operand_kind::Constant && b.kind == Operand_kind::Constant )
    {
        return a.constant.is_valid() && b.constant.is_valid() && a.type == b.type && literals.same( a.constant, b.constant );
    }
    if( !local_part( func, a ) || !local_part( func, b ) || a.place.local != b.place.local ||
        a.place.num_projections != b.place.num_projections )
    {
        return false;
    }
    const Run<const Projection> x = func.projections_of( a.place );
    const Run<const Projection> y = func.projections_of( b.place );
    for( u32 i = 0; i < x.size(); ++i )
    {
        if( x[i].kind != y[i].kind || x[i].field != y[i].field )
        {
            return false;
        }
    }
    return true;
}

// `c = x op y` over constants and locals' parts, as the block's last write of `c`; its index.
std::optional<u32> test_of( const Function& func, const Block& block, Local_id c )
{
    for( u32 s = block.statement_count; s-- > 0; )
    {
        const Statement& statement = func.statements_of( block )[s];
        if( statement.kind != Statement_kind::Assign || statement.place.is_global() || statement.place.local != c )
        {
            continue;
        }

        const Rvalue& value  = func.rvalue( statement );
        const auto    scalar = [&]( const Operand& o ) { return o.kind == Operand_kind::Constant || local_part( func, o ); };
        if( statement.place.is_whole_local() && value.kind == Rvalue_kind::Binary && scalar( value.a ) && scalar( value.b ) )
        {
            return s;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// A branch on a test that a block on its only path in already branched on, with neither operand
// written between: the edge it came by decides it.
bool repeat_tests( Function& func, const Local_set& addressed, const Literal_pool& literals )
{
    std::vector<u32>      counts = predecessor_counts( func );
    std::vector<Block_id> from( func.blocks.size() );
    std::vector<Block_id> targets;
    for( const Block_id b : func.blocks.ids() )
    {
        targets.clear();
        successors( func.blocks[b].terminator, targets );
        for( const Block_id target : targets )
        {
            from[target.v] = b;
        }
    }

    bool changed = false;
    for( const Block_id b : func.blocks.ids() )
    {
        Block& block = func.blocks[b];
        if( block.terminator.kind != Terminator_kind::Branch || !whole_local( block.terminator.condition ) )
        {
            continue;
        }

        const std::optional<u32> at = test_of( func, block, block.terminator.condition.place.local );
        if( !at )
        {
            continue;
        }

        const Rvalue& test     = func.rvalue( func.statements_of( block )[*at] );
        const auto    operands = { test.a, test.b };
        if( std::ranges::any_of(
                operands, [&]( const Operand& o ) { return local_part( func, o ) && addressed[o.place.local.v]; }
            ) )
        {
            continue;
        }

        // Whether statements `[begin, end)` of `on` write an operand.
        const auto writes = [&]( const Block& on, u32 begin, u32 end )
        {
            for( u32 s = begin; s < end; ++s )
            {
                const Statement& statement = func.statements_of( on )[s];
                // `&init` is not in `addressed`, but the call it feeds writes the local.
                const bool addressing = statement.kind == Statement_kind::Assign &&
                                        func.rvalue( statement ).kind == Rvalue_kind::Address_of &&
                                        !func.rvalue( statement ).a.place.is_global();
                for( const Operand& o : operands )
                {
                    if( local_part( func, o ) && ( ( !statement.place.is_global() && statement.place.local == o.place.local ) ||
                                                   ( addressing && func.rvalue( statement ).a.place.local == o.place.local ) ) )
                    {
                        return true;
                    }
                }
            }
            return false;
        };

        if( writes( block, 0, *at ) )
        {
            continue;
        }

        Block_id on = b;
        for( u32 steps = 0; steps < func.blocks.size() && on.v != 0 && counts[on.v] == 1; ++steps )
        {
            const Block_id up   = from[on.v];
            const Block&   prev = func.blocks[up];

            std::optional<u32> earlier;
            if( prev.terminator.kind == Terminator_kind::Branch && whole_local( prev.terminator.condition ) )
            {
                earlier = test_of( func, prev, prev.terminator.condition.place.local );
            }

            if( earlier )
            {
                const Rvalue& seen = func.rvalue( func.statements_of( prev )[*earlier] );
                if( seen.op == test.op && same_operand( func, seen.a, test.a, literals ) &&
                    same_operand( func, seen.b, test.b, literals ) && !writes( prev, *earlier + 1, prev.statement_count ) )
                {
                    const u32 taken  = prev.terminator.targets[0] == on ? 0 : 1;
                    block.terminator = Terminator {
                        .kind    = Terminator_kind::Goto,
                        .span    = block.terminator.span,
                        .targets = { block.terminator.targets[taken] }
                    };
                    changed = true;
                    break;
                }
            }

            if( writes( prev, 0, prev.statement_count ) )
            {
                break;
            }
            on = up;
        }
    }

    return changed;
}

} // namespace

bool thread_jumps( Function& func, const Type_table& types, const Literal_pool& literals )
{
    const Local_set addressed = addressed_locals( func );
    bool            changed   = false;

    for( const Block_id b : func.blocks.ids() )
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
            const Block&   into   = func.blocks[target];

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
                const Block& on = func.blocks[next];
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

    // A join every edge now goes past has no predecessor left, nor a selected flag's false arm.
    const bool copied   = copy_joins( func, types, literals );
    const bool repeated = repeat_tests( func, addressed, literals );
    if( select_flags( func, types, literals ) || changed || copied || repeated )
    {
        remove_unreachable_blocks( func );
    }
    return copied;
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

// A statement before its function holds the rvalue.
struct Draft
{
    Statement_kind        kind = Statement_kind::Assign;
    Place                 place {};
    std::optional<Rvalue> value {};
};

// `_0` is the flag every branch tests, `_1` a `bool` nothing knows, `_2` a pointer.
struct Graph
{
    Type_table   types;
    Literal_pool literals;
    Function     func;

    void build( const std::vector<std::pair<std::vector<Draft>, Terminator>>& blocks )
    {
        func.locals = { Local { .type = flag() }, Local { .type = flag() }, Local {} };
        for( const auto& [statements, terminator] : blocks )
        {
            func.blocks.push_back( Block {
                .first_statement = narrow_cast<u32>( func.statements.size() ),
                .statement_count = narrow_cast<u32>( statements.size() ),
                .terminator      = terminator,
            } );
            for( const Draft& draft : statements )
            {
                func.statements.push_back( Statement {
                    .kind  = draft.kind,
                    .place = draft.place,
                    .value = draft.value ? func.add_rvalue( *draft.value ) : Owned_rvalue {},
                } );
            }
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

    Draft set( bool value )
    {
        const Operand constant { .constant = literals.add_integer( value ? 1 : 0 ), .type = flag() };
        return Draft { .place = Place { .local = Local_id { 0 } }, .value = Rvalue { .type = flag(), .a = constant } };
    }

    Draft unknown() const
    {
        return Draft { .place = Place { .local = Local_id { 0 } }, .value = Rvalue { .type = flag(), .a = read( 1 ) } };
    }

    // `_0 = _1 == true`.
    Draft test()
    {
        const Operand constant { .constant = literals.add_integer( 1 ), .type = flag() };
        return Draft {
            .place = Place { .local = Local_id { 0 } },
            .value =
                Rvalue {
                    .kind = Rvalue_kind::Binary, .type = flag(), .op = Token_kind::Equal_equal, .a = read( 1 ), .b = constant
                }
        };
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
        return func.blocks[Block_id { block }].terminator.targets[edge];
    }

    // Whether the edge still reaches a block that branches, as it does before the pass.
    bool kept( u32 block, u32 edge ) const
    {
        return func.blocks[target( block, edge )].terminator.kind == Terminator_kind::Branch;
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
        { { Draft { .kind = Statement_kind::Storage_dead, .place = Place { .local = Local_id { 1 } } } }, g.branch( 0, 2, 3 ) },
        { {}, done() },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.kept( 0, 0 ) );
}

TEST_CASE( "thread_jumps_keeps_an_addressed_local", "[opt][threading]" )
{
    Graph       g;
    const Draft borrow {
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

// `_0 = _1 ? false : true` becomes `_0 = !_1`, with no branch.
TEST_CASE( "thread_jumps_selects_a_flag", "[opt][threading]" )
{
    Graph g;
    g.build( {
        { {}, g.branch( 1, 1, 2 ) },
        { { g.set( false ) }, jump( 3 ) },
        { { g.set( true ) }, jump( 3 ) },
        { {}, done() },
    } );
    g.run();

    REQUIRE( verify( g.func ).empty() );
    REQUIRE( g.func.blocks.size() == 3 );
    REQUIRE( g.func.blocks[k_entry_block].terminator.kind == Terminator_kind::Goto );
    const Rvalue& value = g.func.rvalue( g.func.statements_of( g.func.blocks[g.target( 0, 0 )] )[0] );
    REQUIRE( value.kind == Rvalue_kind::Unary );
    REQUIRE( value.a.place.local == Local_id { 1 } );
}

// Both arms set the flag `true`: it stays the constant, and the branch goes.
TEST_CASE( "thread_jumps_joins_arms_that_agree", "[opt][threading]" )
{
    Graph g;
    g.build( {
        { {}, g.branch( 1, 1, 2 ) },
        { { g.set( true ) }, jump( 3 ) },
        { { g.set( true ) }, jump( 3 ) },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.func.blocks[k_entry_block].terminator.kind == Terminator_kind::Goto );
    const Rvalue& value = g.func.rvalue( g.func.statements_of( g.func.blocks[g.target( 0, 0 )] )[0] );
    REQUIRE( value.a.kind == Operand_kind::Constant );
}

// The whole release pipeline over `round`'s shape: one test of the flag is left.
// The false edge of `_1 == true` reaches the same test again: it goes the false way.
TEST_CASE( "thread_jumps_folds_a_repeated_test", "[opt][threading]" )
{
    Graph g;
    g.build( {
        { { g.test() }, g.branch( 0, 1, 2 ) },
        { {}, done() },
        { { g.test() }, g.branch( 0, 3, 4 ) },
        { {}, done() },
        { {}, done() },
    } );
    g.run();

    // The true arm, unreached, is gone.
    REQUIRE( g.func.blocks[Block_id { 2 }].terminator.kind == Terminator_kind::Goto );
    REQUIRE( g.func.blocks.size() == 4 );
}

// `_1` is written between the tests, so the second is its own.
TEST_CASE( "thread_jumps_keeps_a_test_of_a_rewritten_operand", "[opt][threading]" )
{
    Graph g;
    Draft write = g.unknown();
    write.place = Place { .local = Local_id { 1 } };
    g.build( {
        { { g.test() }, g.branch( 0, 1, 2 ) },
        { {}, done() },
        { { write, g.test() }, g.branch( 0, 3, 4 ) },
        { {}, done() },
        { {}, done() },
    } );
    g.run();

    REQUIRE( g.func.blocks[Block_id { 2 }].terminator.kind == Terminator_kind::Branch );
}

// `otherwise_drops`: only `empty` reaches `default:`, so its drop of `s` tests nothing.
TEST_CASE( "thread_jumps_drops_nothing_on_a_default_arm", "[opt][threading]" )
{
    const Compiled p(
        "class D { public i32 v; public D( i32 x ) { v = x; } ~D() { v = 0; } };\n"
        "enum E { full( D d ), empty };\n"
        "i32 f( move E s ) { switch( move s ) { case E::full( d ): return d.v; default: return 1; } }\n"
        "i32 main() { return 0; }\n",
        Stage::Elaborated,
        Build { .optimise = true }
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    const Function& f = p.functions[2];
    REQUIRE( verify( f ).empty() );
    REQUIRE(
        std::ranges::count_if( f.blocks, []( const Block& b ) { return b.terminator.kind == Terminator_kind::Branch; } ) == 1
    );
}

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
