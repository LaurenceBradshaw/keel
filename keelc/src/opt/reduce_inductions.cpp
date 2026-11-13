// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/reduce_inductions.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <vector>
#include "ir/live.h"
#include "ir/loops.h"
#include "ir/simplify.h"
#include "opt/fold_constants.h"
#include "opt/forward_reads.h"

namespace keel
{

namespace
{
__extension__ typedef __int128 Wide;

// A local whose one write in the loop is `local = local + step`, at `index` in `block`.
struct Basic
{
    Local_id local;
    Operand  step;
    Block_id block;
    u32      index;
};

// `local` is `from op other`, `other` invariant.
struct Derived
{
    Local_id   local;
    Local_id   from;
    Token_kind op;
    Operand    other;
};

// One loop, as the function now stands.
struct Scope
{
    const Function&    func;
    const Loop&        loop;
    const Local_set&   addressed;
    std::vector<u32>   writes; // by local: statements in the loop that write it or take its address
    std::vector<Basic> basics;

    Scope( const Function& f, const Loop& l, const Local_set& a )
        : func( f ),
          loop( l ),
          addressed( a ),
          writes( f.locals.size(), 0 )
    {
        for( const Block_id b : loop.blocks )
        {
            for( const Statement& statement : func.statements_of( b ) )
            {
                if( !statement.place.is_global() && statement.place.local.is_valid() )
                {
                    ++writes[statement.place.local.v];
                }
                if( statement.kind == Statement_kind::Assign && func.rvalue( statement ).kind == Rvalue_kind::Address_of &&
                    !func.rvalue( statement ).a.place.is_global() )
                {
                    writes[func.rvalue( statement ).a.place.local.v] += 2;
                }
            }
        }

        for( const Block_id b : loop.blocks )
        {
            const Run<const Statement> statements = func.statements_of( b );
            for( u32 s = 0; s < statements.size(); ++s )
            {
                const Statement& statement = statements[s];
                if( statement.kind != Statement_kind::Assign || !statement.place.is_whole_local() )
                {
                    continue;
                }
                const Local_id local = statement.place.local;
                const Rvalue&  value = func.rvalue( statement );
                if( writes[local.v] != 1 || is_addressed( addressed, local ) || value.kind != Rvalue_kind::Binary ||
                    value.op != Token_kind::Plus )
                {
                    continue;
                }
                if( is_local( value.a, local ) && invariant( value.b ) )
                {
                    basics.push_back( Basic { local, value.b, b, s } );
                }
                else if( is_local( value.b, local ) && invariant( value.a ) )
                {
                    basics.push_back( Basic { local, value.a, b, s } );
                }
            }
        }
    }

    static bool is_local( const Operand& operand, Local_id local )
    {
        return operand.kind == Operand_kind::Copy && operand.place.is_whole_local() && operand.place.local == local;
    }

    bool invariant( const Operand& operand ) const
    {
        return operand.kind == Operand_kind::Constant ||
               ( operand.kind == Operand_kind::Copy && operand.place.is_whole_local() && writes[operand.place.local.v] == 0 &&
                 !is_addressed( addressed, operand.place.local ) );
    }

    const Basic* basic( const Operand& operand ) const
    {
        for( const Basic& b : basics )
        {
            if( is_local( operand, b.local ) )
            {
                return &b;
            }
        }
        return nullptr;
    }

    bool is_basic( Local_id local ) const
    {
        return std::ranges::any_of( basics, [&]( const Basic& b ) { return b.local == local; } );
    }
};

std::vector<std::vector<Block_id>> predecessors_of( const Function& func )
{
    std::vector<std::vector<Block_id>> predecessors( func.blocks.size() );
    std::vector<Block_id>              targets;
    for( const Block_id b : func.blocks.ids() )
    {
        targets.clear();
        successors( func.blocks[b].terminator, targets );
        for( const Block_id target : targets )
        {
            predecessors[target.v].push_back( b );
        }
    }
    return predecessors;
}

// The constant `local` holds on entering the loop: its last write up the preheader's one-predecessor chain.
std::optional<Operand> entry_constant( const Function& func, const Loop& loop, Local_id local )
{
    const std::vector<std::vector<Block_id>> predecessors = predecessors_of( func );
    Block_id                                 b            = loop.preheader;
    for( std::size_t steps = 0; steps < func.blocks.size(); ++steps )
    {
        const Run<const Statement> statements = func.statements_of( b );
        for( u32 s = statements.size(); s-- > 0; )
        {
            const Statement& statement = statements[s];
            if( statement.place.is_global() || statement.place.local != local )
            {
                continue;
            }
            if( statement.kind == Statement_kind::Assign && statement.place.is_whole_local() &&
                func.rvalue( statement ).kind == Rvalue_kind::Use && func.rvalue( statement ).a.kind == Operand_kind::Constant )
            {
                return func.rvalue( statement ).a;
            }
            return std::nullopt;
        }
        if( predecessors[b.v].size() != 1 )
        {
            return std::nullopt;
        }
        b = predecessors[b.v].front();
    }
    return std::nullopt;
}

// Reads of `from` as copies of `into`, in `block` after `index`, until either is written.
void forward( Function& func, Block_id block, u32 index, Local_id from, Local_id into )
{
    const auto rewrite = [&]( Operand& operand )
    {
        if( Scope::is_local( operand, from ) )
        {
            operand.place.local = into;
        }
    };

    Block& b = func.blocks[block];
    for( u32 s = index + 1; s < b.statement_count; ++s )
    {
        Statement& statement = func.statements[b.first_statement + s];
        if( statement.kind == Statement_kind::Assign && func.rvalue( statement ).kind != Rvalue_kind::Address_of )
        {
            Rvalue& value = func.rvalue( statement );
            rewrite( value.a );
            rewrite( value.b );
            std::ranges::for_each( value.arguments, rewrite );
        }
        if( !statement.place.is_global() && ( statement.place.local == from || statement.place.local == into ) )
        {
            return;
        }
    }
    rewrite( b.terminator.condition );
    rewrite( b.terminator.message );
    rewrite( b.terminator.at );
}

// Turns one `i op c` into a stepped local: a multiply when `only` is invalid, else a `+`, `-` or
// power-of-two `*` of `only`.
// Whether it found one.
bool reduce_one(
    Function&             func,
    const Type_table&     types,
    Literal_pool&         literals,
    const Scope&          scope,
    std::vector<Derived>& derived,
    Local_id              only
)
{
    for( const Block_id b : scope.loop.blocks )
    {
        const Run<const Statement> statements = func.statements_of( b );
        for( u32 s = 0; s < statements.size(); ++s )
        {
            const Statement& statement = statements[s];
            if( statement.kind != Statement_kind::Assign || !statement.place.is_whole_local() ||
                scope.is_basic( statement.place.local ) )
            {
                continue;
            }

            const Rvalue& value = func.rvalue( statement );
            if( value.kind != Rvalue_kind::Binary ||
                ( value.op != Token_kind::Plus && value.op != Token_kind::Minus && value.op != Token_kind::Star ) )
            {
                continue;
            }

            const Type_kind kind  = types.get( value.type ).kind;
            const Basic*    basic = scope.basic( value.a );
            Operand         other = value.b;
            if( !basic && value.op != Token_kind::Minus )
            {
                basic = scope.basic( value.b );
                other = value.a;
            }
            const bool pointer =
                kind == Type_kind::Many_pointer && value.op == Token_kind::Plus && basic == scope.basic( value.b );
            if( !basic || !scope.invariant( other ) || ( kind != Type_kind::Int && !pointer ) )
            {
                continue;
            }

            // The same `i op c` again takes the local the first made.
            const auto same = [&]( const Derived& d )
            {
                return d.from == basic->local && d.op == value.op && func.locals[d.local].type == value.type &&
                       d.other.kind == other.kind &&
                       ( other.kind == Operand_kind::Constant
                             ? literals.integer( d.other.constant ) == literals.integer( other.constant )
                             : d.other.place.local == other.place.local );
            };
            // `i + 0` is `i`; a preheader's `d + 0` would otherwise be stepped twice.
            const bool zero = value.op != Token_kind::Star && other.kind == Operand_kind::Constant &&
                              literals.integer( other.constant ) == 0 && kind == Type_kind::Int;
            const auto found = std::ranges::find_if( derived, same );
            // A shift is no dearer than the add that would replace it.
            const bool cheap = value.op != Token_kind::Star || ( other.kind == Operand_kind::Constant &&
                                                                 std::has_single_bit( literals.integer( other.constant ) ) );
            if( !zero && found == derived.end() && ( only.is_valid() ? !cheap || basic->local != only : cheap ) )
            {
                continue;
            }
            if( zero || found != derived.end() )
            {
                const Local_id x    = statement.place.local;
                const Local_id into = zero ? basic->local : found->local;
                func.rvalue( func.statements[func.blocks[b].first_statement + s] ) =
                    use( copy( Place { .local = into }, value.type ) );
                if( !is_addressed( scope.addressed, x ) )
                {
                    forward( func, b, s, x, into );
                }
                return true;
            }

            // Copied out: add_rvalue below can move `value`.
            const Local_id   x       = statement.place.local;
            const Token_kind op      = value.op;
            const Type_id    type    = value.type;
            const Span       span    = statement.span;
            const Basic      i       = *basic;
            Rvalue           initial = value;
            if( const std::optional<Operand> entry = entry_constant( func, scope.loop, i.local ) )
            {
                ( Scope::is_local( initial.a, i.local ) ? initial.a : initial.b ) = *entry;
                if( std::optional<Rvalue> f = folded( initial, types, literals ) )
                {
                    initial = *f;
                }
            }

            std::optional<Statement> product;
            Operand                  step = i.step;
            if( op == Token_kind::Star )
            {
                const Rvalue times = binary( Token_kind::Star, other, i.step, type );
                if( i.step.kind == Operand_kind::Constant && literals.integer( i.step.constant ) == 1 )
                {
                    step = other;
                }
                else if( std::optional<Rvalue> f = folded( times, types, literals ) )
                {
                    step = f->a;
                }
                else
                {
                    const Local_id local = func.locals.push_back( Local { .type = type, .span = span } );
                    step                 = copy( Place { .local = local }, type );
                    product = Statement { .span = span, .place = Place { .local = local }, .value = func.add_rvalue( times ) };
                }
            }

            const Local_id d = func.locals.push_back( Local { .type = type, .span = span } );
            derived.push_back( Derived { .local = d, .from = i.local, .op = op, .other = other } );
            func.rvalue( func.statements[func.blocks[b].first_statement + s] ) = use( copy( Place { .local = d }, type ) );
            if( !is_addressed( scope.addressed, x ) )
            {
                forward( func, b, s, x, d );
            }

            if( product )
            {
                insert_statement(
                    func, scope.loop.preheader, func.blocks[scope.loop.preheader].statement_count, std::move( *product )
                );
            }
            insert_statement(
                func,
                scope.loop.preheader,
                func.blocks[scope.loop.preheader].statement_count,
                Statement { .span = span, .place = Place { .local = d }, .value = func.add_rvalue( initial ) }
            );
            insert_statement(
                func,
                i.block,
                i.index + 1,
                Statement {
                    .span  = func.statements_of( i.block )[i.index].span,
                    .place = Place { .local = d },
                    .value = func.add_rvalue( binary( Token_kind::Plus, copy( Place { .local = d }, type ), step, type ) )
                }
            );
            remove_statements( func, std::vector<bool>( func.statements.size(), false ) );
            return true;
        }
    }
    return false;
}

struct Range
{
    Wide low;
    Wide high;

    bool holds( Wide v ) const
    {
        return low <= v && v <= high;
    }
};

std::optional<Range> range_of( const Type_table& types, Type_id type )
{
    const Type& t = types.get( type );
    if( t.kind != Type_kind::Int )
    {
        return std::nullopt;
    }
    if( t.is_signed )
    {
        return Range { -( Wide( 1 ) << ( t.width - 1 ) ), ( Wide( 1 ) << ( t.width - 1 ) ) - 1 };
    }
    return Range { 0, ( Wide( 1 ) << t.width ) - 1 };
}

Wide number( u64 bits, const Type_table& types, Type_id type )
{
    const Type& t = types.get( type );
    return t.is_signed ? Wide( as_signed( bits, t.width ) ) : Wide( truncate( bits, t.width ) );
}

// `at`'s `t = i < n`, after `i`'s step there, with the entry value, step and `n` constants, the step
// positive, and no value the test sees wrapping.
struct Counted
{
    u32   test; // in `at`
    Range range;
    Wide  start;
    Wide  step;
    Wide  n;
    Wide  last; // no less than the last value the test sees
};

std::optional<Counted> counted(
    const Function& func, const Type_table& types, const Literal_pool& literals, const Scope& scope, const Basic& i, Block_id at
)
{
    if( !at.is_valid() || i.block != at || func.blocks[at].terminator.kind != Terminator_kind::Branch )
    {
        return std::nullopt;
    }
    const Operand& condition = func.blocks[at].terminator.condition;
    if( condition.kind != Operand_kind::Copy || !condition.place.is_whole_local() ||
        is_addressed( scope.addressed, condition.place.local ) )
    {
        return std::nullopt;
    }

    // The last write of the condition, after the step.
    const Run<const Statement> statements = func.statements_of( at );
    u32                        test       = statements.size();
    for( u32 s = statements.size(); s-- > i.index + 1; )
    {
        if( !statements[s].place.is_global() && statements[s].place.local == condition.place.local )
        {
            test = s;
            break;
        }
    }
    if( test == statements.size() || !statements[test].place.is_whole_local() ||
        statements[test].kind != Statement_kind::Assign )
    {
        return std::nullopt;
    }

    const Rvalue&                compare = func.rvalue( statements[test] );
    const Type_id                type    = func.locals[i.local].type;
    const std::optional<Range>   range   = range_of( types, type );
    const std::optional<Operand> entry   = entry_constant( func, scope.loop, i.local );
    if( compare.kind != Rvalue_kind::Binary || compare.op != Token_kind::Less || !Scope::is_local( compare.a, i.local ) ||
        compare.b.kind != Operand_kind::Constant || compare.b.type != type || i.step.kind != Operand_kind::Constant || !range ||
        !entry )
    {
        return std::nullopt;
    }

    const Wide start = number( literals.integer( entry->constant ), types, type );
    const Wide step  = number( literals.integer( i.step.constant ), types, type );
    const Wide n     = number( literals.integer( compare.b.constant ), types, type );
    // The tests see `start + step`, `start + 2 step`, … up to the first at or past `n`.
    const Wide last = std::max( start, n - 1 ) + step;
    if( step <= 0 || !range->holds( last ) )
    {
        return std::nullopt;
    }
    return Counted { test, *range, start, step, n, last };
}

// The latch's `t = i < n` reads a derived local instead, and `i`'s step goes, when nothing else reads `i`
// and the constants show the derived local passes `a * n + b` exactly when `i` passes `n`. Whether it did.
bool retire_one(
    Function&                   func,
    const Type_table&           types,
    Literal_pool&               literals,
    const Scope&                scope,
    const std::vector<Derived>& derived,
    Local_id                    only
)
{
    const Block_id latch = scope.loop.latch;
    for( const Basic& i : scope.basics )
    {
        const std::optional<Counted> count = i.local == only ? counted( func, types, literals, scope, i, latch ) : std::nullopt;
        if( !count )
        {
            continue;
        }
        const auto [test, range, start, step, n, last] = *count;
        const Type_id type                             = func.locals[i.local].type;
        const Type_id result                           = func.rvalue( func.statements_of( latch )[test] ).type;

        const Derived* chosen = nullptr;
        Wide           bound  = 0;
        for( const Derived& d : derived )
        {
            if( d.from != i.local || func.locals[d.local].type != type || d.other.kind != Operand_kind::Constant )
            {
                continue;
            }
            const Wide c      = number( literals.integer( d.other.constant ), types, type );
            const Wide scale  = d.op == Token_kind::Star ? c : 1;
            const Wide offset = d.op == Token_kind::Star ? 0 : d.op == Token_kind::Plus ? c : -c;
            Wide       low = 0, high = 0, at = 0;
            if( scale <= 0 || __builtin_mul_overflow( scale, start + step, &low ) ||
                __builtin_mul_overflow( scale, last, &high ) || __builtin_mul_overflow( scale, n, &at ) ||
                !range.holds( low + offset ) || !range.holds( high + offset ) || !range.holds( at + offset ) )
            {
                continue;
            }
            chosen = &d;
            bound  = at + offset;
            break;
        }
        if( !chosen )
        {
            continue;
        }

        // Nothing else reads `i`: not in the loop, and not after it.
        bool read = false;
        for( const Block_id b : scope.loop.blocks )
        {
            const Run<const Statement> run = func.statements_of( b );
            for( u32 s = 0; s < run.size(); ++s )
            {
                read |= !( b == latch && ( s == i.index || s == test ) ) && reads( func, run[s], i.local );
            }
            read |= reads( func, func.blocks[b].terminator, i.local );
        }
        if( read )
        {
            continue;
        }
        const Liveness live = live_locals( func );
        if( std::ranges::any_of( scope.loop.exits, [&]( Block_id exit ) { return live.on_entry[exit.v][i.local.v]; } ) )
        {
            continue;
        }

        const u32 first                              = func.blocks[latch].first_statement;
        func.rvalue( func.statements[first + test] ) = binary(
            Token_kind::Less,
            copy( Place { .local = chosen->local }, type ),
            constant( literals.add_integer( truncate( static_cast<u64>( bound ), types.get( type ).width ) ), type ),
            result
        );
        std::vector<bool> deleted( func.statements.size(), false );
        deleted[first + i.index] = true;
        remove_statements( func, deleted );
        return true;
    }
    return false;
}
} // namespace

std::optional<Trip> trip_count(
    const Function& func, const Type_table& types, const Literal_pool& literals, const Loop& loop, const Local_set& addressed
)
{
    const Scope scope( func, loop, addressed );
    for( const Block_id at : { loop.header, loop.latch } )
    {
        if( !at.is_valid() || func.blocks[at].terminator.kind != Terminator_kind::Branch ||
            !loop.contains( func.blocks[at].terminator.targets[0] ) || loop.contains( func.blocks[at].terminator.targets[1] ) )
        {
            continue;
        }
        for( const Basic& i : scope.basics )
        {
            if( const std::optional<Counted> c = counted( func, types, literals, scope, i, at ) )
            {
                return Trip { static_cast<u64>( c->start >= c->n ? 1 : ( c->n - c->start + c->step - 1 ) / c->step ), at };
            }
        }
    }
    return std::nullopt;
}

bool reduce_inductions( Function& func, const Type_table& types, Literal_pool& literals )
{
    const Loops     found     = find_loops( func );
    const Local_set addressed = addressed_locals_of( func );
    bool            changed   = false;

    for( const Loop& loop : found.loops )
    {
        if( !loop.preheader.is_valid() )
        {
            continue;
        }
        std::vector<Derived> derived;
        while( reduce_one( func, types, literals, Scope( func, loop, addressed ), derived, Local_id {} ) )
        {
            changed = true;
        }

        // A stepped `i + c` or `i << k` saves nothing unless `i` then goes, so it is undone when `i` stays.
        std::vector<Local_id> basics;
        for( const Basic& i : Scope( func, loop, addressed ).basics )
        {
            basics.push_back( i.local );
        }
        for( const Local_id i : basics )
        {
            const Function    before  = func;
            const std::size_t kept    = derived.size();
            bool              reduced = false;
            while( reduce_one( func, types, literals, Scope( func, loop, addressed ), derived, i ) )
            {
                reduced = true;
            }
            if( retire_one( func, types, literals, Scope( func, loop, addressed ), derived, i ) )
            {
                changed = true;
            }
            else if( reduced )
            {
                func = before;
                derived.resize( kept );
            }
        }
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

// Optimised and elaborated, as a release build emits it, without unrolling. The function under test is always the first.
struct Reduced : Compiled
{
    explicit Reduced( std::string_view source, u32 level = 3 )
        : Compiled(
              std::string( source ) + "i32 main() { return 0; }\n",
              Stage::Elaborated,
              Build { .level = level, .loops = Loop_weights { .unrolling = 0 } }
          )
    {
        INFO( rendered() );
        REQUIRE( clean() );
        REQUIRE( verify( functions.front() ).empty() );
    }

    // Binary statements in a loop with `op`; with `bound`, only those whose right operand is that constant.
    std::size_t in_loops( Token_kind op, std::optional<u64> bound = std::nullopt ) const
    {
        const Function&   func = functions.front();
        std::vector<bool> inside( func.blocks.size(), false );
        for( const Loop& loop : find_loops( func ).loops )
        {
            for( const Block_id b : loop.blocks )
            {
                inside[b.v] = true;
            }
        }

        std::size_t count = 0;
        for( const Block_id b : func.blocks.ids() )
        {
            for( const Statement& statement : func.statements_of( b ) )
            {
                if( !inside[b.v] || statement.kind != Statement_kind::Assign )
                {
                    continue;
                }
                const Rvalue& value = func.rvalue( statement );
                count +=
                    value.kind == Rvalue_kind::Binary && value.op == op &&
                    ( !bound || ( value.b.kind == Operand_kind::Constant && literals.integer( value.b.constant ) == *bound ) );
            }
        }
        return count;
    }
};

} // namespace

TEST_CASE( "reduce_inductions_steps_a_product_by_an_invariant", "[opt][reduce_inductions]" )
{
    constexpr std::string_view source =
        "i64 f( i64 n, i64 k ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { t += i * k; } return t; }\n";
    REQUIRE( Reduced( source ).in_loops( Token_kind::Star ) == 0 );
    REQUIRE( Reduced( source, 1 ).in_loops( Token_kind::Star ) == 1 );
}

// A shift costs what its step would, so it stays while `i` does.
TEST_CASE( "reduce_inductions_keeps_a_shift_its_index_stays", "[opt][reduce_inductions]" )
{
    const Reduced f( "u64 f( u64 n ) { u64 t = 0; for( u64 i = 0; i < n; i += 1 ) { t += i * 16; } return t; }\n" );
    REQUIRE( f.in_loops( Token_kind::Star ) == 1 );
}

// `i * 16 + j` is stepped, its bounds check folds, and the outer test reads it against 256.
TEST_CASE( "reduce_inductions_retires_an_index_its_steps_replace", "[opt][reduce_inductions]" )
{
    const Reduced f( "i64 f( const ref i64[256] a ) { i64 t = 0; for( u64 i = 0; i < 16; i += 1 ) { for( u64 j = 0; j < 16; "
                     "j += 1 ) { t += a[ i * 16 + j ]; } } return t; }\n" );
    REQUIRE( f.in_loops( Token_kind::Star ) == 0 );
    REQUIRE( f.in_loops( Token_kind::Less, 256 ) == 1 );
}

// At 200 the test would be `twice < 400`, past a u8; at 100 it fits.
TEST_CASE( "reduce_inductions_keeps_an_index_its_test_would_wrap", "[opt][reduce_inductions]" )
{
    const auto source = []( std::string_view n )
    {
        return "u64 f( u64 stop ) { u64 t = 0; for( u8 i = 0; i < " + std::string( n ) +
               "; i += 1 ) { if( t >= stop ) { break; } const u8 twice = i * 2; t += cast<u64>( twice ); } return t; }\n";
    };
    REQUIRE( Reduced( source( "200" ) ).in_loops( Token_kind::Star ) == 1 );
    const Reduced fits( source( "100" ) );
    REQUIRE( fits.in_loops( Token_kind::Star ) == 0 );
    REQUIRE( fits.in_loops( Token_kind::Less, 200 ) == 1 );
}

// `i` stays for the test against `n`, so `p + i` is not stepped.
TEST_CASE( "reduce_inductions_keeps_an_address_its_index_stays", "[opt][reduce_inductions]" )
{
    constexpr std::string_view source = "i64 f( const i64[*] p, u64 n ) { i64 t = 0; for( u64 i = 0; i < n; i += 1 ) { unsafe "
                                        "{ t += ( p + i )[0]; } } return t; }\n";
    REQUIRE( Reduced( source ).in_loops( Token_kind::Plus ) == Reduced( source, 1 ).in_loops( Token_kind::Plus ) );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
