// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/forward_elements.h"
#include <algorithm>
#include <optional>
#include <utility>
#include "ir/simplify.h"
#include "opt/forward_reads.h"

namespace keel
{
namespace
{
// An index: `base` as it was at `version`, plus `offset`; a constant when `base` is invalid.
struct Key
{
    Local_id base {};
    u32      version = 0;
    u64      offset  = 0;

    bool operator==( const Key& ) const = default;
};

// Whether two keys never name one element.
bool distinct( const Key& a, const Key& b )
{
    return a.base == b.base && a.version == b.version && a.offset != b.offset;
}

// A store to `array` at `key`, still the element's value.
struct Fact
{
    Local_id               array;
    Key                    key;
    std::optional<Operand> value; // a constant or a local's copy, which a read can take
    u32                    value_version = 0;
    u32                    store         = 0;     // into func.statements
    bool                   unread        = false; // no read of `array` since, and the store only writes
};

class Scan
{
public:
    Scan( Function& func, const Type_table& types, const Literal_pool& literals )
        : func_( func ),
          types_( types ),
          literals_( literals ),
          pointed_( func.locals.size(), false ),
          version_( func.locals.size(), 0 ),
          derived_( func.locals.size() )
    {
        for( const Statement& statement : func.statements )
        {
            const Rvalue& value = std::as_const( func ).rvalue( statement );
            if( statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Address_of &&
                value.a.place.local.is_valid() )
            {
                pointed_[value.a.place.local.v] = true;
            }
        }
        for( const Local_id l : func.locals.ids() )
        {
            tracked_.push_back( !pointed_[l.v] && types.is_sized_array( func.locals[l].type ) );
        }
    }

    bool any_tracked() const
    {
        return std::ranges::find( tracked_, true ) != tracked_.end();
    }

    // Marking: `split` each store whose value a later read needs in a local. Else reads take what
    // the facts hold, and `deleted` each overwritten store.
    void block( Block_id b, bool marking, std::vector<bool>& split, std::vector<bool>& deleted, bool& changed )
    {
        facts_.clear();
        for( const Local_id l : touched_ )
        {
            derived_[l.v] = std::nullopt;
        }
        touched_.clear();

        const Block& block = func_.blocks[b];
        for( u32 s = block.first_statement; s < block.first_statement + block.statement_count; ++s )
        {
            Statement& statement = func_.statements[s];
            if( statement.kind == Statement_kind::Assign && reads_values( std::as_const( func_ ).rvalue( statement ) ) )
            {
                Rvalue& value = func_.rvalue( statement );
                take( value.a, marking, split, changed );
                take( value.b, marking, split, changed );
                for( Operand& argument : value.arguments )
                {
                    take( argument, marking, split, changed );
                }
            }
            read( statement );
            write( statement, s, marking, deleted, changed );
        }
    }

private:
    // Rvalues whose operands are values, not places.
    static bool reads_values( const Rvalue& value )
    {
        switch( value.kind )
        {
        case Rvalue_kind::Use:
        case Rvalue_kind::Binary:
        case Rvalue_kind::Unary:
        case Rvalue_kind::Cast:
        case Rvalue_kind::Sqrt:
        case Rvalue_kind::Rotate:
        case Rvalue_kind::Count_ones:
        case Rvalue_kind::Leading_zeros:
        case Rvalue_kind::Select:
        case Rvalue_kind::Call:
            return true;
        default:
            return false;
        }
    }

    // A read of an element at a known key reads the facts it may alias; any other read of an array, all its
    // facts. A store's own place is not a read of its array.
    void read( const Statement& statement )
    {
        const auto reads = [&]( Local_id array, const std::optional<Key>& key )
        {
            for( Fact& fact : facts_ )
            {
                fact.unread = fact.unread && !( fact.array == array && !( key && distinct( fact.key, *key ) ) );
            }
        };
        if( statement.kind != Statement_kind::Assign )
        {
            if( !statement.place.is_global() && statement.place.local.is_valid() )
            {
                reads( statement.place.local, std::nullopt );
            }
            return;
        }
        for_each_operand(
            std::as_const( func_ ).rvalue( statement ),
            [&]( const Operand& operand )
            {
                if( operand.has_place() && !operand.place.is_global() && operand.place.local.is_valid() )
                {
                    const std::optional<std::optional<Key>> at = element( operand.place );
                    reads( operand.place.local, at ? *at : std::nullopt );
                }
            }
        );
    }

    bool wide( Type_id type ) const
    {
        if( !type.is_valid() )
        {
            return false;
        }
        const Type& t = types_.get( type );
        return t.kind == Type_kind::Int && t.width == 64;
    }

    std::optional<Key> key_of( const Operand& operand ) const
    {
        if( operand.kind == Operand_kind::Constant && operand.constant.is_valid() && wide( operand.type ) )
        {
            return Key { .offset = literals_.integer( operand.constant ) };
        }
        return operand.kind == Operand_kind::Copy && operand.place.is_whole_local() ? key_of( operand.place.local )
                                                                                    : std::nullopt;
    }

    std::optional<Key> key_of( Local_id l ) const
    {
        if( pointed_[l.v] )
        {
            return std::nullopt;
        }
        return derived_[l.v] ? *derived_[l.v] : Key { .base = l, .version = version_[l.v] };
    }

    // The key `value` computes: a key, or one plus or minus a constant.
    std::optional<Key> derive( const Rvalue& value ) const
    {
        if( !wide( value.type ) )
        {
            return std::nullopt;
        }
        if( value.kind == Rvalue_kind::Use )
        {
            return key_of( value.a );
        }
        if( value.kind != Rvalue_kind::Binary || ( value.op != Token_kind::Plus && value.op != Token_kind::Minus ) )
        {
            return std::nullopt;
        }
        const std::optional<Key> a = key_of( value.a );
        const std::optional<Key> b = key_of( value.b );
        if( !a || !b || ( a->base.is_valid() && b->base.is_valid() ) ||
            ( value.op == Token_kind::Minus && b->base.is_valid() ) )
        {
            return std::nullopt;
        }
        Key sum    = a->base.is_valid() ? *a : *b;
        sum.offset = value.op == Token_kind::Plus ? a->offset + b->offset : a->offset - b->offset;
        return sum;
    }

    // `place` as one element of a tracked array, with its key if known.
    std::optional<std::optional<Key>> element( const Place& place ) const
    {
        if( place.is_global() || !place.local.is_valid() || !tracked_[place.local.v] || place.num_projections != 1 ||
            func_.projections[place.first_projection].kind != Projection_kind::Index )
        {
            return std::nullopt;
        }
        return key_of( func_.projections[place.first_projection].index );
    }

    Fact* fact_at( Local_id array, const Key& key )
    {
        const auto found = std::ranges::find_if( facts_, [&]( const Fact& f ) { return f.array == array && f.key == key; } );
        return found == facts_.end() ? nullptr : &*found;
    }

    void take( Operand& operand, bool marking, std::vector<bool>& split, bool& changed )
    {
        if( operand.kind != Operand_kind::Copy )
        {
            return;
        }
        const std::optional<std::optional<Key>> at   = element( operand.place );
        Fact* const                             fact = at && *at ? fact_at( operand.place.local, **at ) : nullptr;
        if( fact == nullptr )
        {
            return;
        }
        if( !fact->value )
        {
            if( marking )
            {
                split[fact->store] = true;
            }
            return;
        }
        const Operand& value = *fact->value;
        if( marking || value.type != operand.type ||
            ( value.kind == Operand_kind::Copy && version_[value.place.local.v] != fact->value_version ) )
        {
            return;
        }
        const Span span = operand.span;
        operand         = value;
        operand.span    = span;
        changed         = true;
    }

    void forget( Local_id array )
    {
        std::erase_if( facts_, [&]( const Fact& f ) { return f.array == array; } );
    }

    void write( const Statement& statement, u32 s, bool marking, std::vector<bool>& deleted, bool& changed )
    {
        const Place& place = statement.place;
        if( place.is_global() || !place.local.is_valid() )
        {
            return;
        }
        const Local_id                          l  = place.local;
        const std::optional<std::optional<Key>> at = statement.kind == Statement_kind::Assign ? element( place ) : std::nullopt;
        if( at && *at )
        {
            const Key     key   = **at;
            const Rvalue& value = std::as_const( func_ ).rvalue( statement );
            Fact* const   old   = fact_at( l, key );
            if( old != nullptr && old->unread && !marking )
            {
                deleted[old->store] = true;
                changed             = true;
            }
            std::erase_if( facts_, [&]( const Fact& f ) { return f.array == l && !distinct( f.key, key ); } );
            Fact fact { .array = l, .key = key, .value = std::nullopt, .store = s, .unread = value.kind == Rvalue_kind::Use };
            const Operand& from = value.a;
            if( value.kind == Rvalue_kind::Use &&
                ( from.kind == Operand_kind::Constant || ( from.kind == Operand_kind::Copy && from.place.is_whole_local() &&
                                                           from.place.local != l && !pointed_[from.place.local.v] ) ) )
            {
                fact.value         = from;
                fact.value_version = from.kind == Operand_kind::Copy ? version_[from.place.local.v] : 0;
            }
            facts_.push_back( fact );
            return;
        }

        // Any other write changes the local, and every fact of an array it touches.
        if( tracked_[l.v] )
        {
            forget( l );
        }
        const std::optional<Key> derived = statement.kind == Statement_kind::Assign && place.is_whole_local()
                                               ? derive( std::as_const( func_ ).rvalue( statement ) )
                                               : std::nullopt;
        version_[l.v]                    = ++clock_;
        derived_[l.v]                    = derived;
        touched_.push_back( l );
    }

    Function&                       func_;
    const Type_table&               types_;
    const Literal_pool&             literals_;
    Local_set                       pointed_; // an address of it is taken
    std::vector<bool>               tracked_; // a local array no pointer reaches
    std::vector<u32>                version_; // per local, bumped at each write
    std::vector<std::optional<Key>> derived_; // per local, the key its last write in this block computed
    std::vector<Local_id>           touched_;
    std::vector<Fact>               facts_;
    u32                             clock_ = 0;
};

// Each marked store `a[i] = x + y` becomes `t = x + y; a[i] = copy t`.
void split_stores( Function& func, const std::vector<bool>& split )
{
    for( const Block_id b : func.blocks.ids() )
    {
        const u32 first = func.blocks[b].first_statement;
        for( u32 k = func.blocks[b].statement_count; k-- > 0; )
        {
            if( !split[first + k] )
            {
                continue;
            }
            Statement&     store = func.statements_of( b )[k];
            Rvalue         value = func.rvalue( store );
            const Type_id  type  = value.type;
            const Span     span  = store.span;
            const Local_id local = func.locals.push_back( Local { .type = type, .span = span } );
            func.rvalue( store ) =
                Rvalue { .kind = Rvalue_kind::Use, .type = type, .a = copy( Place { .local = local }, type ) };
            Statement compute {
                .span = span, .place = Place { .local = local }, .value = func.add_rvalue( std::move( value ) )
            };
            insert_statement( func, b, k, std::move( compute ) );
        }
    }
    remove_statements( func, std::vector<bool>( func.statements.size(), false ) );
}

// One scan per block; the first only finds the stores to split.
bool scan( Function& func, const Type_table& types, const Literal_pool& literals, bool marking, std::vector<bool>& split )
{
    Scan scanner( func, types, literals );
    bool changed = false;
    if( !scanner.any_tracked() )
    {
        return false;
    }
    std::vector<bool> deleted( func.statements.size(), false );
    split.assign( func.statements.size(), false );
    for( const Block_id b : func.blocks.ids() )
    {
        scanner.block( b, marking, split, deleted, changed );
    }
    if( std::ranges::find( deleted, true ) != deleted.end() )
    {
        remove_statements( func, deleted );
    }
    return changed;
}
} // namespace

bool forward_elements( Function& func, const Type_table& types, const Literal_pool& literals )
{
    std::vector<bool> split;
    scan( func, types, literals, true, split );
    const bool splitting = std::ranges::find( split, true ) != split.end();
    if( splitting )
    {
        split_stores( func, split );
    }
    return scan( func, types, literals, false, split ) || splitting;
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
// Optimised as a release build emits it; the function under test is first.
struct Forwarded : Compiled
{
    explicit Forwarded( std::string_view source, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + "i32 main() { return 0; }\n",
              Stage::Elaborated,
              Build { .level = 2, .inlining = inlining }
          )
    {
    }

    // Element reads, and element writes.
    std::size_t reads() const
    {
        std::size_t n = 0;
        for( const Statement& s : functions.front().statements )
        {
            for_each_operand(
                functions.front().rvalue( s ),
                [&]( const Operand& o ) { n += o.has_place() && o.place.num_projections == 1 && indexed( o.place ); }
            );
        }
        return n;
    }

    std::size_t writes() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            functions.front().statements,
            [&]( const Statement& s )
            { return s.kind == Statement_kind::Assign && s.place.num_projections == 1 && indexed( s.place ); }
        ) );
    }

    bool indexed( const Place& place ) const
    {
        return functions.front().projections[place.first_projection].kind == Projection_kind::Index;
    }
};
} // namespace

// `a[j + 1]` lies beside `a[j]`, so its store keeps `a[j]`'s; only the first read is of memory.
TEST_CASE( "forward_elements_reads_what_a_store_to_the_element_wrote", "[opt][forward_elements]" )
{
    const Forwarded p( "f64 f( f64 x, u64 k ) { f64[4] a = [ x, .. ]; u64 j = k & 1; a[j] += x; a[j + 1] = 3.0; a[j] += x; "
                       "return a[j] + a[j + 1]; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.functions.front() ).empty() );
    REQUIRE( p.reads() == 1 );
}

// `a[k & 3]` reads the array, so `a[j]`'s last store stays, but nothing read the first.
TEST_CASE( "forward_elements_removes_a_store_a_later_one_overwrites", "[opt][forward_elements]" )
{
    const Forwarded p( "f64 f( f64 x, u64 k ) { f64[4] a = [ x, .. ]; u64 j = k & 1; a[j] += x; a[j] += x; return a[k & 3]; }\n"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.functions.front() ).empty() );
    REQUIRE( p.reads() == 2 );
    REQUIRE( p.writes() == 5 );
}

// `k & 3` may be 1.
TEST_CASE( "forward_elements_keeps_a_read_a_store_may_overwrite", "[opt][forward_elements]" )
{
    const Forwarded p( "f64 f( f64 x, u64 k ) { f64[4] a = [ x, .. ]; a[1] = x; a[k & 3] = 2.0; return a[1]; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.functions.front() ).empty() );
    REQUIRE( p.reads() == 1 );
}
// `set` writes `a[0]` through the borrow its call takes, so the read stays.
TEST_CASE( "forward_elements_keeps_a_read_after_a_borrow_wrote_the_array", "[opt][forward_elements]" )
{
    const Forwarded p(
        "i32 f() { i32[2] a = [ 0, 0 ]; a[0] = 1; set( ref a ); return a[0]; }\n"
        "void set( ref i32[] r ) { r[0] = 5; }\n",
        std::nullopt
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.functions.front() ).empty() );
    REQUIRE( p.reads() == 1 );
}
} // namespace keel
#endif
