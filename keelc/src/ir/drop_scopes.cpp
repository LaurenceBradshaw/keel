// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/drop_scopes.h"
#include <algorithm>
#include <cassert>

namespace keel
{

void drop_place( Builder& builder, const Ast& ast, Types& types, Place place, Type_id type, Span span, bool replacing )
{
    if( !instance_owns( ast, types.table(), type, types.recorded() ) )
    {
        return;
    }

    // Which payload is live is known only at run time, so the drop is a call to the enum's
    // synthesised destructor rather than an expansion here (D30); a union's and an array's likewise,
    // and an interface's goes through its vtable.
    if( types.table().is_enum( type ) || types.table().is_union( type ) || types.table().is_interface( type ) ||
        types.table().is_array( type ) )
    {
        builder.drop( place, span, replacing );
        return;
    }

    const Node_id decl = types.table().get( type ).declaration;

    if( ast.has_destructor( decl ) )
    {
        // The arguments `Spelling::destructor_of` mangles with, so a pass finds the function it calls.
        const std::span<const Type_id> arguments = types.table().get( type ).arguments;
        builder.drop( place, span, replacing, ast.destructor( decl ), { arguments.begin(), arguments.end() } );
    }

    // Reverse declaration order.
    const auto members = ast.members( decl );
    for( std::size_t i = members.size(); i > 0; --i )
    {
        const Node_id member = members[i - 1];

        if( ast.kind( member ) == Node_kind::Field_decl )
        {
            // Through the instance being dropped, not the enclosing function: the field of a
            // `Box<Buffer>` is declared `T`, and only this instance says what that is.
            drop_place(
                builder,
                ast,
                types,
                builder.field( place, member ),
                field_type( ast, types.table(), type, member, types.recorded() ),
                span,
                replacing
            );
        }
    }
}

Drop_scopes::Drop_scopes( Builder& builder, const Ast& ast, Types& types )
    : builder_( builder ),
      ast_( ast ),
      types_( types )
{
}

void Drop_scopes::own_temporary( Local_id local )
{
    if( instance_owns( ast_, types_.table(), builder_.type_of( local ), types_.recorded() ) )
    {
        temporaries_.push_back( local );
    }
}

std::size_t Drop_scopes::mark() const
{
    return temporaries_.size();
}

void Drop_scopes::one_path_since( std::size_t mark )
{
    for( std::size_t i = mark; i < temporaries_.size(); ++i )
    {
        builder_.mark_one_path( temporaries_[i] );
    }
}

void Drop_scopes::adopt_into_scope( Local_id local )
{
    const auto held = std::find( temporaries_.begin(), temporaries_.end(), local );

    if( held != temporaries_.end() )
    {
        scope_locals_.push_back( *held );
        temporaries_.erase( held );
    }
}

void Drop_scopes::adopt_all_into_scope()
{
    scope_locals_.insert( scope_locals_.end(), temporaries_.begin(), temporaries_.end() );
    temporaries_.clear();
}

void Drop_scopes::begin_statement() const
{
    assert( temporaries_.empty() && "a statement left an owning temporary undropped" );
}

void Drop_scopes::end_statement( Span span )
{
    end_since( 0, span );
}

void Drop_scopes::end_since( std::size_t mark, Span span )
{
    for( std::size_t i = temporaries_.size(); i > mark; --i )
    {
        drop( temporaries_[i - 1], span );
    }
    for( std::size_t i = mark; i < temporaries_.size(); ++i )
    {
        builder_.storage_dead( temporaries_[i], span );
    }
    temporaries_.resize( mark );
}

void Drop_scopes::drop_built( Span span )
{
    for( std::size_t i = temporaries_.size(); i > 0; --i )
    {
        drop( temporaries_[i - 1], span );
    }
}

void Drop_scopes::add_to_scope( Local_id local )
{
    scope_locals_.push_back( local );
}

u32 Drop_scopes::depth() const
{
    return narrow_cast<u32>( marks_.size() );
}

void Drop_scopes::push_scope()
{
    marks_.push_back( narrow_cast<u32>( scope_locals_.size() ) );
}

void Drop_scopes::pop_scope( Span span )
{
    unwind_to( depth() - 1, span );
    scope_locals_.resize( marks_.back() );
    marks_.pop_back();
}

void Drop_scopes::unwind_to( u32 depth, Span span )
{
    // A break or return already left this block; nothing after its terminator can run.
    if( builder_.is_terminated() )
    {
        return;
    }

    for( std::size_t i = scope_locals_.size(); i > marks_[depth]; --i )
    {
        drop( scope_locals_[i - 1], span );
        builder_.storage_dead( scope_locals_[i - 1], span );
    }
}

void Drop_scopes::drop( Local_id local, Span span )
{
    drop_place( builder_, ast_, types_, builder_.place( local ), builder_.type_of( local ), span, false );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

struct Dropping : Compiled
{
    Dropping()
        : Compiled( "class Owner { i32 t; ~Owner() { } };\ni32 main() { return 0; }\n", Stage::Lowered )
    {
    }

    Type_id owner()
    {
        for( const Node_id decl : ast.declarations( ast.root() ) )
        {
            if( interner.text( ast.name( decl ) ) == "Owner" )
            {
                return types.table().declared( decl );
            }
        }

        return {};
    }

    static std::ptrdiff_t count( const Function& function, Statement_kind kind )
    {
        return std::ranges::count_if( function.statements, [&]( const Statement& s ) { return s.kind == kind; } );
    }
};

} // namespace

TEST_CASE( "drop_scopes_keeps_the_drop_protocol", "[ir][drop_scopes]" )
{
    Dropping p;
    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Type_id owner   = p.owner();
    const Type_id integer = p.types.table().integer( 32, true );
    REQUIRE( owner.is_valid() );

    Builder     builder( Node_id {}, p.types.table().builtin( Type_kind::Void ), Span {} );
    Drop_scopes drops( builder, p.ast, p.types );

    SECTION( "a statement drops only its owning temporaries, once" )
    {
        drops.own_temporary( builder.add_local( owner, Span {} ) );
        drops.own_temporary( builder.add_local( integer, Span {} ) );
        drops.end_statement( Span {} );
        drops.begin_statement();

        REQUIRE( Dropping::count( builder.finish(), Statement_kind::Drop ) == 1 );
    }

    SECTION( "a temporary built since a mark is one-path" )
    {
        drops.own_temporary( builder.add_local( owner, Span {} ) );
        const std::size_t mark   = drops.mark();
        const Local_id    branch = builder.add_local( owner, Span {} );
        drops.own_temporary( branch );
        drops.one_path_since( mark );
        drops.end_statement( Span {} );

        const Function function = builder.finish();
        REQUIRE( function.one_path_temporaries == std::vector<Local_id> { branch } );
    }

    SECTION( "an adopted temporary is dropped by its scope, not the statement" )
    {
        drops.push_scope();
        const Local_id held = builder.add_local( owner, Span {} );
        drops.own_temporary( held );
        drops.adopt_into_scope( held );
        drops.end_statement( Span {} );

        Builder copy = builder;
        REQUIRE( Dropping::count( copy.finish(), Statement_kind::Drop ) == 0 );

        drops.pop_scope( Span {} );
        REQUIRE( Dropping::count( builder.finish(), Statement_kind::Drop ) == 1 );
    }

    SECTION( "unwinding drops above the depth and keeps the scopes" )
    {
        drops.push_scope();
        drops.add_to_scope( builder.add_local( owner, Span {} ) );
        drops.push_scope();
        drops.add_to_scope( builder.add_local( owner, Span {} ) );
        drops.unwind_to( 1, Span {} );

        REQUIRE( drops.depth() == 2 );
        REQUIRE( Dropping::count( builder.finish(), Statement_kind::Drop ) == 1 );
    }
}

} // namespace keel
#endif
