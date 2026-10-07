// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

// The unit-test fixture. One `Typed` runs lex, parse, resolve and type_check over a source string,
// with the shipped prelude, and answers questions about the result, so every class in sema/ asks
// them the same way. Included only from inside `#ifdef ENABLE_UNIT_TESTS`.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "common/interner.h"
#include "common/source_manager.h"
#include "common/temp_dir.h"
#include "lex/lexer.h"
#include "parse/loader.h"
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
        : Typed( source, prelude_source() )
    {
    }

    // `prelude` in place of the shipped one, which is empty of functions until M9 adds `print`.
    Typed( std::string_view source, std::string_view prelude )
    {
        file_          = sm_.add_file( "t.kl", std::string( source ) );
        Program loaded = load_program( file_, sm_, interner_, literal_pool_, diags_, {}, prelude );
        ast_           = std::move( loaded.ast );
        const auto res = resolve( ast_, sm_, interner_, diags_, loaded.imports );
        earlier_       = diags_.error_count();
        types_         = type_check( ast_, res, literal_pool_, sm_, interner_, diags_ );
    }

    // A program on disk: `main.kl` and its files beside it, with the package `kl` under `kl/`.
    Typed( std::initializer_list<std::pair<const char*, std::string_view>> files, std::string_view prelude )
    {
        dir_.emplace();

        for( const auto& [name, text] : files )
        {
            dir_->write( name, text );
        }

        const std::optional<File_id> input = sm_.load_file( dir_->path / "main.kl" );

        REQUIRE( input.has_value() );
        file_ = *input;

        const Package kl { .name = "kl", .root = dir_->path / "kl" };
        Program       loaded = load_program( file_, sm_, interner_, literal_pool_, diags_, std::span( &kl, 1 ), prelude );
        ast_                 = std::move( loaded.ast );
        const auto res       = resolve( ast_, sm_, interner_, diags_, loaded.imports );
        earlier_             = diags_.error_count();
        types_               = type_check( ast_, res, literal_pool_, sm_, interner_, diags_ );
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

    // Where the `index`th call in the input file went, as `file:line` of the callable it chose -
    // `t:2`, `geom:1` or `<prelude>:3` - or empty when it chose nothing.
    std::string chose( std::size_t index ) const
    {
        for( u32 i = 0; i < ast_.node_count(); ++i )
        {
            const Node_id id { i };

            if( ast_.kind( id ) != Node_kind::Call_expr || ast_.span( id ).file != file_ || index-- != 0 )
            {
                continue;
            }

            const Node_id callable = types_.callee_of( id );
            if( !callable.is_valid() )
            {
                return {};
            }

            const Span span = ast_.span( callable );
            return std::filesystem::path( sm_.file( span.file ).path ).stem().string() + ":" +
                   std::to_string( sm_.line_col( span.file, span.start ).line );
        }

        FAIL( "the input file has no such call" );
        return {};
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
    std::optional<Temp_dir> dir_; // first, so the files outlive everything read from them
    Source_manager          sm_;
    Interner                interner_;
    Literal_pool            literal_pool_;
    Diagnostics             diags_;
    File_id                 file_;
    Ast                     ast_;
    Types                   types_;
    std::size_t             earlier_ = 0;
};

} // namespace
} // namespace keel
