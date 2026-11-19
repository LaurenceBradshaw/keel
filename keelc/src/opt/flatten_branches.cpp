// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/flatten_branches.h"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>
#include "ir/loops.h"
#include "ir/simplify.h"

namespace keel
{
namespace
{
bool scalar( const Type_table& table, Type_id type )
{
    const Type_kind kind = table.get( type ).kind;
    return kind == Type_kind::Int || kind == Type_kind::Float || kind == Type_kind::Bool;
}

// Whether `operand` reads a scalar without going through memory a pointer reaches.
bool safe_read( const Function& func, const Type_table& table, const Operand& operand )
{
    if( !operand.type.is_valid() ) // an operand the kind does not use
    {
        return !operand.has_place();
    }
    if( !scalar( table, operand.type ) )
    {
        return false;
    }
    if( !operand.has_place() )
    {
        return operand.kind == Operand_kind::Constant;
    }
    for( const Projection& projection : func.projections_of( operand.place ) )
    {
        if( projection.kind == Projection_kind::Deref || projection.kind == Projection_kind::Index )
        {
            return false;
        }
    }
    return true;
}

// Whether `statement` may run on a path that did not ask for it: it assigns a whole scalar local
// from scalars and cannot fault.
bool speculable( const Function& func, const Type_table& table, const Statement& statement )
{
    if( statement.kind != Statement_kind::Assign || !statement.value.is_valid() || !statement.place.is_whole_local() ||
        !scalar( table, func.locals[statement.place.local].type ) )
    {
        return false;
    }

    const Rvalue& value = func.rvalue( statement );
    switch( value.kind )
    {
    case Rvalue_kind::Use:
    case Rvalue_kind::Unary:
    case Rvalue_kind::Overflows:
    case Rvalue_kind::Cast:
    case Rvalue_kind::Bits:
    case Rvalue_kind::Sqrt:
    case Rvalue_kind::Rotate:
        break;
    case Rvalue_kind::Binary:
        // An integer division by zero, or of the least value by -1, is undefined.
        if( ( value.op == Token_kind::Slash || value.op == Token_kind::Percent ) &&
            table.get( value.a.type ).kind == Type_kind::Int )
        {
            return false;
        }
        break;
    default:
        return false;
    }

    return !any_operand( value, [&]( const Operand& operand ) { return !safe_read( func, table, operand ); } );
}

// The locals whose scope `b` ends, sorted.
std::vector<Local_id> ended( const Function& func, Block_id b )
{
    std::vector<Local_id> locals;
    for( const Statement& statement : func.statements_of( b ) )
    {
        if( statement.kind == Statement_kind::Storage_dead )
        {
            locals.push_back( statement.place.local );
        }
    }
    std::ranges::sort( locals, {}, &Local_id::v );
    return locals;
}

// Whether `b`, entered only from `from`, is an arm: a `goto` after a few speculable statements, and
// scope ends.
bool arm( const Function& func, const Type_table& table, const std::vector<u32>& counts, Block_id b, Block_id from )
{
    const Block& block = func.blocks[b];
    if( b == from || counts[b.v] != 1 || block.terminator.kind != Terminator_kind::Goto )
    {
        return false;
    }
    u32 assignments = 0;
    for( const Statement& statement : func.statements_of( block ) )
    {
        if( statement.kind == Statement_kind::Storage_dead )
        {
            continue;
        }
        if( !speculable( func, table, statement ) || ++assignments > k_flattened_arm )
        {
            return false;
        }
    }
    return true;
}

struct Flattened
{
    std::array<Block_id, 2> arms {}; // the true arm and the false arm; invalid for none
    Block_id                join {};
    std::vector<Statement>  added;
};

// What `h`'s branch becomes, if it is a triangle or a diamond of arms.
std::optional<Flattened> shape( const Function& func, const Type_table& table, const std::vector<u32>& counts, Block_id h )
{
    const Terminator& branch = func.blocks[h].terminator;
    if( branch.kind != Terminator_kind::Branch || !branch.condition.has_place() || branch.targets[0] == branch.targets[1] )
    {
        return std::nullopt;
    }

    const Block_id t      = branch.targets[0];
    const Block_id f      = branch.targets[1];
    const bool     t_arm  = arm( func, table, counts, t, h );
    const bool     f_arm  = arm( func, table, counts, f, h );
    const Block_id t_next = t_arm ? func.blocks[t].terminator.targets[0] : Block_id {};
    const Block_id f_next = f_arm ? func.blocks[f].terminator.targets[0] : Block_id {};

    Flattened flat;
    if( t_arm && f_arm && t_next == f_next && t_next != t && t_next != f )
    {
        flat = { .arms = { t, f }, .join = t_next, .added = {} };
    }
    else if( t_arm && t_next == f )
    {
        flat = { .arms = { t, Block_id {} }, .join = f, .added = {} };
    }
    else if( f_arm && f_next == t )
    {
        flat = { .arms = { Block_id {}, f }, .join = t, .added = {} };
    }
    else
    {
        return std::nullopt;
    }
    // A scope both arms end ends after the selects; one only one ends would end on the other path too.
    const std::vector<Local_id> none {};
    if( flat.join == h || ( flat.arms[0].is_valid() ? ended( func, flat.arms[0] ) : none ) !=
                              ( flat.arms[1].is_valid() ? ended( func, flat.arms[1] ) : none ) )
    {
        return std::nullopt;
    }

    // The selects read the condition after the arms' statements, which must not have changed it.
    for( const Block_id a : flat.arms )
    {
        if( !a.is_valid() )
        {
            continue;
        }
        for( const Statement& statement : func.statements_of( a ) )
        {
            if( statement.place.local == branch.condition.place.local )
            {
                return std::nullopt;
            }
        }
    }
    return flat;
}

// The statements that replace `flat`'s arms: each arm's into temporaries, then a select for each local either assigns.
void flatten( Function& func, Flattened& flat, const Operand& condition, Span at )
{
    std::vector<std::pair<Local_id, Local_id>> renamed[2]; // each arm's local and the temporary holding its last value
    std::vector<Local_id>                      assigned;

    const auto find = []( const std::vector<std::pair<Local_id, Local_id>>& map, Local_id local ) -> Local_id
    {
        for( auto it = map.rbegin(); it != map.rend(); ++it )
        {
            if( it->first == local )
            {
                return it->second;
            }
        }
        return Local_id {};
    };

    for( u32 side = 0; side < 2; ++side )
    {
        if( !flat.arms[side].is_valid() )
        {
            continue;
        }
        const Block& block = func.blocks[flat.arms[side]];
        for( u32 s = block.first_statement; s < block.first_statement + block.statement_count; ++s )
        {
            if( func.statements[s].kind == Statement_kind::Storage_dead )
            {
                continue;
            }
            Rvalue     value = std::as_const( func ).rvalue( func.statements[s] );
            const auto read  = [&]( Operand& operand )
            {
                if( !operand.has_place() )
                {
                    return;
                }
                operand.kind = Operand_kind::Copy;
                if( const Local_id temporary = find( renamed[side], operand.place.local ); temporary.is_valid() )
                {
                    operand.place.local = temporary;
                }
            };
            read( value.a );
            read( value.b );
            for( Operand& argument : value.arguments )
            {
                read( argument );
            }

            const Local_id local     = func.statements[s].place.local;
            const Span     span      = func.statements[s].span;
            const Local_id temporary = func.locals.push_back( Local { .type = func.locals[local].type, .span = span } );
            flat.added.push_back( Statement {
                .span = span, .place = Place { .local = temporary }, .value = func.add_rvalue( std::move( value ) )
            } );
            renamed[side].emplace_back( local, temporary );
            if( std::ranges::find( assigned, local ) == assigned.end() )
            {
                assigned.push_back( local );
            }
        }
    }

    Operand test = condition;
    test.kind    = Operand_kind::Copy;
    for( const Local_id local : assigned )
    {
        const Type_id type = func.locals[local].type;
        const auto    pick = [&]( u32 side )
        {
            const Local_id temporary = find( renamed[side], local );
            return copy( Place { .local = temporary.is_valid() ? temporary : local }, type );
        };
        flat.added.push_back( Statement {
            .span = at, .place = Place { .local = local }, .value = func.add_rvalue( select( test, pick( 0 ), pick( 1 ) ) )
        } );
    }
    for( const Local_id local : ended( func, flat.arms[0].is_valid() ? flat.arms[0] : flat.arms[1] ) )
    {
        flat.added.push_back( Statement { .kind = Statement_kind::Storage_dead, .span = at, .place = Place { .local = local } }
        );
    }
}
} // namespace

bool flatten_branches( Function& func, const Type_table& table )
{
    const std::vector<u32> counts = predecessor_counts( func );

    // Only in an innermost loop, which the vectoriser can take: elsewhere a predicted branch is cheaper
    // than a select's wait for its condition.
    const Loops       loops = find_loops( func );
    std::vector<bool> parent( loops.loops.size(), false );
    for( const Loop& loop : loops.loops )
    {
        if( loop.parent != k_no_loop )
        {
            parent[loop.parent] = true;
        }
    }
    std::vector<bool> innermost( func.blocks.size(), false );
    for( u32 l = 0; l < loops.loops.size(); ++l )
    {
        for( const Block_id b : parent[l] ? std::vector<Block_id> {} : loops.loops[l].blocks )
        {
            innermost[b.v] = true;
        }
    }

    std::vector<std::optional<Flattened>> flats( func.blocks.size() );
    bool                                  any = false;
    for( const Block_id h : func.blocks.ids() )
    {
        if( !innermost[h.v] )
        {
            continue;
        }
        flats[h.v] = shape( func, table, counts, h );
        if( flats[h.v] )
        {
            const Terminator branch = func.blocks[h].terminator;
            flatten( func, *flats[h.v], branch.condition, branch.span );
            any = true;
        }
    }
    if( !any )
    {
        return false;
    }

    std::vector<bool> emptied( func.blocks.size(), false );
    for( const std::optional<Flattened>& flat : flats )
    {
        for( const Block_id a : flat ? flat->arms : std::array<Block_id, 2> {} )
        {
            if( a.is_valid() )
            {
                emptied[a.v] = true;
            }
        }
    }

    std::vector<Statement> statements;
    statements.reserve( func.statements.size() );
    for( const Block_id b : func.blocks.ids() )
    {
        Block&    block = func.blocks[b];
        const u32 first = narrow_cast<u32>( statements.size() );
        if( !emptied[b.v] )
        {
            const Run<Statement> run = func.statements_of( block );
            statements.insert( statements.end(), std::make_move_iterator( run.begin() ), std::make_move_iterator( run.end() ) );
        }
        if( std::optional<Flattened>& flat = flats[b.v] )
        {
            statements.insert(
                statements.end(), std::make_move_iterator( flat->added.begin() ), std::make_move_iterator( flat->added.end() )
            );
            block.terminator =
                Terminator { .kind = Terminator_kind::Goto, .span = block.terminator.span, .targets = { flat->join } };
        }
        block.first_statement = first;
        block.statement_count = narrow_cast<u32>( statements.size() ) - first;
    }

    func.statements = std::move( statements );
    remove_unreachable_blocks( func );
    return true;
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

// Selects and branches left in the first function, optimised and elaborated.
struct Flat
{
    std::size_t selects  = 0;
    std::size_t branches = 0;
};

Flat flattened( std::string_view source, u32 level = 2 )
{
    const Compiled c( std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, Build { .level = level } );
    INFO( c.rendered() );
    REQUIRE( c.clean() );
    const Function& func = c.functions.front();
    REQUIRE( verify( func ).empty() );
    Flat flat;
    for( const Statement& statement : func.statements )
    {
        flat.selects += statement.value.is_valid() && func.rvalue( statement ).kind == Rvalue_kind::Select;
    }
    for( const Block& block : func.blocks )
    {
        flat.branches += block.terminator.kind == Terminator_kind::Branch;
    }
    return flat;
}

constexpr std::string_view k_counted =
    "i64 f( i64 n, i64 k ) { i64 c = 0; for( i64 i = 0; i < n; i += 1 ) { if( i * k > 7 ) { c += 1; } } return c; }\n";

} // namespace

TEST_CASE( "flatten_branches_makes_a_triangle_in_a_loop_a_select", "[opt][flatten_branches]" )
{
    const Flat flat = flattened( k_counted );
    REQUIRE( flat.selects == 1 );
    REQUIRE( flat.branches == 2 ); // the loop's guard and its latch
}

TEST_CASE( "flatten_branches_makes_a_diamond_one_select_a_local", "[opt][flatten_branches]" )
{
    const Flat flat = flattened( "i64 f( i64 n ) { i64 a = 0; i64 b = 0; for( i64 i = 0; i < n; i += 1 ) { "
                                 "if( i > 3 ) { a += i; b = 1; } else { b = 2; } a += b; } return a; }\n" );
    REQUIRE( flat.selects == 2 );
    REQUIRE( flat.branches == 2 );
}

TEST_CASE( "flatten_branches_runs_from_O2", "[opt][flatten_branches]" )
{
    REQUIRE( flattened( k_counted, 1 ).selects == 0 );
}

TEST_CASE( "flatten_branches_keeps_an_arm_that_may_fault", "[opt][flatten_branches]" )
{
    // A division, and a read through a reference.
    REQUIRE(
        flattened( "i64 f( i64 n ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { if( i != 3 ) { t += 60 / ( i - 3 ); } } "
                   "return t; }\n" )
            .selects == 0
    );
    REQUIRE(
        flattened( "i64 f( const ref i64 p, i64 n ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { if( i == 2 ) { t += p; } } "
                   "return t; }\n" )
            .selects == 0
    );
}

TEST_CASE( "flatten_branches_keeps_a_branch_outside_an_innermost_loop", "[opt][flatten_branches]" )
{
    REQUIRE( flattened( "i64 f( i64 x ) { i64 t = 5; if( x > 0 ) { t = x; } return t; }\n" ).selects == 0 );
    REQUIRE(
        flattened( "i64 f( i64 n ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { if( i > 2 ) { t += 1; } "
                   "for( i64 j = 0; j < i; j += 1 ) { t += j; } } return t; }\n" )
            .selects == 0
    );
}

TEST_CASE( "flatten_branches_keeps_an_arm_over_its_budget", "[opt][flatten_branches]" )
{
    REQUIRE(
        flattened( "i64 f( i64 n ) { i64 a = 0; i64 b = 0; i64 c = 0; i64 d = 0; for( i64 i = 0; i < n; i += 1 ) { "
                   "if( i > 3 ) { a += i; b += a; c += b; d += c; } } return a + b + c + d; }\n" )
            .selects == 0
    );
}
} // namespace keel
#endif
