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

// The flag for a place, or invalid. A global or a projection has none: nothing can be moved but a
// whole local, so nothing else needs tracking.
Local_id flag_of( const Flag_map& flags, const Place& place )
{
    if( place.is_global() )
    {
        return Local_id {};
    }

    return flags[place.local.v];
}

// Locals that are both moved somewhere and dropped somewhere. Moved alone is not enough: a flag
// exists to guard a drop, so a local with none - every non-owning `move a` - would get a bool and
// three assignments guarding nothing. Dropped alone is not enough either: a local never moved is
// always live at its drop, and an unconditional drop is what it should keep.
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

    // A condition can move too - `if ( move ready )` is legal - and its flag write lands at the end
    // of the block's statements, which is before the terminator reads it.
    for( const Block& block : func.blocks )
    {
        note( block.terminator.condition );
    }

    // Intersected with the drops, which is what turns "was moved" into "needs a flag".
    std::vector<bool> dropped( func.locals.size(), false );
    std::vector<bool> replaced( func.locals.size(), false );

    for( const Statement& statement : func.statements )
    {
        // Projections count: a compound with no destructor of its own has its drop expanded into
        // per-field drops, so `drop _8.inner` is how `_8` is dropped. Requiring an unprojected drop
        // here left such a local unflagged, and moving it out then freed the field twice.
        if( statement.kind == Statement_kind::Drop && !statement.place.is_global() )
        {
            dropped[statement.place.local.v] = true;

            if( statement.replacing && !has_deref_projection( func, statement.place ) )
            {
                replaced[statement.place.local.v] = true;
            }
        }
    }

    for( std::size_t i = 0; i < moved.size(); ++i )
    {
        moved[i] = ( moved[i] && dropped[i] ) || replaced[i];
    }

    return moved;
}

// Appends a bool local per moved local and returns the map. The flags are unnamed - the pass has no
// Interner to name them with, and a flag is not something the author wrote - so a dump reads them as
// plain temporaries: `drop _1 if _7`.
Flag_map allocate_flags( Function& func, const std::vector<bool>& moved, const Flag_vocabulary& vocabulary )
{
    Flag_map flags( func.locals.size(), Local_id {} );

    for( u32 local = 0; local < moved.size(); ++local )
    {
        if( !moved[local] )
        {
            continue;
        }

        // Read out before the push_back, which may reallocate the vector this refers into. The flag
        // borrows the local's span so a diagnostic about it points somewhere the author recognises.
        const Span span = func.locals[local].span;

        flags[local] = Local_id { narrow_cast<u32>( func.locals.size() ) };
        func.locals.push_back( Local { .type = vocabulary.bool_type, .span = span } );
    }

    // Grown to the final local count, so flag_of can be asked about any place at all - including
    // one naming a flag, which correctly answers that it needs none of its own.
    flags.resize( func.locals.size(), Local_id {} );

    return flags;
}

// `flag = true` or `flag = false`, as an ordinary Assign of a bool constant - so nothing downstream
// needs a new statement kind.
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
        // The move first, then the assignment: that is the order the statement happens in, and it
        // is what makes `_1 = move _1` end with the flag set rather than cleared.
        //
        // Driven off the operands rather than by testing every flagged local, so this stays linear
        // in the statement's size instead of in the function's locals.
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

        // Taking a local's address counts as initialising it, because a constructed local is never
        // assigned to: `Owned o = Owned( 1 );` lowers to `_3 = &_2` and a call that writes through
        // _3, so the local is never an Assign target and its flag would otherwise stay false and
        // its drop never run. The looseness is deliberate and bounded - `Owned o; Owned* p = &o;`
        // marks an uninitialised local as needing a drop, which is exactly what happens today
        // without flags, so this is no worse than the status quo for a program D9 will reject
        // anyway.
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
        // Read the old range out *before* writing the new one: the loop below indexes the original
        // vector, and overwriting first_statement first would make it index the wrong statements.
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

            // A drop reads its flag rather than being followed by one, so this is set on the way
            // past rather than appended after.
            if( statement.kind == Statement_kind::Drop )
            {
                statement.drop_flag = flag_of( flags, statement.place );
            }

            rebuilt.push_back( statement );
            append_flag_writes( func, statement, flags, vocabulary, rebuilt );
        }

        // A condition can move - `if ( move ready )` is legal - and its flag write belongs at the
        // end of the block's statements, which is before the terminator reads it. Without this the
        // pass would notice such a move in find_moved_locals and then silently never clear it.
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

    // Nothing moved means nothing conditional: every drop stays unconditional, the statement vector
    // is untouched, and no golden that does not use `move` can shift.
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

#include "common/diagnostics.h"
#include "common/source_manager.h"
#include "ir/lower.h"
#include "ir/print.h"
#include "ir/simplify.h"
#include "ir/verify.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{
namespace
{

struct Elaborated
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    Ast            ast;
    Resolution     resolution;
    Types          types;

    std::vector<Function> functions;

    explicit Elaborated( std::string_view source )
    {
        const File_id file = sm.add_file( "t.kl", std::string( source ) );

        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
        resolution = resolve( ast, sm, interner, diags );
        types      = type_check( ast, resolution, literals, sm, interner, diags );

        if( diags.has_errors() )
        {
            return;
        }

        functions = lower( ast, resolution, types, literals );

        // The driver simplifies every function before anything reads it, so these do too.
        for( Function& function : functions )
        {
            simplify( function, literals );
        }

        const Flag_vocabulary vocabulary {
            .bool_type     = types.table().builtin( Type_kind::Bool ),
            .false_literal = literals.add_integer( 0 ),
            .true_literal  = literals.add_integer( 1 )
        };

        for( Function& function : functions )
        {
            elaborate_drops( function, vocabulary );
        }
    }

    bool clean() const
    {
        return !diags.has_errors();
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags.render( sm, out );
        return out.str();
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

// The early return in elaborate_drops is what makes this true by construction rather than by luck:
// a function that moves nothing is not rewritten at all.
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

    // One drop, one flag on it. Counting bool locals instead would also catch the `c == 0`
    // comparison's temporary, which is not a flag.
    SECTION( "and there is exactly one of them" )
    {
        REQUIRE( count( text, "drop " ) == 1 );
        REQUIRE( count( text, " if _" ) == 1 );
    }

    // False on entry, true once constructed, false again once moved. A constructed local is never
    // an Assign target - the constructor writes through a pointer - so the true comes from the
    // address being taken, which is the one place that says "this local now holds something".
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
                                  "  while ( i < 3 ) { if ( c == 0 ) { consume( move o ); } i = i + 1; }\n"
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

// A temporary built in one arm of a conditional has no storage_live, so nothing on the other arm's
// path would clear its flag - and the join tests both.
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

// A parameter arrives holding its value, so a `move` parameter moved on only some paths is still
// dropped on the others. Its flag once started false like a local's, and that path leaked.
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

// Only construction sets the flag. A borrow once set it too, so one after a move destroyed the
// value a second time.
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

} // namespace keel
#endif
