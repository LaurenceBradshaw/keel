#include "ir/verify.h"
#include <fmt/format.h>
#include <fmt/ranges.h>

namespace keel
{

namespace
{

// A place, wherever it appears. Places live in four places - a statement's target, an rvalue's
// operands, a call's argument list, and a branch's condition - so this is shared rather than
// repeated, which is how the operand ones came to be missed.
void check_place( const Function& func, const Place& place, std::string_view where, std::vector<std::string>& errors )
{
    const bool rooted_in_local  = place.local.is_valid();
    const bool rooted_in_global = place.global.is_valid();

    if( rooted_in_global == rooted_in_local )
    {
        errors.push_back( fmt::format( "{}: names {}", where, rooted_in_local ? "both a local and a global" : "no storage" ) );
    }
    else if( rooted_in_local && place.local.v >= func.locals.size() )
    {
        errors.push_back( fmt::format( "{}: local {} is out of range", where, place.local.v ) );
    }

    // In u64, or an unset first_projection wraps and the range passes exactly when it should not.
    const u64 end = static_cast<u64>( place.first_projection ) + place.num_projections;

    if( end > func.projections.size() )
    {
        errors.push_back( fmt::format( "{}: projections {}..{} are out of range", where, place.first_projection, end ) );
    }
}

void check_operand( const Function& func, const Operand& operand, std::string_view where, std::vector<std::string>& errors )
{
    switch( operand.kind )
    {
    case Operand_kind::Copy:
    case Operand_kind::Move:
        check_place( func, operand.place, where, errors );
        return;

    case Operand_kind::Constant:
        if( !operand.constant.is_valid() )
        {
            errors.push_back( fmt::format( "{}: constant is not set", where ) );
        }

        return;
    }
}

// Which fields an rvalue means is decided by its kind, so only those are read. Checking the others
// would report on whatever a lowerer left in them, which is nothing.
void check_rvalue( const Function& func, const Rvalue& value, std::string_view where, std::vector<std::string>& errors )
{
    const auto operand_at = [&]( const Operand& operand, std::string_view which )
    { check_operand( func, operand, fmt::format( "{} {}", where, which ), errors ); };

    switch( value.kind )
    {
    case Rvalue_kind::Use:
    case Rvalue_kind::Cast:
        operand_at( value.a, "operand" );
        return;

    case Rvalue_kind::Unary:
        if( value.op == Token_kind::Unknown )
        {
            errors.push_back( fmt::format( "{}: unary operator is not set", where ) );
        }

        operand_at( value.a, "operand" );
        return;

    case Rvalue_kind::Binary:
        if( value.op == Token_kind::Unknown )
        {
            errors.push_back( fmt::format( "{}: binary operator is not set", where ) );
        }

        operand_at( value.a, "left operand" );
        operand_at( value.b, "right operand" );
        return;

    // The address of a place, not a copy of what lives there - so the place is what matters and
    // the operand's kind does not.
    case Rvalue_kind::Address_of:
        check_place( func, value.a.place, fmt::format( "{} operand", where ), errors );
        return;

    case Rvalue_kind::Call:
    {
        if( !value.callee.is_valid() )
        {
            errors.push_back( fmt::format( "{}: callee is not set", where ) );
        }

        const u64 end = static_cast<u64>( value.first_argument ) + value.argument_count;

        if( end > func.operands.size() )
        {
            errors.push_back( fmt::format( "{}: arguments {}..{} are out of range", where, value.first_argument, end ) );

            return; // indexing them would read past the end
        }

        for( u32 i = 0; i < value.argument_count; ++i )
        {
            check_operand( func, func.operands[value.first_argument + i], fmt::format( "{} argument {}", where, i ), errors );
        }

        return;
    }
    }
}

void check_statements( const Function& func, std::vector<std::string>& errors )
{
    for( u32 i = 0; i < func.statements.size(); ++i )
    {
        const Statement&  statement = func.statements[i];
        const std::string where     = fmt::format( "statement {}", i );

        check_place( func, statement.place, where, errors );

        switch( statement.kind )
        {
        case Statement_kind::Assign:
            check_rvalue( func, statement.value, where, errors );
            break;

        // Storage belongs to a whole local. A projection here would mean a field came alive on its
        // own, which is not a thing.
        case Statement_kind::Storage_live:
        case Statement_kind::Storage_dead:
            if( statement.place.num_projections != 0 )
            {
                errors.push_back( fmt::format( "{}: a storage marker names a whole local, not a projection", where ) );
            }

            break;

        case Statement_kind::Drop:
            break;
        }
    }
}

void check_terminators( const Function& func, std::vector<std::string>& errors )
{
    for( u32 i = 0; i < func.blocks.size(); ++i )
    {
        const Terminator& terminator = func.blocks[i].terminator;
        const std::string where      = fmt::format( "block {}", i );

        const auto check_target = [&]( Block_id target, std::string_view what )
        {
            if( !target.is_valid() )
            {
                errors.push_back( fmt::format( "{}: {} is not set", where, what ) );
            }
            else if( target.v >= func.blocks.size() )
            {
                errors.push_back( fmt::format( "{}: {} {} is out of range", where, what, target.v ) );
            }
        };

        switch( terminator.kind )
        {
        // The invariant that makes the graph a graph: exactly one terminator per block.
        case Terminator_kind::Unset:
            errors.push_back( fmt::format( "{}: has no terminator", where ) );
            break;

        case Terminator_kind::Goto:
            check_target( terminator.targets[0], "Goto target" );
            break;

        case Terminator_kind::Branch:
            check_operand( func, terminator.condition, fmt::format( "{} condition", where ), errors );
            check_target( terminator.targets[0], "Branch true target" );
            check_target( terminator.targets[1], "Branch false target" );
            break;

        case Terminator_kind::Return:
        case Terminator_kind::Unreachable:
            break;
        }
    }
}

void check_statement_ranges( const Function& func, std::vector<std::string>& errors )
{
    for( u32 i = 0; i < func.blocks.size(); ++i )
    {
        const Block& block = func.blocks[i];
        const u64    end   = static_cast<u64>( block.first_statement ) + block.statement_count;

        if( end > func.statements.size() )
        {
            errors.push_back( fmt::format( "block {}: statements {}..{} are out of range", i, block.first_statement, end ) );
        }
    }
}

void check_projections( const Function& func, std::vector<std::string>& errors )
{
    for( u32 i = 0; i < func.projections.size(); ++i )
    {
        const Projection& projection = func.projections[i];

        switch( projection.kind )
        {
        case Projection_kind::Field:
            if( !projection.field.is_valid() )
            {
                errors.push_back( fmt::format( "projection {}: field is not set", i ) );
            }

            break;

        case Projection_kind::Deref:
            if( projection.field.is_valid() )
            {
                errors.push_back( fmt::format( "projection {}: a deref carries no field", i ) );
            }

            break;
        }
    }
}

// Block 0 is the entry and local 0 is the return slot, so neither table may be empty.
void check_shape( const Function& func, std::vector<std::string>& errors )
{
    if( func.blocks.empty() )
    {
        errors.push_back( "function has no blocks; block 0 is the entry" );
    }

    if( func.locals.empty() )
    {
        errors.push_back( "function has no locals; local 0 is the return slot" );
    }
    else if( func.parameter_count >= func.locals.size() )
    {
        errors.push_back( fmt::format(
            "parameter_count {} leaves no room for the return slot among {} locals", func.parameter_count, func.locals.size()
        ) );
    }
}

// Nothing in the pipeline creates dead blocks, so one can only mean a lowerer built a block and
// forgot to branch to it - which loses code silently rather than loudly.
void check_reachability( const Function& func, std::vector<std::string>& errors )
{
    if( func.blocks.empty() )
    {
        return; // check_shape said so
    }

    std::vector<bool>     seen( func.blocks.size(), false );
    std::vector<Block_id> pending { Block_id { 0 } };

    seen[0] = true;

    while( !pending.empty() )
    {
        const Block_id    id         = pending.back();
        const Terminator& terminator = func.blocks[id.v].terminator;

        pending.pop_back();

        const u32 targets = terminator.kind == Terminator_kind::Branch ? 2 : terminator.kind == Terminator_kind::Goto ? 1 : 0;

        for( u32 i = 0; i < targets; ++i )
        {
            const Block_id target = terminator.targets[i];

            // Out of range is check_terminators' error to report, not a reason to index past the end.
            if( !target.is_valid() || target.v >= func.blocks.size() || seen[target.v] )
            {
                continue;
            }

            seen[target.v] = true;
            pending.push_back( target );
        }
    }

    for( u32 i = 0; i < func.blocks.size(); ++i )
    {
        if( !seen[i] )
        {
            errors.push_back( fmt::format( "block {} is not reachable from block 0", i ) );
        }
    }
}

} // namespace

std::vector<std::string> verify( const Function& func )
{
    std::vector<std::string> errors;

    check_shape( func, errors );
    check_statement_ranges( func, errors );
    check_statements( func, errors );
    check_terminators( func, errors );
    check_projections( func, errors );
    check_reachability( func, errors );

    return errors;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{
namespace
{

// The smallest well-formed function: one local (the return slot), one block, and a terminator.
// Every test below starts here and breaks exactly one thing, which is the only way to know a check
// fires for its own reason rather than being masked by another.
Function minimal()
{
    Function func;

    func.locals.push_back( Local {} );
    func.blocks.push_back( Block {} );
    func.blocks[0].terminator.kind = Terminator_kind::Return;

    return func;
}

bool mentions( const std::vector<std::string>& errors, std::string_view needle )
{
    for( const std::string& error : errors )
    {
        if( error.find( needle ) != std::string::npos )
        {
            return true;
        }
    }

    return false;
}

} // namespace

TEST_CASE( "verify_accepts_a_well_formed_function", "[ir][verify]" )
{
    const std::vector<std::string> errors = verify( minimal() );

    INFO( fmt::format( "{}", fmt::join( errors, "\n" ) ) );
    REQUIRE( errors.empty() );
}

TEST_CASE( "verify_rejects_a_malformed_shape", "[ir][verify]" )
{
    SECTION( "no blocks" )
    {
        Function func = minimal();
        func.blocks.clear();

        REQUIRE( mentions( verify( func ), "no blocks" ) );
    }

    SECTION( "no locals" )
    {
        Function func = minimal();
        func.locals.clear();

        REQUIRE( mentions( verify( func ), "no locals" ) );
    }

    SECTION( "parameters leaving no return slot" )
    {
        Function func        = minimal();
        func.parameter_count = 1; // one local, so parameter 0 would be the return slot

        REQUIRE( mentions( verify( func ), "return slot" ) );
    }
}

// The core invariant: exactly one terminator per block. Terminator_kind::Unset exists so that a
// block nobody terminated is this error rather than a silently valid Goto to block 0.
TEST_CASE( "verify_rejects_a_block_with_no_terminator", "[ir][verify]" )
{
    Function func                  = minimal();
    func.blocks[0].terminator.kind = Terminator_kind::Unset;

    REQUIRE( mentions( verify( func ), "no terminator" ) );
}

TEST_CASE( "verify_rejects_bad_terminator_targets", "[ir][verify]" )
{
    SECTION( "an unset Goto target" )
    {
        Function func                  = minimal();
        func.blocks[0].terminator.kind = Terminator_kind::Goto;

        REQUIRE( mentions( verify( func ), "Goto target is not set" ) );
    }

    SECTION( "an out-of-range Goto target" )
    {
        Function func                        = minimal();
        func.blocks[0].terminator.kind       = Terminator_kind::Goto;
        func.blocks[0].terminator.targets[0] = Block_id { 7 };

        REQUIRE( mentions( verify( func ), "out of range" ) );
    }

    SECTION( "both arms of a Branch" )
    {
        Function func                  = minimal();
        func.blocks[0].terminator.kind = Terminator_kind::Branch;

        const std::vector<std::string> errors = verify( func );

        REQUIRE( mentions( errors, "Branch true target is not set" ) );
        REQUIRE( mentions( errors, "Branch false target is not set" ) );
    }

    // A Branch reads its condition, so a bad local in there is as wrong as a bad target.
    SECTION( "a Branch condition naming a local that does not exist" )
    {
        Function func = minimal();

        func.blocks[0].terminator.kind            = Terminator_kind::Branch;
        func.blocks[0].terminator.targets[0]      = Block_id { 0 };
        func.blocks[0].terminator.targets[1]      = Block_id { 0 };
        func.blocks[0].terminator.condition.kind  = Operand_kind::Copy;
        func.blocks[0].terminator.condition.place = Place { .local = Local_id { 9 } };

        REQUIRE( mentions( verify( func ), "condition: local 9 is out of range" ) );
    }
}

// Places live in four locations, and checking only the statement target was how the operand ones
// came to be missed.
TEST_CASE( "verify_checks_places_wherever_they_appear", "[ir][verify]" )
{
    const auto with_statement = []( Statement statement )
    {
        Function func = minimal();

        func.statements.push_back( statement );
        func.blocks[0].statement_count = 1;

        return func;
    };

    SECTION( "the target of an assignment" )
    {
        Statement statement;
        statement.place         = Place { .local = Local_id { 9 } };
        statement.value.kind    = Rvalue_kind::Use;
        statement.value.a.kind  = Operand_kind::Copy;
        statement.value.a.place = Place { .local = Local_id { 0 } };

        REQUIRE( mentions( verify( with_statement( statement ) ), "statement 0: local 9 is out of range" ) );
    }

    // A place is rooted in storage - a local or a file-scope variable - and exactly one of them.
    // Neither is what a default-constructed Place gives.
    SECTION( "a target rooted in nothing" )
    {
        Statement statement;
        statement.value.kind    = Rvalue_kind::Use;
        statement.value.a.kind  = Operand_kind::Copy;
        statement.value.a.place = Place { .local = Local_id { 0 } };

        REQUIRE( mentions( verify( with_statement( statement ) ), "statement 0: names no storage" ) );
    }

    // The other half, and the one that earns its keep: Node_id's absent value is 0xFFFFFFFF, not
    // zero, so a positional `Place { id, 0, 0 }` written against the older three-field struct gives
    // the global root a *valid* node. This is what catches that, silently-wrong-otherwise.
    SECTION( "a target rooted in both a local and a global" )
    {
        Statement statement;
        statement.place         = Place { .local = Local_id { 0 }, .global = Node_id { 0 } };
        statement.value.kind    = Rvalue_kind::Use;
        statement.value.a.kind  = Operand_kind::Copy;
        statement.value.a.place = Place { .local = Local_id { 0 } };

        REQUIRE( mentions( verify( with_statement( statement ) ), "names both a local and a global" ) );
    }

    // A global root is a Node_id into the AST, which verify cannot range-check - it is handed only
    // a Function. Validity is all it can say, and that is enough.
    SECTION( "a target rooted in a global is accepted" )
    {
        Statement statement;
        statement.place         = Place { .global = Node_id { 3 } };
        statement.value.kind    = Rvalue_kind::Use;
        statement.value.a.kind  = Operand_kind::Copy;
        statement.value.a.place = Place { .global = Node_id { 3 } };

        REQUIRE( verify( with_statement( statement ) ).empty() );
    }

    SECTION( "the operand of a Use" )
    {
        Statement statement;
        statement.place         = Place { .local = Local_id { 0 } };
        statement.value.kind    = Rvalue_kind::Use;
        statement.value.a.kind  = Operand_kind::Copy;
        statement.value.a.place = Place { .local = Local_id { 9 } };

        REQUIRE( mentions( verify( with_statement( statement ) ), "operand: local 9 is out of range" ) );
    }

    SECTION( "both operands of a Binary" )
    {
        Statement statement;
        statement.place         = Place { .local = Local_id { 0 } };
        statement.value.kind    = Rvalue_kind::Binary;
        statement.value.op      = Token_kind::Plus;
        statement.value.a.kind  = Operand_kind::Copy;
        statement.value.a.place = Place { .local = Local_id { 8 } };
        statement.value.b.kind  = Operand_kind::Copy;
        statement.value.b.place = Place { .local = Local_id { 9 } };

        const std::vector<std::string> errors = verify( with_statement( statement ) );

        REQUIRE( mentions( errors, "left operand: local 8 is out of range" ) );
        REQUIRE( mentions( errors, "right operand: local 9 is out of range" ) );
    }

    // Only the operands a kind actually uses are read, so an untouched `b` is not reported.
    SECTION( "but not an operand the kind does not use" )
    {
        Statement statement;
        statement.place         = Place { .local = Local_id { 0 } };
        statement.value.kind    = Rvalue_kind::Use;
        statement.value.a.kind  = Operand_kind::Copy;
        statement.value.a.place = Place { .local = Local_id { 0 } };

        REQUIRE( verify( with_statement( statement ) ).empty() );
    }

    SECTION( "a projection range past the end" )
    {
        Statement statement;
        statement.place         = Place { .local = Local_id { 0 }, .num_projections = 3 };
        statement.value.kind    = Rvalue_kind::Use;
        statement.value.a.kind  = Operand_kind::Copy;
        statement.value.a.place = Place { .local = Local_id { 0 } };

        REQUIRE( mentions( verify( with_statement( statement ) ), "projections 0..3 are out of range" ) );
    }

    // The unset sentinel is 0xFFFFFFFF, so a start plus a count overflows u32 and the range check
    // passes on exactly the input it exists to catch. It is done in u64 for this reason.
    SECTION( "a projection range that would overflow u32" )
    {
        Statement statement;
        statement.place         = Place { .local = Local_id { 0 }, .first_projection = 0xFFFF'FFFFu, .num_projections = 4 };
        statement.value.kind    = Rvalue_kind::Use;
        statement.value.a.kind  = Operand_kind::Copy;
        statement.value.a.place = Place { .local = Local_id { 0 } };

        REQUIRE( mentions( verify( with_statement( statement ) ), "out of range" ) );
    }
}

TEST_CASE( "verify_checks_rvalue_fields", "[ir][verify]" )
{
    const auto with_value = []( Rvalue value )
    {
        Function  func = minimal();
        Statement statement;

        statement.place = Place { .local = Local_id { 0 } };
        statement.value = value;

        func.statements.push_back( statement );
        func.blocks[0].statement_count = 1;

        return func;
    };

    SECTION( "a binary with no operator" )
    {
        Rvalue value;
        value.kind    = Rvalue_kind::Binary;
        value.a.kind  = Operand_kind::Copy;
        value.a.place = Place { .local = Local_id { 0 } };
        value.b.kind  = Operand_kind::Copy;
        value.b.place = Place { .local = Local_id { 0 } };

        REQUIRE( mentions( verify( with_value( value ) ), "binary operator is not set" ) );
    }

    SECTION( "a unary with no operator" )
    {
        Rvalue value;
        value.kind    = Rvalue_kind::Unary;
        value.a.kind  = Operand_kind::Copy;
        value.a.place = Place { .local = Local_id { 0 } };

        REQUIRE( mentions( verify( with_value( value ) ), "unary operator is not set" ) );
    }

    SECTION( "a call with no callee" )
    {
        Rvalue value;
        value.kind = Rvalue_kind::Call;

        REQUIRE( mentions( verify( with_value( value ) ), "callee is not set" ) );
    }

    SECTION( "a call whose arguments run past the end" )
    {
        Rvalue value;
        value.kind           = Rvalue_kind::Call;
        value.callee         = Node_id { 1 };
        value.first_argument = 0;
        value.argument_count = 2;

        REQUIRE( mentions( verify( with_value( value ) ), "arguments 0..2 are out of range" ) );
    }

    // Address_of reads the place, not the operand: taking an address is not copying what is there.
    SECTION( "an address of a local that does not exist" )
    {
        Rvalue value;
        value.kind    = Rvalue_kind::Address_of;
        value.a.place = Place { .local = Local_id { 9 } };

        REQUIRE( mentions( verify( with_value( value ) ), "operand: local 9 is out of range" ) );
    }

    SECTION( "a constant operand with no literal" )
    {
        Rvalue value;
        value.kind   = Rvalue_kind::Use;
        value.a.kind = Operand_kind::Constant;

        REQUIRE( mentions( verify( with_value( value ) ), "constant is not set" ) );
    }
}

TEST_CASE( "verify_checks_projections_and_storage", "[ir][verify]" )
{
    SECTION( "a field projection with no field" )
    {
        Function func = minimal();
        func.projections.push_back( Projection { Projection_kind::Field, Node_id {} } );

        REQUIRE( mentions( verify( func ), "field is not set" ) );
    }

    SECTION( "a deref carrying a field" )
    {
        Function func = minimal();
        func.projections.push_back( Projection { Projection_kind::Deref, Node_id { 1 } } );

        REQUIRE( mentions( verify( func ), "carries no field" ) );
    }

    // Storage belongs to a whole local: a field cannot come alive on its own.
    SECTION( "a storage marker with a projection" )
    {
        Function func = minimal();

        func.projections.push_back( Projection { Projection_kind::Deref, Node_id {} } );

        Statement statement;
        statement.kind  = Statement_kind::Storage_live;
        statement.place = Place { .local = Local_id { 0 }, .num_projections = 1 };

        func.statements.push_back( statement );
        func.blocks[0].statement_count = 1;

        REQUIRE( mentions( verify( func ), "names a whole local" ) );
    }
}

TEST_CASE( "verify_checks_statement_ranges", "[ir][verify]" )
{
    Function func                  = minimal();
    func.blocks[0].statement_count = 2; // there are none

    REQUIRE( mentions( verify( func ), "statements 0..2 are out of range" ) );
}

// The check that catches a lowering bug rather than a typo: a block built and never branched to
// loses its code silently. Nothing in the pipeline creates dead blocks, so one can only be a bug.
TEST_CASE( "verify_rejects_an_unreachable_block", "[ir][verify]" )
{
    SECTION( "a block nothing branches to" )
    {
        Function func = minimal();

        func.blocks.push_back( Block {} );
        func.blocks[1].terminator.kind = Terminator_kind::Return;

        REQUIRE( mentions( verify( func ), "block 1 is not reachable" ) );
    }

    SECTION( "but one reached through a Goto is fine" )
    {
        Function func = minimal();

        func.blocks[0].terminator.kind       = Terminator_kind::Goto;
        func.blocks[0].terminator.targets[0] = Block_id { 1 };

        func.blocks.push_back( Block {} );
        func.blocks[1].terminator.kind = Terminator_kind::Return;

        REQUIRE( verify( func ).empty() );
    }

    // A cycle must not hang the walk, and every block in it counts as reached.
    SECTION( "and a loop back to the entry terminates" )
    {
        Function func = minimal();

        func.blocks[0].terminator.kind       = Terminator_kind::Goto;
        func.blocks[0].terminator.targets[0] = Block_id { 1 };

        func.blocks.push_back( Block {} );
        func.blocks[1].terminator.kind       = Terminator_kind::Goto;
        func.blocks[1].terminator.targets[0] = Block_id { 0 };

        REQUIRE( verify( func ).empty() );
    }
}

} // namespace keel
#endif
