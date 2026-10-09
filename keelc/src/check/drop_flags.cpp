// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/drop_flags.h"
#include <vector>

namespace keel
{

namespace
{

// Indexed by local; each entry is that local's flag, or invalid when it needs none.
using Flag_map = std::vector<Local_id>;

// The flag for a place, or invalid: only a whole local can be moved.
Local_id flag_of( const Flag_map& flags, const Place& place )
{
    if( place.is_global() )
    {
        return Local_id {};
    }

    return flags[place.local.v];
}

// Locals both moved and dropped somewhere, and temporaries built on one path of an expression. A
// flag guards a drop, so a moved local never dropped needs none, and one never moved is always live
// at its drop.
std::vector<bool> locals_needing_flags( const Function& func )
{
    std::vector<bool> moved( func.locals.size(), false );

    const auto note = [&]( const Operand& operand )
    {
        if( operand.kind == Operand_kind::Move && !operand.place.is_global() && operand.place.num_projections == 0 )
        {
            moved[operand.place.local.v] = true;
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
    }

    // Intersected with the drops, which is what turns "was moved" into "needs a flag".
    std::vector<bool> dropped( func.locals.size(), false );
    std::vector<bool> replaced( func.locals.size(), false );

    for( const Statement& statement : func.statements )
    {
        // Projections count: a compound without a destructor drops field by field, as `drop _8.inner`.
        if( statement.kind == Statement_kind::Drop && !statement.place.is_global() )
        {
            dropped[statement.place.local.v] = true;

            if( statement.replacing && !has_deref_projection( func, statement.place ) )
            {
                replaced[statement.place.local.v] = true;
            }
        }
    }

    for( const Local_id local : func.one_path_temporaries )
    {
        moved[local.v] = true;
    }

    for( std::size_t i = 0; i < moved.size(); ++i )
    {
        moved[i] = ( moved[i] && dropped[i] ) || replaced[i];
    }

    return moved;
}

// Appends an unnamed bool local per flagged local, so a dump reads `drop _1 if _7`.
Flag_map allocate_flags( Function& func, const std::vector<bool>& moved, const Flag_vocabulary& vocabulary )
{
    Flag_map flags( func.locals.size(), Local_id {} );

    for( u32 local = 0; local < moved.size(); ++local )
    {
        if( !moved[local] )
        {
            continue;
        }

        // Read before the push_back, which may reallocate. The flag borrows the local's span.
        const Span span = func.locals[local].span;

        flags[local] = Local_id { narrow_cast<u32>( func.locals.size() ) };
        func.locals.push_back( Local { .type = vocabulary.bool_type, .span = span } );
    }

    // Grown to the final local count, so flag_of answers any place, a flag included.
    flags.resize( func.locals.size(), Local_id {} );

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
        if( const Local_id flag = flag_of( flags, statement.place ); flag.is_valid() )
        {
            out.push_back( set_flag( flag, false, statement.span, vocabulary ) );
        }
        return;
    case Statement_kind::Assign:
        // The move first, then the assignment, so `_1 = move _1` ends with the flag set. Driven off the
        // operands, so linear in the statement rather than in the locals.
        for_each_operand(
            func,
            statement.value,
            [&]( const Operand& operand )
            {
                if( operand.kind != Operand_kind::Move )
                {
                    return;
                }

                if( const Local_id flag = flag_of( flags, operand.place ); flag.is_valid() )
                {
                    out.push_back( set_flag( flag, false, statement.span, vocabulary ) );
                }
            }
        );
        if( const Local_id flag = flag_of( flags, statement.place ); flag.is_valid() )
        {
            out.push_back( set_flag( flag, true, statement.span, vocabulary ) );
        }

        // A constructed local is written through its Initialise address (`_3 = &_2`), never assigned, so
        // that address sets its flag. A borrow does not, or one after a move would revive it.
        if( statement.value.kind == Rvalue_kind::Address_of && statement.value.address_purpose == Address_purpose::Initialise )
        {
            if( const Local_id flag = flag_of( flags, statement.value.a.place ); flag.is_valid() )
            {
                out.push_back( set_flag( flag, true, statement.span, vocabulary ) );
            }
        }

        return;
    case Statement_kind::Drop:
        return;
    }
}

// The rebuild: copies every block's statements, interleaves flag writes, sets drop_flag on drops,
// and fixes each block's range as it goes.
void rewrite_statements( Function& func, const Flag_map& flags, const Flag_vocabulary& vocabulary )
{
    std::vector<Statement> rebuilt;
    rebuilt.reserve( func.statements.size() * 2 ); // a guess
    for( Block& block : func.blocks )
    {
        // Read the old range before overwriting it; the loop below indexes the original vector.
        const u32 old_first = block.first_statement;
        const u32 count     = block.statement_count;
        const u32 new_first = narrow_cast<u32>( rebuilt.size() );

        // Set every flag on entry: a parameter arrives holding its value, and anything else starts empty,
        // since an arm's temporary has no storage_live to clear it on the path that skips it.
        if( &block == &func.blocks.front() )
        {
            for( u32 local = 0; local < flags.size(); ++local )
            {
                const Local_id flag = flags[local];
                if( !flag.is_valid() )
                {
                    continue;
                }

                rebuilt.push_back(
                    set_flag( flag, local >= 1 && local <= func.parameter_count, func.locals[local].span, vocabulary )
                );
            }
        }

        for( u32 i = 0; i < count; ++i )
        {
            Statement statement = func.statements[old_first + i];

            // A drop reads its flag rather than being followed by a write.
            if( statement.kind == Statement_kind::Drop )
            {
                statement.drop_flag = flag_of( flags, statement.place );
            }

            rebuilt.push_back( statement );
            append_flag_writes( func, statement, flags, vocabulary, rebuilt );
        }

        // A moving condition clears its flag before the terminator reads it.
        if( block.terminator.condition.kind == Operand_kind::Move )
        {
            if( const Local_id flag = flag_of( flags, block.terminator.condition.place ); flag.is_valid() )
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
    const std::vector<bool> moved = locals_needing_flags( func );

    // Nothing flagged: the function is left untouched.
    if( std::find( moved.begin(), moved.end(), true ) == moved.end() )
    {
        return;
    }

    const Flag_map flags = allocate_flags( func, moved, vocabulary );

    rewrite_statements( func, flags, vocabulary );
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
        : Compiled( source, Through::elaborate )
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
