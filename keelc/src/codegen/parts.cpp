// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/parts.h"
#include <algorithm>
#include "ir/place_type.h"
#include "ir/vtables.h"

namespace keel
{

Parts::Parts(
    const Ast&                   ast,
    Types&                       types,
    const Module_graph&          modules,
    File_id                      input,
    const std::vector<Function>& functions,
    std::span<const File_id>     provided
)
    : ast_( ast ),
      functions_( functions ),
      order_( modules.dependencies_first( input ) ),
      reaches_( functions.size() ),
      owners_( functions.size() )
{
    for( u32 f = 0; f < functions.size(); ++f )
    {
        by_declaration_.emplace( functions[f].declaration.v, f );
    }

    const Type_table& table = types.table();

    for( u32 f = 0; f < functions.size(); ++f )
    {
        const auto reach = [&]( u32 callee )
        {
            if( callee != k_invalid_kir_id && std::ranges::find( reaches_[f], callee ) == reaches_[f].end() )
            {
                reaches_[f].push_back( callee );
            }
        };

        for( const Statement& statement : functions[f].statements )
        {
            const Rvalue& value = functions[f].rvalue( statement );

            if( statement.kind == Statement_kind::Assign && value.globals != Globals_call::None )
            {
                const auto built = std::ranges::find_if( functions, [&]( const Function& g ) { return names( value, g ); } );
                reach( built == functions.end() ? k_invalid_kir_id : narrow_cast<u32>( built - functions.begin() ) );
            }
            else if( statement.kind == Statement_kind::Assign &&
                     ( value.kind == Rvalue_kind::Call || value.kind == Rvalue_kind::Function_address ) )
            {
                reach( find( value.callee, value.type_arguments, value.op ) );
            }
            else if( statement.kind == Statement_kind::Drop && value.callee.is_valid() )
            {
                reach( find( value.callee, value.type_arguments ) );
            }
            else if( statement.kind == Statement_kind::Drop )
            {
                // An enum's, a union's or an array's synthesised destructor; an interface's is its vtable's.
                const Type_id dropped = place_type( ast, types, functions[f], statement.place );

                if( table.is_enum( dropped ) )
                {
                    reach( find( table.get( dropped ).declaration, table.get( dropped ).arguments ) );
                }
                else if( table.is_union( dropped ) || table.is_array( dropped ) )
                {
                    reach( find( {}, std::span( &dropped, 1 ) ) );
                }
            }
        }

        for( const Vtable& vtable : vtables_needed( types, std::span( &functions[f], 1 ) ) )
        {
            for( const Node_id requirement : vtable.requirements )
            {
                reach( find( requirement, std::span( &vtable.implementer, 1 ), Token_kind::Unknown, vtable.interface ) );
            }

            reach( find( {}, std::span( &vtable.implementer, 1 ) ) );
        }
    }

    const auto declared = [&]( u32 f ) { return !placed_by_reach( functions[f] ); };

    // Claimed already, so no module takes them again.
    std::ranges::copy( provided, owners_.begin() );

    for( u32 f = 0; f < functions.size(); ++f )
    {
        if( declared( f ) )
        {
            owners_[f] = of( functions[f] );
        }
    }

    // Each module in turn claims what its own functions reach through instances not yet claimed.
    for( const File_id part : order_ )
    {
        std::vector<bool> seen( functions.size(), false );
        std::vector<u32>  stack;

        for( u32 f = 0; f < functions.size(); ++f )
        {
            if( declared( f ) && owners_[f] == part )
            {
                stack.push_back( f );
            }
        }

        while( !stack.empty() )
        {
            const u32 f = stack.back();
            stack.pop_back();

            for( const u32 callee : reaches_[f] )
            {
                if( seen[callee] || declared( callee ) )
                {
                    continue;
                }

                seen[callee] = true;

                if( !owners_[callee].is_valid() )
                {
                    owners_[callee] = part;
                }

                stack.push_back( callee );
            }
        }
    }

    // A destructor queued for a composite nothing drops.
    for( File_id& owner : owners_ )
    {
        if( !owner.is_valid() )
        {
            owner = input;
        }
    }
}

File_id Parts::of( const Function& function ) const
{
    if( function.entry )
    {
        return input();
    }

    if( function.globals.is_valid() )
    {
        return of( function.globals );
    }

    if( function.declaration.is_valid() && function.type_arguments.empty() )
    {
        return of( function.declaration );
    }

    return owners_[find( function.declaration, function.type_arguments, function.compares, function.thunk_of )];
}

File_id Parts::of( Node_id declaration ) const
{
    return ast_.span( declaration ).file;
}

std::span<const u32> Parts::reaches( u32 function ) const
{
    return reaches_[function];
}

std::span<const File_id> Parts::order() const
{
    return order_;
}

File_id Parts::input() const
{
    return order_.back();
}

u32 Parts::find( Node_id declaration, std::span<const Type_id> type_arguments, Token_kind compares, Type_id thunk_of ) const
{
    const auto [first, last] = by_declaration_.equal_range( declaration.v );

    for( auto it = first; it != last; ++it )
    {
        if( functions_[it->second].compares == compares && functions_[it->second].thunk_of == thunk_of &&
            std::ranges::equal( functions_[it->second].type_arguments, type_arguments ) )
        {
            return it->second;
        }
    }

    return k_invalid_kir_id;
}

namespace
{
void add_targets( const Constant_value& value, std::vector<Node_id>& targets )
{
    if( value.kind == Constant_value::Kind::Address )
    {
        targets.push_back( value.target );
    }
    for( const Constant_value& element : value.elements )
    {
        add_targets( element, targets );
    }
}
} // namespace

void remove_unreached(
    std::vector<Function>& functions,
    std::vector<File_id>&  provided,
    const Ast&             ast,
    Types&                 types,
    const Module_graph&    modules,
    File_id                input
)
{
    std::vector<u32>  stack;
    std::vector<bool> reached( functions.size(), false );

    {
        const Parts parts( ast, types, modules, input, functions, provided );

        // Every global is written, so what an initialiser names is a root too.
        std::vector<Node_id> addressed;
        for( const auto& [node, value] : types.constants() )
        {
            add_targets( value, addressed );
        }

        bool has_main = false;
        for( u32 f = 0; f < functions.size(); ++f )
        {
            const Node_id decl = functions[f].declaration;

            has_main = has_main || functions[f].entry;
            if( functions[f].entry || ( decl.is_valid() && std::ranges::find( addressed, decl ) != addressed.end() ) )
            {
                reached[f] = true;
                stack.push_back( f );
            }
        }

        if( !has_main )
        {
            return; // a library: anything may be called
        }

        while( !stack.empty() )
        {
            const u32 f = stack.back();
            stack.pop_back();

            for( const u32 callee : parts.reaches( f ) )
            {
                if( !reached[callee] )
                {
                    reached[callee] = true;
                    stack.push_back( callee );
                }
            }
        }
    }

    u32 kept          = 0;
    u32 kept_provided = 0;
    for( u32 f = 0; f < functions.size(); ++f )
    {
        if( !reached[f] )
        {
            continue;
        }
        if( f < provided.size() )
        {
            provided[kept_provided++] = provided[f];
        }
        if( kept != f )
        {
            functions[kept] = std::move( functions[f] ); // never onto itself, which would empty it
        }
        ++kept;
    }
    functions.resize( kept );
    provided.resize( kept_provided );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "check/pipeline_test_support.h"
#include "codegen_c/emit_kir.h"
#include "common/temp_dir.h"
#include "front_end.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

// A program on disk, `main.kl` the input, lowered as the emitter reads it and split into parts.
class Split
{
public:
    explicit Split( std::initializer_list<std::pair<const char*, std::string_view>> files )
    {
        for( const auto& [name, text] : files )
        {
            dir_.write( name, text );
        }

        const File_id input = *sm_.load_file( dir_.path / "main.kl" );
        front_.emplace( run_front_end( input, Stage::Elaborated, Build {}, sm_, interner_, literals_, diags_ ) );
        parts_.emplace( front_->program.ast, front_->types, front_->program.modules, input, front_->functions );
    }

    bool clean() const
    {
        return !diags_.has_errors();
    }

    // The file whose part holds `name` at the one type argument `argument`, or at none.
    std::string part_of( std::string_view name, std::string_view argument = {} ) const
    {
        return one(
            [&]( const Function& f )
            { return f.declaration.is_valid() && interner_.text( front_->program.ast.name( f.declaration ) ) == name; },
            argument
        );
    }

    // The file whose part holds the drop function of the type `type`.
    std::string drop_part_of( std::string_view type ) const
    {
        return one( [&]( const Function& f ) { return !f.declaration.is_valid(); }, type );
    }

    // Where `file` comes in the order.
    size_t position( std::string_view file ) const
    {
        const std::span<const File_id> order = parts_->order();
        for( size_t i = 0; i < order.size(); ++i )
        {
            if( name( order[i] ) == file )
            {
                return i;
            }
        }
        return order.size();
    }

    // The C `main` the input's part ends with.
    std::string shim()
    {
        const Description program = describe( front_->program.ast, front_->types, interner_ );
        const std::string c       = emit_c_from_kir(
                                  front_->functions,
                                  program,
                                  front_->types,
                                  literals_,
                                  sm_,
                                  interner_,
                                  front_->program.modules,
                                  &*parts_,
                                  parts_->input()
        )
                                  .c;
        return c.substr( c.find( "int main(" ) );
    }

private:
    template <typename Matches>
    std::string one( Matches matches, std::string_view argument ) const
    {
        const Type_table&  table = front_->types.table();
        std::optional<u32> found;

        for( u32 f = 0; f < front_->functions.size(); ++f )
        {
            const Function& function = front_->functions[f];
            const bool      at       = argument.empty()
                                           ? function.type_arguments.empty()
                                           : function.type_arguments.size() == 1 && table.name( function.type_arguments[0] ) == argument;

            if( at && matches( function ) )
            {
                REQUIRE_FALSE( found.has_value() );
                found = f;
            }
        }

        REQUIRE( found.has_value() );
        return name( parts_->of( front_->functions[*found] ) );
    }

    std::string name( File_id file ) const
    {
        return std::filesystem::path( sm_.file( file ).path ).filename().generic_string();
    }

    Temp_dir                 dir_;
    Source_manager           sm_;
    Interner                 interner_;
    Literal_pool             literals_;
    Diagnostics              diags_;
    std::optional<Front_end> front_;
    std::optional<Parts>     parts_;
};

constexpr std::string_view k_pick = "T pick<T>( T x ) where T : copyable { return x; }\n";

constexpr std::string_view k_shapes =
    "interface Shape { f64 area() const; };\n"
    "class Buf { u8[*] b; public Buf() { unsafe { b = alloc<u8>( 1 ); } } ~Buf() { unsafe { free( b ); } } };\n"
    "class Framed implements Shape { Buf frame; public Framed() { frame = Buf(); } public f64 area() const { return 1.0; } };\n"
    "class Spare implements Shape { Buf frame; public Spare() { frame = Buf(); } public f64 area() const { return 2.0; } };\n"
    "i32 made() { box<Shape> b = box<Framed>( move Framed() ); return 0; }\n";

} // namespace

TEST_CASE( "parts_gives_a_function_its_declaration_s_module", "[codegen][parts]" )
{
    const Split split( {
        { "main.kl", "import a;\ni32 main() { return fa(); }\n" },
        { "a.kl", "i32 fa() { return 0; }\n" },
    } );

    REQUIRE( split.clean() );
    REQUIRE( split.part_of( "main" ) == "main.kl" );
    REQUIRE( split.part_of( "fa" ) == "a.kl" );
}

TEST_CASE( "parts_gives_an_instance_to_the_first_module_that_reaches_it", "[codegen][parts][generics]" )
{
    SECTION( "a module and the module that imports it: the dependency" )
    {
        const Split split( {
            { "main.kl", "import a;\nimport g;\ni32 main() { return pick<i32>( 0 ) + fa(); }\n" },
            { "a.kl", "import g;\ni32 fa() { return pick<i32>( 0 ); }\n" },
            { "g.kl", k_pick },
        } );

        REQUIRE( split.clean() );
        REQUIRE( split.part_of( "pick", "i32" ) == "a.kl" );
    }

    SECTION( "two unrelated modules: the first in order" )
    {
        const Split split( {
            { "main.kl", "import a;\nimport b;\ni32 main() { return fa() + fb(); }\n" },
            { "a.kl", "import g;\ni32 fa() { return pick<i32>( 0 ); }\n" },
            { "b.kl", "import g;\ni32 fb() { return pick<i32>( 0 ); }\n" },
            { "g.kl", k_pick },
        } );

        REQUIRE( split.clean() );

        const std::string first = split.position( "a.kl" ) < split.position( "b.kl" ) ? "a.kl" : "b.kl";
        REQUIRE( split.part_of( "pick", "i32" ) == first );
    }

    SECTION( "only the input: the input" )
    {
        const Split split( {
            { "main.kl", "import g;\ni32 main() { return pick<i32>( 0 ); }\n" },
            { "g.kl", k_pick },
        } );

        REQUIRE( split.clean() );
        REQUIRE( split.part_of( "pick", "i32" ) == "main.kl" );
    }
}

TEST_CASE( "the_shim_builds_modules_dependencies_first_and_drops_them_in_reverse", "[codegen][parts][globals]" )
{
    constexpr std::string_view k_buf =
        "class Buf { u8[*] b; public Buf() { unsafe { b = alloc<u8>( 1 ); } } ~Buf() { unsafe { free( b ); } } };\n";

    Split split( {
        { "main.kl", "import a;\nimport b;\nBuf third = Buf();\ni32 main() { return 0; }\n" },
        { "b.kl", "import a;\nBuf second = Buf();\n" },
        { "a.kl", std::string( k_buf ) + "Buf first = Buf();\n" },
    } );
    REQUIRE( split.clean() );

    const std::string shim = split.shim();
    const auto        at   = [&]( std::string_view call ) { return shim.find( call ); };

    REQUIRE( at( "kl_1a_0build(  );" ) < at( "kl_1b_0build(  );" ) );
    REQUIRE( at( "kl_1b_0build(  );" ) < at( "kl__0build(  );" ) );
    REQUIRE( at( "kl__0build(  );" ) < at( "kl__4main__(  );" ) );
    REQUIRE( at( "kl__4main__(  );" ) < at( "kl__0drop(  );" ) );
    REQUIRE( at( "kl__0drop(  );" ) < at( "kl_1b_0drop(  );" ) );
    REQUIRE( at( "kl_1b_0drop(  );" ) < at( "kl_1a_0drop(  );" ) );
    REQUIRE( at( "kl_1a_0drop(  );" ) < at( "return result;" ) );
}

TEST_CASE( "parts_follows_a_vtable_to_its_drop_function", "[codegen][parts][interfaces]" )
{
    // `made` drops a `box<Shape>`, which reaches `Framed`'s drop function only through the vtable.
    const Split split( {
        { "main.kl", "import s;\ni32 main() { return made(); }\n" },
        { "s.kl", k_shapes },
    } );

    REQUIRE( split.clean() );
    REQUIRE( split.drop_part_of( "Framed" ) == "s.kl" );
}

TEST_CASE( "parts_gives_an_unreached_destructor_to_the_input", "[codegen][parts][interfaces]" )
{
    // Every owning implementer has a drop function, and nothing boxes a `Spare`.
    const Split split( {
        { "main.kl", "import s;\ni32 main() { return made(); }\n" },
        { "s.kl", k_shapes },
    } );

    REQUIRE( split.clean() );
    REQUIRE( split.drop_part_of( "Spare" ) == "main.kl" );
}

// `made` reaches `Framed`'s drop only through its vtable; nothing reaches `Spare` or `unused`.
TEST_CASE( "remove_unreached_keeps_what_main_reaches", "[codegen][parts][interfaces]" )
{
    Compiled p(
        std::string( k_shapes ) + "i32 unused() { return 1; }\ni32 main() { return made(); }\n", Stage::Elaborated, Build {}
    );
    REQUIRE( p.clean() );

    std::vector<File_id> provided;
    remove_unreached( p.all, provided, p.ast, p.types, p.modules, p.file );

    const Type_table& table = p.types.table();
    const auto        kept  = [&]( std::string_view name )
    {
        return std::ranges::any_of(
            p.all,
            [&]( const Function& f )
            { return f.declaration.is_valid() && p.interner.text( p.ast.name( f.declaration ) ) == name; }
        );
    };
    const auto dropper = [&]( std::string_view type )
    {
        return std::ranges::any_of(
            p.all,
            [&]( const Function& f )
            { return !f.declaration.is_valid() && f.type_arguments.size() == 1 && table.name( f.type_arguments[0] ) == type; }
        );
    };

    REQUIRE( kept( "main" ) );
    REQUIRE( kept( "made" ) );
    REQUIRE( dropper( "Framed" ) );
    REQUIRE_FALSE( dropper( "Spare" ) );
    REQUIRE_FALSE( kept( "unused" ) );

    // Each kept function whole, not only its name: one moved onto itself would lose its body.
    for( const Function& f : p.all )
    {
        REQUIRE_FALSE( f.blocks.empty() );
        REQUIRE( verify( f ).empty() );
    }
}

// A library, with no `main`, keeps every function.
TEST_CASE( "remove_unreached_keeps_everything_without_main", "[codegen][parts]" )
{
    Compiled p( "i32 one() { return 1; }\n", Stage::Elaborated, Build {} );
    REQUIRE( p.clean() );

    const std::size_t    before = p.all.size();
    std::vector<File_id> provided;
    remove_unreached( p.all, provided, p.ast, p.types, p.modules, p.file );
    REQUIRE( p.all.size() == before );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
