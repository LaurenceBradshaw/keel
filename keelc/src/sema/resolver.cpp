// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/resolver.h"
#include <fmt/format.h>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include "sema/reporter.h"
#include "sema/type.h"

namespace keel
{
namespace
{

class Resolver
{
public:
    Resolver( const Ast& ast, const Source_manager& sm, const Interner& interner, Diagnostics& diags, const Imports& imports )
        : ast_( ast ),
          sm_( sm ),
          interner_( interner ),
          reporter_( sm, diags ),
          imports_( imports )
    {
    }

    Resolution run();

private:
    enum class Scope_kind
    {
        Transparent,
        Barrier
    };

    struct Scope
    {
        std::unordered_map<Symbol_id, Node_id> names;
        Scope_kind                             kind = Scope_kind::Transparent;
    };

    void visit( Node_id id );

    void push_scope( Scope_kind kind = Scope_kind::Transparent );
    void pop_scope();

    void    declare( Scope& scope, Symbol_id name, Node_id decl );
    void    bind( Node_id use, Node_id decl );
    bool    chain_overload( Node_id existing, Node_id added );
    Node_id lookup( Symbol_id name, Node_id use );
    Node_id lookup_in( Symbol_id package, Symbol_id name, Node_id use );
    Node_id lookup_qualified( Node_id package, Node_id use );
    Node_id visible_from( Node_id use, Node_id head );
    bool    refuse_builtin_name( Symbol_id name, Node_id decl );
    void    refuse_unimported( Node_id use, Node_id decl );
    void    refuse_other_package( Node_id use, Node_id decl );

    const Ast& ast_;

    const Source_manager& sm_;
    const Interner&       interner_;
    sema::Reporter        reporter_;
    const Imports&        imports_;

    std::vector<Node_id> bindings_;
    std::vector<bool>    unresolved_;
    std::vector<Node_id> next_overload_;
    std::vector<Scope>   scopes_;

    // The enclosing aggregate's field names, for D19's member clause. Empty outside a member body,
    // so the check it drives costs one lookup per declaration everywhere else.
    std::unordered_set<Symbol_id> current_fields_;

    std::unordered_map<Symbol_id, Scope> packages_;
};

Resolution Resolver::run()
{
    bindings_.assign( ast_.node_count(), Node_id {} );
    unresolved_.assign( ast_.node_count(), false );
    next_overload_.assign( ast_.node_count(), Node_id {} );

    // Two passes at file scope: every top-level declaration is collected before any body is
    // resolved, which is what makes recursion and mutual recursion work with no forward
    // declarations. Inside a function, scopes are populated as the walk proceeds, so using a local
    // before its declaration stays an error.

    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( decl ) == Node_kind::Function_decl || ast_.kind( decl ) == Node_kind::Var_decl ||
            ast_.kind( decl ) == Node_kind::Enum_decl || is_aggregate( ast_.kind( decl ) ) ||
            ( ast_.kind( decl ) == Node_kind::Error && Symbol_id { ast_.aux( decl ) }.is_valid() ) )
        {
            const Symbol_id name { ast_.aux( decl ) };

            // Otherwise `kl::x` could name a member of the type as well as a declaration of the package.
            if( ( ast_.kind( decl ) == Node_kind::Enum_decl || is_aggregate( ast_.kind( decl ) ) ) &&
                imports_.is_package( name ) )
            {
                reporter_.error_at(
                    ast_.name_span( decl ),
                    fmt::format( "`{}` is the name of a package", interner_.text( name ) ),
                    "a type may not take one"
                );
            }

            Scope& scope = packages_[imports_.package_of( ast_.span( decl ).file )];
            scope.kind   = Scope_kind::Barrier;
            declare( scope, name, decl );
        }
    }

    visit( ast_.root() );

    return Resolution( std::move( bindings_ ), std::move( unresolved_ ), std::move( next_overload_ ), imports_ );
}

void Resolver::visit( Node_id id )
{
    if( !id.is_valid() ) // auto's missing type, an absent else, empty for clauses
    {
        return;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Error:
        return; // already reported; resolving inside it only cascades
    case Node_kind::Source_file:
        for( const Node_id decl : ast_.children( id ) )
        {
            // Declared already by the pre-pass. Going through the Var_decl case would declare it a
            // second time and report it against itself. Functions and structs do not hit this
            // because their cases do not declare - only Var_decl does.
            if( ast_.kind( decl ) == Node_kind::Var_decl )
            {
                visit( ast_.child( decl, 0 ) ); // the type annotation
                visit( ast_.child( decl, 1 ) ); // the initialiser
                continue;
            }

            visit( decl );
        }
        return;
    case Node_kind::Block:
        push_scope();
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        pop_scope();
        return;
    case Node_kind::Destructor_decl:
    case Node_kind::Constructor_decl:
    case Node_kind::Method_decl:
    case Node_kind::Function_decl:
        push_scope( Scope_kind::Barrier );
        visit( ast_.child( id, 3 ) ); // type parameters, before anything that can name one
        visit( ast_.child( id, 0 ) ); // return type; invalid for Destructor_decl
        visit( ast_.child( id, 1 ) ); // param list
        visit( ast_.child( id, 2 ) ); // body
        pop_scope();
        return;
    case Node_kind::Param_decl:
        visit( ast_.child( id, 0 ) ); // the type annotation, which may name a struct

        // A function type's parameters are nameless, and may be at file scope with no scope open.
        if( Symbol_id { ast_.aux( id ) }.is_valid() )
        {
            declare( scopes_.back(), Symbol_id { ast_.aux( id ) }, id );
        }
        return;
    case Node_kind::Type_param_decl:
        declare( scopes_.back(), Symbol_id { ast_.aux( id ) }, id );
        return;
    case Node_kind::For_stmt:
        push_scope();
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        pop_scope();
        return;
    case Node_kind::Var_decl:
        visit( ast_.child( id, 0 ) ); // type
        visit( ast_.child( id, 1 ) ); // Var_decl arity of 2: type, initialiser
        declare( scopes_.back(), Symbol_id { ast_.aux( id ) }, id );
        return;
    case Node_kind::Name_expr:
    {
        const Symbol_id name { ast_.aux( id ) };
        const Node_id   decl = lookup( name, id );

        if( decl.is_valid() )
        {
            bind( id, decl );
        }
        else if( is_builtin_type_name( interner_.text( name ) ) )
        {
            reporter_.error_at( ast_.span( id ), fmt::format( "`{}` is a type, not a value", interner_.text( name ) ) );
        }
        else
        {
            reporter_.error_at( ast_.span( id ), fmt::format( "`{}` is not declared", interner_.text( name ) ) );
        }

        if( !ast_.children( id ).empty() )
        {
            visit( ast_.child( id, 0 ) ); // type arguments, if any
        }

        return;
    }
    case Node_kind::Path_expr:
    {
        const Node_id qualifier = ast_.child( id, 0 );
        if( ast_.kind( qualifier ) == Node_kind::Name_expr && ast_.children( qualifier ).empty() &&
            imports_.is_package( Symbol_id { ast_.aux( qualifier ) } ) )
        {
            bindings_[id.v] = lookup_qualified( qualifier, id );

            if( ast_.children( id ).size() > 1 )
            {
                visit( ast_.child( id, 1 ) ); // type arguments
            }

            return;
        }

        if( ast_.kind( qualifier ) == Node_kind::Name_expr && ast_.children( qualifier ).empty() &&
            is_builtin_type_name( interner_.text( Symbol_id { ast_.aux( qualifier ) } ) ) )
        {
            reporter_.error_at(
                ast_.span( qualifier ),
                fmt::format(
                    "`{}` is a builtin type, and has no members", interner_.text( Symbol_id { ast_.aux( qualifier ) } )
                )
            );

            if( ast_.children( id ).size() > 1 )
            {
                visit( ast_.child( id, 1 ) );
            }

            return;
        }

        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }

        if( ast_.kind( qualifier ) == Node_kind::Name_expr && ast_.children( qualifier ).empty() )
        {
            const Node_id name = lookup( Symbol_id { ast_.aux( qualifier ) }, qualifier );
            if( name.is_valid() && is_aggregate( ast_.kind( name ) ) )
            {
                for( const Node_id member : ast_.members( name ) )
                {
                    if( ast_.kind( member ) == Node_kind::Var_decl &&
                        Symbol_id { ast_.aux( member ) } == Symbol_id { ast_.aux( id ) } )
                    {
                        bindings_[id.v] = member;
                        return;
                    }
                }
            }
        }

        return;
    }
    case Node_kind::Named_type:
    {
        // `kl::Point`, whose one child is the package. A miss there is reported, since nothing
        // later can tell a qualified name from a misspelt builtin.
        if( !ast_.children( id ).empty() )
        {
            bindings_[id.v] = lookup_qualified( ast_.child( id, 0 ), id );
            return;
        }

        const Node_id decl = lookup( Symbol_id { ast_.aux( id ) }, id );
        if( decl.is_valid() )
        {
            bind( id, decl );
        }

        return; // Not an error when absent
    }
    case Node_kind::Struct_literal:
    {
        const Symbol_id name { ast_.aux( id ) };

        if( ast_.initialisers( id ).size() != ast_.children( id ).size() )
        {
            bindings_[id.v] = lookup_qualified( ast_.child( id, 0 ), id );
        }
        else if( const Node_id decl = lookup( name, id ); decl.is_valid() )
        {
            bind( id, decl );
        }
        else
        {
            reporter_.error_at( ast_.type_name_span( id ), fmt::format( "`{}` is not declared", interner_.text( name ) ) );
        }

        // Explicit rather than falling into default: the initialiser values are ordinary
        // expressions and still need resolving, and a `return` added here for tidiness would
        // silently stop that happening.
        for( const Node_id init : ast_.initialisers( id ) )
        {
            visit( init );
        }

        return;
    }
    case Node_kind::Case_arm:
    {
        // The arm is the scope, not its body: a pattern's bindings are written *outside* the
        // block - `case Shape::Circle( r ):` - and have to be in scope inside it. One scope over
        // both is what makes `r` visible in the body and invisible in the next arm.
        push_scope();

        const std::span<const Node_id> parts = ast_.children( id );

        for( const Node_id label : parts.subspan( 0, parts.size() - 1 ) )
        {
            if( ast_.kind( label ) != Node_kind::Variant_pattern )
            {
                visit( label );
                continue;
            }

            // The path resolves like any other; the bindings are declarations rather than uses.
            const std::span<const Node_id> pattern = ast_.children( label );

            visit( pattern[0] );

            for( const Node_id binding : pattern.subspan( 1 ) )
            {
                declare( scopes_.back(), Symbol_id { ast_.aux( binding ) }, binding );
            }
        }

        visit( parts.back() );
        pop_scope();
        return;
    }
    case Node_kind::Enum_decl:
    {
        // Child 0 is the underlying type and is invalid when unwritten, so it cannot go through
        // the default walk - visit() asserts on an invalid id, which is the same convention
        // Var_decl follows for its two optional children.
        //
        // The variant *names* are deliberately not declared. D30 gives them enum-class scoping, so
        // they enter no lexical scope at all: `Colour::Red` is looked up against the enum's own
        // type in the checker, exactly as a field name is, and a bare `Red` stays undeclared.
        //
        // Their payload fields still have to be *visited*, though: `Circle( Point centre )` names a
        // type, and nothing else will resolve it.
        const std::span<const Node_id> children = ast_.children( id );

        push_scope( Scope_kind::Barrier );
        visit( ast_.type_param_list( id ) );

        if( !is_builtin_type_name( interner_.text( Symbol_id { ast_.aux( id ) } ) ) )
        {
            scopes_.back().names.emplace( Symbol_id { ast_.aux( id ) }, id );
        }

        if( children[1].is_valid() )
        {
            visit( children[1] );
        }

        for( const Node_id variant : children.subspan( 2 ) )
        {
            for( const Node_id field : ast_.children( variant ) )
            {
                visit( ast_.child( field, 0 ) );
            }
        }

        pop_scope();
        return;
    }
    case Node_kind::Class_decl:
    case Node_kind::Struct_decl:
    {
        // Fields are a member namespace, not a lexical one, so for the struct body itself they are
        // deliberately *not* in scopes_: doing that would put `Point` in scope as a field and let
        // it shadow the type `Point` while the fields' own annotations are being resolved. A local
        // map gives the duplicate check without the pollution, and without D19's shadowing walk,
        // which does not apply between members.
        //
        // A destructor body is the exception, and gets them in a scope of its own below - no
        // annotation is ever inside it, so the hazard above cannot arise there. Two passes rather
        // than one, because declaration order carries meaning inside a function body and none
        // between members: a field written after the destructor must still be visible in it.
        // The type parameters, before anything that can name one. A barrier for the reason a
        // function's is: `T` belongs to this declaration and to nothing outside it, and a scope
        // that carried outer names in would let a top-level `T` be found from a member body.
        push_scope( Scope_kind::Barrier );
        visit( ast_.type_param_list( id ) );

        if( !is_builtin_type_name( interner_.text( Symbol_id { ast_.aux( id ) } ) ) )
        {
            scopes_.back().names.emplace( Symbol_id { ast_.aux( id ) }, id );
        }

        std::unordered_map<u32, Node_id> members;

        for( const Node_id field : ast_.members( id ) )
        {
            if( ast_.kind( field ) != Node_kind::Field_decl && ast_.kind( field ) != Node_kind::Var_decl )
            {
                continue;
            }

            if( ast_.kind( field ) == Node_kind::Field_decl )
            {
                visit( field ); // the field's type annotation still resolves through the normal path
            }
            else
            {
                visit( ast_.child( field, 0 ) );
                visit( ast_.child( field, 1 ) );
            }

            const Symbol_id name { ast_.aux( field ) };

            if( !name.is_valid() )
            {
                continue; // the parser already reported the missing name
            }

            if( refuse_builtin_name( name, field ) )
            {
                continue;
            }

            const auto [it, inserted] = members.try_emplace( name.v, field );

            if( !inserted )
            {
                reporter_.error_at(
                    ast_.name_span( field ),
                    fmt::format(
                        "{}`{}` is already declared",
                        ast_.kind( field ) == Node_kind::Var_decl ? "" : "field ",
                        interner_.text( name )
                    ),
                    reporter_.previous_declaration_note( ast_.span( it->second ) )
                );
            }
        }

        // Methods join the member scope, so a sibling is callable by bare name: `add( by )` rather
        // than `this.add( by )`, which is what C++ does and what any real class needs - a type whose
        // methods have to qualify each other is tiring to write long before it is large.
        //
        // A second loop rather than a branch in the one above, because a method's own annotations
        // are resolved when its body is visited below, not here.
        for( const Node_id member : ast_.members( id ) )
        {
            if( ast_.kind( member ) != Node_kind::Method_decl )
            {
                continue;
            }

            const Symbol_id name { ast_.aux( member ) };

            if( !name.is_valid() )
            {
                continue;
            }

            if( refuse_builtin_name( name, member ) )
            {
                continue;
            }

            const auto [it, inserted] = members.try_emplace( name.v, member );

            if( !inserted && !chain_overload( it->second, member ) )
            {
                reporter_.error_at(
                    ast_.name_span( member ),
                    fmt::format( "`{}` is already declared", interner_.text( name ) ),
                    reporter_.previous_declaration_note( ast_.span( it->second ) )
                );
            }
        }

        push_scope();

        for( const auto& [name, decl] : members )
        {
            // Straight into the scope rather than through declare(): members are not lexical,
            // duplicates were already reported above, and declare()'s walk reads a barrier scope's
            // names before noticing it is a barrier - so it would call a field that happens to
            // share a top-level name a shadow. The set beside it is what D19's member clause reads.
            scopes_.back().names.emplace( Symbol_id { name }, decl );
            current_fields_.insert( Symbol_id { name } );
        }

        for( const Node_id member : ast_.members( id ) )
        {
            if( is_function_like( ast_.kind( member ) ) )
            {
                visit( member );
            }
        }

        // clear() rather than restoring an enclosing set, because an aggregate cannot nest inside
        // another - parse_aggregate_decl's member loop accepts only fields and destructors. If that
        // ever changes this has to become a save and restore.
        current_fields_.clear();
        pop_scope();

        pop_scope(); // the type parameters

        return;
    }

    default:
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        return;
    }
}

void Resolver::push_scope( Scope_kind kind )
{
    scopes_.emplace_back();
    scopes_.back().kind = kind;
}

void Resolver::pop_scope()
{
    scopes_.pop_back();
}

void Resolver::declare( Scope& scope, Symbol_id name, Node_id decl )
{
    if( !name.is_valid() )
    {
        return;
    }

    // An error symbol never reports: it yields to any real declaration of its name, and under a
    // builtin's name it would hide the type.
    if( ast_.kind( decl ) == Node_kind::Error )
    {
        if( !is_builtin_type_name( interner_.text( name ) ) )
        {
            scope.names.try_emplace( name, decl );
        }

        return;
    }

    if( refuse_builtin_name( name, decl ) )
    {
        return;
    }

    const auto [it, inserted] = scope.names.try_emplace( name, decl );

    if( !inserted && ast_.kind( it->second ) == Node_kind::Error )
    {
        it->second = decl;
        return;
    }

    if( !inserted )
    {
        // Two functions of one name are an overload set, not a redeclaration. Whether their
        // signatures actually differ is a question about types, which this pass does not have -
        // the checker asks it once every signature is known.
        if( chain_overload( it->second, decl ) )
        {
            return;
        }

        reporter_.error_at(
            ast_.name_span( decl ),
            fmt::format( "`{}` is already declared in this scope", interner_.text( name ) ),
            reporter_.previous_declaration_note( ast_.span( it->second ) )
        );
        return;
    }

    // D19: a field may be shadowed by a parameter, never by a local. A parameter naming the field
    // it initialises is forced and idiomatic; a local collision is chosen, and is exactly the
    // silent-wrong-value hazard this rule exists to kill. Checked here rather than through the
    // scope walk because the field scope sits below the member body's barrier - it has to, so a
    // parameter wins the lookup - and the walk stops at that barrier.
    if( ast_.kind( decl ) != Node_kind::Param_decl && current_fields_.contains( name ) )
    {
        reporter_.error_at(
            ast_.name_span( decl ),
            fmt::format( "`{}` shadows a field", interner_.text( name ) ),
            "a local may not take a field's name"
        );
        return;
    }

    // D19. The declaration stays in the map even when it shadows, so later uses bind to the
    // variable actually written rather than to the outer one.
    if( scope.kind == Scope_kind::Barrier )
    {
        return;
    }

    for( auto s = scopes_.rbegin() + 1; s != scopes_.rend(); ++s )
    {
        const auto found = s->names.find( name );

        if( found != s->names.end() )
        {
            reporter_.error_at(
                ast_.name_span( decl ),
                fmt::format( "`{}` shadows an outer declaration", interner_.text( name ) ),
                reporter_.previous_declaration_note( ast_.span( found->second ) )
            );
            return;
        }

        if( s->kind == Scope_kind::Barrier )
        {
            return;
        }
    }
}

// Appended at the end of the chain, so walking it gives declaration order - which is what the
// duplicate diagnostic needs to name the *earlier* one.
// A use of an error symbol is unresolved and silent: the declaration failed, and was reported.
void Resolver::bind( Node_id use, Node_id decl )
{
    if( ast_.kind( decl ) == Node_kind::Error )
    {
        unresolved_[use.v] = true;
        return;
    }

    bindings_[use.v] = decl;
}

bool Resolver::chain_overload( Node_id existing, Node_id added )
{
    const Node_kind kind = ast_.kind( existing );

    if( kind != ast_.kind( added ) || ( kind != Node_kind::Function_decl && kind != Node_kind::Method_decl ) )
    {
        return false;
    }

    Node_id last = existing;

    while( next_overload_[last.v].is_valid() )
    {
        last = next_overload_[last.v];
    }

    next_overload_[last.v] = added;

    return true;
}

Node_id Resolver::lookup( Symbol_id name, Node_id use )
{
    if( !name.is_valid() )
    {
        return Node_id {};
    }

    for( auto it = scopes_.rbegin(); it != scopes_.rend(); ++it )
    {
        const auto found = it->names.find( name );
        if( found != it->names.end() )
        {
            return visible_from( use, found->second );
        }
    }

    const Symbol_id own = imports_.package_of( ast_.span( use ).file );

    if( const Node_id decl = lookup_in( own, name, use ); decl.is_valid() )
    {
        return decl;
    }

    if( const Node_id decl = lookup_in( imports_.prelude_package(), name, use ); decl.is_valid() )
    {
        return decl;
    }

    // Only a named package can be written as a qualifier, so the program's own is never offered.
    for( const auto& [package, scope] : packages_ )
    {
        if( package == own || !package.is_valid() )
        {
            continue;
        }

        if( const auto found = scope.names.find( name ); found != scope.names.end() )
        {
            refuse_other_package( use, found->second );
            return found->second;
        }
    }

    return Node_id {};
}

// `use` names its declaration through the package `package`, a Name_expr. Invalid when no module of it
// that the use's file imports declares the name: recorded as unresolved, and reported unless the
// package is missing, whose import was reported instead.
Node_id Resolver::lookup_qualified( Node_id package, Node_id use )
{
    const Symbol_id package_name { ast_.aux( package ) };
    const Symbol_id name { ast_.aux( use ) };
    const Node_id   decl = lookup_in( package_name, name, use );

    if( !decl.is_valid() )
    {
        unresolved_[use.v] = true;

        if( !imports_.is_missing( package_name ) )
        {
            reporter_.error_at(
                ast_.span( use ),
                fmt::format(
                    "no module of `{}` that this file imports declares `{}`",
                    interner_.text( package_name ),
                    interner_.text( name )
                )
            );
        }
    }

    return decl;
}

Node_id Resolver::lookup_in( Symbol_id package, Symbol_id name, Node_id use )
{
    const auto scope = packages_.find( package );

    if( scope == packages_.end() )
    {
        return Node_id {};
    }

    const auto found = scope->second.names.find( name );
    return found != scope->second.names.end() ? visible_from( use, found->second ) : Node_id {};
}

Node_id Resolver::visible_from( Node_id use, Node_id head )
{
    const File_id uses_file = ast_.span( use ).file;

    for( Node_id d = head; d.is_valid(); d = next_overload_[d.v] )
    {
        if( imports_.sees( uses_file, ast_.span( d ).file ) )
        {
            return d;
        }
    }

    refuse_unimported( use, head );
    return head;
}

bool Resolver::refuse_builtin_name( Symbol_id name, Node_id decl )
{
    if( is_builtin_type_name( interner_.text( name ) ) )
    {
        reporter_.error_at(
            ast_.name_span( decl ),
            fmt::format( "`{}` is the name of a builtin type", interner_.text( name ) ),
            "a declaration may not take one"
        );
        return true;
    }
    return false;
}

void Resolver::refuse_unimported( Node_id use, Node_id decl )
{
    const File_id     file = ast_.span( decl ).file;
    std::string_view  name = interner_.text( Symbol_id { ast_.aux( decl ) } );
    const std::string module_name =
        qualified( interner_, imports_.package_of( file ), std::filesystem::path( sm_.file( file ).path ).stem().string() );

    reporter_.error_at(
        ast_.span( use ),
        fmt::format( "`{}` is in `{}`, which this file does not import", name, module_name ),
        fmt::format( "write `import {};` at the top of the file", module_name )
    );
}

void Resolver::refuse_other_package( Node_id use, Node_id decl )
{
    const Symbol_id  package = imports_.package_of( ast_.span( decl ).file );
    std::string_view name    = interner_.text( Symbol_id { ast_.aux( decl ) } );

    reporter_.error_at(
        ast_.span( use ),
        fmt::format( "`{}` is in the package `{}`", name, interner_.text( package ) ),
        fmt::format( "write `{}`", qualified( interner_, package, name ) )
    );
}

} // namespace

Resolution
resolve( const Ast& ast, const Source_manager& sm, const Interner& interner, Diagnostics& diags, const Imports& imports )
{
    return Resolver( ast, sm, interner, diags, imports ).run();
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include "common/interner.h"
#include "common/temp_dir.h"
#include "lex/lexer.h"
#include "parse/loader.h"
#include "parse/parser.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <optional>
#include <sstream>
#include <utility>

namespace keel
{
namespace
{

class Resolved
{
public:
    explicit Resolved( std::string_view source )
    {
        file_         = sm_.add_file( "t.kl", std::string( source ) );
        ast_          = parse( lex( file_, sm_, interner_, literal_pool_, diags_ ), sm_, diags_ );
        parse_errors_ = diags_.error_count();
        resolution_   = resolve( ast_, sm_, interner_, diags_ );
    }

    const Ast& ast() const
    {
        return ast_;
    }

    // Errors from resolution only, so a fixture with a deliberate parse error still says something
    // useful about resolution.
    std::size_t errors() const
    {
        return diags_.error_count() - parse_errors_;
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

    Node_id declaration_of( Node_id use ) const
    {
        return resolution_.declaration_of( use );
    }

    // The nth node of a kind, in the order the parser created them.
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

    std::string_view text( Node_id id ) const
    {
        return sm_.text( ast_.span( id ) );
    }

    const Resolution& resolution() const
    {
        return resolution_;
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Literal_pool   literal_pool_;
    Diagnostics    diags_;
    File_id        file_;
    Ast            ast_;
    Resolution     resolution_;
    std::size_t    parse_errors_ = 0;
};

} // namespace

// --- step 1: the walk ---

TEST_CASE( "resolver_walks_without_crashing", "[sema][resolve]" )
{
    // Every shape the parser can produce, including the invalid child slots that `auto`, a missing
    // `else` and empty `for` clauses leave behind.
    const Resolved p( "i32 f( i32 a )\n"
                      "{\n"
                      "    auto x = 1;\n"
                      "    if( a ) { }\n"
                      "    for( ; ; ) { }\n"
                      "    return a;\n"
                      "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.ast().root().is_valid() );
}

// Resolving inside a failed subtree only produces cascades.
TEST_CASE( "resolver_skips_error_subtrees", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { @ return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 0 );
}

// --- step 2: local variables ---

TEST_CASE( "resolver_binds_a_local", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { i32 x = 0; return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id use  = p.nth( Node_kind::Name_expr, 0 );
    const Node_id decl = p.nth( Node_kind::Var_decl, 0 );

    REQUIRE( use.is_valid() );
    REQUIRE( p.declaration_of( use ) == decl );
}

// The test that a resolver binding every name to the first declaration it saw would fail.
TEST_CASE( "resolver_distinguishes_sibling_scopes", "[sema][resolve]" )
{
    const Resolved p( "i32 main()\n"
                      "{\n"
                      "    { i32 x = 1; return x; }\n"
                      "    { i32 x = 2; return x; }\n"
                      "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id first  = p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) );
    const Node_id second = p.declaration_of( p.nth( Node_kind::Name_expr, 1 ) );

    REQUIRE( first.is_valid() );
    REQUIRE( second.is_valid() );
    REQUIRE( first != second );
}

TEST_CASE( "resolver_reports_an_unknown_name", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { return nowhere; }" );

    REQUIRE( p.errors() == 1 );
    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "nowhere" ) != std::string::npos );
}

// Statements are sequential inside a function, unlike at file scope.
TEST_CASE( "resolver_rejects_use_before_declaration_in_a_block", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { return x; i32 x = 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

// The initialiser is resolved before the name enters scope, or `i32 x = x;` refers to itself.
TEST_CASE( "resolver_initialiser_cannot_see_its_own_variable", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { i32 x = x; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

TEST_CASE( "resolver_reports_redeclaration_in_one_scope", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { i32 x = 0; i32 x = 1; return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

// --- step 3: function scope ---

TEST_CASE( "resolver_binds_a_parameter", "[sema][resolve]" )
{
    const Resolved p( "i32 double_it( i32 value ) { return value; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id use   = p.nth( Node_kind::Name_expr, 0 );
    const Node_id param = p.nth( Node_kind::Param_decl, 0 );

    REQUIRE( p.declaration_of( use ) == param );
}

TEST_CASE( "resolver_parameters_are_not_visible_outside", "[sema][resolve]" )
{
    const Resolved p( "i32 f( i32 a ) { return a; }\ni32 g() { return a; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

// The for variable is visible in the condition, the update and the body, but not after the loop.
TEST_CASE( "resolver_scopes_the_for_variable_to_its_loop", "[sema][resolve]" )
{
    SECTION( "visible throughout the loop" )
    {
        const Resolved p( "i32 main() { for( i32 i = 0; i < 3; i++ ) { i32 y = i; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "not visible after it" )
    {
        const Resolved p( "i32 main() { for( i32 i = 0; i < 3; i++ ) { } return i; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// Every scope-opening construct nests, so a name declared inside one is invisible outside it.
// A transparent scope sees the names around it; the pair to
// resolver_transparent_scopes_do_not_leak_outward, which checks the other direction.
TEST_CASE( "resolver_transparent_scopes_see_outward", "[sema][resolve]" )
{
    static const char* sources[] = {
        "i32 main() { i32 x = 1; { return x; } }",
        "i32 main() { i32 x = 1; if( 1 ) { return x; } return 0; }",
        "i32 main() { i32 x = 1; while( 1 ) { return x; } return 0; }",
        "i32 main() { i32 x = 1; for( ; ; ) { return x; } }",
    };

    for( const char* source : sources )
    {
        const Resolved p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.clean() );

        // Not just resolved - resolved to the outer declaration.
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }
}

TEST_CASE( "resolver_transparent_scopes_do_not_leak_outward", "[sema][resolve]" )
{
    static const char* leaks[] = {
        "i32 main() { if( 1 ) { i32 x = 1; } return x; }",
        "i32 main() { while( 1 ) { i32 x = 1; } return x; }",
        "i32 main() { { i32 x = 1; } return x; }",
        "i32 main() { for( ; ; ) { i32 x = 1; } return x; }",
    };

    for( const char* source : leaks )
    {
        const Resolved p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// D19: a declaration may not hide one that is still reachable by the same unqualified name.
TEST_CASE( "resolver_rejects_shadowing", "[sema][resolve]" )
{
    static const char* shadows[] = {
        "i32 main() { i32 x = 1; { i32 x = 2; return x; } }",
        "i32 main() { i32 x = 1; if( 1 ) { i32 x = 2; return x; } return x; }",
        "i32 f( i32 x ) { i32 x = 1; return x; }",
        "i32 main() { i32 i = 0; for( i32 i = 0; ; ) { } return i; }",
    };

    for( const char* source : shadows )
    {
        const Resolved p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "shadows an outer declaration" ) != std::string::npos );
    }
}

TEST_CASE( "resolver_walks_every_enclosing_transparent_scope", "[sema][resolve]" )
{
    SECTION( "each level collides with the one outside it" )
    {
        const Resolved p( "i32 main() { i32 x = 1; { i32 x = 2; { i32 x = 3; } } }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }

    // The scopes between hold no `x`, so a walk that gave up after one level would miss this.
    SECTION( "the collision is more than one level out" )
    {
        const Resolved p( "i32 main() { i32 x = 1; { { { i32 x = 2; } } } }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Each `x` is out of scope again before the next is declared, so nothing is hidden.
    SECTION( "scopes closed before the outer declaration do not collide" )
    {
        const Resolved p( "i32 main() { { { i32 x = 1; } i32 x = 2; } i32 x = 3; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// File scope is a barrier, so adding a top-level function cannot break a body that already uses
// that name for a local.
TEST_CASE( "resolver_allows_a_local_to_reuse_a_file_scope_name", "[sema][resolve]" )
{
    const Resolved p( "i32 count() { return 1; }\ni32 main() { i32 count = 0; return count; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
}

TEST_CASE( "resolver_reports_duplicate_declarations", "[sema][resolve]" )
{
    // Two functions of one name are an overload set, and whether their signatures differ is a
    // question about types this pass cannot ask. The checker reports the pair that does not.
    SECTION( "two functions with one name are chained rather than reported" )
    {
        const Resolved p( "i32 f() { return 1; }\ni32 f() { return 2; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );

        const Node_id first = p.nth( Node_kind::Function_decl, 0 );

        REQUIRE( p.resolution().next_overload( first ) == p.nth( Node_kind::Function_decl, 1 ) );
        REQUIRE_FALSE( p.resolution().next_overload( p.nth( Node_kind::Function_decl, 1 ) ).is_valid() );
    }

    // The chain is in declaration order, which is what lets a diagnostic name the earlier one.
    SECTION( "three of one name chain in order" )
    {
        const Resolved p( "i32 f() { return 1; }\ni32 f() { return 2; }\ni32 f() { return 3; }\n" );

        const Node_id second = p.resolution().next_overload( p.nth( Node_kind::Function_decl, 0 ) );

        REQUIRE( second == p.nth( Node_kind::Function_decl, 1 ) );
        REQUIRE( p.resolution().next_overload( second ) == p.nth( Node_kind::Function_decl, 2 ) );
    }

    // Only two callables of one kind chain: everything else is still a redeclaration.
    SECTION( "a function and a variable of one name" )
    {
        const Resolved p( "i32 f() { return 1; }\ni32 f = 2;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "already declared" ) != std::string::npos );
    }

    SECTION( "two parameters with one name" )
    {
        const Resolved p( "i32 f( i32 a, i32 a ) { return a; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // The message carries the earlier location, since Diagnostics supports only one span per error.
    SECTION( "the message points at the previous declaration" )
    {
        const Resolved p( "i32 main()\n{\n    i32 x = 0;\n    i32 x = 1;\n    return x;\n}\n" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "previous declaration is at: t.kl:3:" ) != std::string::npos );
    }
}

// Names appear in more places than just `return x;` - every one goes through the same lookup.
TEST_CASE( "resolver_binds_names_in_every_position", "[sema][resolve]" )
{
    const Resolved p( "i32 g( i32 a ) { return a; }\n"
                      "i32 main()\n"
                      "{\n"
                      "    i32 y = 0;\n"
                      "    y = y + 1;\n"
                      "    y += g( y );\n"
                      "    y++;\n"
                      "    if( y < 3 ) { y = g( y ); }\n"
                      "    while( y < 9 ) { y++; }\n"
                      "    return y;\n"
                      "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    // Every Name_expr in the file must have resolved to something.
    std::size_t names = 0;
    for( u32 i = 0; i < p.ast().node_count(); ++i )
    {
        const Node_id id { i };
        if( p.ast().kind( id ) != Node_kind::Name_expr )
        {
            continue;
        }

        ++names;
        INFO( "unresolved name: " << p.text( id ) );
        REQUIRE( p.declaration_of( id ).is_valid() );
    }

    REQUIRE( names > 10 );
}

// --- step 4: file scope ---

TEST_CASE( "resolver_binds_a_call", "[sema][resolve]" )
{
    const Resolved p( "i32 helper() { return 1; }\ni32 main() { return helper(); }\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id call = p.nth( Node_kind::Call_expr, 0 );
    const Node_id use  = p.ast().children( call )[0];

    REQUIRE( p.declaration_of( use ) == p.nth( Node_kind::Function_decl, 0 ) );
}

// The pre-pass over top-level declarations is what makes these work. C++ needs a forward
// declaration for the second; Keel does not.
TEST_CASE( "resolver_allows_recursion", "[sema][resolve]" )
{
    SECTION( "self" )
    {
        const Resolved p( "i32 fib( i32 n ) { return fib( n ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "mutual, with no forward declaration" )
    {
        const Resolved p( "i32 even( i32 n ) { return odd( n ); }\ni32 odd( i32 n ) { return even( n ); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a function used before it is declared" )
    {
        const Resolved p( "i32 main() { return later(); }\ni32 later() { return 1; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "resolver_reports_an_unknown_call", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { return missing(); }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

TEST_CASE( "resolver_declares_struct_names_at_file_scope", "[sema][resolve]" )
{
    SECTION( "a struct is usable as a type" )
    {
        const Resolved p( "struct Point { f64 x; }\ni32 f( Point p ) { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    // D18 again: declaration order carries no meaning between top-level declarations, so the
    // struct's name has to be collected in the first pass rather than as the walk reaches it.
    SECTION( "even when declared after the function that uses it" )
    {
        const Resolved p( "i32 f( Point p ) { return 0; }\nstruct Point { f64 x; };\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "a struct name collides with a function name" )
    {
        const Resolved p( "i32 Point() { return 0; }\nstruct Point { f64 x; };\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "an unknown type name is left for the checker, not reported here" )
    {
        const Resolved p( "i32 f( Widget w ) { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 ); // `i32` has no declaration either; silence is the only option
    }
}

// The file scope keeps the first `counter`, but inside the class its name is still the class, so
// its members are not reported against a variable they never named.
TEST_CASE( "resolver_lets_an_aggregate_name_itself_after_a_collision", "[sema][resolve][aggregates]" )
{
    const Resolved p( "i32 counter = 0;\n"
                      "class counter { i32 n; counter() { n = 0; } i32 get() { return n; } };\n"
                      "i32 main() { return counter; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );

    const Ast&    ast           = p.ast();
    const Node_id cls           = p.nth( Node_kind::Class_decl, 0 );
    const auto    receiver_type = [&]( Node_id member )
    { return ast.child( ast.child( ast.child( ast.child( member, 1 ), 0 ), 0 ), 0 ); };

    REQUIRE( p.declaration_of( receiver_type( p.nth( Node_kind::Constructor_decl, 0 ) ) ) == cls );
    REQUIRE( p.declaration_of( receiver_type( p.nth( Node_kind::Method_decl, 0 ) ) ) == cls );
    REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 2 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
}

TEST_CASE( "resolver_reports_duplicate_fields", "[sema][resolve]" )
{
    SECTION( "one message per repeated name, pointing at the first" )
    {
        const Resolved p( "struct Point\n{\n    f64 x;\n    f64 x;\n};\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "field `x` is already declared" ) != std::string::npos );
        REQUIRE( p.rendered().find( "previous declaration is at: t.kl:3:" ) != std::string::npos );
    }

    SECTION( "two repeated names give two messages" )
    {
        const Resolved p( "struct P { f64 x; f64 x; f64 y; f64 y; };" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }

    // Fields are a member namespace: a field may share a name with a local, a function, or a type,
    // and none of those is a collision.
    SECTION( "a field does not collide with anything outside the struct" )
    {
        const Resolved p( "struct Point { f64 x; };\n"
                          "i32 count() { return 0; }\n"
                          "struct Other { f64 x; f64 count; f64 Point; };\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    // The reverse of the same rule: declaring fields into scopes_ would put `Point` in scope as a
    // field and shadow the type of the same name in the very next field.
    SECTION( "a field named after a type does not shadow that type" )
    {
        const Resolved p( "struct Point { f64 x; };\nstruct Holder { f64 Point; Point inner; };\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

// The values inside a struct literal are ordinary expressions. This is the property that breaks
// silently if the Struct_literal case stops visiting its children.
TEST_CASE( "resolver_resolves_inside_struct_literals", "[sema][resolve]" )
{
    SECTION( "the type name binds to its declaration" )
    {
        const Resolved p( "struct Point { f64 x; };\ni32 main() { auto q = Point { 1.0 }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );

        const Node_id literal = p.nth( Node_kind::Struct_literal, 0 );
        REQUIRE( p.declaration_of( literal ) == p.nth( Node_kind::Struct_decl, 0 ) );
    }

    SECTION( "an unknown type is reported" )
    {
        const Resolved p( "i32 main() { auto q = Widget { 1.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and the initialiser values are resolved too" )
    {
        const Resolved p( "struct Point { f64 x; };\ni32 main() { auto q = Point { missing }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`missing` is not declared" ) != std::string::npos );
    }

    SECTION( "a name used in a literal binds to its declaration" )
    {
        const Resolved p( "struct Point { f64 x; };\ni32 main() { f64 v = 1.0; auto q = Point { v }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }
}

// The field name lives in aux precisely so it is never looked up: `p.x` must not send the resolver
// hunting for a variable called `x`.
TEST_CASE( "resolver_does_not_resolve_field_names", "[sema][resolve]" )
{
    const Resolved p( "struct Point { f64 x; };\ni32 f( Point p ) { auto v = p.nonexistent; return 0; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 0 ); // whether the field exists is the checker's question

    const Node_id field = p.nth( Node_kind::Field_expr, 0 );
    REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Param_decl, 0 ) );
    REQUIRE( field.is_valid() );
}

// D18: every top-level declaration is visible throughout the file, so a global is usable above the
// line that declares it, exactly as a function is.
TEST_CASE( "resolver_declares_globals_at_file_scope", "[sema][resolve][globals]" )
{
    SECTION( "a use inside a function resolves to it" )
    {
        const Resolved p( "i32 counter = 1;\ni32 main() { return counter; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }

    SECTION( "even when the function is written above it" )
    {
        const Resolved p( "i32 main() { return counter; }\ni32 counter = 1;\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }

    // The trap: a global is declared once by the file-scope pre-pass, and visit() must not declare
    // it a second time. If it does, every global reports "already declared" against itself.
    SECTION( "a global is not declared twice against itself" )
    {
        const Resolved p( "i32 counter = 1;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.rendered().find( "already declared" ) == std::string::npos );
    }

    SECTION( "several globals coexist" )
    {
        const Resolved p( "i32 first = 1;\ni32 second = 2;\ni32 main() { return first + second; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a global and a function may not share a name" )
    {
        const Resolved p( "i32 thing = 1;\ni32 thing() { return 0; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "already declared" ) != std::string::npos );
    }

    SECTION( "nor may two globals" )
    {
        const Resolved p( "i32 thing = 1;\ni32 thing = 2;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // A local of the same name shadows it, and D19's barrier rules are unchanged by the global
    // living in the file scope.
    SECTION( "a local shadows a global" )
    {
        const Resolved p( "i32 counter = 1;\ni32 main() { i32 counter = 2; return counter; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 1 ) );
    }

    SECTION( "a parameter shadows a global too" )
    {
        const Resolved p( "i32 counter = 1;\ni32 take( i32 counter ) { return counter; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Param_decl, 0 ) );
    }

    SECTION( "and its address can be taken" )
    {
        const Resolved p( "i32 counter = 1;\ni32* get() { return &counter; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // No scope is open at file scope, and a function type's parameters are nameless.
    SECTION( "a global of function type declares nothing for its parameters" )
    {
        const Resolved p( "fn( i32 )->i32 handler;\n"
                          "fn( fn( i32 )->i32, i32 )->i32 applier;\n"
                          "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A destructor body sees the fields unqualified, as C++ does. The scope goes up on entering the
// body and down on leaving it, so no field's own type annotation ever sees it - which is what the
// Class_decl case above warns about when it keeps members out of scopes_.
TEST_CASE( "resolver_puts_fields_in_scope_inside_a_destructor", "[sema][resolve][aggregates]" )
{
    SECTION( "a bare field name binds to the field" )
    {
        const Resolved p( "class Buffer { u8* ptr; ~Buffer() { ptr = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Field_decl, 0 ) );
    }

    // Declaration order carries no meaning between members, only within a function body, so a field
    // written after the destructor is still visible inside it.
    SECTION( "a field declared after the destructor is still visible" )
    {
        const Resolved p( "class Buffer { ~Buffer() { ptr = nullptr; } u8* ptr; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "`this` binds to the synthesised parameter" )
    {
        const Resolved p( "class Buffer { u8* ptr; ~Buffer() { this.ptr = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Param_decl, 0 ) );
    }

    SECTION( "the scope does not leak past the body" )
    {
        const Resolved p( "class Buffer { u8* ptr; ~Buffer() { } };\ni32 main() { ptr = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "one class's fields are not visible in another's destructor" )
    {
        const Resolved p( "class A { u8* only_in_a; ~A() { } };\n"
                          "class B { u8* ptr; ~B() { only_in_a = nullptr; } };\n"
                          "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// D19: members are shadowable by a parameter but never by a local. A destructor has no parameters
// of its own, so only the restrictive half is reachable until constructors arrive.
TEST_CASE( "resolver_rejects_a_local_shadowing_a_field", "[sema][resolve][aggregates]" )
{
    SECTION( "a local may not take a field's name" )
    {
        const Resolved p( "class Buffer { u64 len; ~Buffer() { u64 len = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a different name is fine" )
    {
        const Resolved p( "class Buffer { u64 len; ~Buffer() { u64 count = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // File scope is a barrier, and members do not change that: a field may share a top-level name.
    SECTION( "a field may share a name with a top-level declaration" )
    {
        const Resolved p( "i32 count = 0;\nclass Buffer { u64 count; ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A builtin type's name is reserved: declaring one would shadow the type for the rest of its scope.
TEST_CASE( "resolver_refuses_a_builtin_type_name", "[sema][resolve]" )
{
    const auto refused = []( const Resolved& p, std::string_view name )
    {
        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( fmt::format( "`{}` is the name of a builtin type", name ) ) != std::string::npos );
    };

    SECTION( "a function, and every later use of the type still names the type" )
    {
        const Resolved p( "i32 i32( i32 n ) { return n; }\ni32 main() { i32 x = 1; return x; }" );

        refused( p, "i32" );

        for( std::size_t i = 0; p.nth( Node_kind::Named_type, i ).is_valid(); ++i )
        {
            INFO( i );
            REQUIRE_FALSE( p.declaration_of( p.nth( Node_kind::Named_type, i ) ).is_valid() );
        }
    }

    SECTION( "a local" )
    {
        refused( Resolved( "i32 main() { i32 i32 = 1; i32 y = 2; return y; }" ), "i32" );
    }

    SECTION( "a parameter" )
    {
        refused( Resolved( "i32 f( bool bool ) { return 0; }\ni32 main() { return 0; }" ), "bool" );
    }

    SECTION( "a global" )
    {
        refused( Resolved( "u8 u8 = 1;\ni32 main() { return 0; }" ), "u8" );
    }

    SECTION( "a struct, a class and an enum" )
    {
        refused( Resolved( "struct f64 { i32 x; };\ni32 main() { return 0; }" ), "f64" );
        refused( Resolved( "class void { i32 x; };\ni32 main() { return 0; }" ), "void" );
        refused( Resolved( "enum u16 { A, B };\ni32 main() { return 0; }" ), "u16" );
    }

    // Each also puts its own name in its own scope, where it would shadow the type all over again.
    SECTION( "inside a refused struct and enum, the name is still the builtin" )
    {
        for( const std::string_view source : {
                 "struct f64 { f64 x; };\ni32 main() { return 0; }",
                 "enum u16 { A( u16 n ), B };\ni32 main() { return 0; }",
             } )
        {
            const Resolved p( source );

            INFO( source );
            REQUIRE( p.errors() == 1 );

            for( std::size_t i = 0; p.nth( Node_kind::Named_type, i ).is_valid(); ++i )
            {
                INFO( i );
                REQUIRE_FALSE( p.declaration_of( p.nth( Node_kind::Named_type, i ) ).is_valid() );
            }
        }
    }

    SECTION( "a type parameter" )
    {
        refused( Resolved( "i64 id<i64>( i64 x ) { return x; }\ni32 main() { return 0; }" ), "i64" );
    }

    // Members enter no lexical scope outside a member body, but every body inside one sees them.
    SECTION( "a field and a method" )
    {
        refused( Resolved( "class C { i32 i32; i32 get() { return 0; } };\ni32 main() { return 0; }" ), "i32" );
        refused( Resolved( "class C { i32 n; i32 f32() { return n; } };\ni32 main() { return 0; }" ), "f32" );
    }

    SECTION( "two of one name are two refusals, not a redeclaration" )
    {
        const Resolved p( "i32 main() { i32 i8 = 1; i32 i8 = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "already declared" ) == std::string::npos );
    }

    SECTION( "a name that only starts like one is fine" )
    {
        const Resolved p( "i32 i320 = 1;\nstruct u8x { i32 boolean; };\ni32 main() { i32 f = 1; i32 i = 2; return f + i; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A builtin type's name where a value goes is declared, as a type: saying it is not would be false.
TEST_CASE( "resolver_says_a_builtin_type_is_not_a_value", "[sema][resolve]" )
{
    const auto typed = []( std::string_view source, std::string_view name )
    {
        const Resolved p( source );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        CHECK( p.rendered().find( fmt::format( "`{}` is a type, not a value", name ) ) != std::string::npos );
        CHECK( p.rendered().find( "is not declared" ) == std::string::npos );
    };

    SECTION( "as an initialiser" )
    {
        typed( "i32 main() { i32 x = i32; return x; }", "i32" );
    }

    SECTION( "as an operand" )
    {
        typed( "i32 main() { i32 x = 1; return x + u8; }", "u8" );
    }

    SECTION( "a declaration that lost its name" )
    {
        typed( "i32 main() { i32 x = 0; f64 = 0.5; return x; }", "f64" );
    }

    SECTION( "as a callee" )
    {
        typed( "i32 main() { bool( 1 ); return 0; }", "bool" );
    }

    // There it is a type, and what is missing is the member.
    SECTION( "as a qualifier" )
    {
        const Resolved p( "i32 main() { return i32::make( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        CHECK( p.rendered().find( "`i32` is a builtin type, and has no members" ) != std::string::npos );
    }

    SECTION( "a name that only starts like one is still not declared" )
    {
        const Resolved p( "i32 main() { return i320; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        CHECK( p.rendered().find( "`i320` is not declared" ) != std::string::npos );
    }
}

// A constructor body sees the fields exactly as a destructor does - and unlike a destructor it has
// parameters of its own, which is what finally makes D19's member clause reachable.
TEST_CASE( "resolver_puts_fields_in_scope_inside_a_constructor", "[sema][resolve][aggregates]" )
{
    SECTION( "a bare field name binds to the field" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a field declared after the constructor is still visible" )
    {
        const Resolved p( "class Buffer { Buffer( u64 n ) { len = n; } u64 len; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "`this` binds to the synthesised parameter" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 n ) { this.len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D19's member clause, live for the first time: a parameter naming the field it initialises is the
// one place the shadow is forced, and `this.len` is how C++ programmers already disambiguate it.
TEST_CASE( "resolver_lets_a_parameter_shadow_a_field", "[sema][resolve][aggregates]" )
{
    SECTION( "a parameter may take a field's name" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 len ) { this.len = len; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The bare name is the parameter, not the field: the parameter scope sits above the field one.
    SECTION( "the bare name resolves to the parameter" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 len ) { this.len = len; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 1 ) ) == p.nth( Node_kind::Param_decl, 1 ) );
    }

    SECTION( "a local still may not" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 n ) { u64 len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }
}

namespace
{

// A program on disk, loaded and resolved the way the driver does it. The package `kl` is the directory
// `kl/` beside it.
class Resolved_program
{
public:
    explicit Resolved_program(
        std::initializer_list<std::pair<const char*, std::string_view>> files, std::string_view prelude = prelude_source()
    )
    {
        for( const auto& [name, text] : files )
        {
            dir_.write( name, text );
        }

        const std::optional<File_id> input = sm_.load_file( dir_.path / "main.kl" );

        REQUIRE( input.has_value() );
        input_ = *input;

        const Package kl { .name = "kl", .root = dir_.path / "kl" };
        program_    = load_program( input_, sm_, interner_, literals_, diags_, std::span( &kl, 1 ), prelude );
        earlier_    = diags_.error_count();
        resolution_ = resolve( program_.ast, sm_, interner_, diags_, program_.imports );
    }

    // Errors from resolution only.
    std::size_t errors() const
    {
        return diags_.error_count() - earlier_;
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

    // The file, without `.kl`, whose declaration main.kl's `nth` `kind` named `name` is bound to, as
    // `a`, `kl/geom` or `<prelude>`; empty when it is bound to nothing.
    std::string bound_into( Node_kind kind, std::string_view name, u32 nth = 0 ) const
    {
        const Ast& ast = program_.ast;

        for( u32 i = 0; i < ast.node_count(); ++i )
        {
            const Node_id id { i };

            if( ast.kind( id ) != kind || ast.span( id ).file != input_ ||
                interner_.text( Symbol_id { ast.aux( id ) } ) != name )
            {
                continue;
            }

            if( nth > 0 )
            {
                --nth;
                continue;
            }

            const Node_id decl = resolution_.declaration_of( id );
            if( !decl.is_valid() )
            {
                return {};
            }

            if( sm_.file( ast.span( decl ).file ).path == prelude_path )
            {
                return std::string( prelude_path );
            }

            std::filesystem::path file = std::filesystem::path( sm_.file( ast.span( decl ).file ).path );
            return file.lexically_relative( dir_.path ).replace_extension().generic_string();
        }

        FAIL( "main.kl has no such node" );
        return {};
    }

private:
    Temp_dir       dir_;
    Source_manager sm_;
    Interner       interner_;
    Literal_pool   literals_;
    Diagnostics    diags_;
    File_id        input_;
    Program        program_;
    Resolution     resolution_;
    std::size_t    earlier_ = 0;
};

} // namespace

TEST_CASE( "resolver_sees_only_imported_modules", "[sema][resolve][modules]" )
{
    SECTION( "an imported module's function is visible" )
    {
        const Resolved_program p( {
            { "main.kl", "import a;\ni32 main() { return fa(); }\n" },
            { "a.kl", "i32 fa() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Name_expr, "fa" ) == "a" );
    }

    SECTION( "a module imported only by an import is not" )
    {
        const Resolved_program p( {
            { "main.kl", "import a;\ni32 main() { return fb(); }\n" },
            { "a.kl", "import b;\ni32 fa() { return fb(); }\n" },
            { "b.kl", "i32 fb() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`fb` is in `b`, which this file does not import" ) != std::string::npos );
        REQUIRE( p.rendered().find( "write `import b;` at the top of the file" ) != std::string::npos );
        REQUIRE( p.rendered().find( "main.kl:2:" ) != std::string::npos );
    }

    // Bound anyway, so the checker types the call rather than reporting it again.
    SECTION( "a refused name is still bound" )
    {
        const Resolved_program p( {
            { "main.kl", "import a;\ni32 main() { return fb(); }\n" },
            { "a.kl", "import b;\ni32 fa() { return 1; }\n" },
            { "b.kl", "i32 fb() { return 1; }\n" },
        } );

        REQUIRE( p.errors() == 1 );
        REQUIRE( p.bound_into( Node_kind::Name_expr, "fb" ) == "b" );
    }

    SECTION( "a type's name is refused" )
    {
        const Resolved_program p( {
            { "main.kl", "import a;\ni32 main() { Point q = origin(); return 0; }\n" },
            { "a.kl", "import geom;\nPoint origin() { return Point { 1, 2 }; }\n" },
            { "geom.kl", "struct Point { i32 x; i32 y; };\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Point` is in `geom`, which this file does not import" ) != std::string::npos );
    }

    SECTION( "a struct literal's name is refused" )
    {
        const Resolved_program p( {
            { "main.kl", "import a;\ni32 main() { auto q = Point { 1, 2 }; return q.x; }\n" },
            { "a.kl", "import geom;\ni32 fa() { return 1; }\n" },
            { "geom.kl", "struct Point { i32 x; i32 y; };\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Point` is in `geom`, which this file does not import" ) != std::string::npos );
    }

    SECTION( "a global is refused" )
    {
        const Resolved_program p( {
            { "main.kl", "import a;\ni32 main() { return count; }\n" },
            { "a.kl", "import b;\ni32 fa() { return count; }\n" },
            { "b.kl", "i32 count = 0;\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`count` is in `b`, which this file does not import" ) != std::string::npos );
    }

    // Naming needs the import, using does not: a field read is not a lookup.
    SECTION( "a value of an unimported type can be used" )
    {
        const Resolved_program p( {
            { "main.kl", "import a;\ni32 main() { auto q = origin(); return q.x; }\n" },
            { "a.kl", "import geom;\nPoint origin() { return Point { 1, 2 }; }\n" },
            { "geom.kl", "struct Point { i32 x; i32 y; };\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    // `far` is loaded (through `route`) before `near`, so its overload heads the chain and main
    // cannot see it; the use binds to the one it can.
    SECTION( "a call binds to the overload its file sees" )
    {
        const Resolved_program p( {
            { "main.kl", "import near;\nimport route;\ni32 main() { return pick( 1 ); }\n" },
            { "near.kl", "i32 pick( i64 v ) { return 1; }\n" },
            { "route.kl", "import far;\ni32 via() { return pick( 1 ); }\n" },
            { "far.kl", "i32 pick( i32 v ) { return 2; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Name_expr, "pick" ) == "near" );
    }

    SECTION( "an overload set none of which is visible is refused" )
    {
        const Resolved_program p( {
            { "main.kl", "import route;\ni32 main() { return pick( 1 ); }\n" },
            { "route.kl", "import far;\ni32 via() { return pick( 1 ); }\n" },
            { "far.kl", "i32 pick( i32 v ) { return 2; }\ni32 pick( i64 v ) { return 3; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`pick` is in `far`, which this file does not import" ) != std::string::npos );
    }

    // One package, one namespace: neither file imports the other, and the second is still refused.
    SECTION( "a duplicate between two modules is still an error" )
    {
        const Resolved_program p( {
            { "main.kl", "import a;\nimport b;\ni32 main() { return 0; }\n" },
            { "a.kl", "i32 n = 1;\n" },
            { "b.kl", "i32 n = 2;\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`n` is already declared in this scope" ) != std::string::npos );
    }

    // Locals, parameters and main.kl's own declarations never pass through the import check.
    SECTION( "a file's own names need no import" )
    {
        const Resolved_program p( {
            { "main.kl", "i32 g = 1;\ni32 f( i32 v ) { i32 w = v; return w + g; }\ni32 main() { return f( 1 ); }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

TEST_CASE( "resolver_looks_in_the_prelude_after_the_file's_own_package", "[sema][resolve][prelude]" )
{
    constexpr std::string_view prelude = "struct Pair { i32 a; };\ni32 answer() { return 42; }\n";

    SECTION( "a bare name reaches it" )
    {
        const Resolved_program p( { { "main.kl", "i32 main() { Pair p = Pair { 1 }; return answer(); }\n" } }, prelude );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Name_expr, "answer" ) == "<prelude>" );
    }

    SECTION( "so does one in a package" )
    {
        const Resolved_program p(
            {
                { "main.kl", "import kl::geom;\ni32 main() { return kl::area(); }\n" },
                { "kl/geom.kl", "i32 area() { Pair p = Pair { 1 }; return answer(); }\n" },
            },
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    // Shadowed rather than refused, so a name added to the prelude never breaks a program that had it.
    SECTION( "the program's own declaration of a name shadows it" )
    {
        const Resolved_program p(
            { { "main.kl", "struct Pair { i64 x; };\ni32 answer() { return 1; }\ni32 main() { return answer(); }\n" } }, prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Name_expr, "answer" ) == "main" );
    }

    SECTION( "so does a package's" )
    {
        const Resolved_program p(
            {
                { "main.kl", "import kl::geom;\ni32 main() { return kl::area(); }\n" },
                { "kl/geom.kl", "struct Pair { i64 x; };\ni32 answer() { return 3; }\ni32 area() { return answer(); }\n" },
            },
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "its package is not a qualifier" )
    {
        const Resolved_program p( { { "main.kl", "i32 main() { return prelude::answer(); }\n" } }, prelude );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`prelude` is not declared" ) != std::string::npos );
    }

    SECTION( "it sees nothing of the program" )
    {
        const Resolved_program p( { { "main.kl", "i32 main() { return 0; }\n" } }, "i32 answer() { return main(); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`main` is not declared" ) != std::string::npos );
    }
}

TEST_CASE( "resolver_names_another_package_through_its_name", "[sema][resolve][packages]" )
{
    SECTION( "a qualified call binds into the package" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 main() { return kl::area(); }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Path_expr, "area" ) == "kl/geom" );
    }

    // A function, a type and a global of each name, in each package, and none of them clash.
    SECTION( "two packages may declare one name" )
    {
        const Resolved_program p( {
            { "main.kl",
              "import kl::geom;\n"
              "struct Point { i32 x; };\n"
              "i32 count = 0;\n"
              "i32 area() { return 1; }\n"
              "i32 main() { return area() + kl::area(); }\n" },
            { "kl/geom.kl", "struct Point { i32 x; };\ni32 count = 0;\ni32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Name_expr, "area" ) == "main" );
        REQUIRE( p.bound_into( Node_kind::Path_expr, "area" ) == "kl/geom" );
    }

    // A bare call looks only in its own package, so `kl`'s closer match is never a candidate.
    SECTION( "overloads never merge across packages" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 pick( i64 v ) { return 1; }\ni32 main() { return pick( 1 ) + kl::pick( 1 ); }\n"
            },
            { "kl/geom.kl", "i32 pick( i32 v ) { return 2; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Name_expr, "pick" ) == "main" );
        REQUIRE( p.bound_into( Node_kind::Path_expr, "pick" ) == "kl/geom" );
    }

    SECTION( "a duplicate within a package is still an error" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::a;\nimport kl::b;\ni32 main() { return 0; }\n" },
            { "kl/a.kl", "i32 n = 1;\n" },
            { "kl/b.kl", "i32 n = 2;\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`n` is already declared in this scope" ) != std::string::npos );
    }

    SECTION( "inside a package the qualifier is optional" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 main() { return kl::area(); }\n" },
            { "kl/geom.kl", "import shape;\ni32 area() { return side() + kl::side(); }\n" },
            { "kl/shape.kl", "i32 side() { return 2; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    // Bound anyway, as an unimported name is, so the checker does not report the call again.
    SECTION( "a bare name from another package is refused" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 main() { return area(); }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`area` is in the package `kl`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "write `kl::area`" ) != std::string::npos );
        REQUIRE( p.bound_into( Node_kind::Name_expr, "area" ) == "kl/geom" );
    }

    SECTION( "a bare type from another package is refused with its qualified name" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 main() { Point q = kl::origin(); return q.x; }\n" },
            { "kl/geom.kl", "struct Point { i32 x; };\nPoint origin() { return Point { 1 }; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Point` is in the package `kl`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "write `kl::Point`" ) != std::string::npos );
    }

    SECTION( "a qualified name needs its module imported" )
    {
        const Resolved_program p( {
            { "main.kl", "import route;\ni32 main() { return kl::area(); }\n" },
            { "route.kl", "import kl::geom;\ni32 via() { return kl::area(); }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`area` is in `kl::geom`, which this file does not import" ) != std::string::npos );
        REQUIRE( p.rendered().find( "write `import kl::geom;` at the top of the file" ) != std::string::npos );
        REQUIRE( p.bound_into( Node_kind::Path_expr, "area" ) == "kl/geom" );
    }

    SECTION( "a package's module is named qualified even inside it" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::a;\ni32 main() { return 0; }\n" },
            { "kl/a.kl", "import b;\ni32 fa() { return fc(); }\n" },
            { "kl/b.kl", "import c;\ni32 fb() { return fc(); }\n" },
            { "kl/c.kl", "i32 fc() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`fc` is in `kl::c`, which this file does not import" ) != std::string::npos );
    }

    SECTION( "a qualified name no imported module declares" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 area() { return 1; }\ni32 main() { return kl::nothing() + kl::area(); }\n" },
            { "kl/geom.kl", "i32 side() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "no module of `kl` that this file imports declares `nothing`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "no module of `kl` that this file imports declares `area`" ) != std::string::npos );
    }

    // The program's package has no name to qualify with, so nothing in `kl` is offered one.
    SECTION( "a package cannot name the program" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 helper() { return 1; }\ni32 main() { return kl::area(); }\n" },
            { "kl/geom.kl", "i32 area() { return helper(); }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`helper` is not declared" ) != std::string::npos );
    }

    // `kl` is named on the command line, so it is a package before anything is imported from it.
    SECTION( "a package nothing is imported from" )
    {
        const Resolved_program p( {
            { "main.kl", "i32 main() { return kl::area(); }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no module of `kl` that this file imports declares `area`" ) != std::string::npos );
    }
}

TEST_CASE( "resolver_names_a_package's_types_and_globals", "[sema][resolve][packages]" )
{
    SECTION( "a qualified type binds into the package" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 main() { kl::Point q = kl::origin(); return q.x; }\n" },
            { "kl/geom.kl", "struct Point { i32 x; };\nPoint origin() { return Point { 1 }; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Named_type, "Point" ) == "kl/geom" );
    }

    // In a type argument, and in a signature, as well as in a declaration.
    SECTION( "a qualified type is a type wherever one is written" )
    {
        const Resolved_program p( {
            { "main.kl",
              "import kl::geom;\n"
              "struct Box<T> where T : Copyable { T v; };\n"
              "kl::Point first( Box<kl::Point> b ) { return b.v; }\n"
              "i32 main() { return 0; }\n" },
            { "kl/geom.kl", "struct Point { i32 x; };\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Named_type, "Point", 0 ) == "kl/geom" );
        REQUIRE( p.bound_into( Node_kind::Named_type, "Point", 1 ) == "kl/geom" );
    }

    SECTION( "a package's type and the program's may share a name" )
    {
        const Resolved_program p( {
            { "main.kl",
              "import kl::geom;\n"
              "struct Point { i32 x; };\n"
              "i32 main() { Point a = Point { 1 }; kl::Point b = kl::Point { 2 }; return a.x + b.x; }\n" },
            { "kl/geom.kl", "struct Point { i32 x; };\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Named_type, "Point", 0 ) == "main" );
        REQUIRE( p.bound_into( Node_kind::Named_type, "Point", 1 ) == "kl/geom" );
        REQUIRE( p.bound_into( Node_kind::Struct_literal, "Point", 0 ) == "main" );
        REQUIRE( p.bound_into( Node_kind::Struct_literal, "Point", 1 ) == "kl/geom" );
    }

    SECTION( "a qualified type needs its module imported" )
    {
        const Resolved_program p( {
            { "main.kl", "import route;\ni32 main() { kl::Point q = via(); return q.x; }\n" },
            { "route.kl", "import kl::geom;\nkl::Point via() { return kl::Point { 1 }; }\n" },
            { "kl/geom.kl", "struct Point { i32 x; };\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Point` is in `kl::geom`, which this file does not import" ) != std::string::npos );
    }

    SECTION( "a qualified type no imported module declares" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 main() { kl::Nothing n; return 0; }\n" },
            { "kl/geom.kl", "struct Point { i32 x; };\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no module of `kl` that this file imports declares `Nothing`" ) != std::string::npos );
    }

    // The resolver binds the qualifier; which variant or static method follows is the checker's.
    SECTION( "a package's enum or aggregate can qualify a path" )
    {
        const Resolved_program p( {
            { "main.kl",
              "import kl::geom;\n"
              "i32 main() { kl::Colour c = kl::Colour::Red; kl::Point q = kl::Point::make(); return q.x; }\n" },
            { "kl/geom.kl",
              "enum Colour { Red, Green };\n"
              "struct Point { i32 x; static Point make() { return Point { 1 }; } };\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Path_expr, "Colour" ) == "kl/geom" );
        REQUIRE( p.bound_into( Node_kind::Path_expr, "Point" ) == "kl/geom" );
    }

    SECTION( "a package's global" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 main() { kl::count = 2; return kl::count; }\n" },
            { "kl/geom.kl", "i32 count = 0;\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.bound_into( Node_kind::Path_expr, "count", 0 ) == "kl/geom" );
        REQUIRE( p.bound_into( Node_kind::Path_expr, "count", 1 ) == "kl/geom" );
    }

    SECTION( "a bare global from another package is refused with its qualified name" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 main() { return count; }\n" },
            { "kl/geom.kl", "i32 count = 0;\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`count` is in the package `kl`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "write `kl::count`" ) != std::string::npos );
    }

    // Otherwise `kl::x` could name a member of the type as well as a declaration of the package.
    SECTION( "a type may not take a package's name" )
    {
        const Resolved_program p( {
            { "main.kl", "struct kl { i32 x; };\nenum kl_free { A };\ni32 main() { return 0; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`kl` is the name of a package" ) != std::string::npos );
        REQUIRE( p.rendered().find( "main.kl:1:8" ) != std::string::npos );
    }

    // Neither can stand before `::`, so neither can be mistaken for the package.
    SECTION( "a function or a variable may" )
    {
        const Resolved_program p( {
            { "main.kl", "i32 kl( i32 v ) { i32 kl_ = v; return kl_; }\ni32 main() { i32 kl = 1; return kl; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

// A static field joins the member scope as a field does, and is also reached through its type's name.
TEST_CASE( "resolver_binds_a_static_field", "[sema][resolve][static]" )
{
    SECTION( "a bare name in a method binds to it" )
    {
        const Resolved p( "class C { static i32 count = 0; i32 x; i32 get() const { return count; } };\n"
                          "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }

    SECTION( "and in a static method, which has no object to need" )
    {
        const Resolved p( "class C { static i32 count = 0; i32 x; static i32 get() { return count; } };\n"
                          "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }

    SECTION( "the type's name reaches it from outside" )
    {
        const Resolved p( "struct S { i32 x; static i32 count = 0; };\ni32 main() { return S::count; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Path_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }

    // Two types each holding a `count` are two variables, and neither is a global called `count`.
    SECTION( "it is not a global" )
    {
        const Resolved p( "struct S { i32 x; static i32 count = 0; };\ni32 main() { return count; }" );

        REQUIRE( p.rendered().find( "`count` is not declared" ) != std::string::npos );
    }

    SECTION( "it shares one namespace with the fields and methods" )
    {
        for( const char* members :
             { "i32 count; static i32 count;",
               "static i32 count; static i32 count;",
               "static i32 count; i32 count() const { return 1; }" } )
        {
            const Resolved p( fmt::format( "struct S {{ i32 x; {} }};\ni32 main() {{ return 0; }}", members ) );

            INFO( members << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "`count` is already declared" ) != std::string::npos );
        }
    }
}

// The loader reports a package that is not there, once; every name written through it is then
// unknowable, and saying so again at each one is the cascade.
TEST_CASE( "resolver_says_nothing_more_about_a_missing_package", "[sema][resolve][packages]" )
{
    SECTION( "a call, a type, a literal, a variant and a global" )
    {
        const Resolved_program p( {
            { "main.kl",
              "import foo::geom;\n"
              "i32 main()\n"
              "{\n"
              "    foo::Point q = foo::Point { 1 };\n"
              "    foo::Colour c = foo::Colour::Red;\n"
              "    foo::made = foo::made + 1;\n"
              "    return foo::area();\n"
              "}\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "a missing name in a package that is there is still reported" )
    {
        const Resolved_program p( {
            { "main.kl", "import kl::geom;\ni32 main() { return kl::nothing(); }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no module of `kl` that this file imports declares `nothing`" ) != std::string::npos );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
