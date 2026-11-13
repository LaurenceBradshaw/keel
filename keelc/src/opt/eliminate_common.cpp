// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/eliminate_common.h"
#include <algorithm>
#include <optional>
#include <vector>
#include "ir/simplify.h"
#include "opt/forward_reads.h"

namespace keel
{
namespace
{
// A computation, and the local holding it.
struct Made
{
    Rvalue   value;
    Local_id local;
};
using Table = std::vector<Made>;

bool pure( Rvalue_kind kind )
{
    switch( kind )
    {
    case Rvalue_kind::Binary:
    case Rvalue_kind::Unary:
    case Rvalue_kind::Overflows:
    case Rvalue_kind::Cast:
    case Rvalue_kind::Bits:
    case Rvalue_kind::Run_of:
    case Rvalue_kind::Run_data:
    case Rvalue_kind::Run_size:
        return true;
    default:
        return false;
    }
}

// The operands a kind reads.
bool uses_b( Rvalue_kind kind )
{
    return kind == Rvalue_kind::Binary || kind == Rvalue_kind::Overflows || kind == Rvalue_kind::Run_of;
}

bool same_operand( const Function& func, const Literal_pool& literals, const Operand& a, const Operand& b )
{
    if( a.kind != b.kind || a.type != b.type )
    {
        return false;
    }
    if( a.kind == Operand_kind::Constant )
    {
        return a.constant.is_valid() && b.constant.is_valid() && literals.same( a.constant, b.constant );
    }
    return a.kind == Operand_kind::Copy && same_place( func, a.place, b.place );
}

bool same_value( const Function& func, const Literal_pool& literals, const Rvalue& a, const Rvalue& b )
{
    return a.kind == b.kind && a.type == b.type && a.op == b.op && same_operand( func, literals, a.a, b.a ) &&
           ( !uses_b( a.kind ) || same_operand( func, literals, a.b, b.b ) );
}

// Copies of places and constants only: a move or a debug flag is not a value to share.
bool sharable( const Rvalue& value )
{
    const auto plain = []( const Operand& operand )
    { return operand.kind == Operand_kind::Copy || operand.kind == Operand_kind::Constant; };
    return pure( value.kind ) && plain( value.a ) && ( !uses_b( value.kind ) || plain( value.b ) );
}

bool reads_local( const Function& func, const Operand& operand, Local_id local )
{
    return operand.has_place() && mentions( func, operand.place, local );
}

// Whether a call or a store through a pointer may change what `operand` reads.
bool in_memory( const Function& func, const Local_set& addressed, const Operand& operand )
{
    return operand.has_place() && ( operand.place.is_global() || has_deref_projection( func, operand.place ) ||
                                    is_addressed( addressed, operand.place.local ) );
}

const Made* held( const Function& func, const Literal_pool& literals, const Table& table, const Rvalue& value )
{
    for( const Made& made : table )
    {
        if( same_value( func, literals, made.value, value ) )
        {
            return &made;
        }
    }
    return nullptr;
}

struct Walk
{
    const Function&     func;
    const Literal_pool& literals;
    const Local_set     addressed;

    void step( const Statement& statement, Table& table ) const
    {
        if( clobbers( func, addressed, statement ) )
        {
            std::erase_if(
                table,
                [&]( const Made& made )
                { return in_memory( func, addressed, made.value.a ) || in_memory( func, addressed, made.value.b ); }
            );
        }
        if( !statement.place.is_global() && statement.place.local.is_valid() )
        {
            const Local_id local = statement.place.local;
            std::erase_if(
                table,
                [&]( const Made& made ) {
                    return made.local == local || reads_local( func, made.value.a, local ) ||
                           reads_local( func, made.value.b, local );
                }
            );
        }

        if( statement.kind != Statement_kind::Assign || !statement.place.is_whole_local() )
        {
            return;
        }

        const Rvalue&  value = func.rvalue( statement );
        const Local_id into  = statement.place.local;
        if( sharable( value ) && value.type == func.locals[into].type && !is_addressed( addressed, into ) &&
            !reads_local( func, value.a, into ) && !reads_local( func, value.b, into ) &&
            !held( func, literals, table, value ) )
        {
            table.push_back( Made { .value = value, .local = into } );
        }
    }

    // What every path into each block made: available expressions, met by intersection.
    std::vector<std::optional<Table>> on_entry() const
    {
        std::vector<std::optional<Table>> entry( func.blocks.size(), std::nullopt );
        entry[0].emplace();

        // Entries only go away, and a block is reached once, so this ends.
        bool                  changed = true;
        std::vector<Block_id> targets;
        std::vector<bool>     dirty( func.blocks.size(), true );
        Table                 table;
        while( changed )
        {
            changed = false;
            for( const Block_id b : func.blocks.ids() )
            {
                if( !entry[b.v] || !dirty[b.v] )
                {
                    continue;
                }
                dirty[b.v] = false;

                const Block& block = func.blocks[b];
                table              = *entry[b.v];
                for( const Statement& statement : func.statements_of( block ) )
                {
                    step( statement, table );
                }

                targets.clear();
                successors( block.terminator, targets );
                for( const Block_id target : targets )
                {
                    std::optional<Table>& into = entry[target.v];
                    if( !into )
                    {
                        into            = table;
                        changed         = true;
                        dirty[target.v] = true;
                        continue;
                    }

                    // An entry survives a join only if this path holds it in the same local.
                    const std::size_t before = into->size();
                    std::erase_if(
                        *into,
                        [&]( const Made& made )
                        {
                            const Made* other = held( func, literals, table, made.value );
                            return !other || other->local != made.local;
                        }
                    );
                    if( into->size() != before )
                    {
                        changed         = true;
                        dirty[target.v] = true;
                    }
                }
            }
        }

        return entry;
    }
};
} // namespace

bool eliminate_common( Function& func, const Literal_pool& literals )
{
    const Walk walk { .func = func, .literals = literals, .addressed = addressed_locals_of( func ) };
    const std::vector<std::optional<Table>> entry   = walk.on_entry();
    bool                                    changed = false;
    std::vector<bool>                       deleted( func.statements.size(), false );

    for( const Block_id b : func.blocks.ids() )
    {
        if( !entry[b.v] )
        {
            continue;
        }

        Block& block = func.blocks[b];
        Table  table = *entry[b.v];
        for( u32 s = 0; s < block.statement_count; ++s )
        {
            Statement& statement = func.statements_of( block )[s];
            // `i = i + 1` stays a step the loop passes see, though a cast's `i + 1` made it.
            if( statement.kind == Statement_kind::Assign && statement.place.is_whole_local() &&
                !reads_local( func, func.rvalue( statement ).a, statement.place.local ) &&
                !reads_local( func, func.rvalue( statement ).b, statement.place.local ) )
            {
                Rvalue& value = func.rvalue( statement );
                if( const Made* made = sharable( value ) ? held( func, literals, table, value ) : nullptr;
                    made && func.locals[made->local].type == func.locals[statement.place.local].type )
                {
                    changed = true;
                    // The unrolled `_4 = k / 3` again into `_4`, which holds it.
                    if( made->local == statement.place.local )
                    {
                        deleted[block.first_statement + s] = true;
                        continue;
                    }
                    value = use( copy( Place { .local = made->local }, value.type ) );
                }
            }
            walk.step( statement, table );
        }
    }

    if( changed )
    {
        remove_statements( func, deleted );
    }

    return changed;
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

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Eliminated : Compiled
{
    explicit Eliminated( std::string_view source, u32 level = 3 )
        : Compiled( std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, Build { .level = level } )
    {
        INFO( rendered() );
        REQUIRE( clean() );
        REQUIRE( verify( functions.front() ).empty() );
    }

    std::size_t divisions() const
    {
        const Function& f = functions.front();
        return static_cast<std::size_t>( std::ranges::count_if(
            f.statements,
            [&]( const Statement& statement )
            {
                return statement.kind == Statement_kind::Assign && f.rvalue( statement ).kind == Rvalue_kind::Binary &&
                       f.rvalue( statement ).op == Token_kind::Slash;
            }
        ) );
    }
};

} // namespace

TEST_CASE( "eliminate_common_divides_a_repeated_division_once", "[opt][eliminate_common]" )
{
    REQUIRE( Eliminated( "i32 f( i32 a, i32 b ) { return a / b + a / b; }\n" ).divisions() == 1 );
    REQUIRE( Eliminated( "i32 f( i32 a, i32 b ) { return a / b + a / b; }\n", 2 ).divisions() == 2 );
}

// `a` changes between them.
TEST_CASE( "eliminate_common_keeps_a_division_its_operand_changed", "[opt][eliminate_common]" )
{
    REQUIRE( Eliminated( "i32 f( i32 a, i32 b ) { i32 x = a / b; a += 1; return x + a / b; }\n" ).divisions() == 2 );
}

// One path into the second never divided.
TEST_CASE( "eliminate_common_keeps_a_division_one_path_did_not_make", "[opt][eliminate_common]" )
{
    REQUIRE(
        Eliminated( "i32 f( i32 a, i32 b, bool c ) { i32 x = 0; if( c ) { x = a / b; } return x + a / b; }\n" ).divisions() == 2
    );
}

// `g` is a global the call may change.
TEST_CASE( "eliminate_common_keeps_a_division_of_a_global_a_call_separates", "[opt][eliminate_common]" )
{
    REQUIRE(
        Eliminated( "i32 f( i32 b ) { i32 x = g / b; touch(); return x + g / b; }\n"
                    "i32 g = 7;\nvoid touch() { g += 1; }\n" )
            .divisions() == 2
    );
}

// A read through a reference is a value until something may store through a pointer.
TEST_CASE( "eliminate_common_divides_a_read_through_a_reference_once", "[opt][eliminate_common]" )
{
    REQUIRE(
        Eliminated( "i32 f( const ref Pair p, i32 b ) { return p.a / b + p.a / b; }\n"
                    "struct Pair { i32 a; i32 b; };\n" )
            .divisions() == 1
    );
}

// `q` may point into `p`.
TEST_CASE( "eliminate_common_keeps_a_read_a_store_through_a_pointer_separates", "[opt][eliminate_common]" )
{
    REQUIRE(
        Eliminated( "i32 f( const ref Pair p, i32* q, i32 b ) { i32 x = p.a / b; *q = 1; return x + p.a / b; }\n"
                    "struct Pair { i32 a; i32 b; };\n" )
            .divisions() == 2
    );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
