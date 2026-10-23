// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/split_aggregates.h"
#include <algorithm>
#include <map>
#include "ir/live.h"
#include "ir/place_type.h"
#include "ir/simplify.h"

namespace keel
{

namespace
{

// `_L`, with no projection.
bool whole_local( const Place& place )
{
    return !place.is_global() && place.local.is_valid() && place.num_projections == 0;
}

// `_D = copy _S` or `move`, of a struct.
bool copies_whole( const Statement& statement, const Type_table& table )
{
    return statement.kind == Statement_kind::Assign && statement.value.kind == Rvalue_kind::Use &&
           statement.value.a.has_place() && table.is_struct( statement.value.type );
}

// `_P = &init _L`, building the whole of `_L`.
bool initialises_whole( const Statement& statement )
{
    return statement.kind == Statement_kind::Assign && statement.value.kind == Rvalue_kind::Address_of &&
           statement.value.address_purpose == Address_purpose::Initialise && whole_local( statement.value.a.place );
}

// The locals to split, from `first`. A local's storage markers don't count, nor does an `&init` of it
// whose pointer nothing else names: an inlined constructor's, left after its body; and a whole copy
// doesn't count on a side that is a whole local.
Local_set splittable( const Function& func, const Type_table& table, u32 first )
{
    Local_set        split( func.locals.size(), false );
    std::vector<u32> mentions( func.locals.size(), 0 );
    std::vector<u32> inits;

    for( u32 l = first; l < func.locals.size(); ++l )
    {
        split[l] = table.is_struct( func.locals[l].type );
    }

    const auto use = [&]( const Place& place )
    {
        if( place.is_global() || !place.local.is_valid() )
        {
            return;
        }

        mentions[place.local.v] += 1;

        if( place.num_projections == 0 || func.projections[place.first_projection].kind != Projection_kind::Field )
        {
            split[place.local.v] = false;
        }
    };

    for( u32 s = 0; s < func.statements.size(); ++s )
    {
        const Statement& statement = func.statements[s];

        if( statement.kind == Statement_kind::Storage_dead || statement.kind == Statement_kind::Storage_live )
        {
            continue;
        }

        if( initialises_whole( statement ) )
        {
            use( statement.place );
            inits.push_back( s );
            continue;
        }

        if( copies_whole( statement, table ) )
        {
            if( !whole_local( statement.place ) )
            {
                use( statement.place );
            }
            if( !whole_local( statement.value.a.place ) )
            {
                use( statement.value.a.place );
            }
            continue;
        }

        use( statement.place );

        if( statement.kind == Statement_kind::Assign )
        {
            for_each_operand(
                func,
                statement.value,
                [&]( const Operand& operand )
                {
                    if( operand.has_place() )
                    {
                        use( operand.place );
                    }
                }
            );

            if( statement.value.kind == Rvalue_kind::Address_of )
            {
                use( statement.value.a.place );
            }
        }
    }

    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        const Terminator& terminator = func.blocks[b].terminator;

        if( terminator.condition.has_place() )
        {
            use( terminator.condition.place );
        }

        if( terminator.message.has_place() )
        {
            use( terminator.message.place );
        }

        if( terminator.at.has_place() )
        {
            use( terminator.at.place );
        }
    }

    for( u32 s : inits )
    {
        const Statement& statement = func.statements[s];
        const Local_id   built     = statement.value.a.place.local;

        if( statement.place.is_global() || statement.place.num_projections != 0 || mentions[statement.place.local.v] != 1 )
        {
            split[built.v] = false;
        }
    }

    for( u32 l = 0; l < func.locals.size(); ++l )
    {
        split[l] = split[l] && mentions[l] > 0;
    }

    return split;
}

// One new local per split local and field named, keyed by both; each place loses its first
// projection, the split locals' markers and `&init`s go, and a whole copy of a split local becomes one
// copy per field. Rebuilt block by block.
void split_locals( Function& func, const Local_set& split, const Ast& ast, Types& types )
{
    std::map<std::pair<u32, u32>, Local_id> parts;
    std::vector<Statement>                  statements;
    statements.reserve( func.statements.size() );

    const auto rewrite = [&]( Place& place )
    {
        if( place.is_global() || !place.local.is_valid() || !split[place.local.v] )
        {
            return;
        }

        const Node_id field = func.projections[place.first_projection].field;

        if( !parts.contains( { place.local.v, field.v } ) )
        {
            const Local   whole = func.locals[place.local.v];
            const Type_id type  = place_type(
                ast,
                types,
                func,
                Place { .local = place.local, .first_projection = place.first_projection, .num_projections = 1 }
            );
            parts[{ place.local.v, field.v }] = Local_id { narrow_cast<u32>( func.locals.size() ) };
            func.locals.push_back( Local { .type = type, .span = whole.span, .name = whole.name } );
        }

        place.local = parts[{ place.local.v, field.v }];
        place.first_projection += 1;
        place.num_projections -= 1;
    };

    for( Block& block : func.blocks )
    {
        const u32 first       = block.first_statement;
        const u32 count       = block.statement_count;
        block.first_statement = narrow_cast<u32>( statements.size() );

        for( u32 s = first; s < first + count; ++s )
        {
            Statement statement = func.statements[s];

            if( statement.kind == Statement_kind::Storage_dead || statement.kind == Statement_kind::Storage_live )
            {
                if( !split[statement.place.local.v] )
                {
                    statements.push_back( statement );
                }
                continue;
            }

            if( initialises_whole( statement ) && split[statement.value.a.place.local.v] )
            {
                continue;
            }

            if( copies_whole( statement, types.table() ) &&
                ( ( whole_local( statement.place ) && split[statement.place.local.v] ) ||
                  ( whole_local( statement.value.a.place ) && split[statement.value.a.place.local.v] ) ) )
            {
                const Type_id type = statement.value.type;
                for( const Node_id& member : ast.members( types.table().get( type ).declaration ) )
                {
                    if( ast.kind( member ) != Node_kind::Field_decl )
                    {
                        continue;
                    }

                    Statement part = statement;
                    part.place =
                        projected( func, statement.place, Projection { .kind = Projection_kind::Field, .field = member } );
                    part.value.a.place = projected(
                        func, statement.value.a.place, Projection { .kind = Projection_kind::Field, .field = member }
                    );
                    part.value.type   = place_type( ast, types, func, part.place );
                    part.value.a.type = place_type( ast, types, func, part.value.a.place );

                    if( types.table().is_builtin( part.value.a.type ) )
                    {
                        part.value.a.kind = Operand_kind::Copy;
                    }

                    rewrite( part.place );
                    rewrite( part.value.a.place );

                    statements.push_back( part );
                }
                continue;
            }

            rewrite( statement.place );
            rewrite( statement.value.a.place );
            rewrite( statement.value.b.place );

            for( u32 i = 0; i < statement.value.argument_count; ++i )
            {
                rewrite( func.operands[statement.value.first_argument + i].place );
            }

            statements.push_back( statement );
        }

        block.statement_count = narrow_cast<u32>( statements.size() ) - block.first_statement;

        rewrite( block.terminator.condition.place );
        rewrite( block.terminator.message.place );
        rewrite( block.terminator.at.place );
    }

    func.statements = std::move( statements );
}

// A fresh local like `local`, which every place naming `local` now names, its markers included.
Local_id renamed( Function& func, Local_id local )
{
    const Local whole = func.locals[local.v];
    func.locals.push_back( whole );
    const Local_id new_local = Local_id { narrow_cast<u32>( func.locals.size() - 1 ) };

    const auto rename = [&]( Place& place )
    {
        if( !place.is_global() && place.local == local )
        {
            place.local = new_local;
        }
    };

    for( Statement& statement : func.statements )
    {
        rename( statement.place );
        rename( statement.value.a.place );
        rename( statement.value.b.place );

        for( u32 i = 0; i < statement.value.argument_count; ++i )
        {
            rename( func.operands[statement.value.first_argument + i].place );
        }
    }

    for( Block& block : func.blocks )
    {
        rename( block.terminator.condition.place );
        rename( block.terminator.message.place );
        rename( block.terminator.at.place );
    }

    return new_local;
}

// Whether some assignment writes a field of `local`.
bool writes_a_field( const Function& func, Local_id local )
{
    for( const Statement& statement : func.statements )
    {
        if( statement.kind == Statement_kind::Assign && statement.place.local == local && !statement.place.is_global() &&
            statement.place.num_projections > 0 )
        {
            return true;
        }
    }

    return false;
}

// Whether some operand or borrow reads a field of `local`.
bool reads_a_field( const Function& func, Local_id local )
{
    const auto names = [&]( const Place& place )
    { return !place.is_global() && place.local == local && place.num_projections > 0; };
    const auto reads = [&]( const Operand& operand ) { return operand.has_place() && names( operand.place ); };

    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        bool any = statement.value.kind == Rvalue_kind::Address_of && names( statement.value.a.place );
        for_each_operand( func, statement.value, [&]( const Operand& operand ) { any = any || reads( operand ); } );

        if( any )
        {
            return true;
        }
    }

    return std::ranges::any_of(
        func.blocks,
        [&]( const Block& b )
        { return reads( b.terminator.condition ) || reads( b.terminator.message ) || reads( b.terminator.at ); }
    );
}

// A split parameter with a field written is copied whole into a fresh local, which takes its place, in
// a new entry block, since block 0 may be a loop header.
void copy_parameters( Function& func, const Local_set& split )
{
    std::vector<u32> kept;
    for( u32 p = 1; p <= func.parameter_count; ++p )
    {
        if( !split[p] || !writes_a_field( func, Local_id { p } ) )
        {
            continue;
        }
        kept.push_back( p );
    }

    if( kept.empty() )
    {
        return;
    }

    const Block_id moved { narrow_cast<u32>( func.blocks.size() ) };
    const Block    head = func.blocks[0];
    func.blocks.push_back( head );

    for( Block& block : func.blocks )
    {
        for( Block_id& target : block.terminator.targets )
        {
            if( target.v == 0 )
            {
                target = moved;
            }
        }
    }

    func.blocks[0] = Block {
        .first_statement = narrow_cast<u32>( func.statements.size() ),
        .terminator      = Terminator { .kind = Terminator_kind::Goto, .span = head.terminator.span, .targets = { moved } },
    };

    for( u32 p : kept )
    {
        const Local_id fresh = renamed( func, Local_id { p } );
        func.statements.push_back( Statement {
            .kind  = Statement_kind::Assign,
            .span  = func.locals[p].span,
            .place = Place { .local = fresh },
            .value = use( copy( Place { .local = Local_id { p } }, func.locals[p].type ) ),
        } );
        func.blocks[0].statement_count += 1;
    }
}

// The return slot's fields are built in a fresh local, moved into `_0` before each return.
void move_into_return_slot( Function& func )
{
    const Local_id fresh = renamed( func, k_return_slot );
    for( Block& block : func.blocks )
    {
        if( block.terminator.kind != Terminator_kind::Return )
        {
            continue;
        }

        const u32 first       = block.first_statement;
        block.first_statement = narrow_cast<u32>( func.statements.size() );
        for( u32 s = first; s < first + block.statement_count; ++s )
        {
            const Statement statement = func.statements[s];
            func.statements.push_back( statement );
        }

        func.statements.push_back( Statement {
            .kind  = Statement_kind::Assign,
            .span  = block.terminator.span,
            .place = Place { .local = k_return_slot },
            .value = use( move( Place { .local = fresh }, func.locals[0].type ) ),
        } );
        block.statement_count += 1;
    }

    remove_statements( func, std::vector<bool>( func.statements.size(), false ) );
}

} // namespace

void split_aggregates( Function& func, const Ast& ast, Types& types )
{
    const Local_set whole = splittable( func, types.table(), 0 );
    copy_parameters( func, whole );
    if( whole[0] && reads_a_field( func, k_return_slot ) )
    {
        move_into_return_slot( func );
    }

    // A field's new local may be a struct named only through its own fields in turn.
    while( true )
    {
        const Local_set split = splittable( func, types.table(), func.parameter_count + 1 );

        if( std::ranges::none_of( split, []( bool s ) { return s; } ) )
        {
            return;
        }

        split_locals( func, split, ast, types );
    }
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

constexpr std::string_view k_types = "struct Point { i32 x; i32 y; };\n"
                                     "enum Shape { dot( Point at ), none };\n"
                                     "class Box { public Box( i32 v ) { value = v; } public i32 value; };\n"
                                     "class Held { public Held( i32 v ) { box = Box( v ); n = v + 1; } "
                                     "public Box box; public i32 n; };\n";

// The release pipeline with splitting off, then the pass over the function declaring `name`.
struct Split : Compiled
{
    Function before;
    Function func;
    Local_id local {};

    explicit Split( std::string_view source, std::string_view name, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build { .optimise = true, .inlining = inlining, .splitting = false }
          )
    {
        for( const Function& f : functions )
        {
            for( u32 l = 0; l < f.locals.size() && !local.is_valid(); ++l )
            {
                if( f.locals[l].name.is_valid() && interner.text( f.locals[l].name ) == name )
                {
                    func  = f;
                    local = Local_id { l };
                }
            }
        }

        before = func;
        split_aggregates( func, ast, types );
    }

    // Whether anything but a storage marker still names the local.
    bool named() const
    {
        const auto names = [&]( const Place& place ) { return !place.is_global() && place.local == local; };
        const auto reads = [&]( const Operand& operand ) { return operand.has_place() && names( operand.place ); };

        for( const Statement& s : func.statements )
        {
            if( s.kind == Statement_kind::Storage_live || s.kind == Statement_kind::Storage_dead )
            {
                continue;
            }

            bool any = names( s.place ) || ( s.value.kind == Rvalue_kind::Address_of && names( s.value.a.place ) );
            for_each_operand( func, s.value, [&]( const Operand& operand ) { any = any || reads( operand ); } );

            if( any )
            {
                return true;
            }
        }

        return std::ranges::any_of( func.blocks, [&]( const Block& b ) { return reads( b.terminator.condition ); } );
    }

    std::size_t struct_locals_named() const
    {
        std::size_t count = 0;
        for( const Statement& s : func.statements )
        {
            count += s.kind == Statement_kind::Assign && !s.place.is_global() &&
                     types.table().is_struct( func.locals[s.place.local.v].type );
        }
        return count;
    }

    // Whether some operand reads a field of `of`, outside block `in` when given.
    bool reads( Local_id of, std::optional<u32> in = std::nullopt ) const
    {
        for( u32 b = 0; b < func.blocks.size(); ++b )
        {
            if( in == b )
            {
                continue;
            }

            const Block& block = func.blocks[b];
            for( u32 s = block.first_statement; s < block.first_statement + block.statement_count; ++s )
            {
                bool any = false;
                for_each_operand(
                    func,
                    func.statements[s].value,
                    [&]( const Operand& o ) { any = any || ( o.has_place() && !o.place.is_global() && o.place.local == of ); }
                );
                if( any )
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool writes_a_field_of( Local_id of ) const
    {
        return std::ranges::any_of(
            func.statements,
            [&]( const Statement& s ) {
                return s.kind == Statement_kind::Assign && !s.place.is_global() && s.place.local == of &&
                       s.place.num_projections > 0;
            }
        );
    }

    // Whether some whole assignment in `in` moves a value whose type `is` takes.
    template <typename Is>
    bool moves_a( Is is, const Function& in ) const
    {
        return std::ranges::any_of(
            in.statements,
            [&]( const Statement& s )
            {
                return s.kind == Statement_kind::Assign && s.value.kind == Rvalue_kind::Use &&
                       s.value.a.kind == Operand_kind::Move && is( s.value.a.type );
            }
        );
    }

    bool initialises() const
    {
        return std::ranges::any_of(
            func.statements,
            []( const Statement& s )
            { return s.value.kind == Rvalue_kind::Address_of && s.value.address_purpose == Address_purpose::Initialise; }
        );
    }
};

} // namespace

TEST_CASE( "split_aggregates_splits_a_constructed_local", "[opt][split]" )
{
    const Split p( "i32 f() { Box b = Box( 4 ); return b.value; }\n", "b" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE_FALSE( p.named() );
    REQUIRE_FALSE( p.initialises() );
    REQUIRE( p.struct_locals_named() == 0 );
}

// `h` splits into `box` and `n` on the first round, and `box` into `value` on the second.
TEST_CASE( "split_aggregates_splits_a_nested_field", "[opt][split]" )
{
    const Split p( "i32 f() { Held h = Held( 5 ); return h.box.value + h.n; }\n", "h" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE_FALSE( p.named() );
    REQUIRE( p.struct_locals_named() == 0 );
}

TEST_CASE( "split_aggregates_keeps_an_addressed_local", "[opt][split]" )
{
    const Split p( "i32 f( i32 a ) { Point p = Point { a, 1 }; ref Point r = p; r.x = 3; return p.x; }\n", "p" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.named() );
}

// `q = copy p` becomes one copy per field, and both split.
TEST_CASE( "split_aggregates_splits_a_whole_copy", "[opt][split]" )
{
    const Split p( "i32 f( i32 a ) { Point p = Point { a, 1 }; Point q = p; q.x = 7; return p.x + q.x; }\n", "p" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE_FALSE( p.named() );
    REQUIRE( p.struct_locals_named() == 0 );
}

// An inlined callee's parameter moved into its result, then into `y`: each link splits.
TEST_CASE( "split_aggregates_splits_a_chain_of_moves", "[opt][split]" )
{
    const Split p(
        "class Kept { public i32 v; public Kept( i32 n ) { v = n; } ~Kept() { } };\n"
        "Kept pass( move Kept k ) { return k; }\n"
        "i32 f( i32 a ) { Kept x = Kept( a ); Kept y = pass( pass( move x ) ); return y.v; }\n",
        "y"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE_FALSE( p.named() );
    REQUIRE( p.struct_locals_named() == 0 );
}

// A number's field of a split move is a copy, which propagation follows.
TEST_CASE( "split_aggregates_copies_a_number_field_moved_whole", "[opt][split]" )
{
    const Split p(
        "class Kept { public i32 v; public Kept( i32 n ) { v = n; } ~Kept() { } };\n"
        "Kept pass( move Kept k ) { return k; }\n"
        "i32 f( i32 a ) { Kept x = Kept( a ); Kept y = pass( pass( move x ) ); return y.v; }\n",
        "y"
    );
    REQUIRE( p.clean() );
    REQUIRE_FALSE( p.moves_a( [&]( Type_id t ) { return p.types.table().is_builtin( t ); }, p.func ) );
}

// A pointer is not a number, so its field keeps its move.
TEST_CASE( "split_aggregates_keeps_a_pointer_field_moved_whole", "[opt][split]" )
{
    const Split p(
        "class Kept { public i32* p; public i32 v; public Kept( i32* q, i32 n ) { p = q; v = n; } ~Kept() { } };\n"
        "Kept pass( move Kept k ) { return k; }\n"
        "i32 f( i32* q, i32 a ) { Kept x = Kept( q, a ); Kept y = pass( pass( move x ) ); return *y.p + y.v; }\n",
        "y"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.moves_a( [&]( Type_id t ) { return p.types.table().get( t ).kind == Type_kind::Pointer; }, p.func ) );
    REQUIRE_FALSE( p.moves_a( [&]( Type_id t ) { return p.types.table().is_builtin( t ); }, p.func ) );
}

// Not inlined, so the call takes `q` whole.
TEST_CASE( "split_aggregates_keeps_a_local_passed_whole", "[opt][split]" )
{
    const Split p(
        "i32 g( Point p ) { return p.x; }\ni32 f( i32 a ) { Point q = Point { a, 1 }; return g( q ) + q.y; }\n",
        "q",
        std::nullopt
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.named() );
}

// `p` is copied once into a fresh local in a new entry block, which splits.
TEST_CASE( "split_aggregates_splits_a_written_parameter", "[opt][split]" )
{
    const Split p( "i32 f( Point p ) { p.x = 3; return p.x + p.y; }\n", "p" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.func.blocks.size() == p.before.blocks.size() + 1 );
    REQUIRE_FALSE( p.writes_a_field_of( p.local ) );
    REQUIRE_FALSE( p.reads( p.local, 0 ) );
    REQUIRE( p.struct_locals_named() == 0 );
}

TEST_CASE( "split_aggregates_keeps_a_parameter_only_read", "[opt][split]" )
{
    const Split p( "i32 f( Point p ) { return p.x + p.y; }\n", "p" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.func.blocks.size() == p.before.blocks.size() );
    REQUIRE( p.func.locals.size() == p.before.locals.size() );
}

// The loop's back edge is sent to block 0 by hand, which lowering never does: the copy must land
// in a new entry, not run again each turn.
TEST_CASE( "split_aggregates_copies_a_parameter_once", "[opt][split]" )
{
    Split p( "i32 f( Point p ) { while( p.x < 10 ) { p.x = p.x + 1; } return p.x; }\n", "p" );
    REQUIRE( p.clean() );

    Function func = p.before;
    REQUIRE( func.blocks[0].statement_count == 0 );
    for( Block& block : func.blocks )
    {
        if( block.terminator.kind == Terminator_kind::Goto &&
            block.terminator.targets[0] == func.blocks[0].terminator.targets[0] )
        {
            block.terminator.targets[0] = Block_id { 0 };
        }
    }
    func.blocks[0].terminator.targets[0] = p.before.blocks[0].terminator.targets[0];
    REQUIRE( verify( func ).empty() );

    split_aggregates( func, p.ast, p.types );
    p.func = func;

    REQUIRE( verify( p.func ).empty() );
    REQUIRE_FALSE( p.reads( p.local, 0 ) );
    REQUIRE( std::ranges::none_of(
        p.func.blocks,
        []( const Block& b ) { return b.terminator.targets[0] == Block_id { 0 } || b.terminator.targets[1] == Block_id { 0 }; }
    ) );
}

// `_0.y = _0.x + 1` reads the result, which is built in a fresh local and moved in.
TEST_CASE( "split_aggregates_splits_a_return_slot_read", "[opt][split]" )
{
    const Split p( "Point f( i32 a ) { Point p = Point { a, 1 }; p.y = p.x + 1; return p; }\n", "a" );
    REQUIRE( p.clean() );
    REQUIRE_FALSE( p.reads( k_return_slot ) );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.before.locals.size() < p.func.locals.size() );
}

TEST_CASE( "split_aggregates_keeps_a_return_slot_only_written", "[opt][split]" )
{
    const Split p( "Point f( i32 a ) { return Point { a, 1 }; }\n", "a" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.func.locals.size() == p.before.locals.size() );
}

// Each return gets its own move of every field into `_0`.
TEST_CASE( "split_aggregates_moves_into_every_return", "[opt][split]" )
{
    const Split p(
        "Point f( i32 a, bool c ) { Point p = Point { a, 1 }; p.y = p.x + 1; if( c ) { return p; } p.x = p.y; return p; }\n",
        "a"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE_FALSE( p.reads( k_return_slot ) );

    u32 returns = 0;
    for( const Block& block : p.func.blocks )
    {
        if( block.terminator.kind != Terminator_kind::Return )
        {
            continue;
        }
        returns += 1;
        REQUIRE( block.statement_count >= 2 );
        for( u32 s = block.first_statement + block.statement_count - 2; s < block.first_statement + block.statement_count; ++s )
        {
            REQUIRE( p.func.statements[s].place.local == k_return_slot );
            REQUIRE( p.func.statements[s].place.num_projections == 1 );
        }
    }
    REQUIRE( returns == 2 );
}

TEST_CASE( "split_aggregates_keeps_an_enum", "[opt][split]" )
{
    const Split p(
        "i32 f( i32 a ) { Shape s = Shape::dot( Point { a, 2 } ); "
        "switch( s ) { case Shape::dot( at ): return at.x; case Shape::none: return 0; } }\n",
        "s"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.named() );
}

// Not inlined, so the constructor's call reads the `&init` pointer and `b` stays whole.
TEST_CASE( "split_aggregates_keeps_a_local_a_constructor_still_builds", "[opt][split]" )
{
    const Split p( "i32 f() { Box b = Box( 4 ); return b.value; }\n", "b", std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.initialises() );
    REQUIRE( p.named() );
}

} // namespace keel

#endif
