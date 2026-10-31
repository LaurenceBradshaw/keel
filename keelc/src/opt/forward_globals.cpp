// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/forward_globals.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>
#include "ir/live.h"
#include "ir/simplify.h"
#include "opt/fold_constants.h"

namespace keel
{

namespace
{

// `x op const c` as written, `op` `+` or `-`: a sum, or the check of one.
struct Def
{
    Rvalue value;
    bool   current = true;  // `value.a` not written since
    bool   exact   = false; // a sum a branch proved does not overflow
};

struct Known
{
    std::vector<std::pair<Place, Operand>> globals; // a whole global, and the constant or local holding it
    std::vector<std::pair<Local_id, Def>>  defs;    // few locals hold a sum, so a vector
    std::vector<Def>                       checked; // sums a branch's no-overflow arm proved
};

bool whole_global( const Place& place )
{
    return place.is_global() && place.num_projections == 0;
}

// A `_t` added this round is past `addressed`'s end, and never addressed.
bool open( const Local_set& addressed, Local_id local )
{
    return local.v >= addressed.size() || !addressed[local.v];
}

// The same local, or the same global, whatever the projections.
bool rooted_at( const Place& place, const Place& root )
{
    return root.is_global() ? place.is_global() && place.global == root.global && place.owner == root.owner
                            : !place.is_global() && place.local.is_valid() && place.local == root.local;
}

bool same( const Operand& a, const Operand& b )
{
    if( a.kind != b.kind )
    {
        return false;
    }

    if( a.kind == Operand_kind::Constant )
    {
        return a.constant.is_valid() && a.constant == b.constant;
    }

    return a.kind == Operand_kind::Copy && a.place.num_projections == 0 && b.place.num_projections == 0 &&
           rooted_at( a.place, b.place );
}

bool same_constant( const Operand& a, const Operand& b, const Literal_pool& literals )
{
    return a.kind == Operand_kind::Constant && b.kind == Operand_kind::Constant && a.constant.is_valid() &&
           b.constant.is_valid() && a.type == b.type && literals.integer( a.constant ) == literals.integer( b.constant );
}

bool same_global( const Place& a, const Place& b )
{
    return a.is_global() && b.is_global() && a.num_projections == b.num_projections && a.num_projections == 0 &&
           a.global == b.global && a.owner == b.owner;
}

bool barrier( const Function& func, const Statement& statement )
{
    if( statement.kind == Statement_kind::Assign )
    {
        if( statement.value.kind == Rvalue_kind::Call || statement.value.kind == Rvalue_kind::Indirect_call ||
            statement.value.kind == Rvalue_kind::Allocate || statement.value.kind == Rvalue_kind::Release ||
            statement.value.kind == Rvalue_kind::Field_read || statement.value.kind == Rvalue_kind::Run_data ||
            statement.value.kind == Rvalue_kind::Run_size )
        {
            return true;
        }
    }

    if( statement.kind == Statement_kind::Drop )
    {
        return true;
    }

    if( has_deref_projection( func, statement.place ) )
    {
        return true;
    }

    bool deref_operand = false;
    for_each_operand(
        func,
        statement.value,
        [&]( const Operand& operand ) { deref_operand = deref_operand || has_deref_projection( func, operand.place ); }
    );

    return deref_operand;
}

// `root`, a local or a global, changed: every fact that names it goes.
void written( Known& known, const Place& root )
{
    std::erase_if(
        known.globals,
        [&]( const std::pair<Place, Operand>& pair ) {
            return rooted_at( pair.first, root ) ||
                   ( pair.second.kind == Operand_kind::Copy && rooted_at( pair.second.place, root ) );
        }
    );

    std::erase_if(
        known.defs, [&]( const std::pair<Local_id, Def>& def ) { return !root.is_global() && def.first == root.local; }
    );

    for( std::pair<Local_id, Def>& def : known.defs )
    {
        if( rooted_at( def.second.value.a.place, root ) )
        {
            def.second.current = false;
        }
    }

    std::erase_if( known.checked, [&]( const Def& check ) { return rooted_at( check.value.a.place, root ); } );
}

void forget( Known& known, const Function& func, const Statement& statement )
{
    if( barrier( func, statement ) )
    {
        known.globals.clear();
        for( std::pair<Local_id, Def>& def : known.defs )
        {
            if( def.second.value.a.place.is_global() )
            {
                def.second.current = false;
            }
        }
        std::erase_if( known.checked, []( const Def& check ) { return check.value.a.place.is_global(); } );
    }

    // A write through a pointer reaches only a global or an addressed local, and is a barrier.
    if( !has_deref_projection( func, statement.place ) && ( statement.place.is_global() || statement.place.local.is_valid() ) )
    {
        written( known, statement.place );
    }

    // `addressed_locals` leaves out `&init`, but the call it feeds writes the local.
    if( statement.kind == Statement_kind::Assign && statement.value.kind == Rvalue_kind::Address_of &&
        statement.value.address_purpose == Address_purpose::Initialise )
    {
        written( known, statement.value.a.place );
    }
}

// `x op const c`, `op` `+` or `-` of an integer, with `x` a whole local no pointer reaches or a whole global.
bool sum( const Rvalue& value, Rvalue_kind kind, const Type_table& types, const Local_set& addressed )
{
    if( value.kind != kind || ( value.op != Token_kind::Plus && value.op != Token_kind::Minus ) ||
        types.get( value.a.type ).kind != Type_kind::Int || value.a.kind != Operand_kind::Copy ||
        value.b.kind != Operand_kind::Constant || !value.b.constant.is_valid() )
    {
        return false;
    }

    const Place& x = value.a.place;
    return whole_global( x ) || ( x.is_whole_local() && open( addressed, x.local ) );
}

// Whether `check` proves `value`, a sum or its check, cannot overflow.
bool proves( const Def& check, const Rvalue& value, const Literal_pool& literals )
{
    return check.value.op == value.op && same( check.value.a, value.a ) && same_constant( check.value.b, value.b, literals );
}

const Operand* held( const Known& known, const Place& global )
{
    for( const std::pair<Place, Operand>& pair : known.globals )
    {
        if( same_global( pair.first, global ) )
        {
            return &pair.second;
        }
    }

    return nullptr;
}

const Def* def_of( const Known& known, const Operand& operand )
{
    if( operand.kind != Operand_kind::Copy || !operand.place.is_whole_local() )
    {
        return nullptr;
    }

    for( const std::pair<Local_id, Def>& def : known.defs )
    {
        if( def.first == operand.place.local )
        {
            return &def.second;
        }
    }

    return nullptr;
}

void learn(
    Known&              known,
    const Function&     func,
    const Type_table&   types,
    const Literal_pool& literals,
    const Local_set&    addressed,
    const Statement&    statement
)
{
    if( statement.kind != Statement_kind::Assign )
    {
        return;
    }

    const Place&   place = statement.place;
    const Rvalue&  value = statement.value;
    const Operand& from  = value.a;

    if( whole_global( place ) )
    {
        if( value.kind == Rvalue_kind::Use && types.is_builtin( from.type ) &&
            ( ( from.kind == Operand_kind::Constant && from.constant.is_valid() ) ||
              ( from.kind == Operand_kind::Copy && from.place.is_whole_local() && open( addressed, from.place.local ) ) ) )
        {
            known.globals.emplace_back( place, from );
        }
        return;
    }

    if( !place.is_whole_local() || !open( addressed, place.local ) )
    {
        return;
    }

    const Type_id to = func.locals[place.local].type;

    if( value.kind == Rvalue_kind::Use && from.kind == Operand_kind::Copy && whole_global( from.place ) &&
        types.is_builtin( from.type ) && from.type == to && !held( known, from.place ) )
    {
        known.globals.emplace_back( from.place, copy( place, to ) );
    }

    const bool is_sum = sum( value, Rvalue_kind::Binary, types, addressed ) && value.type == to;
    if( ( is_sum || sum( value, Rvalue_kind::Overflows, types, addressed ) ) && !rooted_at( from.place, place ) )
    {
        Def def { .value = value };
        def.exact = is_sum &&
                    std::ranges::any_of( known.checked, [&]( const Def& check ) { return proves( check, value, literals ); } );
        known.defs.emplace_back( place.local, std::move( def ) );
    }
}

// A read of a whole global with a fact reads the fact instead.
bool rewrite( Operand& operand, const Known& known )
{
    if( operand.kind != Operand_kind::Copy || !whole_global( operand.place ) )
    {
        return false;
    }

    const Operand* fact = held( known, operand.place );
    if( !fact || fact->type != operand.type )
    {
        return false;
    }

    operand =
        fact->kind == Operand_kind::Constant ? constant( fact->constant, operand.type ) : copy( fact->place, operand.type );
    return true;
}

// What `value` reduces to by what the walk knows: a chain of sums combined, or a check another proves.
std::optional<Rvalue> fold( const Rvalue& value, const Known& known, const Type_table& types, Literal_pool& literals )
{
    const Def* inner = def_of( known, value.a );

    if( value.kind == Rvalue_kind::Binary )
    {
        return inner && inner->current ? combined( value, inner->value, types, literals ) : std::nullopt;
    }

    if( value.kind != Rvalue_kind::Overflows )
    {
        return std::nullopt;
    }

    // `_t - c` with `_t = x + c` that cannot overflow is `x`, whatever `x` holds since.
    const Token_kind inverse = value.op == Token_kind::Plus    ? Token_kind::Minus
                               : value.op == Token_kind::Minus ? Token_kind::Plus
                                                               : Token_kind::Unknown;
    const bool       undone =
        inner && inner->exact && inner->value.op == inverse && same_constant( inner->value.b, value.b, literals );

    if( undone || std::ranges::any_of( known.checked, [&]( const Def& check ) { return proves( check, value, literals ); } ) )
    {
        return value_of( 0, value.type, types, literals );
    }

    return std::nullopt;
}

// Whether a statement after `from` reads `global` before a barrier or its next write, in this block or
// one the walk carries what it knows into.
bool read_later( const Function& func, const std::vector<u32>& counts, Block_id block, u32 from, const Place& global )
{
    const auto reads = [&]( const Operand& operand )
    { return operand.kind == Operand_kind::Copy && same_global( operand.place, global ); };

    std::vector<std::pair<Block_id, u32>> work { { block, from + 1 } };
    std::vector<bool>                     entered( func.blocks.size(), false );
    std::vector<Block_id>                 targets;

    while( !work.empty() )
    {
        const auto [id, start] = work.back();
        work.pop_back();

        const Block& b       = func.blocks[id];
        bool         stopped = false;
        for( u32 s = start; s < b.statement_count && !stopped; ++s )
        {
            const Statement& statement = func.statements_of( b )[s];

            bool read = false;
            if( statement.kind == Statement_kind::Assign )
            {
                for_each_operand( func, statement.value, [&]( const Operand& operand ) { read = read || reads( operand ); } );
            }
            if( read )
            {
                return true;
            }

            stopped = barrier( func, statement ) ||
                      ( statement.kind == Statement_kind::Assign && rooted_at( statement.place, global ) );
        }

        if( stopped )
        {
            continue;
        }

        if( reads( b.terminator.condition ) || reads( b.terminator.message ) || reads( b.terminator.at ) )
        {
            return true;
        }

        targets.clear();
        successors( b.terminator, targets );
        for( const Block_id target : targets )
        {
            if( counts[target.v] == 1 && !entered[target.v] )
            {
                entered[target.v] = true;
                work.emplace_back( target, 0 );
            }
        }
    }

    return false;
}

bool forward_round( Function& func, const Type_table& types, Literal_pool& literals, const Local_set& addressed )
{
    const std::vector<u32>                  counts = predecessor_counts( func );
    std::vector<std::vector<Statement>>     rebuilt( func.blocks.size() );
    std::vector<bool>                       walked( func.blocks.size(), false );
    std::vector<std::pair<Block_id, Known>> work;
    std::vector<Block_id>                   targets;
    bool                                    changed = false;

    const auto settle = [&]( Known& known, std::vector<Statement>& out, const Statement& statement )
    {
        forget( known, func, statement );
        learn( known, func, types, literals, addressed, statement );
        out.push_back( statement );
    };

    // A worklist, not recursion: a long chain of asserts is a long chain of one-predecessor blocks.
    const auto drain = [&]()
    {
        while( !work.empty() )
        {
            auto [id, known] = std::move( work.back() );
            work.pop_back();
            if( walked[id.v] )
            {
                continue;
            }
            walked[id.v] = true;

            const Block             block = func.blocks[id];
            std::vector<Statement>& out   = rebuilt[id.v];

            for( u32 s = 0; s < block.statement_count; ++s )
            {
                Statement statement = func.statements_of( block )[s];

                if( statement.kind != Statement_kind::Assign )
                {
                    settle( known, out, statement );
                    continue;
                }

                // Reads first, so `live = copy live + const 1` reads the old `live`.
                Rvalue& value = statement.value;
                changed |= rewrite( value.a, known );
                changed |= rewrite( value.b, known );
                for( u32 i = 0; i < value.argument_count; ++i )
                {
                    changed |= rewrite( func.operands[value.first_argument + i], known );
                }

                if( std::optional<Rvalue> folded = fold( value, known, types, literals ) )
                {
                    value   = std::move( *folded );
                    changed = true;
                }

                const Place& place = statement.place;
                if( whole_global( place ) && value.kind == Rvalue_kind::Use )
                {
                    const Operand* fact = held( known, place );
                    if( ( value.a.kind == Operand_kind::Copy && same_global( value.a.place, place ) ) ||
                        ( fact && same( *fact, value.a ) ) )
                    {
                        changed = true;
                        continue;
                    }
                }

                // A read follows, so it can read `_t`; otherwise the write stays one statement.
                if( whole_global( place ) && value.kind != Rvalue_kind::Use && types.is_builtin( value.type ) &&
                    read_later( func, counts, id, s, place ) )
                {
                    const Local_id temporary { narrow_cast<u32>( func.locals.size() ) };
                    func.locals.push_back( Local { .type = value.type, .span = statement.span } );

                    Statement computed = statement;
                    computed.place     = Place { .local = temporary };
                    statement.value    = use( copy( computed.place, computed.value.type ) );

                    settle( known, out, computed );
                    changed = true;
                }

                settle( known, out, statement );
            }

            Terminator& terminator = func.blocks[id].terminator;
            changed |= rewrite( terminator.condition, known );
            changed |= rewrite( terminator.message, known );
            changed |= rewrite( terminator.at, known );

            targets.clear();
            successors( terminator, targets );
            for( u32 t = 0; t < targets.size(); ++t )
            {
                if( counts[targets[t].v] != 1 || walked[targets[t].v] )
                {
                    continue;
                }

                // Past a check's false arm, its sum does not overflow.
                Known next = known;
                if( terminator.kind == Terminator_kind::Branch && t == 1 )
                {
                    const Def* check = def_of( known, terminator.condition );
                    if( check && check->current && check->value.kind == Rvalue_kind::Overflows )
                    {
                        next.checked.push_back( *check );
                    }
                }
                work.emplace_back( targets[t], std::move( next ) );
            }
        }
    };

    // A join, or a block nothing reaches, starts knowing nothing.
    for( u32 pass = 0; pass < 2; ++pass )
    {
        for( const Block_id b : func.blocks.ids() )
        {
            if( !walked[b.v] && ( pass == 1 || b.v == 0 || counts[b.v] != 1 ) )
            {
                work.emplace_back( b, Known {} );
                drain();
            }
        }
    }

    std::vector<Statement> statements;
    statements.reserve( func.statements.size() );
    for( const Block_id b : func.blocks.ids() )
    {
        func.blocks[b].first_statement = narrow_cast<u32>( statements.size() );
        func.blocks[b].statement_count = narrow_cast<u32>( rebuilt[b.v].size() );
        statements.insert( statements.end(), rebuilt[b.v].begin(), rebuilt[b.v].end() );
    }
    func.statements = std::move( statements );

    return changed;
}

// Whether every path from statement `from` of `block` writes the whole `global` before anything reads it.
bool overwritten( const Function& func, const std::vector<u32>& counts, const Place& global, Block_id block, u32 from )
{
    // An address taken is a read: whatever holds it may read it later.
    const auto reads = [&]( const Operand& operand )
    { return operand.place.is_global() && rooted_at( operand.place, global ); };

    std::vector<std::pair<Block_id, u32>> work { { block, from } };
    std::vector<bool>                     entered( func.blocks.size(), false );
    std::vector<Block_id>                 targets;

    while( !work.empty() )
    {
        const auto [id, start] = work.back();
        work.pop_back();

        const Block& b       = func.blocks[id];
        bool         covered = false;
        for( u32 s = start; s < b.statement_count && !covered; ++s )
        {
            const Statement& statement = func.statements_of( b )[s];

            bool read = false;
            if( statement.kind == Statement_kind::Assign )
            {
                for_each_operand( func, statement.value, [&]( const Operand& operand ) { read = read || reads( operand ); } );
            }
            if( read || barrier( func, statement ) )
            {
                return false;
            }

            covered = statement.kind == Statement_kind::Assign && same_global( statement.place, global );
        }

        if( covered )
        {
            continue;
        }

        const Terminator& terminator = b.terminator;
        if( reads( terminator.condition ) || reads( terminator.message ) || reads( terminator.at ) )
        {
            return false;
        }

        switch( terminator.kind )
        {
        // Nothing runs after either.
        case Terminator_kind::Panic:
        case Terminator_kind::Unreachable:
            break;

        case Terminator_kind::Goto:
        case Terminator_kind::Branch:
            targets.clear();
            successors( terminator, targets );
            for( const Block_id target : targets )
            {
                if( counts[target.v] != 1 )
                {
                    return false;
                }
                if( !entered[target.v] )
                {
                    entered[target.v] = true;
                    work.emplace_back( target, 0 );
                }
            }
            break;

        case Terminator_kind::Return:
        case Terminator_kind::Unset:
            return false;
        }
    }

    return true;
}

// Removes each write to a whole global that every path overwrites before reading it.
bool remove_overwritten( Function& func, const Type_table& types )
{
    const std::vector<u32> counts = predecessor_counts( func );
    std::vector<bool>      deleted( func.statements.size(), false );
    bool                   any = false;

    for( const Block_id b : func.blocks.ids() )
    {
        const Block& block = func.blocks[b];
        for( u32 s = 0; s < block.statement_count; ++s )
        {
            const Statement& statement = func.statements_of( block )[s];
            if( statement.kind == Statement_kind::Assign && whole_global( statement.place ) &&
                types.is_builtin( statement.value.type ) && !barrier( func, statement ) &&
                overwritten( func, counts, statement.place, b, s + 1 ) )
            {
                deleted[func.statements_of( block ).position( s )] = true;
                any                                                = true;
            }
        }
    }

    if( any )
    {
        remove_statements( func, deleted );
    }

    return any;
}

} // namespace

// Each round's sweep can leave a sum unchanged for the next round's walk to undo.
bool forward_globals( Function& func, const Type_table& types, Literal_pool& literals )
{
    bool any     = false;
    bool changed = true;
    while( changed )
    {
        const Local_set addressed = addressed_locals( func );
        changed                   = forward_round( func, types, literals, addressed );
        changed |= remove_overwritten( func, types );
        any |= changed;
    }

    return any;
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
struct Forwarded : Compiled
{
    explicit Forwarded( std::string_view source, Build build = Build { .optimise = true } )
        : Compiled( std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, build )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    std::size_t reads_of_globals() const
    {
        std::size_t count = 0;
        for( const Statement& statement : first().statements )
        {
            if( statement.kind == Statement_kind::Assign )
            {
                for_each_operand(
                    first(),
                    statement.value,
                    [&]( const Operand& operand ) { count += operand.kind == Operand_kind::Copy && operand.place.is_global(); }
                );
            }
        }
        return count;
    }

    std::ptrdiff_t writes_of_globals() const
    {
        return std::ranges::count_if(
            first().statements, []( const Statement& s ) { return s.kind == Statement_kind::Assign && s.place.is_global(); }
        );
    }

    std::ptrdiff_t count( Rvalue_kind kind, Token_kind op ) const
    {
        return std::ranges::count_if(
            first().statements,
            [&]( const Statement& s ) { return s.kind == Statement_kind::Assign && s.value.kind == kind && s.value.op == op; }
        );
    }
};

} // namespace

// Not inlined, so `touch` may write `g`.
TEST_CASE( "forward_globals_keeps_a_read_after_a_call", "[opt][globals]" )
{
    const Forwarded p(
        "i32 g;\ni32 f() { g = 5; touch(); return g; }\nvoid touch() { g = 1; }\n",
        Build { .optimise = true, .inlining = std::nullopt }
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.reads_of_globals() == 1 );
}

// The path where `c` is false reaches the join with `g = 1` unread.
TEST_CASE( "forward_globals_keeps_a_write_a_join_reads", "[opt][globals]" )
{
    const Forwarded p( "i32 g;\ni32 f( bool c ) { g = 1; if( c ) { g = 2; } return 0; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.writes_of_globals() == 2 );
}

// `t - 3` is the `x` before `x = 7`, which nothing holds, so `(t - 3) + 7` is `t + 4` and `t` stays. One
// round, as the next finds `x = 7` gone.
TEST_CASE( "forward_globals_keeps_a_sum_whose_source_changed", "[opt][globals]" )
{
    const Forwarded p(
        "i32 f( i32 x ) { i32 t = x + 3; x = 7; return t - 3 + x; }\n", Build { .optimise = true, .rounds = 1 }
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.count( Rvalue_kind::Binary, Token_kind::Plus ) == 2 );
}

// bb1 is the check's true arm and bb2 its false arm, each `_3 = _2 + 1` then `_4 = overflows _3 - 1`:
// only the false arm knows the sum fits.
TEST_CASE( "forward_globals_learns_nothing_on_a_true_arm", "[opt][globals]" )
{
    Type_table    types;
    Literal_pool  literals;
    const Type_id i32     = types.integer( 32, true );
    const Type_id boolean = types.builtin( Type_kind::Bool );
    const Operand one     = constant( literals.add_integer( 1 ), i32 );

    const auto local = []( u32 v ) { return Place { .local = Local_id { v } }; };

    Function func;
    func.parameter_count = 1;
    func.locals          = {
        Local { .type = i32 },
        Local { .type = boolean },
        Local { .type = i32 },
        Local { .type = i32 },
        Local { .type = boolean }
    };

    const auto statement = [&]( u32 into, Rvalue value ) { return Statement { .place = local( into ), .value = value }; };

    func.statements = {
        statement( 1, overflows( Token_kind::Plus, copy( local( 2 ), i32 ), one, boolean ) ),
        statement( 3, binary( Token_kind::Plus, copy( local( 2 ), i32 ), one, i32 ) ),
        statement( 4, overflows( Token_kind::Minus, copy( local( 3 ), i32 ), one, boolean ) ),
        statement( 3, binary( Token_kind::Plus, copy( local( 2 ), i32 ), one, i32 ) ),
        statement( 4, overflows( Token_kind::Minus, copy( local( 3 ), i32 ), one, boolean ) ),
    };

    const Terminator done { .kind = Terminator_kind::Return };
    func.blocks = {
        Block {
            .first_statement = 0,
            .statement_count = 1,
            .terminator =
                Terminator {
                    .kind      = Terminator_kind::Branch,
                    .condition = copy( local( 1 ), boolean ),
                    .targets   = { Block_id { 1 }, Block_id { 2 } }
                }
        },
        Block { .first_statement = 1, .statement_count = 2, .terminator = done },
        Block { .first_statement = 3, .statement_count = 2, .terminator = done },
    };

    REQUIRE( forward_globals( func, types, literals ) );

    const auto checks = [&]( u32 block )
    {
        const Statement& s = func.statements_of( func.blocks[Block_id { block }] )[1];
        return s.value.kind == Rvalue_kind::Overflows;
    };
    REQUIRE( checks( 1 ) );
    REQUIRE( !checks( 2 ) );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
