#include "ir/builder.h"
#include <cassert>

namespace keel
{

Builder::Builder( Node_id declaration, Type_id return_type, Span span )
{
    function_.declaration = declaration;
    function_.locals.push_back( Local { .type = return_type, .span = span } );
    switch_to( add_block() );
}

Local_id Builder::add_parameter( Type_id type, Span span, Symbol_id name )
{
    assert( function_.locals.size() == function_.parameter_count + 1 && "parameter count must match locals" );
    function_.locals.push_back( Local { .type = type, .span = span, .name = name } );
    function_.parameter_count += 1;
    return Local_id { static_cast<u32>( function_.locals.size() - 1 ) };
}

Local_id Builder::add_local( Type_id type, Span span, Symbol_id name )
{
    function_.locals.push_back( Local { .type = type, .span = span, .name = name } );
    return Local_id { static_cast<u32>( function_.locals.size() - 1 ) };
}

Block_id Builder::add_block()
{
    function_.blocks.push_back( Block {} );
    pending_.push_back( {} );
    return Block_id { static_cast<u32>( function_.blocks.size() - 1 ) };
}

void Builder::switch_to( Block_id block )
{
    assert( block.is_valid() && block.v < function_.blocks.size() && "switch_to: invalid block" );
    current_ = block;
}

Block_id Builder::current() const
{
    assert( current_.is_valid() && current_.v < function_.blocks.size() && "current: invalid block" );
    return current_;
}

// Designated, and deliberately: a bare `Place { id, ... }` would give the *global* root whatever
// sat in the second position, and Node_id { 0 } is a valid node rather than an absent one.
Place Builder::place( Local_id id ) const
{
    return Place { .local = id };
}

Place Builder::global( Node_id declaration ) const
{
    return Place { .global = declaration };
}

Place Builder::field( Place base, Node_id field_decl )
{
    return projected( base, Projection { Projection_kind::Field, field_decl } );
}

Place Builder::deref( Place base )
{
    return projected( base, Projection { Projection_kind::Deref, Node_id {} } );
}

// Projections live in one table addressed by (first, count), so a place's own must be contiguous -
// and base's may sit anywhere. Copy base's to the end, append the new one, and return a place over
// the copy. The root comes across with it, which is what keeps `counter.x` rooted in the global.
Place Builder::projected( Place base, Projection projection )
{
    const u32 first = static_cast<u32>( function_.projections.size() );

    for( u32 i = 0; i < base.num_projections; ++i )
    {
        // Read out before appending: push_back from a reference into the same vector is legal but
        // needlessly subtle.
        const Projection copied = function_.projections[base.first_projection + i];

        function_.projections.push_back( copied );
    }

    function_.projections.push_back( projection );

    Place result = base;

    result.first_projection = first;
    result.num_projections  = base.num_projections + 1;

    return result;
}

void Builder::assign( Place target, Rvalue value, Span span )
{
    push_statement( Statement { .kind = Statement_kind::Assign, .span = span, .place = target, .value = value } );
}

void Builder::drop( Place place, Span span )
{
    push_statement( Statement { .kind = Statement_kind::Drop, .span = span, .place = place } );
}

void Builder::storage_live( Local_id local, Span span )
{
    push_statement( Statement { .kind = Statement_kind::Storage_live, .span = span, .place = place( local ) } );
}

void Builder::storage_dead( Local_id local, Span span )
{
    push_statement( Statement { .kind = Statement_kind::Storage_dead, .span = span, .place = place( local ) } );
}

Local_id Builder::into_temp( Rvalue value, Type_id type, Span span )
{
    Local_id temp = add_local( type, span );
    assign( place( temp ), value, span );
    return temp;
}

void Builder::terminate_goto( Block_id target, Span span )
{
    set_terminator( Terminator { .kind = Terminator_kind::Goto, .span = span, .targets = { target } } );
}

void Builder::terminate_branch( Operand condition, Block_id true_target, Block_id false_target, Span span )
{
    set_terminator( Terminator {
        .kind = Terminator_kind::Branch, .span = span, .condition = condition, .targets = { true_target, false_target }
    } );
}

void Builder::terminate_return( Span span )
{
    set_terminator( Terminator { .kind = Terminator_kind::Return, .span = span } );
}

bool Builder::is_terminated() const
{
    return function_.blocks[current_.v].terminator.kind != Terminator_kind::Unset;
}

u32 Builder::add_operands( std::span<const Operand> operands )
{
    const u32 first = static_cast<u32>( function_.operands.size() );
    function_.operands.insert( function_.operands.end(), operands.begin(), operands.end() );
    return first;
}

Function Builder::finish()
{
    assert( !finished_ && "finish() called twice" );
    finished_ = true;

    for( u32 i = 0; i < function_.blocks.size(); ++i )
    {
        const std::vector<Statement>& block_statements = pending_[i];
        Block&                        block            = function_.blocks[i];

        block.first_statement = static_cast<u32>( function_.statements.size() );
        block.statement_count = static_cast<u32>( block_statements.size() );

        function_.statements.insert( function_.statements.end(), block_statements.begin(), block_statements.end() );
    }

    return std::move( function_ );
}

void Builder::push_statement( Statement statement )
{
    pending_[current_.v].push_back( statement );
}

void Builder::set_terminator( Terminator terminator )
{
    assert( function_.blocks[current_.v].terminator.kind == Terminator_kind::Unset && "block already has a terminator" );
    function_.blocks[current_.v].terminator = terminator;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

struct Fixture
{
    Type_table table;
    Literals   literals;

    Type_id i32() const
    {
        return table.integer( 32, true );
    }

    Type_id boolean() const
    {
        return table.builtin( Type_kind::Bool );
    }

    Operand one()
    {
        return constant( literals.add_integer( 1 ), table.integer( 32, true ) );
    }

    // A real Literal_id: verify rejects a constant operand carrying an invalid one, which is a
    // check worth having and not worth working around here.
    Operand yes()
    {
        return constant( literals.add_integer( 1 ), table.builtin( Type_kind::Bool ) );
    }
};

// Anything the builder produces has to satisfy verify, which is the real assertion in most of
// these: the shapes below are the ones a lowerer will actually build.
void require_verifies( const Function& func )
{
    const std::vector<std::string> errors = verify( func );

    INFO( fmt::format( "{}", fmt::join( errors, "\n" ) ) );
    REQUIRE( errors.empty() );
}

} // namespace

TEST_CASE( "builder_establishes_the_conventions", "[ir][builder]" )
{
    Fixture f;
    Builder builder( Node_id { 1 }, f.i32(), Span {} );

    builder.terminate_return( Span {} );

    const Function func = builder.finish();

    // Block 0 is the entry and local 0 is the return slot, established by the constructor so no
    // caller can get them wrong.
    REQUIRE( func.blocks.size() == 1 );
    REQUIRE( func.locals.size() == 1 );
    REQUIRE( func.locals[0].type == f.i32() );
    REQUIRE( !func.locals[0].name.is_valid() ); // the return slot has no source name
    REQUIRE( func.parameter_count == 0 );
    REQUIRE( func.declaration == Node_id { 1 } );

    require_verifies( func );
}

TEST_CASE( "builder_numbers_locals_and_parameters", "[ir][builder]" )
{
    Fixture f;
    Builder builder( Node_id { 1 }, f.i32(), Span {} );

    const Local_id first  = builder.add_parameter( f.i32(), Span {}, Symbol_id { 7 } );
    const Local_id second = builder.add_parameter( f.i32(), Span {}, Symbol_id { 8 } );
    const Local_id temp   = builder.add_local( f.i32(), Span {} );

    builder.terminate_return( Span {} );

    const Function func = builder.finish();

    // Parameters are locals 1..parameter_count, in declaration order, after the return slot.
    REQUIRE( first == Local_id { 1 } );
    REQUIRE( second == Local_id { 2 } );
    REQUIRE( temp == Local_id { 3 } );
    REQUIRE( func.parameter_count == 2 );
    REQUIRE( func.locals.size() == 4 );

    // A temporary is a local with no name; nothing else distinguishes it.
    REQUIRE( func.locals[1].name == Symbol_id { 7 } );
    REQUIRE( !func.locals[3].name.is_valid() );

    require_verifies( func );
}

// The reason statements are staged per block rather than appended as they are emitted: the flat
// form's (first, count) ranges require a block's statements to be contiguous, and a lowerer that
// returns to an earlier block would otherwise interleave them.
TEST_CASE( "builder_keeps_a_blocks_statements_contiguous", "[ir][builder]" )
{
    Fixture f;
    Builder builder( Node_id { 1 }, f.i32(), Span {} );

    const Block_id entry  = builder.current();
    const Block_id second = builder.add_block();

    // Emit into the entry, leave for another block, and come back.
    builder.assign( builder.place( Local_id { 0 } ), use( f.one() ), Span {} );

    builder.switch_to( second );
    builder.assign( builder.place( Local_id { 0 } ), use( f.one() ), Span {} );
    builder.terminate_return( Span {} );

    builder.switch_to( entry );
    builder.assign( builder.place( Local_id { 0 } ), use( f.one() ), Span {} );
    builder.terminate_goto( second, Span {} );

    const Function func = builder.finish();

    REQUIRE( func.statements.size() == 3 );

    // Two of the three belong to the entry, and they are adjacent despite being emitted either
    // side of a visit to another block.
    REQUIRE( func.blocks[0].statement_count == 2 );
    REQUIRE( func.blocks[1].statement_count == 1 );
    REQUIRE( func.blocks[0].first_statement + func.blocks[0].statement_count == func.blocks[1].first_statement );

    require_verifies( func );
}

TEST_CASE( "builder_flattens_empty_blocks", "[ir][builder]" )
{
    Fixture f;
    Builder builder( Node_id { 1 }, f.i32(), Span {} );

    const Block_id second = builder.add_block();

    builder.terminate_goto( second, Span {} );
    builder.switch_to( second );
    builder.terminate_return( Span {} );

    const Function func = builder.finish();

    REQUIRE( func.statements.empty() );
    REQUIRE( func.blocks[0].statement_count == 0 );
    REQUIRE( func.blocks[1].statement_count == 0 );

    require_verifies( func );
}

// Projections are addressed by (first, count) in a shared table, so a place's own must be
// contiguous. field() and deref() copy the base's rather than assuming it sits last.
TEST_CASE( "builder_builds_nested_places", "[ir][builder]" )
{
    Fixture f;
    Builder builder( Node_id { 1 }, f.i32(), Span {} );

    const Local_id local = builder.add_local( f.i32(), Span {} );

    SECTION( "a bare local has none" )
    {
        const Place bare = builder.place( local );

        REQUIRE( bare.local == local );
        REQUIRE( bare.num_projections == 0 );
    }

    SECTION( "one field" )
    {
        const Place one_deep = builder.field( builder.place( local ), Node_id { 5 } );

        REQUIRE( one_deep.local == local );
        REQUIRE( one_deep.num_projections == 1 );
    }

    // (*q).z - a deref then a field, which is the shape the emitter spells with parentheses.
    SECTION( "a deref then a field" )
    {
        const Place two_deep = builder.field( builder.deref( builder.place( local ) ), Node_id { 5 } );

        builder.assign( two_deep, use( f.one() ), Span {} );
        builder.terminate_return( Span {} );

        const Function func = builder.finish();

        REQUIRE( two_deep.num_projections == 2 );
        REQUIRE( func.projections[two_deep.first_projection].kind == Projection_kind::Deref );
        REQUIRE( func.projections[two_deep.first_projection + 1].kind == Projection_kind::Field );
        REQUIRE( func.projections[two_deep.first_projection + 1].field == Node_id { 5 } );

        require_verifies( func );
    }

    // Two places built from one base: the second must not scribble on the first, which is what
    // copying rather than appending in place buys.
    SECTION( "two places sharing a base stay independent" )
    {
        const Place base  = builder.deref( builder.place( local ) );
        const Place left  = builder.field( base, Node_id { 5 } );
        const Place right = builder.field( base, Node_id { 6 } );

        builder.assign( left, use( f.one() ), Span {} );
        builder.assign( right, use( f.one() ), Span {} );
        builder.terminate_return( Span {} );

        const Function func = builder.finish();

        REQUIRE( left.first_projection != right.first_projection );
        REQUIRE( func.projections[left.first_projection + 1].field == Node_id { 5 } );
        REQUIRE( func.projections[right.first_projection + 1].field == Node_id { 6 } );

        require_verifies( func );
    }
}

TEST_CASE( "builder_emits_each_statement_kind", "[ir][builder]" )
{
    Fixture f;
    Builder builder( Node_id { 1 }, f.i32(), Span {} );

    const Local_id local = builder.add_local( f.i32(), Span {} );

    builder.storage_live( local, Span {} );
    builder.assign( builder.place( local ), use( f.one() ), Span {} );
    builder.drop( builder.place( local ), Span {} );
    builder.storage_dead( local, Span {} );
    builder.terminate_return( Span {} );

    const Function func = builder.finish();

    REQUIRE( func.statements.size() == 4 );
    REQUIRE( func.statements[0].kind == Statement_kind::Storage_live );
    REQUIRE( func.statements[1].kind == Statement_kind::Assign );
    REQUIRE( func.statements[2].kind == Statement_kind::Drop );
    REQUIRE( func.statements[3].kind == Statement_kind::Storage_dead );

    // A storage marker names the local it is about - verify rejects one that names nothing.
    REQUIRE( func.statements[0].place.local == local );
    REQUIRE( func.statements[3].place.local == local );

    require_verifies( func );
}

TEST_CASE( "builder_makes_a_temporary_per_value", "[ir][builder]" )
{
    Fixture f;
    Builder builder( Node_id { 1 }, f.i32(), Span {} );

    // `1 + 1`, in the three-address form the whole IR is written in.
    const Local_id product = builder.into_temp( binary( Token_kind::Plus, f.one(), f.one(), f.i32() ), f.i32(), Span {} );

    builder.assign( builder.place( Local_id { 0 } ), use( copy( builder.place( product ), f.i32() ) ), Span {} );
    builder.terminate_return( Span {} );

    const Function func = builder.finish();

    REQUIRE( product == Local_id { 1 } );
    REQUIRE( func.statements.size() == 2 );
    REQUIRE( func.statements[0].place.local == product );
    REQUIRE( func.statements[0].value.kind == Rvalue_kind::Binary );

    require_verifies( func );
}

TEST_CASE( "builder_records_call_arguments_contiguously", "[ir][builder]" )
{
    Fixture f;
    Builder builder( Node_id { 1 }, f.i32(), Span {} );

    const Operand arguments[] = { f.one(), f.one(), f.one() };
    const u32     first       = builder.add_operands( arguments );

    builder.into_temp( call( Node_id { 2 }, first, 3, f.i32() ), f.i32(), Span {} );
    builder.terminate_return( Span {} );

    const Function func = builder.finish();

    REQUIRE( first == 0 );
    REQUIRE( func.operands.size() == 3 );
    REQUIRE( func.statements[0].value.argument_count == 3 );

    require_verifies( func );
}

// The two shapes a lowerer builds, and the reason add_block does not switch to the block it makes:
// a terminator needs the id of a block that does not exist yet.
TEST_CASE( "builder_builds_the_control_flow_shapes", "[ir][builder]" )
{
    SECTION( "an if, with both arms joining" )
    {
        Fixture f;
        Builder builder( Node_id { 1 }, f.i32(), Span {} );

        const Local_id condition = builder.into_temp( use( f.yes() ), f.boolean(), Span {} );

        const Block_id then_block = builder.add_block();
        const Block_id else_block = builder.add_block();
        const Block_id join       = builder.add_block();

        builder.terminate_branch( copy( builder.place( condition ), f.boolean() ), then_block, else_block, Span {} );

        builder.switch_to( then_block );
        builder.assign( builder.place( Local_id { 0 } ), use( f.one() ), Span {} );
        builder.terminate_goto( join, Span {} );

        builder.switch_to( else_block );
        builder.assign( builder.place( Local_id { 0 } ), use( f.one() ), Span {} );
        builder.terminate_goto( join, Span {} );

        builder.switch_to( join );
        builder.terminate_return( Span {} );

        const Function func = builder.finish();

        REQUIRE( func.blocks.size() == 4 );
        REQUIRE( func.blocks[0].terminator.kind == Terminator_kind::Branch );
        REQUIRE( func.blocks[0].terminator.targets[0] == then_block );
        REQUIRE( func.blocks[0].terminator.targets[1] == else_block );

        require_verifies( func );
    }

    // A back edge, which is what makes verify's reachability walk need a seen set rather than
    // recursion.
    SECTION( "a while, with a back edge to the header" )
    {
        Fixture f;
        Builder builder( Node_id { 1 }, f.i32(), Span {} );

        const Block_id header = builder.add_block();
        const Block_id body   = builder.add_block();
        const Block_id exit   = builder.add_block();

        builder.terminate_goto( header, Span {} );

        builder.switch_to( header );

        const Local_id condition = builder.into_temp( use( f.yes() ), f.boolean(), Span {} );

        builder.terminate_branch( copy( builder.place( condition ), f.boolean() ), body, exit, Span {} );

        builder.switch_to( body );
        builder.terminate_goto( header, Span {} );

        builder.switch_to( exit );
        builder.terminate_return( Span {} );

        const Function func = builder.finish();

        REQUIRE( func.blocks[body.v].terminator.targets[0] == header );

        require_verifies( func );
    }
}

} // namespace keel
#endif
