#include "sema/type.h"
#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>

namespace keel
{
Type_table::Type_table()
{
    // Since slot 0 is reserved for "invalid", push a dummy entry
    types_.push_back( Type {} );
    composed_.push_back( {} );

    error_ = add( { Type_kind::Error, 0, false, Type_id {} }, "<error>" );
    void_  = add( { Type_kind::Void, 0, false, Type_id {} }, "void" );
    bool_  = add( { Type_kind::Bool, 1, false, Type_id {} }, "bool" );

    for( u8 i = 0; i < 4; ++i )
    {
        const u8 width  = narrow_cast<u8>( 1 << ( i + 3 ) ); // 8,16,32,64
        integers_[i][0] = add( { Type_kind::Int, width, true, Type_id {} }, fmt::format( "i{}", width ) );
        integers_[i][1] = add( { Type_kind::Int, width, false, Type_id {} }, fmt::format( "u{}", width ) );
    }

    floats_[0] = add( { Type_kind::Float, 32, false, Type_id {} }, "f32" );
    floats_[1] = add( { Type_kind::Float, 64, false, Type_id {} }, "f64" );

    // Built last, from the names already in composed_ - a deque, so these views stay valid.
    for( const Type_id id : { void_, bool_, floats_[0], floats_[1] } )
    {
        by_spelling_.emplace( composed_[id.v], id );
    }

    for( const auto& widths : integers_ )
    {
        by_spelling_.emplace( composed_[widths[0].v], widths[0] );
        by_spelling_.emplace( composed_[widths[1].v], widths[1] );
    }
}

Type_id Type_table::builtin( Type_kind kind ) const
{
    switch( kind )
    {
    case Type_kind::Error:
        return error_;
    case Type_kind::Void:
        return void_;
    case Type_kind::Bool:
        return bool_;
    default:
        assert( false );
        return Type_id {};
    }
}

Type_id Type_table::integer( u8 width, bool is_signed ) const
{
    return integers_[width_index( width )][is_signed ? 0 : 1];
}

Type_id Type_table::floating( u8 width ) const
{
    assert( width == 32 || width == 64 );
    return floats_[width == 64];
}

Type_id Type_table::pointer_to( Type_id element )
{
    // same element -> same pointer type
    const auto it = pointers_.find( element.v );
    if( it != pointers_.end() )
    {
        return it->second;
    }

    const std::string spelling = fmt::format( "{}*", name( element ) );
    const Type_id     id       = add( { Type_kind::Pointer, 0, false, element }, spelling );
    pointers_.emplace( element.v, id );
    return id;
}

Type_id Type_table::enumeration( Node_id declaration, std::string_view name, Type_id underlying )
{
    const auto it = enums_.find( declaration.v );

    if( it != enums_.end() )
    {
        return it->second;
    }

    // The declaration is carried the way a struct's is: it is how a consumer gets from the type
    // back to the variants, which is what exhaustiveness checking walks.
    const Type_id id = add( Type { Type_kind::Enum, 0, false, underlying, declaration }, name );
    enums_.emplace( declaration.v, id );
    return id;
}

Type_id Type_table::structure( Node_id declaration, std::span<const Type_id> arguments, std::string_view name )
{
    std::vector<Instance>& instances = structs_[declaration.v];

    for( const Instance& seen : instances )
    {
        if( std::equal( seen.arguments.begin(), seen.arguments.end(), arguments.begin(), arguments.end() ) )
        {
            return seen.type;
        }
    }

    // Copied into storage the table owns, because the caller's span is a local: a Type holds a view
    // of this for the rest of the run. An empty argument list keeps an empty view rather than an
    // entry, so a non-generic aggregate costs nothing.
    const std::span<const Type_id> owned =
        arguments.empty() ? std::span<const Type_id> {}
                          : std::span<const Type_id>( arguments_.emplace_back( arguments.begin(), arguments.end() ) );

    // `Box<i32>`, composed from the arguments' own names so that a nested one reads as written -
    // `Box<Pair<i32>>` rather than anything the caller had to assemble.
    std::string spelling( name );

    for( std::size_t i = 0; i < owned.size(); ++i )
    {
        spelling += i == 0 ? "<" : ", ";
        spelling += this->name( owned[i] );
    }

    if( !owned.empty() )
    {
        spelling += ">";
    }

    const Type_id id = add( Type { Type_kind::Struct, 0, false, Type_id {}, declaration, owned }, spelling );

    instances.push_back( Instance { .arguments = owned, .type = id } );

    return id;
}

Type_id Type_table::parameter( Node_id declaration, std::string_view name )
{
    const auto it = params_.find( declaration.v );

    if( it != params_.end() )
    {
        return it->second;
    }

    const Type_id id = add( Type { Type_kind::Parameter, 0, false, Type_id {}, declaration }, name );

    params_.emplace( declaration.v, id );

    return id;
}

bool Type_table::mentions_parameter( Type_id id ) const
{
    if( !id.is_valid() )
    {
        return false;
    }

    const Type& described = get( id );

    switch( described.kind )
    {
    case Type_kind::Parameter:
        return true;
    case Type_kind::Pointer:
        return mentions_parameter( described.element );
    case Type_kind::Struct:
        for( Type_id arg : described.arguments )
        {
            if( mentions_parameter( arg ) )
            {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

Type_id Type_table::substitute( Type_id type, const Bindings& bindings )
{
    if( !type.is_valid() )
    {
        return Type_id {}; // a parameter whose own annotation failed to resolve; check() absorbs it
    }

    const Type& described = get( type );

    switch( described.kind )
    {
    case Type_kind::Parameter:
    {
        const auto found = bindings.find( type.v );

        // Unbound means the caller built the map from the wrong declaration's parameters, which is
        // a compiler bug rather than a program one - every parameter in scope is bound by
        // construction at the one call site that makes a map.
        assert( found != bindings.end() && "type parameter is not bound" );

        return found->second;
    }
    case Type_kind::Pointer:
    {
        return pointer_to( substitute( described.element, bindings ) );
    }
    case Type_kind::Struct:
    {
        // A non-generic aggregate substitutes to itself, and asking the table to re-intern it would
        // only find it again. Worth the branch because most struct types are this one.
        if( described.arguments.empty() )
        {
            return type;
        }

        std::vector<Type_id> substituted;

        substituted.reserve( described.arguments.size() );

        for( const Type_id argument : described.arguments )
        {
            substituted.push_back( substitute( argument, bindings ) );
        }

        // The *base* name, not this type's: `name( type )` is the whole rendering - `Box<T>` - and
        // handing that back would intern `Box<T><i32>`.
        return structure( described.declaration, substituted, base_name( type ) );
    }
    default:
        return type;
    }

    return type;
}

const Type& Type_table::get( Type_id id ) const
{
    assert( id.is_valid() );
    return types_[id.v];
}

std::string_view Type_table::name( Type_id id ) const
{
    assert( id.is_valid() );
    return std::string_view( composed_[id.v] );
}

std::string_view Type_table::base_name( Type_id id ) const
{
    const std::string_view rendered = name( id );

    return rendered.substr( 0, rendered.find( '<' ) );
}

std::vector<Type_id> Type_table::struct_types() const
{
    std::vector<Type_id> result;

    for( const auto& [declaration, instances] : structs_ )
    {
        for( const Instance& instance : instances )
        {
            result.push_back( instance.type );
        }
    }

    // Sorted, because the map is unordered and the emitter writes these out in the order it is
    // given: without this the generated C would differ between runs of the same compiler on the
    // same source. Type_id is interning order, which is deterministic for a given program.
    std::sort( result.begin(), result.end(), []( Type_id a, Type_id b ) { return a.v < b.v; } );

    return result;
}

Type_id Type_table::from_spelling( std::string_view spelling ) const
{
    const auto it = by_spelling_.find( spelling );
    return it == by_spelling_.end() ? Type_id {} : it->second;
}

bool Type_table::holds( Type_id from, Type_id to ) const
{
    if( is_error( from ) || is_error( to ) )
    {
        return true;
    }

    if( from == to )
    {
        return true;
    }

    const Type& f = get( from );
    const Type& t = get( to );

    // int -> int: same signdness, from.width <= to.width
    //             unsigned -> signed, to.width > from.width strictly
    //             signed -> unsigned: never
    if( is_integer( from ) && is_integer( to ) )
    {
        if( f.is_signed == t.is_signed )
        {
            return f.width <= t.width;
        }
        else if( !f.is_signed && t.is_signed )
        {
            return f.width < t.width;
        }
        else
        {
            return false;
        }
    }

    // int -> float: the bound is the mantissa, not the width.
    if( is_integer( from ) && is_float( to ) )
    {
        const u8 mantissa = t.width == 32 ? 24 : 53;
        const u8 bits     = f.is_signed ? f.width - 1 : f.width;
        return bits <= mantissa;
    }

    // float -> float: to.width >= from.width
    if( is_float( from ) && is_float( to ) )
    {
        return t.width >= f.width;
    }

    // float -> int, and every pair of unrelated kinds - which is where **D30 is enforced**. An
    // enum reaches an integer through no branch above, so `i32 x = Colour::Red;` is rejected by
    // there being nothing here to accept it. Adding a case would be undoing the decision, not
    // completing the function.
    return false;
}

Type_id Type_table::common( Type_id a, Type_id b ) const
{
    if( is_error( a ) || is_error( b ) )
    {
        return error_;
    }

    if( a == b )
    {
        return a;
    }

    // walk the ten scalars in this order
    // i8 u8 i16 u16 i32 u32 f32 i64 u64 f64
    // e.g., common(i16,u16) is held by both i32 and f32 at width 32,
    // and picking f32 shifts the whole table.
    const Type_id scalars[] = {
        integer( 8, true ),
        integer( 8, false ),
        integer( 16, true ),
        integer( 16, false ),
        integer( 32, true ),
        integer( 32, false ),
        floating( 32 ),
        integer( 64, true ),
        integer( 64, false ),
        floating( 64 ),
    };

    for( Type_id candidate : scalars )
    {
        if( holds( a, candidate ) && holds( b, candidate ) )
        {
            return candidate;
        }
    }

    return Type_id {};
}

Type_id Type_table::cpp_result( Type_id a, Type_id b ) const
{
    if( is_error( a ) || is_error( b ) )
    {
        return error_;
    }

    // A float operand wins, and the wider float wins over the narrower - including int plus float,
    // however much precision the integer loses. That loss is exactly what D5 rejects downstream.
    if( is_float( a ) || is_float( b ) )
    {
        if( !is_float( b ) )
        {
            return a;
        }

        if( !is_float( a ) )
        {
            return b;
        }

        return get( a ).width >= get( b ).width ? a : b;
    }

    // Nothing but a number has a result here. Without this a struct's width of 0 falls into the
    // promotion below and becomes i32, and the answer is nonsense.
    if( !is_integer( a ) && !is_float( a ) )
    {
        return Type_id {};
    }

    if( !is_integer( b ) && !is_float( b ) )
    {
        return Type_id {};
    }

    // Integral promotion: anything narrower than int becomes i32.
    if( get( a ).width < 32 )
    {
        a = integer( 32, true );
    }

    if( get( b ).width < 32 )
    {
        b = integer( 32, true );
    }

    if( a == b )
    {
        return a;
    }

    // Same signedness takes the wider. Testing kinds here instead would always be true - both are
    // Int by this point - which would leave the mixed case below unreachable.
    if( get( a ).is_signed == get( b ).is_signed )
    {
        return get( a ).width >= get( b ).width ? a : b;
    }

    // Mixed: the unsigned type wins at equal or greater width, and negatives are reinterpreted.
    // Otherwise the signed type is wide enough to hold every unsigned value and wins instead.
    // This is the split D5's second clause is stated against.
    const Type_id signed_type   = get( a ).is_signed ? a : b;
    const Type_id unsigned_type = get( a ).is_signed ? b : a;

    return get( unsigned_type ).width >= get( signed_type ).width ? unsigned_type : signed_type;
}

Type_id Type_table::arithmetic_result( Type_id a, Type_id b ) const
{
    // Without this the error is absorbed into a real type - holds() is true for errors, so the
    // candidate walk in common() would answer i32 for `unknown + 1` and cascade from there.
    if( is_error( a ) || is_error( b ) )
    {
        return error_;
    }

    const Type_id c = cpp_result( a, b );

    // cpp_result has no answer for a non-numeric operand. holds() treats an invalid id as an error
    // and returns true for it, so without this the two checks below would both pass and common()
    // would hand back the struct type - which is how `p + q` starts compiling.
    if( !c.is_valid() )
    {
        return Type_id {};
    }

    if( !holds( a, c ) || !holds( b, c ) )
    {
        return Type_id {};
    }

    return common( a, b );
}

bool Type_table::is_error( Type_id id ) const
{
    return !id.is_valid() || id == error_;
}

bool Type_table::is_integer( Type_id id ) const
{
    assert( id.is_valid() );
    return get( id ).kind == Type_kind::Int;
}

bool Type_table::is_float( Type_id id ) const
{
    assert( id.is_valid() );
    return get( id ).kind == Type_kind::Float;
}

bool Type_table::is_struct( Type_id id ) const
{
    assert( id.is_valid() );
    return get( id ).kind == Type_kind::Struct;
}

bool Type_table::is_enum( Type_id id ) const
{
    assert( id.is_valid() );
    return get( id ).kind == Type_kind::Enum;
}

bool Type_table::is_pointer( Type_id id ) const
{
    assert( id.is_valid() );
    return get( id ).kind == Type_kind::Pointer;
}

bool Type_table::is_parameter( Type_id id ) const
{
    assert( id.is_valid() );
    return get( id ).kind == Type_kind::Parameter;
}

bool Type_table::is_void( Type_id id ) const
{
    assert( id.is_valid() );
    return get( id ).kind == Type_kind::Void;
}

bool Type_table::fits( u64 magnitude, bool negative, Type_id type ) const
{
    if( is_error( type ) )
    {
        return true;
    }

    if( is_float( type ) )
    {
        // Integers are exact up to and including 2^mantissa; the next one is not.
        const u8 mantissa = get( type ).width == 32 ? 24 : 53;
        return magnitude <= ( 1ull << mantissa );
    }

    if( !is_integer( type ) )
    {
        return false;
    }

    const Type& t = get( type );

    if( negative && !t.is_signed )
    {
        return false;
    }

    // The inclusive maximum magnitude. A signed type reaches one further downwards than upwards,
    // which is what makes -2147483648 a valid i32 while 2147483648 is not.
    u64 max = 0;

    if( t.is_signed )
    {
        max = ( 1ull << ( t.width - 1 ) ) - ( negative ? 0 : 1 );
    }
    else
    {
        // 1ull << 64 is undefined, so the widest unsigned type cannot be computed by shifting.
        max = t.width == 64 ? std::numeric_limits<u64>::max() : ( 1ull << t.width ) - 1;
    }

    return magnitude <= max;
}

bool Type_table::fits_float( f64 value, Type_id type ) const
{
    if( is_error( type ) )
    {
        return true;
    }

    if( !is_float( type ) )
    {
        return false;
    }

    // f64 always fits: the lexer rejected anything strtod could not hold and recorded no value.
    if( get( type ).width == 64 )
    {
        return true;
    }

    // Range only. Precision loss is not range failure - 0.1 is inexact in every binary float, and
    // rejecting inexactness would reject nearly every literal written. Underflow to a denormal or
    // to zero is IEEE working as specified.
    return std::abs( value ) <= static_cast<f64>( std::numeric_limits<f32>::max() );
}

Type_id Type_table::default_integer() const
{
    return integer( 32, true );
}

Type_id Type_table::default_float() const
{
    return floating( 64 );
}

Type_id Type_table::add( const Type& type, std::string_view name )
{
    types_.push_back( type );
    composed_.emplace_back( name );
    return Type_id { narrow_cast<u32>( types_.size() - 1 ) };
}

u8 Type_table::width_index( u8 width )
{
    switch( width )
    {
    case 8:
        return 0;
    case 16:
        return 1;
    case 32:
        return 2;
    case 64:
        return 3;
    default:
        assert( false );
        return 0;
    }
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{
namespace
{

// The ten scalar types, in the row/column order PLAN §6.4 prints them.
constexpr std::string_view scalars[] = { "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64" };

Type_id named( const Type_table& t, std::string_view name )
{
    const u8 width = name.size() == 2 ? 8 : narrow_cast<u8>( ( name[1] - '0' ) * 10 + ( name[2] - '0' ) );

    return name[0] == 'f' ? t.floating( width ) : t.integer( width, name[0] == 'i' );
}

} // namespace

TEST_CASE( "type_id_default_is_invalid", "[sema][type]" )
{
    const Type_table table;

    REQUIRE_FALSE( Type_id {}.is_valid() );

    // Every builtin must be distinguishable from "no type", or `common` cannot report failure.
    for( const std::string_view name : scalars )
    {
        INFO( name );
        REQUIRE( named( table, name ).is_valid() );
    }

    REQUIRE( table.builtin( Type_kind::Bool ).is_valid() );
    REQUIRE( table.builtin( Type_kind::Void ).is_valid() );
    REQUIRE( table.builtin( Type_kind::Error ).is_valid() );
}

// Interning is the whole point: it makes `==` mean type equality.
TEST_CASE( "type_table_interns", "[sema][type]" )
{
    Type_table table;

    REQUIRE( table.integer( 32, true ) == table.integer( 32, true ) );
    REQUIRE( table.floating( 64 ) == table.floating( 64 ) );

    REQUIRE( table.integer( 32, true ) != table.integer( 32, false ) ); // i32 vs u32
    REQUIRE( table.integer( 32, true ) != table.integer( 64, true ) );  // i32 vs i64
    REQUIRE( table.builtin( Type_kind::Bool ) != table.builtin( Type_kind::Void ) );

    // Distinct across the whole scalar set, not merely pairwise where it is convenient.
    for( const std::string_view a : scalars )
    {
        for( const std::string_view b : scalars )
        {
            INFO( a << " vs " << b );
            REQUIRE( ( named( table, a ) == named( table, b ) ) == ( a == b ) );
        }
    }

    REQUIRE( table.pointer_to( table.integer( 8, false ) ) == table.pointer_to( table.integer( 8, false ) ) );
    REQUIRE( table.pointer_to( table.integer( 8, false ) ) != table.pointer_to( table.integer( 16, false ) ) );
}

TEST_CASE( "type_table_get_round_trips", "[sema][type]" )
{
    Type_table table;

    const Type& i32 = table.get( table.integer( 32, true ) );
    REQUIRE( i32.kind == Type_kind::Int );
    REQUIRE( i32.width == 32 );
    REQUIRE( i32.is_signed );

    const Type& u8 = table.get( table.integer( 8, false ) );
    REQUIRE( u8.kind == Type_kind::Int );
    REQUIRE( u8.width == 8 );
    REQUIRE_FALSE( u8.is_signed );

    const Type& f64 = table.get( table.floating( 64 ) );
    REQUIRE( f64.kind == Type_kind::Float );
    REQUIRE( f64.width == 64 );

    REQUIRE( table.get( table.builtin( Type_kind::Bool ) ).kind == Type_kind::Bool );

    const Type_id element = table.integer( 8, false );
    const Type&   pointer = table.get( table.pointer_to( element ) );
    REQUIRE( pointer.kind == Type_kind::Pointer );
    REQUIRE( pointer.element == element );
}

TEST_CASE( "type_table_names_match_the_source_spelling", "[sema][type]" )
{
    Type_table table;

    for( const std::string_view name : scalars )
    {
        INFO( name );
        REQUIRE( table.name( named( table, name ) ) == name );
    }

    REQUIRE( table.name( table.builtin( Type_kind::Bool ) ) == "bool" );
    REQUIRE( table.name( table.builtin( Type_kind::Void ) ) == "void" );
    REQUIRE( table.name( table.pointer_to( table.integer( 8, false ) ) ) == "u8*" );
}

// PLAN §6.4, assignment direction. Listed as "types that hold this one", excluding itself.
TEST_CASE( "type_table_holds_matches_the_assignment_table", "[sema][type]" )
{
    const Type_table table;

    struct Row
    {
        std::string_view from;
        std::string_view wider; // space separated
    };

    static const Row rows[] = {
        { "i8", "i16 i32 i64 f32 f64" },
        { "i16", "i32 i64 f32 f64" },
        { "i32", "i64 f64" },
        { "i64", "" },
        { "u8", "i16 i32 i64 u16 u32 u64 f32 f64" },
        { "u16", "i32 i64 u32 u64 f32 f64" },
        { "u32", "i64 u64 f64" },
        { "u64", "" },
        { "f32", "f64" },
        { "f64", "" },
    };

    for( const Row& row : rows )
    {
        for( const std::string_view to : scalars )
        {
            const bool expected = to == row.from || row.wider.find( to ) != std::string_view::npos;

            INFO( row.from << " -> " << to );
            REQUIRE( table.holds( named( table, row.from ), named( table, to ) ) == expected );
        }
    }
}

// Signed cannot hold unsigned of the same width, and no float value is exactly an integer type.
TEST_CASE( "type_table_holds_rejects_the_lossy_directions", "[sema][type]" )
{
    const Type_table table;

    REQUIRE_FALSE( table.holds( table.integer( 32, false ), table.integer( 32, true ) ) ); // u32 -> i32
    REQUIRE_FALSE( table.holds( table.integer( 32, true ), table.integer( 32, false ) ) ); // i32 -> u32
    REQUIRE_FALSE( table.holds( table.integer( 64, true ), table.floating( 64 ) ) );       // i64 -> f64
    REQUIRE_FALSE( table.holds( table.integer( 32, true ), table.floating( 32 ) ) );       // i32 -> f32
    REQUIRE_FALSE( table.holds( table.floating( 64 ), table.floating( 32 ) ) );            // f64 -> f32
    REQUIRE_FALSE( table.holds( table.floating( 32 ), table.integer( 64, true ) ) );       // float -> int
}

// C++'s own rule, which D5's second clause is stated in terms of. Integral promotion included.
TEST_CASE( "type_table_cpp_result_models_the_usual_arithmetic_conversions", "[sema][type]" )
{
    const Type_table table;
    const auto       cpp = [&]( std::string_view a, std::string_view b )
    { return table.name( table.cpp_result( named( table, a ), named( table, b ) ) ); };

    SECTION( "narrower than int promotes to i32" )
    {
        REQUIRE( cpp( "u8", "u8" ) == "i32" );
        REQUIRE( cpp( "i8", "u8" ) == "i32" );
        REQUIRE( cpp( "u16", "u16" ) == "i32" );
    }

    SECTION( "same signedness takes the wider" )
    {
        REQUIRE( cpp( "i32", "i64" ) == "i64" );
        REQUIRE( cpp( "u32", "u64" ) == "u64" );
    }

    SECTION( "unsigned wins at equal or greater rank - the case D5 rejects" )
    {
        REQUIRE( cpp( "i32", "u32" ) == "u32" );
        REQUIRE( cpp( "i32", "u64" ) == "u64" );
        REQUIRE( cpp( "i64", "u64" ) == "u64" );
    }

    SECTION( "a strictly wider signed type wins instead" )
    {
        REQUIRE( cpp( "i64", "u32" ) == "i64" );
        REQUIRE( cpp( "i32", "u16" ) == "i32" );
    }

    SECTION( "a float operand wins, even when it loses precision" )
    {
        REQUIRE( cpp( "i32", "f32" ) == "f32" );
        REQUIRE( cpp( "i64", "f64" ) == "f64" );
        REQUIRE( cpp( "f32", "f64" ) == "f64" );
    }
}

// PLAN §6.4, transcribed. Rows and columns are `scalars`; "--" is a compile error.
constexpr std::string_view arithmetic_table[10][10] = {
    /*       i8     i16    i32    i64    u8     u16    u32    u64    f32    f64  */
    /* i8 */ { "i8", "i16", "i32", "i64", "i16", "i32", "--", "--", "f32", "f64" },
    /* i16*/ { "i16", "i16", "i32", "i64", "i16", "i32", "--", "--", "f32", "f64" },
    /* i32*/ { "i32", "i32", "i32", "i64", "i32", "i32", "--", "--", "--", "f64" },
    /* i64*/ { "i64", "i64", "i64", "i64", "i64", "i64", "i64", "--", "--", "--" },
    /* u8 */ { "i16", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64" },
    /* u16*/ { "i32", "i32", "i32", "i64", "u16", "u16", "u32", "u64", "f32", "f64" },
    /* u32*/ { "--", "--", "--", "i64", "u32", "u32", "u32", "u64", "--", "f64" },
    /* u64*/ { "--", "--", "--", "--", "u64", "u64", "u64", "u64", "--", "--" },
    /* f32*/ { "f32", "f32", "--", "--", "f32", "f32", "--", "--", "f32", "f64" },
    /* f64*/ { "f64", "f64", "f64", "--", "f64", "f64", "f64", "--", "f64", "f64" },
};

TEST_CASE( "type_table_arithmetic_result_matches_plan_6_4", "[sema][type]" )
{
    const Type_table table;

    for( std::size_t row = 0; row < std::size( scalars ); ++row )
    {
        for( std::size_t col = 0; col < std::size( scalars ); ++col )
        {
            const Type_id          got = table.arithmetic_result( named( table, scalars[row] ), named( table, scalars[col] ) );
            const std::string_view expected = arithmetic_table[row][col];

            INFO( scalars[row] << " op " << scalars[col] << " -> expected " << expected );

            if( expected == "--" )
            {
                REQUIRE_FALSE( got.is_valid() );
            }
            else
            {
                REQUIRE( got.is_valid() );
                REQUIRE( table.name( got ) == expected );
            }
        }
    }
}

// Properties that must hold whatever the table says, so a future edit to it cannot go unnoticed.
TEST_CASE( "type_table_arithmetic_result_properties", "[sema][type]" )
{
    const Type_table table;

    for( const std::string_view a : scalars )
    {
        // Closure: `T op T` is `T`, or D5 breaks `n = n + 1` for every type but the widest.
        INFO( a << " op " << a );
        REQUIRE( table.arithmetic_result( named( table, a ), named( table, a ) ) == named( table, a ) );

        for( const std::string_view b : scalars )
        {
            const Type_id ab = table.arithmetic_result( named( table, a ), named( table, b ) );
            const Type_id ba = table.arithmetic_result( named( table, b ), named( table, a ) );

            INFO( a << " op " << b );
            REQUIRE( ab == ba ); // operand order cannot matter

            if( ab.is_valid() )
            {
                // Whatever it picks must be lossless for both sides - that is the entire rule.
                REQUIRE( table.holds( named( table, a ), ab ) );
                REQUIRE( table.holds( named( table, b ), ab ) );
            }
        }
    }
}

// `common` answers "is there a lossless type", `arithmetic_result` also asks "would C++ agree".
// The gap between them is the §5.1 containment clause, and it must be visible.
TEST_CASE( "type_table_common_is_wider_than_arithmetic_result", "[sema][type]" )
{
    const Type_table table;
    const Type_id    i32 = table.integer( 32, true );
    const Type_id    u32 = table.integer( 32, false );

    SECTION( "i32 and u32 do have a lossless common type" )
    {
        REQUIRE( table.common( i32, u32 ) == table.integer( 64, true ) );
    }

    SECTION( "but the operator is still an error, because C++ answers u32" )
    {
        REQUIRE_FALSE( table.arithmetic_result( i32, u32 ).is_valid() );
    }

    SECTION( "common has no answer when no type holds both" )
    {
        REQUIRE_FALSE( table.common( table.integer( 64, true ), table.integer( 64, false ) ).is_valid() );
        REQUIRE_FALSE( table.common( table.integer( 8, true ), table.integer( 64, false ) ).is_valid() );
    }

    SECTION( "and is the smallest such type, not merely some such type" )
    {
        REQUIRE( table.common( table.integer( 8, true ), table.integer( 8, false ) ) == table.integer( 16, true ) );
        REQUIRE( table.common( table.integer( 8, true ), table.integer( 16, false ) ) == table.integer( 32, true ) );
        REQUIRE( table.common( table.integer( 32, false ), table.integer( 64, true ) ) == table.integer( 64, true ) );
    }
}

TEST_CASE( "type_table_fits_at_the_boundaries", "[sema][type]" )
{
    const Type_table table;

    struct Bound
    {
        std::string_view type;
        u64              largest;  // greatest positive magnitude that fits
        u64              smallest; // greatest negative magnitude, or 0 for an unsigned type
    };

    static const Bound bounds[] = {
        { "i8", 127, 128 },
        { "i16", 32767, 32768 },
        { "i32", 2147483647ull, 2147483648ull },
        { "i64", 9223372036854775807ull, 9223372036854775808ull },
        { "u8", 255, 0 },
        { "u16", 65535, 0 },
        { "u32", 4294967295ull, 0 },
        { "u64", 18446744073709551615ull, 0 },
    };

    for( const Bound& b : bounds )
    {
        const Type_id type = named( table, b.type );

        INFO( b.type );

        REQUIRE( table.fits( 0, false, type ) );
        REQUIRE( table.fits( b.largest, false, type ) );

        // u64's maximum has no successor to test with.
        if( b.largest != std::numeric_limits<u64>::max() )
        {
            REQUIRE_FALSE( table.fits( b.largest + 1, false, type ) );
        }

        if( b.smallest != 0 )
        {
            // The asymmetry: a signed type reaches one further downwards than upwards, so
            // `i32 x = -2147483648;` is legal while `i32 x = 2147483648;` is not.
            REQUIRE( table.fits( b.smallest, true, type ) );
            REQUIRE_FALSE( table.fits( b.smallest + 1, true, type ) );
            REQUIRE_FALSE( table.fits( b.smallest, false, type ) );
        }
        else
        {
            REQUIRE_FALSE( table.fits( 1, true, type ) ); // no negative fits an unsigned type
        }
    }
}

TEST_CASE( "type_table_fits_bounds_a_float_by_its_mantissa", "[sema][type]" )
{
    const Type_table table;

    REQUIRE( table.fits( 1ull << 53, false, table.floating( 64 ) ) );
    REQUIRE_FALSE( table.fits( ( 1ull << 53 ) + 1, false, table.floating( 64 ) ) );

    REQUIRE( table.fits( 1ull << 24, false, table.floating( 32 ) ) );
    REQUIRE_FALSE( table.fits( ( 1ull << 24 ) + 1, false, table.floating( 32 ) ) );
}

// One unresolved name must produce one diagnostic, not one per enclosing expression. That only
// holds if the error type survives every operation rather than being absorbed into a real one.
TEST_CASE( "type_table_errors_absorb", "[sema][type]" )
{
    const Type_table table;

    const Type_id error = table.builtin( Type_kind::Error );
    const Type_id i32   = table.integer( 32, true );

    SECTION( "an error operand propagates instead of yielding a plausible type" )
    {
        REQUIRE( table.arithmetic_result( error, i32 ) == error );
        REQUIRE( table.arithmetic_result( i32, error ) == error );
        REQUIRE( table.common( error, i32 ) == error );
        REQUIRE( table.cpp_result( error, i32 ) == error );
    }

    SECTION( "an invalid id counts as an error, so callers need only one guard" )
    {
        REQUIRE( table.is_error( Type_id {} ) );
        REQUIRE( table.arithmetic_result( Type_id {}, i32 ) == error );
        REQUIRE( table.common( Type_id {}, i32 ) == error );
    }

    SECTION( "and every check against an error succeeds silently" )
    {
        REQUIRE( table.holds( error, i32 ) );
        REQUIRE( table.holds( i32, error ) );
        REQUIRE( table.fits( 999999, false, error ) );
    }
}

// Nested pointers are what would expose a name() view invalidated by add().
TEST_CASE( "type_table_composes_nested_pointer_names", "[sema][type]" )
{
    Type_table table;

    const Type_id u8   = table.integer( 8, false );
    const Type_id once = table.pointer_to( u8 );

    REQUIRE( table.name( once ) == "u8*" );
    REQUIRE( table.name( table.pointer_to( once ) ) == "u8**" );
    REQUIRE( table.name( table.pointer_to( table.pointer_to( once ) ) ) == "u8***" );
    REQUIRE( table.name( once ) == "u8*" ); // still valid after three more appends
}

TEST_CASE( "type_table_from_spelling_round_trips_every_builtin", "[sema][type]" )
{
    Type_table table;

    for( const std::string_view spelling : scalars )
    {
        INFO( spelling );
        REQUIRE( table.from_spelling( spelling ) == named( table, spelling ) );
    }

    REQUIRE( table.from_spelling( "bool" ) == table.builtin( Type_kind::Bool ) );
    REQUIRE( table.from_spelling( "void" ) == table.builtin( Type_kind::Void ) );

    SECTION( "an unknown spelling has no type" )
    {
        REQUIRE_FALSE( table.from_spelling( "Widget" ).is_valid() );
        REQUIRE_FALSE( table.from_spelling( "int" ).is_valid() );
        REQUIRE_FALSE( table.from_spelling( "" ).is_valid() );
    }

    // Only spellings a program can write. These live in composed_ alongside the real ones, so a
    // scan of that container rather than a table of names would wrongly match them.
    SECTION( "internal and composed names are not spellings" )
    {
        REQUIRE_FALSE( table.from_spelling( "<error>" ).is_valid() );

        table.pointer_to( table.integer( 8, false ) );
        REQUIRE_FALSE( table.from_spelling( "u8*" ).is_valid() );
    }
}

// A generic aggregate is not one type but a family, and the arguments are what tell them apart.
// Interned on the pair, so `Box<i32>` written in two places is one type and `Box<f64>` is another -
// which is what makes a field of type `T` answerable, and what a C struct per instantiation needs.
TEST_CASE( "type_table_interns_a_generic_struct_by_its_arguments", "[sema][type][generic]" )
{
    Type_table table;

    const Node_id box { 7 };
    const Type_id i32 = table.integer( 32, true );
    const Type_id f64 = table.floating( 64 );

    const Type_id of_i32 = table.structure( box, std::array { i32 }, "Box" );
    const Type_id of_f64 = table.structure( box, std::array { f64 }, "Box" );

    SECTION( "different arguments are different types" )
    {
        REQUIRE( of_i32 != of_f64 );
        REQUIRE( table.is_struct( of_i32 ) );
        REQUIRE( table.is_struct( of_f64 ) );
    }

    SECTION( "the same arguments are the same type" )
    {
        REQUIRE( table.structure( box, std::array { i32 }, "Box" ) == of_i32 );
    }

    SECTION( "both still name the declaration they came from" )
    {
        // Which is what lets a member lookup find the one set of fields written for them.
        REQUIRE( table.get( of_i32 ).declaration == box );
        REQUIRE( table.get( of_f64 ).declaration == box );
    }

    SECTION( "the arguments are readable back off the type" )
    {
        REQUIRE( table.get( of_i32 ).arguments.size() == 1 );
        REQUIRE( table.get( of_i32 ).arguments[0] == i32 );
        REQUIRE( table.get( of_f64 ).arguments[0] == f64 );
    }

    SECTION( "the name is composed from the arguments' own" )
    {
        // So a diagnostic says `Box<i32>` rather than `Box`, and a nested one reads as written.
        REQUIRE( table.name( of_i32 ) == "Box<i32>" );
        REQUIRE( table.name( of_f64 ) == "Box<f64>" );
        REQUIRE( table.name( table.structure( box, std::array { of_i32 }, "Box" ) ) == "Box<Box<i32>>" );
    }

    SECTION( "several arguments are separated as written" )
    {
        const Node_id pair { 11 };

        REQUIRE( table.name( table.structure( pair, std::array { i32, f64 }, "Pair" ) ) == "Pair<i32, f64>" );
    }

    SECTION( "a non-generic aggregate carries none, and is unchanged" )
    {
        const Node_id point { 13 };

        REQUIRE( table.name( table.structure( point, {}, "Point" ) ) == "Point" );
        REQUIRE( table.get( table.structure( point, {}, "Point" ) ).arguments.empty() );
    }

    SECTION( "the arguments are copied, not borrowed from the caller" )
    {
        // Type::arguments is a span, so what it views has to belong to the table: every caller
        // builds its argument list in a local and lets it go, and a type outlives all of them.
        Type_id interned {};

        {
            std::vector<Type_id> caller_local { i32 };

            interned = table.structure( Node_id { 21 }, caller_local, "Held" );

            // Scribbled over before it dies, so a borrowed view reads the wrong type rather than
            // merely reading freed memory that happens to still hold the right value.
            caller_local[0] = f64;
        }

        REQUIRE( table.get( interned ).arguments.size() == 1 );
        REQUIRE( table.get( interned ).arguments[0] == i32 );
        REQUIRE( table.name( interned ) == "Held<i32>" );
    }

    SECTION( "a view stays valid as the table grows" )
    {
        // Interning more types must not move what an earlier one points at, which is why the
        // storage behind these views is a deque rather than a vector.
        const std::span<const Type_id> early = table.get( of_i32 ).arguments;

        for( int i = 0; i < 64; ++i )
        {
            table.structure( Node_id { static_cast<u32>( 100 + i ) }, std::array { i32, f64 }, "Filler" );
        }

        REQUIRE( early[0] == i32 );
        REQUIRE( table.get( of_i32 ).arguments[0] == i32 );
    }
}

TEST_CASE( "type_table_interns_structs_by_declaration", "[sema][type]" )
{
    Type_table table;

    // Node ids stand in for Struct_decl nodes; the table only ever compares them.
    const Node_id first { 7 };
    const Node_id second { 11 };

    const Type_id point = table.structure( first, {}, "Point" );

    REQUIRE( point.is_valid() );
    REQUIRE( table.is_struct( point ) );
    REQUIRE( table.name( point ) == "Point" );
    REQUIRE( table.get( point ).declaration == first );

    SECTION( "the same declaration gives the same type" )
    {
        REQUIRE( table.structure( first, {}, "Point" ) == point );
    }

    // The property the whole design turns on: at M7 two modules may each declare `Point`, and they
    // must not be the same type.
    SECTION( "two declarations of the same name are two types" )
    {
        REQUIRE( table.structure( second, {}, "Point" ) != point );
    }

    // Struct names are resolved through the resolver, never through by_spelling_, which stays the
    // eleven builtins forever.
    SECTION( "a struct name is not a spelling" )
    {
        REQUIRE_FALSE( table.from_spelling( "Point" ).is_valid() );
    }

    SECTION( "a struct is none of the scalar kinds" )
    {
        REQUIRE_FALSE( table.is_integer( point ) );
        REQUIRE_FALSE( table.is_float( point ) );
        REQUIRE_FALSE( table.is_error( point ) );
        REQUIRE_FALSE( table.is_struct( table.integer( 32, true ) ) );
    }
}

// v0 has no operator overloading and no conversions between struct types, so every one of these
// must fail. `p + q` compiling is the failure mode to watch for.
TEST_CASE( "type_table_rejects_struct_conversions_and_arithmetic", "[sema][type]" )
{
    Type_table table;

    const Type_id point = table.structure( Node_id { 7 }, {}, "Point" );
    const Type_id line  = table.structure( Node_id { 11 }, {}, "Line" );
    const Type_id i32   = table.integer( 32, true );

    SECTION( "holds is identity only" )
    {
        REQUIRE( table.holds( point, point ) );
        REQUIRE_FALSE( table.holds( point, line ) );
        REQUIRE_FALSE( table.holds( point, i32 ) );
        REQUIRE_FALSE( table.holds( i32, point ) );
        REQUIRE_FALSE( table.holds( point, table.builtin( Type_kind::Bool ) ) );
    }

    SECTION( "there is no arithmetic on a struct" )
    {
        REQUIRE_FALSE( table.arithmetic_result( point, point ).is_valid() );
        REQUIRE_FALSE( table.arithmetic_result( point, line ).is_valid() );
        REQUIRE_FALSE( table.arithmetic_result( point, i32 ).is_valid() );
        REQUIRE_FALSE( table.arithmetic_result( i32, point ).is_valid() );
    }

    SECTION( "and no common type with anything but itself" )
    {
        REQUIRE( table.common( point, point ) == point );
        REQUIRE_FALSE( table.common( point, line ).is_valid() );
        REQUIRE_FALSE( table.common( point, i32 ).is_valid() );
    }

    SECTION( "a literal never fits a struct" )
    {
        REQUIRE_FALSE( table.fits( 0, false, point ) );
        REQUIRE_FALSE( table.fits( 42, false, point ) );
    }
}

// Range only, and the two things it must *not* reject are the point: rejecting inexactness would
// reject nearly every float literal ever written.
TEST_CASE( "type_table_fits_float_checks_range_only", "[sema][type]" )
{
    const Type_table table;

    const Type_id f32_type = table.floating( 32 );
    const Type_id f64_type = table.floating( 64 );

    SECTION( "a value beyond f32's range does not fit" )
    {
        REQUIRE_FALSE( table.fits_float( 1e40, f32_type ) );
        REQUIRE_FALSE( table.fits_float( -1e40, f32_type ) );
    }

    SECTION( "one at the edge does" )
    {
        REQUIRE( table.fits_float( 3.4e38, f32_type ) );
        REQUIRE( table.fits_float( -3.4e38, f32_type ) );
        REQUIRE( table.fits_float( 0.0, f32_type ) );
    }

    // 0.1 is inexact in every binary float. Range and representability are different questions,
    // and only the first is checked - exactly as fits() checks an integer's range, not whether it
    // round-trips.
    SECTION( "an inexact value fits" )
    {
        REQUIRE( table.fits_float( 0.1, f32_type ) );
        REQUIRE( table.fits_float( 0.1, f64_type ) );
    }

    // Underflow to a denormal or to zero is IEEE working as specified, not a range failure.
    SECTION( "so does one that underflows" )
    {
        REQUIRE( table.fits_float( 1e-50, f32_type ) );
    }

    // The lexer already rejected anything strtod could not hold, and recorded no value for it.
    SECTION( "f64 always fits" )
    {
        REQUIRE( table.fits_float( 1e300, f64_type ) );
        REQUIRE( table.fits_float( -1e300, f64_type ) );
    }

    SECTION( "and a float value never fits a non-float type" )
    {
        REQUIRE_FALSE( table.fits_float( 1.5, table.integer( 32, true ) ) );
        REQUIRE_FALSE( table.fits_float( 0.0, table.integer( 64, false ) ) );
        REQUIRE_FALSE( table.fits_float( 1.5, table.builtin( Type_kind::Bool ) ) );
        REQUIRE( table.fits_float( 1.5, table.builtin( Type_kind::Error ) ) ); // absorbs
    }
}

TEST_CASE( "type_table_pointers_convert_to_nothing", "[sema][type]" )
{
    Type_table table;

    const Type_id i32       = table.integer( 32, true );
    const Type_id u8        = table.integer( 8, false );
    const Type_id to_i32    = table.pointer_to( i32 );
    const Type_id to_u8     = table.pointer_to( u8 );
    const Type_id to_to_i32 = table.pointer_to( to_i32 );

    REQUIRE( table.is_pointer( to_i32 ) );
    REQUIRE_FALSE( table.is_pointer( i32 ) );
    REQUIRE_FALSE( table.is_integer( to_i32 ) );
    REQUIRE( table.name( to_i32 ) == "i32*" );
    REQUIRE( table.name( to_to_i32 ) == "i32**" );

    // Identity only. No void*, no pointer-to-integer, no widening between pointee types - a
    // pointer conversion is exactly the escape hatch a type system exists to refuse.
    SECTION( "holds is identity only" )
    {
        REQUIRE( table.holds( to_i32, to_i32 ) );
        REQUIRE_FALSE( table.holds( to_i32, to_u8 ) );
        REQUIRE_FALSE( table.holds( to_u8, to_i32 ) );
        REQUIRE_FALSE( table.holds( to_i32, to_to_i32 ) );
        REQUIRE_FALSE( table.holds( to_i32, i32 ) );
        REQUIRE_FALSE( table.holds( i32, to_i32 ) );
        REQUIRE_FALSE( table.holds( to_i32, table.integer( 64, false ) ) );
    }

    // D27: `p + 1` on a single-item pointer is nonsense, not merely unsafe. §6.4 answers nothing
    // for a pointer, so rejection is the default rather than a rule that had to be added.
    SECTION( "there is no arithmetic on a pointer" )
    {
        REQUIRE_FALSE( table.arithmetic_result( to_i32, i32 ).is_valid() );
        REQUIRE_FALSE( table.arithmetic_result( to_i32, to_i32 ).is_valid() );
        REQUIRE_FALSE( table.common( to_i32, to_u8 ).is_valid() );
        REQUIRE_FALSE( table.fits( 0, false, to_i32 ) );
    }
}

} // namespace keel
#endif
