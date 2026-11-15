// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/eliminate_common.h"
#include <algorithm>
#include <optional>
#include <vector>
#include "ir/simplify.h"
#include "opt/forward_reads.h"
#include "opt/held_table.h"

namespace keel
{
namespace
{

bool pure( Rvalue_kind kind )
{
    switch( kind )
    {
    case Rvalue_kind::Binary:
    case Rvalue_kind::Unary:
    case Rvalue_kind::Overflows:
    case Rvalue_kind::Cast:
    case Rvalue_kind::Bits:
    case Rvalue_kind::Sqrt:
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
    if( a.kind != b.kind || a.type != b.type || a.op != b.op || a.callee != b.callee || a.type_arguments != b.type_arguments ||
        a.arguments.size() != b.arguments.size() ||
        ( a.kind != Rvalue_kind::Call && !same_operand( func, literals, a.a, b.a ) ) ||
        ( uses_b( a.kind ) && !same_operand( func, literals, a.b, b.b ) ) )
    {
        return false;
    }
    for( std::size_t i = 0; i < a.arguments.size(); ++i )
    {
        if( !same_operand( func, literals, a.arguments[i], b.arguments[i] ) )
        {
            return false;
        }
    }
    return true;
}

// Copies of places and constants only: a move or a debug flag is not a value to share.
bool sharable( const Rvalue& value, const Purity& purity )
{
    const auto plain = []( const Operand& operand )
    { return operand.kind == Operand_kind::Copy || operand.kind == Operand_kind::Constant; };
    if( value.kind == Rvalue_kind::Call )
    {
        return purity.pure( value ) && std::ranges::all_of( value.arguments, plain );
    }
    return pure( value.kind ) && plain( value.a ) && ( !uses_b( value.kind ) || plain( value.b ) );
}

bool reads_local( const Function& func, const Rvalue& value, Local_id local )
{
    return any_operand(
        value, [&]( const Operand& operand ) { return operand.has_place() && mentions( func, operand.place, local ); }
    );
}

// Whether a call or a store through a pointer may change what `operand` reads.
bool in_memory( const Function& func, const Local_set& addressed, const Operand& operand )
{
    return operand.has_place() && ( operand.place.is_global() || has_deref_projection( func, operand.place ) ||
                                    is_addressed( addressed, operand.place.local ) );
}

struct Value_traits
{
    const Function&     func;
    const Literal_pool& literals;
    const Local_set&    addressed;

    u64 hash( const Rvalue& v ) const
    {
        u64 h = mixed(
            mixed( mixed( mixed( 0xcbf29ce484222325ull, static_cast<u64>( v.kind ) ), v.type.v ), static_cast<u64>( v.op ) ),
            v.callee.v
        );
        const auto operand = [&]( const Operand& o )
        {
            h = mixed( mixed( h, static_cast<u64>( o.kind ) ), o.type.v );
            if( o.has_place() )
            {
                h = mixed( h, place_hash( func, o.place ) );
            }
        };
        if( v.kind != Rvalue_kind::Call )
        {
            operand( v.a );
        }
        if( uses_b( v.kind ) )
        {
            operand( v.b );
        }
        for( const Operand& o : v.arguments )
        {
            operand( o );
        }
        return h;
    }

    bool same( const Rvalue& a, const Rvalue& b ) const
    {
        return same_value( func, literals, a, b );
    }

    void mentioned( const Rvalue& v, std::vector<Local_id>& out ) const
    {
        for_each_operand(
            v,
            [&]( const Operand& o )
            {
                if( o.has_place() )
                {
                    mentioned_by( func, o.place, out );
                }
            }
        );
    }

    bool in_memory( const Rvalue& v ) const
    {
        return any_operand( v, [&]( const Operand& o ) { return keel::in_memory( func, addressed, o ); } );
    }
};

// A computation, and the local holding it.
using Table = Held_table<Rvalue, Value_traits>;
using Made  = Table::Entry;

const Made* held( const Table& table, const Rvalue& value )
{
    return table.find( value );
}

struct Walk
{
    const Function&     func;
    const Literal_pool& literals;
    const Purity&       purity;
    const Local_set     addressed;
    const Value_traits  traits { func, literals, addressed };

    void step( const Statement& statement, Table& table ) const
    {
        if( clobbers( func, addressed, statement ) )
        {
            table.forget_memory();
        }
        if( !statement.place.is_global() && statement.place.local.is_valid() )
        {
            table.forget( statement.place.local );
        }

        if( statement.kind != Statement_kind::Assign || !statement.place.is_whole_local() )
        {
            return;
        }

        const Rvalue&  value = func.rvalue( statement );
        const Local_id into  = statement.place.local;
        if( sharable( value, purity ) && value.type == func.locals[into].type && !is_addressed( addressed, into ) &&
            !reads_local( func, value, into ) && !held( table, value ) )
        {
            table.add( value, into );
        }
    }

    // What every path into each block made: available expressions, met by intersection.
    std::vector<std::optional<Table>> on_entry() const
    {
        std::vector<std::optional<Table>> entry( func.blocks.size(), std::nullopt );
        entry[0].emplace( traits );

        // Entries only go away, and a block is reached once, so this ends.
        bool                  changed = true;
        std::vector<Block_id> targets;
        std::vector<bool>     dirty( func.blocks.size(), true );
        Table                 table( traits );
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
                    into->erase_if(
                        [&]( const Made& made )
                        {
                            const Made* other = held( table, made.key );
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

bool eliminate_common( Function& func, const Literal_pool& literals, const Purity& purity )
{
    const Walk walk { .func = func, .literals = literals, .purity = purity, .addressed = addressed_locals_of( func ) };
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
                !reads_local( func, func.rvalue( statement ), statement.place.local ) )
            {
                Rvalue& value = func.rvalue( statement );
                if( const Made* made = sharable( value, purity ) ? held( table, value ) : nullptr;
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
    explicit Eliminated( std::string_view source, u32 level = 3, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + "i32 main() { return 0; }\n",
              Stage::Elaborated,
              Build { .level = level, .inlining = inlining }
          )
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

    std::size_t calls() const
    {
        const Function& f = functions.front();
        return static_cast<std::size_t>( std::ranges::count_if(
            f.statements,
            [&]( const Statement& statement )
            { return statement.kind == Statement_kind::Assign && f.rvalue( statement ).kind == Rvalue_kind::Call; }
        ) );
    }
};

// Not inlined, so `g`'s calls are still calls.
std::size_t calls_of( std::string_view g, std::string_view f = "i64 f( i64 a ) { return g( a ) + g( a ); }\n" )
{
    return Eliminated( std::string( f ) + std::string( g ), 3, std::nullopt ).calls();
}

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

// Its result depends on its argument alone; recursion and a `const` table do not change that.
TEST_CASE( "eliminate_common_calls_a_pure_function_once", "[opt][eliminate_common]" )
{
    REQUIRE( calls_of( "i64 g( i64 a ) { return a * a + 1; }\n" ) == 1 );
    REQUIRE( calls_of( "i64 g( i64 n ) { return n <= 0 ? 0 : g( n - 1 ) + 1; }\n" ) == 1 );
    REQUIRE( calls_of( "const i64[2] t = [ 3, 5 ];\ni64 g( i64 a ) { return t[cast<u64>( a & 1 )]; }\n" ) == 1 );
}

// An extern, a write to a global, a read of one that changes, and a read through a reference.
TEST_CASE( "eliminate_common_calls_an_impure_function_each_time", "[opt][eliminate_common]" )
{
    REQUIRE( calls_of( "extern i64 g( i64 a );\n", "i64 f( i64 a ) { unsafe { return g( a ) + g( a ); } }\n" ) == 2 );
    REQUIRE( calls_of( "i64 t = 0;\ni64 g( i64 a ) { t += a; return a; }\n" ) == 2 );
    REQUIRE( calls_of( "i64 t = 0;\ni64 g( i64 a ) { return a + t; }\nvoid h() { t = 1; }\n" ) == 2 );
    REQUIRE( calls_of( "i64 g( const ref i64 a ) { return a; }\n" ) == 2 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
