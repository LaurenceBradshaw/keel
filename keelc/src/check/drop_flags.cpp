// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/drop_flags.h"
#include <algorithm>
#include <cassert>
#include <vector>
#include "ir/slots.h"

namespace keel
{

namespace
{

// Indexed by slot (ir/slots): each leaf's flag, or invalid when it needs none.
using Flag_map = std::vector<Local_id>;

// The flag of the one leaf a place lies in. A drop is always inside one: drop_place never drops a
// tracked compound whole, and a destructor's own field drops lie inside its leaf.
Local_id flag_of( const Flag_map& flags, const Slot_map& slots, const Place& place )
{
    if( place.is_global() )
    {
        return Local_id {};
    }

    const Slots range = slots.of( place );
    assert( slots.is_leaf( range.node ) && "a place across several leaves has no one flag" );
    return flags[range.first];
}

// Calls `visit( flag )` for each flagged leaf a place covers: a whole split local covers every field.
template <typename Visit>
void for_each_flag( const Flag_map& flags, const Slot_map& slots, const Place& place, Visit&& visit )
{
    if( place.is_global() )
    {
        return;
    }

    const Slots range = slots.of( place );

    for( u32 slot = range.first; slot < range.last; ++slot )
    {
        if( flags[slot].is_valid() )
        {
            visit( flags[slot] );
        }
    }
}

bool is_out_parameter( const Function& func, Local_id local )
{
    return std::find( func.out_parameters.begin(), func.out_parameters.end(), local ) != func.out_parameters.end();
}

// Leaves both moved and dropped somewhere, and temporaries built on one path of an expression. A
// flag guards a drop, so a moved leaf never dropped needs none, and one never moved is always live
// at its drop.
std::vector<bool> slots_needing_flags( const Function& func, const Slot_map& slots )
{
    std::vector<bool> moved( slots.count(), false );

    const auto mark = [&]( std::vector<bool>& set, const Slots& range )
    {
        for( u32 slot = range.first; slot < range.last; ++slot )
        {
            if( slots.is_leaf( slot ) )
            {
                set[slot] = true;
            }
        }
    };

    // A whole local or a path of fields to a tracked node; a move from inside an enum or through a
    // pointer is no move of a leaf.
    const auto note = [&]( const Operand& operand )
    {
        if( operand.kind == Operand_kind::Move && is_field_path( func, operand.place ) )
        {
            if( const Slots range = slots.of( operand.place ); range.exact )
            {
                mark( moved, range );
            }
        }
    };

    for( const Statement& statement : func.statements )
    {
        for_each_operand( func, statement.value, note );
    }

    // A condition can move (`if ( move ready )`); its flag write lands before the terminator reads it.
    for( const Block& block : func.blocks )
    {
        note( block.terminator.condition );
        note( block.terminator.message );
        note( block.terminator.at );
    }

    // Intersected with the drops, which is what turns "was moved" into "needs a flag".
    std::vector<bool> dropped( slots.count(), false );
    std::vector<bool> replaced( slots.count(), false );

    for( const Statement& statement : func.statements )
    {
        if( statement.kind == Statement_kind::Drop && !statement.place.is_global() )
        {
            const u32 leaf = slots.of( statement.place ).first;
            dropped[leaf]  = true;

            // Through a pointer the place always holds a value, except an `out` parameter's and a
            // constructor's object's.
            if( statement.replacing &&
                ( !has_deref_projection( func, statement.place ) || is_out_parameter( func, statement.place.local ) ||
                  statement.place.local == func.constructed ) )
            {
                replaced[leaf] = true;
            }
        }
    }

    for( const Local_id local : func.one_path_temporaries )
    {
        mark( moved, slots.under( local.v ) );
    }

    for( std::size_t i = 0; i < moved.size(); ++i )
    {
        moved[i] = ( moved[i] && dropped[i] ) || replaced[i];
    }

    return moved;
}

// Appends an unnamed bool local per flagged leaf, so a dump reads `drop _1.b if _7`.
Flag_map
allocate_flags( Function& func, const Slot_map& slots, const std::vector<bool>& moved, const Flag_vocabulary& vocabulary )
{
    Flag_map  flags( slots.count(), Local_id {} );
    const u32 locals = narrow_cast<u32>( func.locals.size() );

    // In local order, a local's leaves together, so a dump numbers flags as its locals.
    for( u32 local = 0; local < locals; ++local )
    {
        const Slots whole = slots.under( local );

        for( u32 slot = whole.first; slot < whole.last; ++slot )
        {
            if( !moved[slot] )
            {
                continue;
            }

            // Read before the push_back, which may reallocate. The flag borrows its local's span.
            const Span span = func.locals[Local_id { local }].span;

            flags[slot] = Local_id { narrow_cast<u32>( func.locals.size() ) };
            func.locals.push_back( Local { .type = vocabulary.bool_type, .span = span } );
        }
    }

    return flags;
}

// An ordinary Assign of a bool constant, so nothing downstream needs a new statement kind.
Statement set_flag( Local_id flag, bool value, Span span, const Flag_vocabulary& vocabulary )
{
    return Statement {
        .kind  = Statement_kind::Assign,
        .span  = span,
        .place = Place { .local = flag },
        .value = use( constant( value ? vocabulary.true_literal : vocabulary.false_literal, vocabulary.bool_type ) )
    };
}

// The flag writes a statement implies, appended after it.
void append_flag_writes(
    const Function&         func,
    const Slot_map&         slots,
    const Statement&        statement,
    const Flag_map&         flags,
    const Flag_vocabulary&  vocabulary,
    std::vector<Statement>& out
)
{
    switch( statement.kind )
    {
    case Statement_kind::Storage_live:
    case Statement_kind::Storage_dead:
        // Storage begins and ends holding nothing, so there is nothing to drop on either side of it.
        for_each_flag(
            flags,
            slots,
            Place { .local = statement.place.local },
            [&]( Local_id flag ) { out.push_back( set_flag( flag, false, statement.span, vocabulary ) ); }
        );
        return;
    case Statement_kind::Assign:
        // The move first, then the assignment, so `_1 = move _1` ends with the flag set. Driven off the
        // operands, so linear in the statement rather than in the locals.
        for_each_operand(
            func,
            statement.value,
            [&]( const Operand& operand )
            {
                // A move out of part of a leaf, a payload, ends the whole leaf: nothing drops it after.
                if( operand.kind == Operand_kind::Move )
                {
                    for_each_flag(
                        flags,
                        slots,
                        operand.place,
                        [&]( Local_id flag ) { out.push_back( set_flag( flag, false, statement.span, vocabulary ) ); }
                    );
                }
            }
        );
        for_each_flag(
            flags,
            slots,
            statement.place,
            [&]( Local_id flag ) { out.push_back( set_flag( flag, true, statement.span, vocabulary ) ); }
        );

        // A constructed local is written through its Initialise address (`_3 = &_2`), never assigned, so
        // that address sets its flag. A borrow does not, or one after a move would revive it.
        if( statement.value.kind == Rvalue_kind::Address_of && statement.value.address_purpose == Address_purpose::Initialise )
        {
            for_each_flag(
                flags,
                slots,
                statement.value.a.place,
                [&]( Local_id flag ) { out.push_back( set_flag( flag, true, statement.span, vocabulary ) ); }
            );
        }

        return;
    case Statement_kind::Drop:
        return;
    }
}

// The first line of code the body runs: storage statements make none.
Span entry_span( const Function& func )
{
    const Block& entry = func.blocks.front();
    for( const Statement& statement : func.statements_of( entry ) )
    {
        if( statement.kind != Statement_kind::Storage_live && statement.kind != Statement_kind::Storage_dead )
        {
            return statement.span;
        }
    }

    return entry.terminator.span;
}

// The rebuild: copies every block's statements, interleaves flag writes, sets drop_flag on drops,
// and fixes each block's range as it goes.
void rewrite_statements( Function& func, const Slot_map& slots, const Flag_map& flags, const Flag_vocabulary& vocabulary )
{
    const Span             entry = entry_span( func );
    std::vector<Statement> rebuilt;
    rebuilt.reserve( func.statements.size() * 2 ); // a guess
    for( Block& block : func.blocks )
    {
        // Read the old range before overwriting it; the loop below indexes the original vector.
        const u32 old_first = block.first_statement;
        const u32 count     = block.statement_count;
        const u32 new_first = narrow_cast<u32>( rebuilt.size() );

        // Set every flag on entry: a parameter arrives holding its value, except an `out` one and a
        // constructor's object, and anything else starts empty, since an arm's temporary has no storage_live to clear it on the
        // path that skips it. The writes take the first statement's line, so a debugger's first stop
        // is on it.
        if( &block == &func.blocks.front() )
        {
            // In the order allocate_flags numbered them.
            std::vector<u32> flagged;
            for( u32 slot = 0; slot < flags.size(); ++slot )
            {
                if( flags[slot].is_valid() )
                {
                    flagged.push_back( slot );
                }
            }
            std::ranges::sort( flagged, [&]( u32 a, u32 b ) { return flags[a].v < flags[b].v; } );

            for( const u32 slot : flagged )
            {
                const Local_id flag  = flags[slot];
                const Local_id local = slots.local( slot );
                rebuilt.push_back( set_flag(
                    flag,
                    local.v >= 1 && local.v <= func.parameter_count && !is_out_parameter( func, local ) &&
                        local != func.constructed,
                    entry,
                    vocabulary
                ) );
            }
        }

        for( u32 i = 0; i < count; ++i )
        {
            Statement statement = func.statements[old_first + i];

            // A drop reads its flag rather than being followed by a write.
            if( statement.kind == Statement_kind::Drop )
            {
                statement.drop_flag = flag_of( flags, slots, statement.place );
            }

            rebuilt.push_back( statement );
            append_flag_writes( func, slots, statement, flags, vocabulary, rebuilt );
        }

        // A moving condition clears its flag before the terminator reads it.
        if( block.terminator.condition.kind == Operand_kind::Move )
        {
            if( const Local_id flag = flag_of( flags, slots, block.terminator.condition.place ); flag.is_valid() )
            {
                rebuilt.push_back( set_flag( flag, false, block.terminator.span, vocabulary ) );
            }
        }

        block.first_statement = new_first;
        block.statement_count = narrow_cast<u32>( rebuilt.size() ) - new_first;
    }

    func.statements = std::move( rebuilt );
}

} // namespace

void elaborate_drops( Function& func, const Flag_vocabulary& vocabulary )
{
    // Built before any flag is appended, so slot numbers stay fixed while flags are added.
    const Slot_map          slots( func );
    const std::vector<bool> moved = slots_needing_flags( func, slots );

    // Nothing flagged: the function is left untouched.
    if( std::find( moved.begin(), moved.end(), true ) == moved.end() )
    {
        return;
    }

    const Flag_map flags = allocate_flags( func, slots, moved, vocabulary );

    rewrite_statements( func, slots, flags, vocabulary );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include "check/pipeline_test_support.h"
#include "ir/print.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

struct Elaborated : Compiled
{
    explicit Elaborated( std::string_view source )
        : Compiled( source, Stage::Elaborated )
    {
    }

    std::string text( std::size_t index )
    {
        return print( functions[index], ast, types.table(), literals, interner );
    }

    std::string text_of( std::string_view name )
    {
        const std::string header = fmt::format( "fn {} {{", name );

        for( std::size_t i = 0; i < functions.size(); ++i )
        {
            if( std::string rendered = text( i ); rendered.starts_with( header ) )
            {
                return rendered;
            }
        }

        return {};
    }
};

// A class that owns something, plus somewhere for a move to go. Every fixture below builds on it.
constexpr std::string_view k_owning = "class Owned { u64 id; Owned( u64 n ) { id = n; } ~Owned() { } };\n"
                                      "void consume( move Owned o ) { }\n";

std::size_t count( const std::string& text, std::string_view needle )
{
    std::size_t found = 0;

    for( std::size_t at = text.find( needle ); at != std::string::npos; at = text.find( needle, at + 1 ) )
    {
        ++found;
    }

    return found;
}

} // namespace

// A function that moves nothing is not rewritten at all.
TEST_CASE( "drop_flags_leaves_a_function_that_moves_nothing_alone", "[check][drop]" )
{
    Elaborated p( std::string( k_owning ) + "i32 main() { { Owned o = Owned( 1 ); } return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( p.functions.size() - 1 );

    INFO( text );
    REQUIRE( text.find( "drop _1" ) != std::string::npos );
    REQUIRE( text.find( " if _" ) == std::string::npos ); // unconditional
    REQUIRE( text.find( "bool" ) == std::string::npos );  // and no flag was added
}

// Each field of an `out` class without a destructor fills under its own flag, so writing `p.b`
// replaces nothing `p.a`'s write left.
TEST_CASE( "drop_flags_gives_each_field_of_an_out_parameter_its_own_flag", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "class P { public Owned a; public Owned b; };\n"
                                  "void fill( out P p ) { p.a = Owned( 1 ); p.b = Owned( 2 ); }\n"
                                  "i32 main() { P q; fill( out q ); return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "fill" );

    INFO( text );
    const std::size_t a = text.find( "drop (*_1).a if _" );
    const std::size_t b = text.find( "drop (*_1).b if _" );
    REQUIRE( a != std::string::npos );
    REQUIRE( b != std::string::npos );
    REQUIRE( text.substr( a + 17, 2 ) != text.substr( b + 17, 2 ) );
}

// On the first statement's line, not the flagged local's or the signature's: a debugger stops at
// the first line of code after the prologue.
TEST_CASE( "drop_flags_sets_flags_on_entry_at_the_first_statement_s_line", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "i32 run( i32 c ) {\n"
                                  "  i32 k = c;\n"
                                  "  Owned o = Owned( 1 );\n"
                                  "  if ( k == 0 ) { consume( move o ); }\n"
                                  "  return 0; }\n"
                                  "i32 main() { return run( 0 ); }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Function& run   = p.functions[p.functions.size() - 2];
    const Block&    entry = run.blocks.front();

    INFO( p.text( p.functions.size() - 2 ) );
    REQUIRE( entry.statement_count > 0 );
    REQUIRE( p.sm.line_col( p.file, run.statements[entry.first_statement].span.start ).line == 4 );
}

TEST_CASE( "drop_flags_guards_a_conditionally_moved_local", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "i32 run( i32 c ) {\n"
                                  "  Owned o = Owned( 1 );\n"
                                  "  if ( c == 0 ) { consume( move o ); }\n"
                                  "  return 0; }\n"
                                  "i32 main() { return run( 0 ); }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( p.functions.size() - 2 ); // run

    INFO( text );

    // _1 is the parameter, so the owning local is _2.
    SECTION( "the drop is conditional" )
    {
        REQUIRE( text.find( "drop _2 if _" ) != std::string::npos );
    }

    // Counted by drop, since the `c == 0` comparison's temporary is a bool too.
    SECTION( "and there is exactly one of them" )
    {
        REQUIRE( count( text, "drop " ) == 1 );
        REQUIRE( count( text, " if _" ) == 1 );
    }

    // False on entry, true once constructed (its Initialise address), false once moved.
    SECTION( "the flag is cleared, set and cleared again" )
    {
        REQUIRE( count( text, "= const 0" ) >= 2 );
        REQUIRE( count( text, "= const 1" ) >= 1 );
    }
}

// A rebuilt statement vector with a mis-fixed block range is silent everywhere except here.
TEST_CASE( "drop_flags_leaves_every_function_verifiable", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "i32 run( i32 c ) {\n"
                                  "  Owned o = Owned( 1 );\n"
                                  "  i32 i = 0;\n"
                                  "  while ( i < 3 ) { if ( c == 0 ) { consume( move o ); break; } i = i + 1; }\n"
                                  "  return 0; }\n"
                                  "i32 main() { return run( 1 ); }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    for( const Function& function : p.functions )
    {
        INFO( print( function, p.ast, p.types.table(), p.literals, p.interner ) );
        REQUIRE( verify( function ).empty() );
    }
}

// Assignment replaces a value, so the old one is destroyed first.
TEST_CASE( "drop_flags_drops_a_local_before_reassigning_it", "[check][drop]" )
{
    Elaborated p( std::string( k_owning ) + "i32 main() { Owned o = Owned( 1 ); o = Owned( 2 ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "main" );

    INFO( text );
    REQUIRE( count( text, "drop _1" ) == 2 ); // the old value, then the scope's
    REQUIRE( text.find( "drop _1" ) < text.find( "_1 = move" ) );
}

// Declared without a value, so the first assignment has nothing to destroy, and the drop before it
// must not run.
TEST_CASE( "drop_flags_guards_the_drop_before_a_first_assignment", "[check][drop]" )
{
    Elaborated p( std::string( k_owning ) + "i32 main() { Owned o; o = Owned( 1 ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "main" );

    INFO( text );
    REQUIRE( count( text, "drop _1" ) == 2 );
    REQUIRE( count( text, "drop _1 if _" ) == 2 );
}

// Assigned on one path only, so the scope's drop is conditional too, though nothing was moved.
TEST_CASE( "drop_flags_guards_a_local_assigned_on_some_paths", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "i32 run( bool c ) { Owned o; if ( c ) { o = Owned( 1 ); } return 0; }\n"
                                  "i32 main() { return run( true ); }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "run" );

    INFO( text );
    REQUIRE( count( text, "drop _2" ) == count( text, "drop _2 if _" ) );
}

// `o = move o` must not destroy the value it is about to read.
TEST_CASE( "drop_flags_reads_a_self_move_before_dropping", "[check][drop]" )
{
    Elaborated p( std::string( k_owning ) + "i32 main() { Owned o = Owned( 1 ); o = move o; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "main" );

    INFO( text );
    REQUIRE( text.find( "_1 = move _1" ) == std::string::npos );
    REQUIRE( text.find( "= move _1" ) < text.find( "drop _1" ) );
}

// A field outside a constructor and a `ref` parameter's referent both hold a value already.
TEST_CASE( "drop_flags_drops_a_projection_before_reassigning_it", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "class Holder { Owned b; Holder() { b = Owned( 1 ); } void reset() { b = Owned( 2 ); } };\n"
                                  "void put( ref Owned o ) { o = Owned( 3 ); }\n"
                                  "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string reset = p.text_of( "reset" );
    const std::string put   = p.text_of( "put" );

    INFO( reset );
    INFO( put );
    REQUIRE( reset.find( "drop (*_1).b" ) < reset.find( "(*_1).b = move" ) );
    REQUIRE( put.find( "drop (*_1)" ) < put.find( "(*_1) = move" ) );
}

// An `out` parameter arrives empty: its first write fills it, and only a later one drops.
TEST_CASE( "drop_flags_starts_an_out_parameter_empty", "[check][drop][out]" )
{
    Elaborated p(
        std::string( k_owning ) + "void fill( out Owned o ) { o = Owned( 1 ); o = Owned( 2 ); }\n"
                                  "i32 main() { Owned o; fill( out o ); return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "fill" );

    INFO( text );
    REQUIRE( count( text, "drop (*_1) if _" ) == 2 );

    // The flag's first write is the entry's, and it is false.
    const std::size_t guard = text.find( "drop (*_1) if " ) + std::string_view( "drop (*_1) if " ).size();
    const std::string flag  = text.substr( guard, text.find( '\n', guard ) - guard );

    REQUIRE( text.find( flag + " = const 0" ) < text.find( flag + " = const 1" ) );
}

// A constructor's first write to a field is what gives it a value, so there is nothing to destroy.
TEST_CASE( "drop_flags_does_not_drop_a_field_a_constructor_initialises", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "class Holder { Owned b; Holder() { b = Owned( 1 ); } };\n"
                                  "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "Holder" );

    INFO( text );
    REQUIRE( text.find( "drop (*_1)" ) == std::string::npos );
}

// An arm's temporary has no storage_live on the other arm's path, and the join tests both.
TEST_CASE( "drop_flags_clears_every_flag_on_entry", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "i32 run( bool c ) { Owned o = c ? Owned( 1 ) : Owned( 2 ); return 0; }\n"
                                  "i32 main() { return run( true ); }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text  = p.text( p.functions.size() - 2 ); // run
    const std::string entry = text.substr( text.find( "bb0:" ), text.find( "bb1:" ) - text.find( "bb0:" ) );

    INFO( text );
    REQUIRE( count( text, " if _" ) == 3 );

    for( std::size_t at = text.find( " if _" ); at != std::string::npos; at = text.find( " if _", at + 1 ) )
    {
        const std::string flag = text.substr( at + 4, text.find( '\n', at ) - at - 4 );

        INFO( flag );
        REQUIRE( entry.find( flag + " = const 0" ) != std::string::npos );
    }
}

// A `move` parameter moved on only some paths is still dropped on the others, so its flag starts
// true.
TEST_CASE( "drop_flags_sets_a_parameter's_flag_on_entry", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "void maybe( bool c, move Owned o ) { if ( c ) { consume( move o ); } }\n"
                                  "i32 main() { maybe( true, move Owned( 1 ) ); return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "maybe" );
    const std::size_t at   = text.find( "drop _2 if _" );

    INFO( text );
    REQUIRE( at != std::string::npos );

    const std::string flag  = text.substr( at + 11, text.find( '\n', at ) - at - 11 );
    const std::string entry = text.substr( text.find( "bb0:" ), text.find( "bb1:" ) - text.find( "bb0:" ) );

    INFO( flag );
    REQUIRE( entry.find( flag + " = const 1" ) != std::string::npos );
}

// Only construction sets the flag, so a borrow after a move does not revive the value.
TEST_CASE( "drop_flags_is_not_set_by_a_borrow", "[check][drop]" )
{
    Elaborated p(
        std::string( k_owning ) + "u64 peek( Owned o ) { return 0; }\n"
                                  "i32 run( i32 c ) {\n"
                                  "  Owned o = Owned( 1 );\n"
                                  "  if ( c == 0 ) { consume( move o ); } else { u64 n = peek( o ); }\n"
                                  "  return 0; }\n"
                                  "i32 main() { return run( 0 ); }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( p.functions.size() - 2 ); // run
    const std::size_t at   = text.find( "drop _2 if _" );

    INFO( text );
    REQUIRE( at != std::string::npos );

    const std::string flag = text.substr( at + 10, text.find( '\n', at ) - at - 10 );

    INFO( flag );
    REQUIRE( count( text, flag + " = const 1" ) == 1 );
}

// A class whose field a temporary can be read through, so nothing moves the temporary.
constexpr std::string_view k_tagged = "class Tagged { public u64 id; Tagged( u64 n ) { id = n; } ~Tagged() { } };\n";

// Built in one arm, or on the right of `&&`, so the end of the statement drops it only if it was.
TEST_CASE( "drop_flags_guards_a_temporary_built_on_one_path", "[check][drop]" )
{
    Elaborated p(
        std::string( k_tagged ) + "u64 arm( bool c ) { return c ? Tagged( 1 ).id : 2; }\n"
                                  "bool right( bool c ) { return c && Tagged( 1 ).id > 0; }\n"
                                  "bool other( bool c ) { return c || Tagged( 1 ).id > 0; }\n"
                                  "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    for( const std::string_view name : { "arm", "right", "other" } )
    {
        const std::string text = p.text_of( name );

        INFO( text );
        REQUIRE( count( text, "drop " ) == 1 );
        REQUIRE( count( text, " if _" ) == 1 );
    }
}

// The condition's temporary is built on every path, so only the arm's is guarded.
TEST_CASE( "drop_flags_leaves_a_temporary_built_on_every_path_alone", "[check][drop]" )
{
    Elaborated p(
        std::string( k_tagged ) + "u64 run( bool c ) { return Tagged( 1 ).id > 0 && c ? Tagged( 2 ).id : 0; }\n"
                                  "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "run" );

    INFO( text );
    REQUIRE( count( text, "drop " ) == 2 );
    REQUIRE( count( text, " if _" ) == 1 );
}

// Its statement ends its storage, so a temporary built on one iteration is not dropped again on the
// next, which skips building it.
TEST_CASE( "drop_flags_clears_a_one_path_temporary's_flag_after_its_drop", "[check][drop]" )
{
    Elaborated p(
        std::string( k_tagged ) +
        "u64 run( u64 n ) {\n"
        "  u64 all = 0;\n"
        "  for ( u64 i = 0; i < n; i++ ) { u64 got = i % 2 == 0 ? Tagged( i ).id : 0; all = all + got; }\n"
        "  return all; }\n"
        "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text_of( "run" );
    const std::size_t at   = text.find( " if _" );

    INFO( text );
    REQUIRE( at != std::string::npos );

    const std::string flag = text.substr( at + 4, text.find( '\n', at ) - at - 4 );

    INFO( flag );
    REQUIRE( text.find( flag + " = const 0", at ) != std::string::npos );
}

} // namespace keel
#endif
