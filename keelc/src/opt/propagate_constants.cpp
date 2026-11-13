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

// alignas: clang-cl lays the MSVC STL out under pack(8), so std::optional of a Wide-holding struct
// is 8-aligned while reads of a Wide assume 16 and may use movaps. Only a struct's own alignas
// survives the pack.
struct alignas( 16 ) Bounds
{
    Wide lo;
    Wide hi;
};

// What a local holds: no write reached yet, one value as its bits at its type's width, numbers within bounds, or
// any its type holds. alignas as Bounds'.
struct alignas( 16 ) Value
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

// An enum as its tag.
const Type& scalar( const Type_table& types, Type_id type )
{
    const Type& t = types.get( type );
    return t.kind == Type_kind::Enum ? types.get( t.element ) : t;
}

std::optional<Bounds> limits( const Type_table& types, Type_id type )
{
    const Type& t = scalar( types, type );

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
        const Type& t = scalar( types, type );
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
        const Type& t    = scalar( types, type );
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

// Integers, `bool` and enums no pointer reaches; every other read is Varying. An enum is its tag
// only when nothing projects from it: a payload enum is always named through `.tag`.
Local_set tracked_locals( const Function& func, const Type_table& types )
{
    const Local_set addressed = addressed_locals( func );
    Local_set       projected( func.locals.size(), false );
    Local_set       tracked( func.locals.size(), false );

    for_each_place(
        func,
        [&]( const Place& place, const Statement* )
        {
            if( !place.is_global() && place.local.is_valid() && place.num_projections > 0 )
            {
                projected[place.local.v] = true;
            }
        }
    );

    for( const Local_id l : func.locals.ids() )
    {
        const Type_id type = func.locals[l].type;
        if( !addressed[l.v] &&
            ( is_integer( types, type ) || ( types.get( type ).kind == Type_kind::Enum && !projected[l.v] ) ) )
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
    bool                relating;
};

// Which of `a < b`, `a == b` and `a > b` may hold.
using Order                 = u8;
constexpr Order k_less      = 1;
constexpr Order k_equal     = 2;
constexpr Order k_greater   = 4;
constexpr Order k_any_order = 7;

Order order_of( Token_kind op )
{
    switch( op )
    {
    case Token_kind::Less:
        return k_less;
    case Token_kind::Less_equal:
        return k_less | k_equal;
    case Token_kind::Greater:
        return k_greater;
    case Token_kind::Greater_equal:
        return k_greater | k_equal;
    case Token_kind::Equal_equal:
        return k_equal;
    case Token_kind::Bang_equal:
        return k_less | k_greater;
    default:
        return k_any_order;
    }
}

Order swapped( Order o )
{
    return static_cast<Order>( ( o & k_equal ) | ( o & k_less ? k_greater : 0 ) | ( o & k_greater ? k_less : 0 ) );
}

Order order_of( Bounds x, Bounds y )
{
    return static_cast<Order>(
        ( x.lo < y.hi ? k_less : 0 ) | ( x.lo <= y.hi && y.lo <= x.hi ? k_equal : 0 ) | ( x.hi > y.lo ? k_greater : 0 )
    );
}

// Two locals of one type and how they may compare, `a` the lower.
struct Relation
{
    Local_id a;
    Local_id b;
    Order    may = k_any_order;

    bool operator==( const Relation& ) const = default;
};

using Relations = std::vector<Relation>; // ascending by `a`, then `b`

// What holds on entry to a block, or after a statement.
struct State
{
    Values    values;
    Relations relations;
};

bool whole_tracked( const Analysis& analysis, const Place& place )
{
    return place.is_whole_local() && analysis.tracked[place.local.v];
}

Value read( const Analysis& analysis, const Operand& operand, const Values& values )
{
    // An enum constant is its tag.
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

// The numbers `value` gives from its operands' bounds: a cast that keeps them, `+`, `-` and `*`, `&` with a
// non-negative side, `>>` or `%` by a constant, and a comparison the bounds decide.
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

    // Bounds past 2^63 could overflow a product; a result the type cannot hold is Varying.
    const Wide big     = Wide( 1 ) << 63;
    const bool smaller = x->lo > -big && x->hi < big && y->lo > -big && y->hi < big;

    const bool by_constant = y->lo == y->hi;
    switch( value.op )
    {
    case Token_kind::Plus:
        return smaller ? of_bounds( { x->lo + y->lo, x->hi + y->hi }, value.type, types ) : any;

    case Token_kind::Minus:
        return smaller ? of_bounds( { x->lo - y->hi, x->hi - y->lo }, value.type, types ) : any;

    case Token_kind::Star:
        if( smaller )
        {
            const Wide p[] = { x->lo * y->lo, x->lo * y->hi, x->hi * y->lo, x->hi * y->hi };
            return of_bounds( { *std::ranges::min_element( p ), *std::ranges::max_element( p ) }, value.type, types );
        }
        return any;

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

Relations::const_iterator find( const Relations& relations, Local_id a, Local_id b )
{
    return std::ranges::find_if( relations, [&]( const Relation& r ) { return r.a == a && r.b == b; } );
}

// What the bounds allow of `a` against `b`.
Order bounded( const Analysis& analysis, const Values& values, Local_id a, Local_id b )
{
    const Type_id               type = analysis.func.locals[a].type;
    const std::optional<Bounds> x    = bounds( values[a.v], type, analysis.types );
    const std::optional<Bounds> y    = bounds( values[b.v], type, analysis.types );
    return x && y ? order_of( *x, *y ) : k_any_order;
}

// `x op y` on two locals of one type, as a relation oriented `a < b`; nullopt for any other rvalue.
std::optional<Relation> as_relation( const Analysis& analysis, const Rvalue& test )
{
    const auto local = [&]( const Operand& o ) { return o.kind == Operand_kind::Copy && whole_tracked( analysis, o.place ); };

    if( test.kind != Rvalue_kind::Binary || !is_comparison( test.op ) || test.op == Token_kind::Less_equal_greater ||
        !local( test.a ) || !local( test.b ) || test.a.type != test.b.type )
    {
        return std::nullopt;
    }

    const Local_id x = test.a.place.local;
    const Local_id y = test.b.place.local;
    const Type_id  t = analysis.func.locals[x].type;
    if( x == y || analysis.func.locals[y].type != t || test.a.type != t )
    {
        return std::nullopt;
    }

    const Order o = order_of( test.op );
    return x.v < y.v ? Relation { x, y, o } : Relation { y, x, swapped( o ) };
}

// A comparison of two locals the relations decide.
std::optional<bool> related( const Analysis& analysis, const Rvalue& value, const Relations& relations )
{
    const std::optional<Relation> test = analysis.relating ? as_relation( analysis, value ) : std::nullopt;
    if( !test )
    {
        return std::nullopt;
    }

    const auto known = find( relations, test->a, test->b );
    if( known == relations.end() )
    {
        return std::nullopt;
    }

    return ( known->may & ~test->may ) == 0  ? std::optional( true )
           : ( known->may & test->may ) == 0 ? std::optional( false )
                                             : std::nullopt;
}

// Adds what an edge knows; false when the relations already rule it out.
bool relate( Relations& relations, Relation r )
{
    const auto at = std::ranges::find_if(
        relations, [&]( const Relation& q ) { return q.a.v > r.a.v || ( q.a == r.a && q.b.v >= r.b.v ); }
    );

    if( at != relations.end() && at->a == r.a && at->b == r.b )
    {
        at->may &= r.may;
        return at->may != 0;
    }

    relations.insert( at, r );
    return true;
}

// Each pair either path orders, as either may; a path with no relation for a pair orders it by its bounds.
Relations joined( const Analysis& analysis, const State& x, const State& y )
{
    Relations result;
    auto      i = x.relations.begin();
    auto      j = y.relations.begin();

    while( i != x.relations.end() || j != y.relations.end() )
    {
        const bool from_x =
            j == y.relations.end() || ( i != x.relations.end() && ( i->a.v < j->a.v || ( i->a == j->a && i->b.v <= j->b.v ) ) );
        const bool from_y = j != y.relations.end() && ( i == x.relations.end() || !from_x || ( i->a == j->a && i->b == j->b ) );
        const Relation& r = from_x ? *i : *j;

        const Order a = from_x ? i->may : bounded( analysis, x.values, r.a, r.b );
        const Order b = from_y ? j->may : bounded( analysis, y.values, r.a, r.b );
        if( ( a | b ) != k_any_order )
        {
            result.push_back( Relation { r.a, r.b, static_cast<Order>( a | b ) } );
        }

        i += from_x ? 1 : 0;
        j += from_y ? 1 : 0;
    }

    return result;
}

Value evaluated( const Analysis& analysis, const Rvalue& value, const State& state )
{
    const Values& values = state.values;

    if( const std::optional<bool> decided = related( analysis, value, state.relations ) )
    {
        return Value { .state = Value::State::Constant, .bits = *decided ? 1u : 0u };
    }

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

bool writes( const Analysis& analysis, const Statement& statement, Local_id local );

void step( const Analysis& analysis, const Statement& statement, State& state )
{
    if( statement.kind != Statement_kind::Assign )
    {
        return;
    }

    Values&       values = state.values;
    const Rvalue& rvalue = analysis.func.rvalue( statement );

    std::erase_if(
        state.relations,
        [&]( const Relation& r ) { return writes( analysis, statement, r.a ) || writes( analysis, statement, r.b ); }
    );

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

    Value value = evaluated( analysis, rvalue, state );

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

// Where in `block` the comparison its branch reads is written, one past; nullopt if it is not a comparison.
std::optional<u32> deciding_test( const Analysis& analysis, const Block& block )
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

    return s;
}

bool unchanged_after( const Analysis& analysis, const Block& block, u32 s, Local_id local )
{
    const auto statements = analysis.func.statements_of( block );
    for( u32 after = s; after < statements.size(); ++after )
    {
        if( writes( analysis, statements[after], local ) )
        {
            return false;
        }
    }
    return true;
}

// What `block`'s branch tells its `taken` edge of two locals it compares, unchanged after the test.
std::optional<Relation> relation( const Analysis& analysis, const Block& block, bool taken )
{
    const std::optional<u32> s = analysis.relating ? deciding_test( analysis, block ) : std::nullopt;
    if( !s )
    {
        return std::nullopt;
    }

    const Local_id          condition = block.terminator.condition.place.local;
    std::optional<Relation> r = as_relation( analysis, analysis.func.rvalue( analysis.func.statements_of( block )[*s - 1] ) );
    if( !r || r->a == condition || r->b == condition || !unchanged_after( analysis, block, *s, r->a ) ||
        !unchanged_after( analysis, block, *s, r->b ) )
    {
        return std::nullopt;
    }

    if( !taken )
    {
        r->may = static_cast<Order>( k_any_order & ~r->may );
    }
    return r;
}

// What `block`'s branch tells its `taken` edge of `x`, when the branch reads `x op const k` (either way round) written
// in the block and `x` is unchanged after; nullopt otherwise.
std::optional<Narrowing> narrowing( const Analysis& analysis, const Block& block, bool taken )
{
    const std::optional<u32> at = deciding_test( analysis, block );
    if( !at )
    {
        return std::nullopt;
    }

    const Operand& condition  = block.terminator.condition;
    const auto     statements = analysis.func.statements_of( block );
    const u32      s          = *at;
    const Rvalue&  test       = analysis.func.rvalue( statements[s - 1] );

    const bool     flipped = test.a.kind == Operand_kind::Constant;
    const Operand& x       = flipped ? test.b : test.a;
    const Operand& k       = flipped ? test.a : test.b;
    if( x.kind != Operand_kind::Copy || !whole_tracked( analysis, x.place ) || x.place.local == condition.place.local ||
        k.kind != Operand_kind::Constant || !k.constant.is_valid() || x.type != k.type )
    {
        return std::nullopt;
    }

    if( !unchanged_after( analysis, block, s, x.place.local ) )
    {
        return std::nullopt;
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

// Each side of every comparison with a constant `k`, as `k - 1`, `k` and `k + 1`, sorted.
std::vector<Wide> stops_of( const Analysis& analysis )
{
    std::vector<Wide> stops;
    for( const Block& block : analysis.func.blocks )
    {
        for( const Statement& statement : analysis.func.statements_of( block ) )
        {
            if( statement.kind != Statement_kind::Assign )
            {
                continue;
            }
            const Rvalue& test = analysis.func.rvalue( statement );
            if( test.kind != Rvalue_kind::Binary || !is_comparison( test.op ) )
            {
                continue;
            }
            for( const Operand* k : { &test.a, &test.b } )
            {
                if( k->kind != Operand_kind::Constant || !k->constant.is_valid() || !is_integer( analysis.types, k->type ) )
                {
                    continue;
                }
                const std::optional<Bounds> n = bounds(
                    Value { .state = Value::State::Constant, .bits = analysis.literals.integer( k->constant ) },
                    k->type,
                    analysis.types
                );
                if( n )
                {
                    stops.insert( stops.end(), { n->lo - 1, n->lo, n->lo + 1 } );
                }
            }
        }
    }
    std::ranges::sort( stops );
    return stops;
}

// `now`, which grew from the range `old`, with each side that moved pushed out to the next stop or the type's end.
Value widened( Value old, Value now, const std::vector<Wide>& stops, Type_id type, const Type_table& types )
{
    const Bounds all = *limits( types, type );
    Bounds       b { now.lo, now.hi };
    if( b.lo < old.lo )
    {
        const auto at = std::ranges::upper_bound( stops, b.lo );
        b.lo          = at == stops.begin() ? all.lo : std::max( all.lo, *( at - 1 ) );
    }
    if( b.hi > old.hi )
    {
        const auto at = std::ranges::lower_bound( stops, b.hi );
        b.hi          = at == stops.end() ? all.hi : std::min( all.hi, *at );
    }
    return of_bounds( b, type, types );
}

std::vector<std::optional<State>> values_on_entry( const Analysis& analysis )
{
    const Function& func = analysis.func;

    std::vector<std::optional<State>> values( func.blocks.size(), std::nullopt );
    values[0].emplace( State { Values( func.locals.size(), Value {} ), {} } );
    for( u32 l = 1; l <= func.parameter_count; ++l )
    {
        values[0]->values[l] = Value { .state = Value::State::Varying };
    }

    struct Edge
    {
        Block_id                 target;
        std::optional<Narrowing> narrowed;
        std::optional<Relation>  related;
    };

    // Values only rise, a range that grows again is widened to the next stop, relations only weaken, and a block is
    // reached once, so this ends.
    const std::vector<Wide> stops   = stops_of( analysis );
    bool                    changed = true;
    std::vector<Edge>       targets;
    std::vector<bool>       dirty( func.blocks.size(), true );
    State                   state;
    State                   arriving;
    while( changed )
    {
        changed = false;

        for( const Block_id b : func.blocks.ids() )
        {
            if( !values[b.v] || !dirty[b.v] )
            {
                continue;
            }
            dirty[b.v] = false;

            const Block& block = func.blocks[b];
            state              = *values[b.v];

            for( const Statement& statement : func.statements_of( block ) )
            {
                step( analysis, statement, state );
            }

            // A branch on a constant takes one edge, and on a value not yet known, none.
            targets.clear();
            const Terminator& terminator = block.terminator;
            if( terminator.kind == Terminator_kind::Goto )
            {
                targets.push_back( Edge { terminator.targets[0], std::nullopt, std::nullopt } );
            }
            else if( terminator.kind == Terminator_kind::Branch )
            {
                const Value condition = read( analysis, terminator.condition, state.values );
                if( condition.state == Value::State::Constant )
                {
                    targets.push_back( Edge { terminator.targets[condition.bits != 0 ? 0 : 1], std::nullopt, std::nullopt } );
                }
                else if( condition.state == Value::State::Varying )
                {
                    for( const bool taken : { true, false } )
                    {
                        targets.push_back( Edge {
                            terminator.targets[taken ? 0 : 1],
                            narrowing( analysis, block, taken ),
                            relation( analysis, block, taken ),
                        } );
                    }
                }
            }

            for( const Edge& edge : targets )
            {
                arriving = state;

                // The tested local, within what the edge allows; an edge it cannot take is left out.
                if( edge.narrowed )
                {
                    const Local_id              local   = edge.narrowed->local;
                    const Type_id               type    = func.locals[local].type;
                    const std::optional<Bounds> current = bounds( state.values[local.v], type, analysis.types );
                    if( current )
                    {
                        const Bounds within {
                            std::max( current->lo, edge.narrowed->bounds.lo ), std::min( current->hi, edge.narrowed->bounds.hi )
                        };
                        if( within.lo > within.hi )
                        {
                            continue;
                        }
                        arriving.values[local.v] = of_bounds( within, type, analysis.types );
                    }
                }

                if( edge.related && !relate( arriving.relations, *edge.related ) )
                {
                    continue;
                }

                std::optional<State>& entry = values[edge.target.v];

                if( !entry )
                {
                    entry                = arriving;
                    changed              = true;
                    dirty[edge.target.v] = true;
                    continue;
                }

                // Before the values join, so a pair one path does not relate is ordered by that path's bounds.
                Relations relations = joined( analysis, *entry, arriving );

                for( const Local_id l : func.locals.ids() )
                {
                    const Value old    = entry->values[l.v];
                    Value       joined = join( old, arriving.values[l.v], func.locals[l].type, analysis.types );
                    if( joined != old )
                    {
                        if( old.state == Value::State::Range && joined.state == Value::State::Range )
                        {
                            joined = widened( old, joined, stops, func.locals[l].type, analysis.types );
                        }
                        entry->values[l.v]   = joined;
                        changed              = true;
                        dirty[edge.target.v] = true;
                    }
                }

                if( relations != entry->relations )
                {
                    entry->relations     = std::move( relations );
                    changed              = true;
                    dirty[edge.target.v] = true;
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

void propagate_constants( Function& func, const Type_table& types, Literal_pool& literals, bool relating )
{
    const Analysis analysis { func, types, literals, tracked_locals( func, types ), relating };
    const auto     entry = values_on_entry( analysis );

    Links links( func.locals.size() );

    for( const Block_id b : func.blocks.ids() )
    {
        if( !entry[b.v] )
        {
            continue;
        }

        State  state = *entry[b.v];
        Block& block = func.blocks[b];
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
            const Value result = evaluated( analysis, func.rvalue( statement ), state );
            // Wherever it is written: a global's or a field's sum folds as a local's does.
            if( result.state == Value::State::Constant && !is_constant( func.rvalue( statement ) ) )
            {
                func.rvalue( statement ) = value_of( result.bits, func.rvalue( statement ).type, types, literals );
            }
            else
            {
                rewrite( analysis, func.rvalue( statement ).a, state.values, literals );
                rewrite( analysis, func.rvalue( statement ).b, state.values, literals );

                for( Operand& argument : func.rvalue( statement ).arguments )
                {
                    rewrite( analysis, argument, state.values, literals );
                }
            }

            links.note( analysis, statement, state.values );
            step( analysis, statement, state );
        }

        rewrite( analysis, block.terminator.condition, state.values, literals );
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
    explicit Propagated( std::string_view source, u32 level = 2, std::optional<Loop_weights> loops = Loop_weights {} )
        : Compiled(
              std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, Build { .level = level, .loops = loops }
          )
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

// `k` is `1` round the loop: the only arm that changes it tests `k != 1`, which never holds. Unrotated.
TEST_CASE( "propagate_constants_keeps_a_constant_a_dead_arm_would_change", "[opt][constants]" )
{
    const Propagated p(
        "i32 f( i32 n ) { i32 k = 1; i32 t = 0; for( i32 i = 0; i < n; i++ ) { if( k != 1 ) { k = 2; } "
        "t = t + k; } return t; }\n",
        2,
        std::nullopt
    );
    REQUIRE( p.branches() == 1 );
    REQUIRE( p.reads( "k" ) == 0 );
}

// `i` is `0` from the entry, but `1` round the back edge, so the loop keeps reading it. Unrotated.
TEST_CASE( "propagate_constants_keeps_a_loop_counter", "[opt][constants]" )
{
    const Propagated p(
        "i32 f( i32 n ) { i32 t = 0; for( i32 i = 0; i < n; i++ ) { t = t + i; } return t; }\n", 2, std::nullopt
    );
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

// Under `x < y`, `!( x >= y )`, `x <= y` and `y > x` all hold, and `x == y` fails.
TEST_CASE( "propagate_constants_folds_a_test_of_two_locals_a_dominating_one_decides", "[opt][constants]" )
{
    const char* const inner[] = { "!( x >= y )", "x <= y", "y > x", "!( x == y )" };

    for( const char* test : inner )
    {
        const Propagated p(
            std::string( "i32 f( i32 x, i32 y ) { if( x < y ) { if( " ) + test + " ) { return 1; } return 2; } return 3; }\n", 3
        );
        REQUIRE( p.branches() == 1 );
    }
}

// `x` is written between the two tests, so the second stays; `-O0` keeps both.
TEST_CASE( "propagate_constants_keeps_a_test_of_two_locals_a_store_separates", "[opt][constants]" )
{
    const Propagated stored(
        "i32 f( i32 x, i32 y, i32 z ) { if( x < y ) { x = z; if( x < y ) { return 1; } return 2; } return 3; }\n", 3
    );
    REQUIRE( stored.branches() == 2 );

    const Propagated O0( "i32 f( i32 x, i32 y ) { if( x < y ) { if( x <= y ) { return 1; } return 2; } return 3; }\n", 0 );
    REQUIRE( O0.branches() == 2 );
}

// One path tests `i < n`, the other enters with `i` 0 and `n` at least 1, so `i < n` holds at the join.
TEST_CASE( "propagate_constants_orders_a_pair_one_path_knows_by_the_other_paths_bounds", "[opt][constants]" )
{
    const Propagated p(
        "i32 f( i32 n, bool c ) { i32 i = 0; if( n < 1 ) { return 0; } if( c ) { i = g( n ); if( i >= n ) { return 0; } } "
        "if( i < n ) { return 1; } return 2; }\n"
        "i32 g( i32 n ) { return n / 2; }\n",
        3
    );
    REQUIRE( p.branches() == 3 );
}

// Rotated, the loop is entered with `i` 0 under `0 < n`, and again under `i < n`, so `i >= n` never holds.
TEST_CASE( "propagate_constants_drops_a_check_a_rotated_loops_test_decides", "[opt][constants]" )
{
    const Propagated p(
        "i32 f( i32 n ) { i32 t = 0; for( i32 i = 0; i < n; i += 1 ) { if( i >= n ) { return -1; } t += i; } return t; }\n", 3
    );
    REQUIRE( p.branches() == 2 );
}

// Past `x == 3`, `x` is the constant.
TEST_CASE( "propagate_constants_reads_the_constant_an_equality_tested", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 x ) { if( x == 3 ) { return x + 1; } return 0; }\n" );
    REQUIRE( p.reads( "x" ) == 1 );
}

// `i` grows round the loop until it is widened to the test's 300, past a `u8`, so the check stays and the pass ends.
TEST_CASE( "propagate_constants_widens_a_growing_range", "[opt][constants]" )
{
    const Propagated p( "u8 f() { u8 t = 0; for( i32 i = 0; i < 300; i++ ) { t = cast<u8>( i ); } return t; }\n" );
    REQUIRE( p.branches() == 2 );
}

// `k` widens to the test's 16, not to any `u64`, so `i * 16 + k` stays under 256 and the bounds check goes.
TEST_CASE( "propagate_constants_widens_to_a_tested_bound", "[opt][constants]" )
{
    const Propagated p( "i64 f( const ref i64[256] a, u64 i ) { i64 s = 0; if( i < 16 ) { for( u64 k = 0; k < 16; k += 1 ) "
                        "{ s += a[i * 16 + k]; } } return s; }\n" );
    REQUIRE( p.branches() == 2 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
