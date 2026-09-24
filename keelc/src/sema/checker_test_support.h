#pragma once

// The unit-test fixture. One `Typed` runs lex, parse, resolve and type_check over a source string
// and answers questions about the result, so every class in sema/ asks them the same way. Included
// only from inside `#ifdef ENABLE_UNIT_TESTS`.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "common/interner.h"
#include "common/source_manager.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/type_checker.h"

namespace keel
{
namespace
{

class Typed
{
public:
    explicit Typed( std::string_view source )
    {
        file_          = sm_.add_file( "t.kl", std::string( source ) );
        ast_           = parse( lex( file_, sm_, interner_, literal_pool_, diags_ ), sm_, diags_ );
        const auto res = resolve( ast_, sm_, interner_, diags_ );
        earlier_       = diags_.error_count();
        types_         = type_check( ast_, res, literal_pool_, sm_, interner_, diags_ );
    }

    // Errors from type checking only, so a fixture with a deliberate parse or name error still
    // says something useful about the types.
    std::size_t errors() const
    {
        return diags_.error_count() - earlier_;
    }

    bool clean() const
    {
        return diags_.error_count() == 0;
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

    // The spelling of a node's type, or "<none>" when it was never typed.
    std::string_view type_name( Node_id id ) const
    {
        const Type_id type = types_.type_of( id );
        return type.is_valid() ? types_.table().name( type ) : "<none>";
    }

    Node_id child( Node_id parent, std::size_t index ) const
    {
        return ast_.children( parent )[index];
    }

    std::optional<Constant_value> constant_of( Node_id node ) const
    {
        return types_.constant_of( node );
    }

    // Non-const because a question about an instance substitutes, and substituting interns.
    Types& types()
    {
        return types_;
    }

    const Types& types() const
    {
        return types_;
    }

    const Ast& ast() const
    {
        return ast_;
    }

    Node_id nth( Node_kind kind, std::size_t index ) const
    {
        for( u32 i = 0; i < ast_.node_count(); ++i )
        {
            const Node_id id { i };
            if( ast_.kind( id ) == kind && index-- == 0 )
            {
                return id;
            }
        }
        return Node_id {};
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Literal_pool   literal_pool_;
    Diagnostics    diags_;
    File_id        file_;
    Ast            ast_;
    Types          types_;
    std::size_t    earlier_ = 0;
};

} // namespace
} // namespace keel
