// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/recognise_idioms.h"

#include <algorithm>
#include "ir/simplify.h"

namespace keel
{
namespace
{

// The shift `operand` holds at `index` of `block`, written there by `x << c` or `x >> c` of a whole
// local `x` and a constant `c`.
struct Shift
{
    u32        at = 0;
    Token_kind op {};
    Local_id   x {};
    u64        by = 0;
};

// What a counting loop steps and counts.
struct Count
{
    Rvalue_kind kind = Rvalue_kind::Count_ones; // Leading_zeros for a shift
    Local_id    x {};
    Local_id    n {};
    Token_kind  sign = Token_kind::Plus; // `n`'s `+ 1` or `- 1`
    Local_id    less {};                 // `x - 1`, which `x &=`; none for a shift
};

class Recogniser
{
public:
    Recogniser( Function& func, const Type_table& table, Literal_pool& literals )
        : func_( func ),
          table_( table ),
          literals_( literals ),
          addressed_( func.locals.size(), false )
    {
        // A store through a pointer could change an addressed local between its reads.
        for( const Statement& statement : func.statements )
        {
            if( statement.value.is_valid() && func.rvalue( statement ).kind == Rvalue_kind::Address_of &&
                !func.rvalue( statement ).a.place.is_global() )
            {
                addressed_[func.rvalue( statement ).a.place.local.v] = true;
            }
        }
    }

    bool run()
    {
        // First, as a loop's rewrite moves its block's statements.
        for( u32 b = 0; b < func_.blocks.size(); ++b )
        {
            counting_loop( Block_id { b } );
        }
        if( changed_ )
        {
            remove_statements( func_, std::vector<bool>( func_.statements.size(), false ) );
        }

        for( const Block& block : func_.blocks )
        {
            for( u32 s = 0; s < block.statement_count; ++s )
            {
                rotate( block, s );
            }
        }

        return changed_;
    }

private:
    Function&         func_;
    const Type_table& table_;
    Literal_pool&     literals_;
    std::vector<bool> addressed_;
    bool              changed_ = false;

    // A whole local nothing takes the address of.
    bool plain( const Place& place ) const
    {
        return place.is_whole_local() && !addressed_[place.local.v];
    }

    bool unsigned_int( Type_id type ) const
    {
        const Type& t = table_.get( type );
        return t.kind == Type_kind::Int && !t.is_signed;
    }

    bool is_integer( const Operand& operand, u64 value ) const
    {
        return operand.kind == Operand_kind::Constant && literals_.kind( operand.constant ) == Literal_kind::Integer &&
               literals_.integer( operand.constant ) == value;
    }

    // The unsigned local `value` holds true exactly when it is not 0: `x != 0`, `x > 0` or `0 < x`.
    std::optional<Local_id> nonzero_test( const Rvalue& value ) const
    {
        if( value.kind != Rvalue_kind::Binary || !unsigned_int( value.a.type ) )
        {
            return std::nullopt;
        }
        const bool left = value.a.kind == Operand_kind::Copy && plain( value.a.place ) && is_integer( value.b, 0 ) &&
                          ( value.op == Token_kind::Bang_equal || value.op == Token_kind::Greater );
        const bool right = value.b.kind == Operand_kind::Copy && plain( value.b.place ) && is_integer( value.a, 0 ) &&
                           ( value.op == Token_kind::Bang_equal || value.op == Token_kind::Less );
        if( left )
        {
            return value.a.place.local;
        }
        if( right )
        {
            return value.b.place.local;
        }
        return std::nullopt;
    }

    // The reads of `local` anywhere, an index into it among them.
    u32 reads( Local_id local ) const
    {
        u32 count = 0;
        for_each_place(
            func_,
            [&]( const Place& place, const Statement* target )
            { count += target == nullptr && !place.is_global() && place.local == local; }
        );
        for( const Projection& projection : func_.projections )
        {
            count += projection.kind == Projection_kind::Index && projection.index == local;
        }
        return count;
    }

    // Whether `x != 0` whenever `from` enters `into`: `from` branches there on a test of it, or wrote it a constant other than
    // 0.
    bool entered_nonzero( const Block& from, Block_id into, Local_id x ) const
    {
        const Run<const Statement> statements = std::as_const( func_ ).statements_of( from );
        const Terminator&          end        = from.terminator;
        if( end.kind == Terminator_kind::Branch )
        {
            if( end.targets[0] != into || end.targets[1] == into )
            {
                return false;
            }
            const std::optional<u32> test = last_write( func_, from, from.statement_count, end.condition );
            if( !test || nonzero_test( func_.rvalue( statements[*test] ) ) != x )
            {
                return false;
            }
            for( u32 s = *test + 1; s < statements.size(); ++s )
            {
                if( statements[s].kind == Statement_kind::Assign && statements[s].place.is_whole_local() &&
                    statements[s].place.local == x )
                {
                    return false;
                }
            }
            return true;
        }

        const std::optional<u32> write =
            last_write( func_, from, from.statement_count, copy( Place { .local = x }, func_.locals[x].type ) );
        if( !write )
        {
            return false;
        }
        const Rvalue& value = func_.rvalue( statements[*write] );
        return value.kind == Rvalue_kind::Use && value.a.kind == Operand_kind::Constant &&
               literals_.kind( value.a.constant ) == Literal_kind::Integer && literals_.integer( value.a.constant ) != 0;
    }

    // `steps`' statements as a counting loop's body, in any order but `x - 1` before `x &=`.
    std::optional<Count> counting( Run<const Statement> steps, Local_id x ) const
    {
        const Type_id type = func_.locals[x].type;
        Count         count { .x = x };
        bool          shift = false, masked = false, lessened = false;
        for( const Statement& statement : steps )
        {
            if( statement.kind != Statement_kind::Assign || !plain( statement.place ) )
            {
                return std::nullopt;
            }
            const Rvalue&  value   = func_.rvalue( statement );
            const Local_id local   = statement.place.local;
            const auto     reads_x = [&]( const Operand& o )
            { return o.kind == Operand_kind::Copy && plain( o.place ) && o.place.local == x; };
            if( value.kind != Rvalue_kind::Binary )
            {
                return std::nullopt;
            }

            if( local == x && value.type == type && value.op == Token_kind::Greater_greater && reads_x( value.a ) &&
                is_integer( value.b, 1 ) && !shift )
            {
                shift = true;
            }
            else if( local == x && value.type == type && value.op == Token_kind::Amp && !masked &&
                     ( reads_x( value.a ) || reads_x( value.b ) ) )
            {
                const Operand& other = reads_x( value.a ) ? value.b : value.a;
                if( other.kind != Operand_kind::Copy || !plain( other.place ) || other.place.local != count.less || !lessened )
                {
                    return std::nullopt;
                }
                masked = true;
            }
            else if( local != x && value.type == type && value.op == Token_kind::Minus && reads_x( value.a ) &&
                     is_integer( value.b, 1 ) && !lessened )
            {
                count.less = local;
                lessened   = true;
            }
            else if( local != x && !count.n.is_valid() && ( value.op == Token_kind::Plus || value.op == Token_kind::Minus ) &&
                     value.a.kind == Operand_kind::Copy && plain( value.a.place ) && value.a.place.local == local &&
                     is_integer( value.b, 1 ) && table_.get( value.type ).kind == Type_kind::Int )
            {
                count.n    = local;
                count.sign = value.op;
            }
            else
            {
                return std::nullopt;
            }
        }

        const bool ones = masked && lessened && !shift && steps.size() == 3;
        const bool bits = shift && !masked && !lessened && steps.size() == 2;
        if( !count.n.is_valid() || ( !ones && !bits ) || count.n == count.less || ( ones && reads( count.less ) != 1 ) )
        {
            return std::nullopt;
        }
        count.kind = ones ? Rvalue_kind::Count_ones : Rvalue_kind::Leading_zeros;
        return count;
    }

    // `b` as one counting loop: it ends in its own test, or goes to a header that is only the test.
    void counting_loop( Block_id b )
    {
        if( b == k_entry_block )
        {
            return;
        }

        const Block&            block = func_.blocks[b];
        const Terminator&       end   = block.terminator;
        u32                     steps = block.statement_count;
        Block_id                exit {};
        Block_id                header {}; // the test's, when not `b`'s own
        std::optional<Local_id> x;
        if( end.kind == Terminator_kind::Branch && end.targets[0] == b && end.targets[1] != b && steps > 0 )
        {
            const std::optional<u32> test = last_write( func_, block, steps, end.condition );
            // Read by the branch alone, so the rewrite drops it.
            if( !test || *test != steps - 1 || reads( end.condition.place.local ) != 1 )
            {
                return;
            }
            x = nonzero_test( func_.rvalue( func_.statements_of( block )[*test] ) );
            steps--;
            exit = end.targets[1];
        }
        else if( end.kind == Terminator_kind::Goto && end.targets[0] != b )
        {
            const Block& test = func_.blocks[end.targets[0]];
            if( test.statement_count != 1 || test.terminator.kind != Terminator_kind::Branch ||
                test.terminator.targets[0] != b || test.terminator.targets[1] == b ||
                last_write( func_, test, 1, test.terminator.condition ) != 0u )
            {
                return;
            }
            x      = nonzero_test( func_.rvalue( std::as_const( func_ ).statements_of( test )[0] ) );
            header = end.targets[0];
            exit   = test.terminator.targets[1];
        }
        if( !x || !unsigned_int( func_.locals[*x].type ) )
        {
            return;
        }

        const std::optional<Count> count =
            counting( Run<const Statement>( func_.statements, block.first_statement, steps ), *x );
        if( !count )
        {
            return;
        }

        // Every other way in has `x != 0`, as the loop's own does. A guard that leaves for the loop's exit
        // otherwise, the header among them, can enter regardless: for 0 the rewrite moves `n` by 0 and leaves `x` 0.
        std::vector<Block_id> targets, guards;
        if( header.is_valid() )
        {
            guards.push_back( header );
        }
        for( u32 p = 0; p < func_.blocks.size(); ++p )
        {
            targets.clear();
            successors( func_.blocks[Block_id { p }].terminator, targets );
            const bool enters = std::find( targets.begin(), targets.end(), b ) != targets.end();
            const bool own    = header.is_valid() ? p == header.v : p == b.v;
            if( enters && !own && ( header.is_valid() || !entered_nonzero( func_.blocks[Block_id { p }], b, *x ) ) )
            {
                return;
            }
            if( enters && !own && func_.blocks[Block_id { p }].terminator.kind == Terminator_kind::Branch &&
                onward( func_.blocks[Block_id { p }].terminator.targets[1] ) == onward( exit ) )
            {
                guards.push_back( Block_id { p } );
            }
        }

        rewrite( b, *count, exit );
        for( const Block_id guard : guards )
        {
            Terminator& test = func_.blocks[guard].terminator;
            test             = Terminator { .kind = Terminator_kind::Goto, .span = test.span, .targets = { b } };
        }
    }

    // Where `block` leads past empty gotos.
    Block_id onward( Block_id block ) const
    {
        for( u32 hops = 0; hops < func_.blocks.size(); ++hops )
        {
            const Block& at = func_.blocks[block];
            if( at.statement_count != 0 || at.terminator.kind != Terminator_kind::Goto )
            {
                break;
            }
            block = at.terminator.targets[0];
        }
        return block;
    }

    Local_id temporary( Type_id type, Span span )
    {
        addressed_.push_back( false );
        return func_.locals.push_back( Local { .type = type, .span = span } );
    }

    Statement assign( Local_id local, Rvalue value, Span span )
    {
        return Statement { .span = span, .place = Place { .local = local }, .value = func_.add_rvalue( std::move( value ) ) };
    }

    // `n` moved by what the loop counts, and `x` 0; then the loop's exit.
    void rewrite( Block_id b, const Count& count, Block_id exit )
    {
        const Span    span   = func_.statements_of( b )[0].span;
        const Type_id type   = func_.locals[count.x].type;
        const Type_id n_type = func_.locals[count.n].type;
        const Operand x      = copy( Place { .local = count.x }, type );

        const bool             ones = count.kind == Rvalue_kind::Count_ones;
        std::vector<Statement> statements;
        Local_id               counted = temporary( type, span );
        statements.push_back( assign( counted, ones ? count_ones( x ) : leading_zeros( x ), span ) );
        if( n_type != type )
        {
            const Local_id cast = temporary( n_type, span );
            statements.push_back( assign( cast, cast_to( copy( Place { .local = counted }, type ), n_type ), span ) );
            counted = cast;
        }

        // The bits up to the highest set are the width less the zeros above it: `n` moves by the width, then
        // back by the zeros, so a constant `n` folds with the width.
        Operand    from = copy( Place { .local = count.n }, n_type );
        Token_kind sign = count.sign;
        if( !ones )
        {
            const Local_id moved = temporary( n_type, span );
            const Operand  width = constant( literals_.add_integer( table_.get( type ).width ), n_type );
            statements.push_back( assign( moved, binary( sign, from, width, n_type ), span ) );
            from = copy( Place { .local = moved }, n_type );
            sign = sign == Token_kind::Plus ? Token_kind::Minus : Token_kind::Plus;
        }
        statements.push_back( assign( count.n, binary( sign, from, copy( Place { .local = counted }, n_type ), n_type ), span )
        );
        statements.push_back( assign( count.x, use( constant( literals_.add_integer( 0 ), type ) ), span ) );

        Block& block     = func_.blocks[b];
        block.terminator = Terminator { .kind = Terminator_kind::Goto, .span = block.terminator.span, .targets = { exit } };

        block.first_statement = narrow_cast<u32>( func_.statements.size() );
        block.statement_count = narrow_cast<u32>( statements.size() );
        for( Statement& statement : statements )
        {
            func_.statements.push_back( std::move( statement ) );
        }
        changed_ = true;
    }

    std::optional<Shift> shift_of( const Block& block, u32 index, const Operand& operand ) const
    {
        const std::optional<u32> s = last_write( func_, block, index, operand );
        if( !s )
        {
            return std::nullopt;
        }

        const Rvalue& value = func_.rvalue( std::as_const( func_ ).statements_of( block )[*s] );
        if( value.kind != Rvalue_kind::Binary ||
            ( value.op != Token_kind::Less_less && value.op != Token_kind::Greater_greater ) ||
            value.a.kind != Operand_kind::Copy || !value.a.place.is_whole_local() || value.b.kind != Operand_kind::Constant ||
            literals_.kind( value.b.constant ) != Literal_kind::Integer )
        {
            return std::nullopt;
        }

        return Shift { .at = *s, .op = value.op, .x = value.a.place.local, .by = literals_.integer( value.b.constant ) };
    }

    // Statement `s` of `block` as a rotate.
    void rotate( const Block& block, u32 s )
    {
        const Run<Statement> statements = func_.statements_of( block );
        const Statement&     statement  = statements[s];
        if( statement.kind != Statement_kind::Assign || !statement.value.is_valid() )
        {
            return;
        }

        const Rvalue& value = func_.rvalue( statement );
        if( value.kind != Rvalue_kind::Binary || value.op != Token_kind::Pipe || !unsigned_int( value.type ) )
        {
            return;
        }
        const u8 width = table_.get( value.type ).width;

        const std::optional<Shift> a = shift_of( block, s, value.a );
        const std::optional<Shift> b = shift_of( block, s, value.b );
        if( !a || !b || a->x != b->x || a->op == b->op || addressed_[a->x.v] || func_.locals[a->x].type != value.type )
        {
            return;
        }

        const Shift& left  = a->op == Token_kind::Less_less ? *a : *b;
        const Shift& right = a->op == Token_kind::Less_less ? *b : *a;
        if( left.by == 0 || left.by >= width || left.by + right.by != width )
        {
            return;
        }

        // `x` as both shifts read it.
        for( u32 between = std::min( a->at, b->at ) + 1; between < s; ++between )
        {
            const Statement& other = statements[between];
            if( other.kind == Statement_kind::Assign && !other.place.is_global() && other.place.local == left.x )
            {
                return;
            }
        }

        const Rvalue& shifted     = func_.rvalue( statements[left.at] );
        func_.rvalue( statement ) = rotate_left( copy( Place { .local = left.x }, value.type ), shifted.b );
        changed_                  = true;
    }
};

} // namespace

bool recognise_idioms( Function& func, const Type_table& table, Literal_pool& literals )
{
    return Recogniser( func, table, literals ).run();
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

// Statements of `kind` in the first function, optimised and elaborated.
std::size_t made( std::string_view source, Rvalue_kind kind, u32 level = 2 )
{
    const Compiled c( std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, Build { .level = level } );
    INFO( c.rendered() );
    REQUIRE( c.clean() );
    const Function& func = c.functions.front();
    REQUIRE( verify( func ).empty() );
    std::size_t count = 0;
    for( const Statement& statement : func.statements )
    {
        count += statement.value.is_valid() && func.rvalue( statement ).kind == kind;
    }
    return count;
}

std::size_t rotates( std::string_view source, u32 level = 2 )
{
    return made( source, Rvalue_kind::Rotate, level );
}

// The blocks left in the first function: one when neither the loop nor its guard is.
std::size_t blocks( std::string_view source )
{
    const Compiled c( std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, Build { .level = 2 } );
    REQUIRE( c.clean() );
    return c.functions.front().blocks.size();
}

} // namespace

TEST_CASE( "recognise_idioms_makes_a_shift_or_a_rotate", "[opt][recognise_idioms]" )
{
    REQUIRE( rotates( "u64 f( u64 x ) { return ( x << 13 ) | ( x >> 51 ); }\n" ) == 1 );
    REQUIRE( rotates( "u32 f( u32 x ) { return ( x >> 25 ) | ( x << 7 ); }\n" ) == 1 );
    REQUIRE( rotates( "u8 f( u8 x ) { return ( x << 3 ) | ( x >> 5 ); }\n" ) == 1 );
}

TEST_CASE( "recognise_idioms_takes_a_helper_once_inlined", "[opt][recognise_idioms]" )
{
    REQUIRE(
        rotates( "u64 f( u64 x ) { return spin( x, 21 ); }\n"
                 "u64 spin( u64 x, u64 by ) { return ( x << by ) | ( x >> ( 64 - by ) ); }\n" ) == 1
    );
}

TEST_CASE( "recognise_idioms_leaves_what_is_not_a_rotate", "[opt][recognise_idioms]" )
{
    // A signed `>>` copies the sign.
    REQUIRE( rotates( "i64 f( i64 x ) { return ( x << 13 ) | ( x >> 51 ); }\n" ) == 0 );
    // The amounts do not make the width.
    REQUIRE( rotates( "u64 f( u64 x ) { return ( x << 13 ) | ( x >> 50 ); }\n" ) == 0 );
    // Two values.
    REQUIRE( rotates( "u64 f( u64 x, u64 y ) { return ( x << 13 ) | ( y >> 51 ); }\n" ) == 0 );
    // `x` changes between the shifts.
    REQUIRE( rotates( "u64 f( u64 x ) { u64 a = x << 13; x += 1; u64 b = x >> 51; return a | b; }\n" ) == 0 );
    // A variable amount.
    REQUIRE( rotates( "u64 f( u64 x, u64 by ) { return ( x << by ) | ( x >> ( 64 - by ) ); }\n" ) == 0 );
}

TEST_CASE( "recognise_idioms_runs_from_O2", "[opt][recognise_idioms]" )
{
    REQUIRE( rotates( "u64 f( u64 x ) { return ( x << 13 ) | ( x >> 51 ); }\n", 1 ) == 0 );
    REQUIRE(
        made( "u64 f( u64 w ) { u64 n = 0; while( w != 0 ) { w &= w - 1; n++; } return n; }\n", Rvalue_kind::Count_ones, 1 ) ==
        0
    );
}

TEST_CASE( "recognise_idioms_counts_ones_in_a_loop", "[opt][recognise_idioms]" )
{
    const std::string_view ones = "u64 f( u64 w ) { u64 n = 0; while( w != 0 ) { w &= w - 1; n++; } return n; }\n";
    REQUIRE( made( ones, Rvalue_kind::Count_ones ) == 1 );
    REQUIRE( blocks( ones ) == 1 );
    // Counted into another type, the step written out, and the count first.
    REQUIRE(
        made(
            "i32 f( u32 w ) { i32 n = 0; while( w > 0 ) { n += 1; w = w & ( w - 1 ); } return n; }\n", Rvalue_kind::Count_ones
        ) == 1
    );
}

TEST_CASE( "recognise_idioms_counts_bits_in_a_shift_loop", "[opt][recognise_idioms]" )
{
    const std::string_view width = "u64 f( u64 x ) { u64 n = 0; while( x != 0 ) { x >>= 1; n++; } return n; }\n";
    REQUIRE( made( width, Rvalue_kind::Leading_zeros ) == 1 );
    REQUIRE( blocks( width ) == 1 );
    REQUIRE(
        made( "u64 f( u64 x ) { u64 n = 64; while( x != 0 ) { x >>= 1; n--; } return n; }\n", Rvalue_kind::Leading_zeros ) == 1
    );
}

TEST_CASE( "recognise_idioms_leaves_what_is_not_a_counting_loop", "[opt][recognise_idioms]" )
{
    // A do-while runs once for 0.
    REQUIRE(
        made( "u64 f( u64 w ) { u64 n = 0; do { w &= w - 1; n++; } while( w != 0 ); return n; }\n", Rvalue_kind::Count_ones ) ==
        0
    );
    // More work each step.
    REQUIRE(
        made(
            "u64 f( u64 w, u64 m ) { u64 n = 0; while( w != 0 ) { w &= w - 1; n++; m++; } return n + m; }\n",
            Rvalue_kind::Count_ones
        ) == 0
    );
    // Counted by 2.
    REQUIRE(
        made( "u64 f( u64 w ) { u64 n = 0; while( w != 0 ) { w &= w - 1; n += 2; } return n; }\n", Rvalue_kind::Count_ones ) ==
        0
    );
    // A signed `>>` never reaches 0 from a negative.
    REQUIRE(
        made( "i64 f( i64 x ) { i64 n = 0; while( x != 0 ) { x >>= 1; n++; } return n; }\n", Rvalue_kind::Leading_zeros ) == 0
    );
    // Shifted by 2.
    REQUIRE(
        made( "u64 f( u64 x ) { u64 n = 0; while( x != 0 ) { x >>= 2; n++; } return n; }\n", Rvalue_kind::Leading_zeros ) == 0
    );
    // Tested against something else.
    REQUIRE(
        made( "u64 f( u64 x ) { u64 n = 0; while( x > 1 ) { x >>= 1; n++; } return n; }\n", Rvalue_kind::Leading_zeros ) == 0
    );
}

} // namespace keel
#endif
