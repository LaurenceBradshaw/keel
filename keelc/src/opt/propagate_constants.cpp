// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/propagate_constants.h"
#include <algorithm>
#include <optional>
#include "ir/live.h"
#include "ir/simplify.h"
#include "opt/fold_constants.h"

namespace keel
{

namespace
{

// Every number any integer type holds.
__extension__ typedef __int128 Wide;

struct Bounds
{
    Wide lo;
    Wide hi;
};

// What a local holds: no write reached yet, one value as its bits at its type's width, numbers within bounds, or
// any its type holds.
struct Value
{
    enum class State : u8
    {
        Undefined,
        Constant,
        Range,
        Varying
    };
    State state = State::Undefined;
    u64   bits  = 0;
    Wide  lo    = 0; // Range only, as numbers
    Wide  hi    = 0;

    bool operator==( const Value& ) const = default;
};

using Values = std::vector<Value>;

std::optional<Bounds> limits( const Type_table& types, Type_id type )
{
    const Type& t = types.get( type );

    if( t.kind == Type_kind::Bool )
    {
        return Bounds { 0, 1 };
    }

    if( t.kind != Type_kind::Int )
    {
        return std::nullopt;
    }

    if( t.is_signed )
    {
        const Wide half = Wide( 1 ) << ( t.width - 1 );
        return Bounds { -half, half - 1 };
    }

    return Bounds { 0, ( Wide( 1 ) << t.width ) - 1 };
}

bool fits( Bounds b, const Type_table& types, Type_id type )
{
    const std::optional<Bounds> all = limits( types, type );
    return all && b.lo >= all->lo && b.hi <= all->hi;
}

// The numbers `v` may be as a `type`; nullopt before a write, or for a type that is not an integer.
std::optional<Bounds> bounds( const Value& v, Type_id type, const Type_table& types )
{
    const std::optional<Bounds> all = limits( types, type );

    if( !all || v.state == Value::State::Undefined )
    {
        return std::nullopt;
    }

    if( v.state == Value::State::Constant )
    {
        const Type& t = types.get( type );
        Wide        n = v.bits;
        if( t.is_signed && t.width < 64 && ( v.bits >> ( t.width - 1 ) ) & 1 )
        {
            n -= Wide( 1 ) << t.width;
        }
        else if( t.is_signed && t.width == 64 )
        {
            n = static_cast<i64>( v.bits );
        }
        return Bounds { n, n };
    }

    if( v.state == Value::State::Range )
    {
        return Bounds { v.lo, v.hi };
    }

    return all;
}

// `b` as a `type`'s value: Varying when it spans the type or leaves it.
Value of_bounds( Bounds b, Type_id type, const Type_table& types )
{
    const std::optional<Bounds> all = limits( types, type );

    if( !all || b.lo < all->lo || b.hi > all->hi || ( b.lo == all->lo && b.hi == all->hi ) )
    {
        return Value { .state = Value::State::Varying };
    }

    if( b.lo == b.hi )
    {
        const Type& t    = types.get( type );
        u64         bits = static_cast<u64>( b.lo );
        if( t.kind == Type_kind::Int && t.width < 64 )
        {
            bits &= ( 1ull << t.width ) - 1;
        }
        return Value { .state = Value::State::Constant, .bits = bits };
    }

    return Value { .state = Value::State::Range, .lo = b.lo, .hi = b.hi };
}

Value join( Value a, Value b, Type_id type, const Type_table& types )
{
    if( a.state == Value::State::Undefined || a == b )
    {
        return b;
    }

    if( b.state == Value::State::Undefined )
    {
        return a;
    }

    const std::optional<Bounds> x = bounds( a, type, types );
    const std::optional<Bounds> y = bounds( b, type, types );

    if( !x || !y )
    {
        return Value { .state = Value::State::Varying };
    }

    return of_bounds( { std::min( x->lo, y->lo ), std::max( x->hi, y->hi ) }, type, types );
}

// Whether `x op y` holds for every pair the bounds allow, or fails for every pair; nullopt if it varies.
std::optional<bool> compared( Token_kind op, Bounds x, Bounds y )
{
    switch( op )
    {
    case Token_kind::Less:
        return x.hi < y.lo ? std::optional( true ) : x.lo >= y.hi ? std::optional( false ) : std::nullopt;

    case Token_kind::Less_equal:
        return x.hi <= y.lo ? std::optional( true ) : x.lo > y.hi ? std::optional( false ) : std::nullopt;

    case Token_kind::Greater:
        return compared( Token_kind::Less, y, x );

    case Token_kind::Greater_equal:
        return compared( Token_kind::Less_equal, y, x );

    case Token_kind::Equal_equal:
        return x.hi < y.lo || y.hi < x.lo ? std::optional( false ) : std::nullopt;

    case Token_kind::Bang_equal:
        return x.hi < y.lo || y.hi < x.lo ? std::optional( true ) : std::nullopt;

    default:
        return std::nullopt;
    }
}

bool is_integer( const Type_table& types, Type_id type )
{
    const Type_kind kind = types.get( type ).kind;

    if( kind == Type_kind::Int || kind == Type_kind::Bool )
    {
        return true;
    }

    return false;
}

// Integers and `bool` no pointer reaches; every other read is Varying.
Local_set tracked_locals( const Function& func, const Type_table& types )
{
    const Local_set addressed = addressed_locals( func );
    Local_set       tracked( func.locals.size(), false );

    for( const Local_id l : func.locals.ids() )
    {
        if( !addressed[l.v] && is_integer( types, func.locals[l].type ) )
        {
            tracked[l.v] = true;
        }
    }

    return tracked;
}

struct Analysis
{
    const Function&     func;
    const Type_table&   types;
    const Literal_pool& literals;
    Local_set           tracked;
};

bool whole_tracked( const Analysis& analysis, const Place& place )
{
    return place.is_whole_local() && analysis.tracked[place.local.v];
}

Value read( const Analysis& analysis, const Operand& operand, const Values& values )
{
    // An enum constant is its tag; an enum local is not tracked, since it may carry payloads.
    if( operand.kind == Operand_kind::Constant && operand.constant.is_valid() &&
        ( is_integer( analysis.types, operand.type ) || analysis.types.get( operand.type ).kind == Type_kind::Enum ) )
    {
        return Value { .state = Value::State::Constant, .bits = analysis.literals.integer( operand.constant ) };
    }

    if( operand.kind == Operand_kind::Copy && whole_tracked( analysis, operand.place ) )
    {
        return values[operand.place.local.v];
    }

    // A `move` keeps its read, as in propagate_values.
    return Value { .state = Value::State::Varying };
}

// The numbers `value` gives from its operands' bounds: a cast that keeps them, `&` with a non-negative side,
// `>>` or `%` by a constant, and a comparison the bounds decide.
Value ranged( const Analysis& analysis, const Rvalue& value, Value a, Value b )
{
    const Type_table&           types = analysis.types;
    const Value                 any { .state = Value::State::Varying };
    const std::optional<Bounds> x = bounds( a, value.a.type, types );

    if( !x )
    {
        return any;
    }

    if( value.kind == Rvalue_kind::Cast )
    {
        return types.get( value.type ).kind == Type_kind::Int && fits( *x, types, value.type )
                   ? of_bounds( *x, value.type, types )
                   : any;
    }

    if( value.kind != Rvalue_kind::Binary )
    {
        return any;
    }

    const std::optional<Bounds> y = bounds( b, value.b.type, types );
    if( !y )
    {
        return any;
    }

    if( is_comparison( value.op ) )
    {
        const std::optional<bool> decided = value.a.type == value.b.type ? compared( value.op, *x, *y ) : std::nullopt;
        return decided ? Value { .state = Value::State::Constant, .bits = *decided ? 1u : 0u } : any;
    }

    const bool by_constant = y->lo == y->hi;
    switch( value.op )
    {
    case Token_kind::Amp:
        if( x->lo >= 0 || y->lo >= 0 )
        {
            const Wide hi = x->lo >= 0 && y->lo >= 0 ? std::min( x->hi, y->hi ) : x->lo >= 0 ? x->hi : y->hi;
            return of_bounds( { 0, hi }, value.type, types );
        }
        return any;

    // Masked as the run masks a count, so `>>` is floor division and keeps the order.
    case Token_kind::Greater_greater:
        if( by_constant )
        {
            const int k = static_cast<int>( y->lo & ( types.get( value.a.type ).width - 1 ) );
            return of_bounds( { x->lo >> k, x->hi >> k }, value.type, types );
        }
        return any;

    // Truncated, so the remainder takes the dividend's sign.
    case Token_kind::Percent:
        if( by_constant && y->lo > 0 )
        {
            const Wide c = y->lo;
            return of_bounds(
                { x->lo >= 0 ? 0 : std::max( x->lo, 1 - c ), x->hi <= 0 ? 0 : std::min( x->hi, c - 1 ) }, value.type, types
            );
        }
        return any;

    default:
        return any;
    }
}

Value evaluated( const Analysis& analysis, const Rvalue& value, const Values& values )
{
    if( value.kind != Rvalue_kind::Use && value.kind != Rvalue_kind::Binary && value.kind != Rvalue_kind::Overflows &&
        value.kind != Rvalue_kind::Unary && value.kind != Rvalue_kind::Cast )
    {
        return Value { .state = Value::State::Varying };
    }

    // An operand a kind does not use is the default Constant, with no id, so it reads Varying.
    const Value a = read( analysis, value.a, values );
    const Value b = read( analysis, value.b, values );

    if( value.kind == Rvalue_kind::Use )
    {
        return a;
    }

    // Not known yet: a later sweep brings the write.
    if( a.state == Value::State::Undefined || b.state == Value::State::Undefined )
    {
        return Value { .state = Value::State::Undefined };
    }

    const auto known = [&]( Value v ) -> std::optional<u64>
    {
        if( v.state == Value::State::Constant )
        {
            return v.bits;
        }

        return std::nullopt;
    };

    const std::optional<u64> result = evaluate( value, known( a ), known( b ), analysis.types );

    if( !result )
    {
        return ranged( analysis, value, a, b );
    }

    return Value { .state = Value::State::Constant, .bits = *result };
}

void step( const Analysis& analysis, const Statement& statement, Values& values )
{
    if( statement.kind != Statement_kind::Assign )
    {
        return;
    }

    const Rvalue& rvalue = analysis.func.rvalue( statement );

    // `addressed_locals` leaves out `&init`, but the call it feeds writes the local.
    if( rvalue.kind == Rvalue_kind::Address_of && rvalue.address_purpose == Address_purpose::Initialise &&
        !rvalue.a.place.is_global() && analysis.tracked[rvalue.a.place.local.v] )
    {
        values[rvalue.a.place.local.v] = Value { .state = Value::State::Varying };
    }

    if( !whole_tracked( analysis, statement.place ) )
    {
        return;
    }

    Value value = evaluated( analysis, rvalue, values );

    // An assignment converts: `i64 w = n` reads an `i32`, so `w` holds only what an `i32` does.
    const Type_id to = analysis.func.locals[statement.place.local].type;
    if( value.state == Value::State::Constant )
    {
        const std::optional<u64> converted = convert( value.bits, rvalue.type, to, analysis.types );
        value                              = converted ? Value { .state = Value::State::Constant, .bits = *converted }
                                                       : Value { .state = Value::State::Varying };
    }
    else if( value.state != Value::State::Undefined && rvalue.type != to )
    {
        const std::optional<Bounds> b = bounds( value, rvalue.type, analysis.types );
        value                         = b && fits( *b, analysis.types, to ) ? of_bounds( *b, to, analysis.types )
                                                                            : Value { .state = Value::State::Varying };
    }

    values[statement.place.local.v] = value;
}

bool writes( const Analysis& analysis, const Statement& statement, Local_id local )
{
    if( statement.kind != Statement_kind::Assign )
    {
        return false;
    }

    const Rvalue& rvalue = analysis.func.rvalue( statement );
    const auto    is     = [&]( const Place& place ) { return !place.is_global() && place.local == local; };

    return is( statement.place ) || ( rvalue.kind == Rvalue_kind::Address_of &&
                                      rvalue.address_purpose == Address_purpose::Initialise && is( rvalue.a.place ) );
}

struct Narrowing
{
    Local_id local;
    Bounds   bounds;
};

// What `block`'s branch tells its `taken` edge of `x`, when the branch reads `x op const k` (either way round) written
// in the block and `x` is unchanged after; nullopt otherwise.
std::optional<Narrowing> narrowing( const Analysis& analysis, const Block& block, bool taken )
{
    const Operand& condition = block.terminator.condition;
    if( condition.kind != Operand_kind::Copy || !whole_tracked( analysis, condition.place ) )
    {
        return std::nullopt;
    }

    const auto statements = analysis.func.statements_of( block );
    u32        s          = static_cast<u32>( statements.size() );
    while( s > 0 && !writes( analysis, statements[s - 1], condition.place.local ) )
    {
        --s;
    }

    if( s == 0 || !statements[s - 1].place.is_whole_local() || statements[s - 1].place.local != condition.place.local )
    {
        return std::nullopt;
    }

    const Rvalue& test = analysis.func.rvalue( statements[s - 1] );
    if( test.kind != Rvalue_kind::Binary || !is_comparison( test.op ) || test.op == Token_kind::Less_equal_greater )
    {
        return std::nullopt;
    }

    const bool     flipped = test.a.kind == Operand_kind::Constant;
    const Operand& x       = flipped ? test.b : test.a;
    const Operand& k       = flipped ? test.a : test.b;
    if( x.kind != Operand_kind::Copy || !whole_tracked( analysis, x.place ) || x.place.local == condition.place.local ||
        k.kind != Operand_kind::Constant || !k.constant.is_valid() || x.type != k.type )
    {
        return std::nullopt;
    }

    for( u32 after = s; after < statements.size(); ++after )
    {
        if( writes( analysis, statements[after], x.place.local ) )
        {
            return std::nullopt;
        }
    }

    // As `x op k`, then negated for the edge a false test takes.
    Token_kind op = test.op;
    if( flipped )
    {
        op = op == Token_kind::Less            ? Token_kind::Greater
             : op == Token_kind::Greater       ? Token_kind::Less
             : op == Token_kind::Less_equal    ? Token_kind::Greater_equal
             : op == Token_kind::Greater_equal ? Token_kind::Less_equal
                                               : op;
    }
    if( !taken )
    {
        op = op == Token_kind::Less            ? Token_kind::Greater_equal
             : op == Token_kind::Greater_equal ? Token_kind::Less
             : op == Token_kind::Greater       ? Token_kind::Less_equal
             : op == Token_kind::Less_equal    ? Token_kind::Greater
             : op == Token_kind::Equal_equal   ? Token_kind::Bang_equal
                                               : Token_kind::Equal_equal;
    }

    const Bounds all = *limits( analysis.types, x.type );
    const Wide   n =
        bounds(
            Value { .state = Value::State::Constant, .bits = analysis.literals.integer( k.constant ) }, x.type, analysis.types
        )
            ->lo;

    switch( op )
    {
    case Token_kind::Less:
        return Narrowing { x.place.local, { all.lo, n - 1 } };
    case Token_kind::Less_equal:
        return Narrowing { x.place.local, { all.lo, n } };
    case Token_kind::Greater:
        return Narrowing { x.place.local, { n + 1, all.hi } };
    case Token_kind::Greater_equal:
        return Narrowing { x.place.local, { n, all.hi } };
    case Token_kind::Equal_equal:
        return Narrowing { x.place.local, { n, n } };
    default:
        return std::nullopt;
    }
}

std::vector<std::optional<Values>> values_on_entry( const Analysis& analysis )
{
    const Function& func = analysis.func;

    std::vector<std::optional<Values>> values( func.blocks.size(), std::nullopt );
    values[0].emplace( func.locals.size(), Value {} );
    for( u32 l = 1; l <= func.parameter_count; ++l )
    {
        ( *values[0] )[l] = Value { .state = Value::State::Varying };
    }

    // Values only rise, a range that grows again is widened to Varying, and a block is reached once, so this ends.
    bool                                                       changed = true;
    std::vector<std::pair<Block_id, std::optional<Narrowing>>> targets;
    while( changed )
    {
        changed = false;

        for( const Block_id b : func.blocks.ids() )
        {
            if( !values[b.v] )
            {
                continue;
            }

            const Block& block = func.blocks[b];
            Values       state = *values[b.v];

            for( const Statement& statement : func.statements_of( block ) )
            {
                step( analysis, statement, state );
            }

            // A branch on a constant takes one edge, and on a value not yet known, none.
            targets.clear();
            const Terminator& terminator = block.terminator;
            if( terminator.kind == Terminator_kind::Goto )
            {
                targets.emplace_back( terminator.targets[0], std::nullopt );
            }
            else if( terminator.kind == Terminator_kind::Branch )
            {
                const Value condition = read( analysis, terminator.condition, state );
                if( condition.state == Value::State::Constant )
                {
                    targets.emplace_back( terminator.targets[condition.bits != 0 ? 0 : 1], std::nullopt );
                }
                else if( condition.state == Value::State::Varying )
                {
                    targets.emplace_back( terminator.targets[0], narrowing( analysis, block, true ) );
                    targets.emplace_back( terminator.targets[1], narrowing( analysis, block, false ) );
                }
            }

            for( const auto& [target, narrowed] : targets )
            {
                // The tested local, within what the edge allows; an edge it cannot take is left out.
                std::optional<Value> tested;
                if( narrowed )
                {
                    const Type_id               type    = func.locals[narrowed->local].type;
                    const std::optional<Bounds> current = bounds( state[narrowed->local.v], type, analysis.types );
                    if( current )
                    {
                        const Bounds within {
                            std::max( current->lo, narrowed->bounds.lo ), std::min( current->hi, narrowed->bounds.hi )
                        };
                        if( within.lo > within.hi )
                        {
                            continue;
                        }
                        tested = of_bounds( within, type, analysis.types );
                    }
                }

                const auto arriving = [&]( Local_id l ) { return tested && l == narrowed->local ? *tested : state[l.v]; };

                std::optional<Values>& entry = values[target.v];

                if( !entry )
                {
                    entry = state;
                    if( tested )
                    {
                        ( *entry )[narrowed->local.v] = *tested;
                    }
                    changed = true;
                    continue;
                }

                for( const Local_id l : func.locals.ids() )
                {
                    const Value old    = ( *entry )[l.v];
                    Value       joined = join( old, arriving( l ), func.locals[l].type, analysis.types );
                    if( joined != old )
                    {
                        if( old.state == Value::State::Range )
                        {
                            joined = Value { .state = Value::State::Varying };
                        }
                        ( *entry )[l.v] = joined;
                        changed         = true;
                    }
                }
            }
        }
    }

    return values;
}

void rewrite( const Analysis& analysis, Operand& operand, const Values& values, Literal_pool& literals )
{
    if( operand.kind != Operand_kind::Copy || !whole_tracked( analysis, operand.place ) )
    {
        return;
    }

    const Value v = values[operand.place.local.v];

    if( v.state == Value::State::Constant )
    {
        operand = value_of( v.bits, operand.type, analysis.types, literals ).a;
    }
}

// Already folded; `add_integer` appends, so folding it again only grows the pool.
bool is_constant( const Rvalue& value )
{
    return value.kind == Rvalue_kind::Use && value.a.kind == Operand_kind::Constant;
}
// Within a block, which local each holds the same number as, through copies and casts that keep it: so a checked
// cast's round trip compares a number with itself.
class Links
{
public:
    explicit Links( std::size_t count )
        : same_( count ),
          writes_( count, 0 )
    {
    }

    void clear()
    {
        std::ranges::fill( same_, Link {} );
    }

    // Before `statement` steps, so `values` is what it reads.
    void note( const Analysis& analysis, const Statement& statement, const Values& values )
    {
        const Rvalue& rvalue = analysis.func.rvalue( statement );

        if( rvalue.kind == Rvalue_kind::Address_of && rvalue.address_purpose == Address_purpose::Initialise &&
            !rvalue.a.place.is_global() && analysis.tracked[rvalue.a.place.local.v] )
        {
            wrote( rvalue.a.place.local );
        }

        if( !whole_tracked( analysis, statement.place ) )
        {
            return;
        }

        const Local_id l = statement.place.local;
        Local_id       r {};
        if( ( rvalue.kind == Rvalue_kind::Use || rvalue.kind == Rvalue_kind::Cast ) && rvalue.a.kind == Operand_kind::Copy &&
            whole_tracked( analysis, rvalue.a.place ) )
        {
            const Local_id              from = rvalue.a.place.local;
            const std::optional<Bounds> b    = bounds( values[from.v], rvalue.a.type, analysis.types );
            const Type_id               to   = analysis.func.locals[l].type;
            if( b && fits( *b, analysis.types, rvalue.type ) && fits( *b, analysis.types, to ) &&
                ( rvalue.kind == Rvalue_kind::Use || analysis.types.get( rvalue.type ).kind == Type_kind::Int ) )
            {
                r = root( from ) == l ? from : root( from );
            }
        }

        wrote( l );
        if( r.is_valid() )
        {
            same_[l.v] = Link { r, writes_[r.v] };
        }
    }

    // A comparison of two reads of one number.
    std::optional<bool> decide( const Analysis& analysis, const Rvalue& rvalue ) const
    {
        const auto read = [&]( const Operand& o )
        { return o.kind == Operand_kind::Copy && whole_tracked( analysis, o.place ); };

        if( rvalue.kind != Rvalue_kind::Binary || !is_comparison( rvalue.op ) || rvalue.op == Token_kind::Less_equal_greater ||
            !read( rvalue.a ) || !read( rvalue.b ) || rvalue.a.type != rvalue.b.type ||
            root( rvalue.a.place.local ) != root( rvalue.b.place.local ) )
        {
            return std::nullopt;
        }

        return rvalue.op == Token_kind::Equal_equal || rvalue.op == Token_kind::Less_equal ||
               rvalue.op == Token_kind::Greater_equal;
    }

private:
    struct Link
    {
        Local_id root {};
        u32      writes = 0;
    };

    Local_id root( Local_id l ) const
    {
        const Link& link = same_[l.v];
        return link.root.is_valid() && writes_[link.root.v] == link.writes ? link.root : l;
    }

    void wrote( Local_id l )
    {
        ++writes_[l.v];
        same_[l.v] = Link {};
    }

    std::vector<Link> same_;
    std::vector<u32>  writes_;
};

} // namespace

void propagate_constants( Function& func, const Type_table& types, Literal_pool& literals )
{
    const Analysis analysis { func, types, literals, tracked_locals( func, types ) };
    const auto     entry = values_on_entry( analysis );

    Links links( func.locals.size() );

    for( const Block_id b : func.blocks.ids() )
    {
        if( !entry[b.v] )
        {
            continue;
        }

        Values values = *entry[b.v];
        Block& block  = func.blocks[b];
        links.clear();

        for( u32 s = 0; s < block.statement_count; ++s )
        {
            Statement& statement = func.statements_of( block )[s];
            if( statement.kind != Statement_kind::Assign )
            {
                continue;
            }

            if( const std::optional<bool> decided = links.decide( analysis, func.rvalue( statement ) ) )
            {
                func.rvalue( statement ) = value_of( *decided ? 1 : 0, func.rvalue( statement ).type, types, literals );
            }

            // Before the step, so `_1 = copy _1 + const 1` reads the old `_1`.
            const Value result = evaluated( analysis, func.rvalue( statement ), values );
            // Wherever it is written: a global's or a field's sum folds as a local's does.
            if( result.state == Value::State::Constant && !is_constant( func.rvalue( statement ) ) )
            {
                func.rvalue( statement ) = value_of( result.bits, func.rvalue( statement ).type, types, literals );
            }
            else
            {
                rewrite( analysis, func.rvalue( statement ).a, values, literals );
                rewrite( analysis, func.rvalue( statement ).b, values, literals );

                for( Operand& argument : func.rvalue( statement ).arguments )
                {
                    rewrite( analysis, argument, values, literals );
                }
            }

            links.note( analysis, statement, values );
            step( analysis, statement, values );
        }

        rewrite( analysis, block.terminator.condition, values, literals );
    }

    // A branch that now reads a constant folds, and the arm it never takes goes.
    simplify( func, literals );
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
struct Propagated : Compiled
{
    explicit Propagated( std::string_view source )
        : Compiled( std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, Build { .optimise = true } )
    {
        INFO( rendered() );
        REQUIRE( clean() );
        REQUIRE( verify( first() ).empty() );
    }

    const Function& first() const
    {
        return functions.front();
    }

    // How many operands read the local called `name` by copy, statements and terminators both.
    std::size_t reads( std::string_view name ) const
    {
        const Symbol_id symbol = interner.find( name );
        std::size_t     count  = 0;
        const auto      note   = [&]( const Operand& operand )
        {
            if( operand.kind == Operand_kind::Copy && !operand.place.is_global() && operand.place.local.is_valid() &&
                first().locals[operand.place.local].name == symbol )
            {
                ++count;
            }
        };

        for( const Statement& statement : first().statements )
        {
            if( statement.kind == Statement_kind::Assign )
            {
                for_each_operand( first().rvalue( statement ), note );
            }
        }

        for( const Block& block : first().blocks )
        {
            note( block.terminator.condition );
        }

        return count;
    }

    std::size_t branches() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().blocks, []( const Block& block ) { return block.terminator.kind == Terminator_kind::Branch; }
        ) );
    }

    // Whether some statement assigns the constant of these bits.
    bool assigns( u64 bits ) const
    {
        return std::ranges::any_of(
            first().statements,
            [&]( const Statement& s )
            {
                const Rvalue& value = first().rvalue( s );

                return s.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Use &&
                       value.a.kind == Operand_kind::Constant && literals.integer( value.a.constant ) == bits;
            }
        );
    }
};

} // namespace

// The `||`'s other arm never runs, so its flag is `true` at the join and `f` returns `1` outright.
TEST_CASE( "propagate_constants_takes_only_the_arm_that_runs", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 n ) { if( big( 20 ) || n > 0 ) { return 1; } return 0; }\n"
                        "bool big( i32 x ) { return x > 10; }\n" );
    REQUIRE( p.branches() == 0 );
    REQUIRE( p.first().blocks.size() == 1 );
}

// `k` is `1` round the loop: the only arm that changes it tests `k != 1`, which never holds.
TEST_CASE( "propagate_constants_keeps_a_constant_a_dead_arm_would_change", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 n ) { i32 k = 1; i32 t = 0; for( i32 i = 0; i < n; i++ ) { if( k != 1 ) { k = 2; } "
                        "t = t + k; } return t; }\n" );
    REQUIRE( p.branches() == 1 );
    REQUIRE( p.reads( "k" ) == 0 );
}

// `i` is `0` from the entry, but `1` round the back edge, so the loop keeps reading it.
TEST_CASE( "propagate_constants_keeps_a_loop_counter", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 n ) { i32 t = 0; for( i32 i = 0; i < n; i++ ) { t = t + i; } return t; }\n" );
    REQUIRE( p.branches() == 1 );
    REQUIRE( p.reads( "i" ) == 3 );
}

// `clamp`'s argument is `-4`, so its comparison folds and only `t - 4` is left.
TEST_CASE( "propagate_constants_reads_a_negative_through_a_branch", "[opt][constants]" )
{
    const Propagated p( "i64 f( i64 t ) { i64 a = 3; i64 b = a + 4; return t - clamp( a - b ); }\n"
                        "i64 clamp( i64 x ) { return x < 0 ? 0 - x : x; }\n" );
    REQUIRE( p.branches() == 0 );
    REQUIRE( p.first().blocks.size() == 1 );
}

// `w = n` widens: `w` is `-4` as an `i64`, so `w + 1` is `-3`, not `4294967293`.
TEST_CASE( "propagate_constants_sign_extends_an_assignment", "[opt][constants]" )
{
    const Propagated p( "i64 f() { i32 a = 3; i32 n = a - 7; i64 w = n; return w + 1; }\n" );
    REQUIRE( p.assigns( static_cast<u64>( -3 ) ) );
}

// `n` is `-4`, and the recursive call, never inlined, reads the constant.
TEST_CASE( "propagate_constants_forwards_a_negative", "[opt][constants]" )
{
    const Propagated p( "i64 f( i64 t ) { i64 a = 3; i64 n = a - 7; if( t > 0 ) { return f( n ); } return t; }\n" );
    REQUIRE( p.reads( "n" ) == 0 );
}

// `step` is `2` both from the entry and round the back edge.
TEST_CASE( "propagate_constants_reads_a_constant_round_a_loop", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 n ) { i32 step = 2; i32 t = 0; for( i32 i = 0; i < n; i++ ) { t = t + step; } return t; }\n"
    );
    REQUIRE( p.reads( "step" ) == 0 );
}

// `k` is `1` on one path and `2` on the other, and both run.
TEST_CASE( "propagate_constants_keeps_a_local_the_paths_disagree_on", "[opt][constants]" )
{
    const Propagated p( "i32 f( bool c ) { i32 k = 1; if( c ) { k = 2; } return k + 1; }\n" );
    REQUIRE( p.reads( "k" ) == 1 );
}

// One pass: `c - 1` is `13` without a round per fold.
TEST_CASE( "propagate_constants_folds_a_chain", "[opt][constants]" )
{
    const Propagated p( "i32 f() { i32 a = 3; i32 b = a + 4; i32 c = b * 2; return c - 1; }\n" );
    REQUIRE( p.assigns( 13 ) );

    for( const Statement& s : p.first().statements )
    {
        REQUIRE( ( s.kind != Statement_kind::Assign || p.first().rvalue( s ).kind != Rvalue_kind::Binary ) );
    }
}

// A mask, a shift and a remainder each leave a number a `u8` holds, so the narrowing check goes.
TEST_CASE( "propagate_constants_drops_a_check_the_operand_range_passes", "[opt][constants]" )
{
    const char* const sources[] = {
        "u8 f( u32 x ) { return cast<u8>( x & 255 ); }\n",
        "u8 f( u32 x ) { return cast<u8>( x >> 24 ); }\n",
        "u8 f( u32 x ) { return cast<u8>( x % 200 ); }\n",
        "u8 f( u8 x ) { return cast<u8>( cast<i32>( x ) ); }\n",
    };

    for( const char* source : sources )
    {
        const Propagated p( source );
        REQUIRE( p.branches() == 0 );
    }
}

// `x & 511` may be 256, so the check stays.
TEST_CASE( "propagate_constants_keeps_a_check_the_range_may_fail", "[opt][constants]" )
{
    const Propagated p( "u8 f( u32 x ) { return cast<u8>( x & 511 ); }\n" );
    REQUIRE( p.branches() == 1 );
}

// The two tests leave `x` within `0..99`, so the cast needs none of its own.
TEST_CASE( "propagate_constants_narrows_a_local_on_a_branch", "[opt][constants]" )
{
    const Propagated p( "u8 f( i32 x ) { if( x >= 0 && x < 100 ) { return cast<u8>( x ); } return 0; }\n" );
    REQUIRE( p.branches() == 2 );
}

// Only `x > 10` reaches the inner test, so `x > 5` holds there.
TEST_CASE( "propagate_constants_folds_a_test_a_dominating_one_decides", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 x ) { if( x > 10 ) { if( x > 5 ) { return 1; } return 2; } return 3; }\n" );
    REQUIRE( p.branches() == 1 );
}

// Past `x == 3`, `x` is the constant.
TEST_CASE( "propagate_constants_reads_the_constant_an_equality_tested", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 x ) { if( x == 3 ) { return x + 1; } return 0; }\n" );
    REQUIRE( p.reads( "x" ) == 1 );
}

// `i` grows round the loop until its range is widened away, so the check stays and the pass ends.
TEST_CASE( "propagate_constants_widens_a_growing_range", "[opt][constants]" )
{
    const Propagated p( "u8 f() { u8 t = 0; for( i32 i = 0; i < 300; i++ ) { t = cast<u8>( i ); } return t; }\n" );
    REQUIRE( p.branches() == 2 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
