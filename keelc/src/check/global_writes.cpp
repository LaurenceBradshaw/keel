// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/global_writes.h"

#include <algorithm>
#include <unordered_map>
#include "ir/call_graph.h"

namespace keel
{

namespace
{

using Set = std::vector<bool>; // by owning-global index

bool join( Set& into, const Set& from )
{
    bool changed = false;
    for( std::size_t g = 0; g < from.size(); ++g )
    {
        if( from[g] && !into[g] )
        {
            into[g] = changed = true;
        }
    }
    return changed;
}

class Effects
{
public:
    Effects( std::span<const Function> functions, const Ast& ast, Types& types )
        : functions_( functions ),
          ast_( ast ),
          types_( types )
    {
        for( u32 f = 0; f < functions.size(); ++f )
        {
            by_declaration_[functions[f].declaration.v].push_back( f );
        }
        gather();
    }

    std::vector<Global_effects> run()
    {
        std::vector<Global_effects> effects( functions_.size() );
        if( globals_.empty() )
        {
            return effects;
        }

        const std::size_t count = functions_.size();
        writes_.assign( count, Set( globals_.size(), false ) );
        into_.assign( count, Set( globals_.size(), false ) );
        callees_.resize( count );
        for( u32 f = 0; f < count; ++f )
        {
            callees_[f] = callees_of( f );
        }

        // What a callee returns into feeds its callers' pointers, so those settle first.
        std::vector<u32> returning;
        for( u32 f = 0; f < count; ++f )
        {
            if( returns_address( f ) )
            {
                returning.push_back( f );
            }
        }
        for( bool changed = true; changed; )
        {
            changed = false;
            for( const u32 f : returning )
            {
                changed = join( into_[f], pointers_of( f )[0] ) || changed;
            }
        }

        for( u32 f = 0; f < count; ++f )
        {
            direct_writes( f, pointers_of( f ) );
        }

        for( bool changed = true; changed; )
        {
            changed = false;
            for( u32 f = 0; f < count; ++f )
            {
                for( const u32 c : callees_[f] )
                {
                    changed = join( writes_[f], writes_[c] ) || changed;
                }
            }
        }

        for( u32 f = 0; f < count; ++f )
        {
            effects[f].writes       = listed( writes_[f] );
            effects[f].returns_into = listed( into_[f] );
            effects[f].owning       = listed( owning_in( f ) );
            effects[f].calls        = calls_of( f );
        }
        return effects;
    }

private:
    std::span<const Function>                 functions_;
    const Ast&                                ast_;
    Types&                                    types_;
    std::unordered_map<u32, std::vector<u32>> by_declaration_;
    std::unordered_map<u32, std::size_t>      index_; // owning global → index
    std::vector<Node_id>                      globals_;
    std::unordered_map<u32, std::vector<u32>> answering_; // requirement → functions
    std::vector<u32>                          addressed_; // whose address is taken
    std::vector<u32>                          destructors_;
    std::vector<Set>                          named_; // the owning globals each function names
    std::vector<Set>                          writes_;
    std::vector<Set>                          into_;
    std::vector<std::vector<u32>>             callees_;

    // In declaration order, as Node_ids are.
    std::vector<Node_id> listed( const Set& set ) const
    {
        std::vector<Node_id> globals;
        for( std::size_t g = 0; g < set.size(); ++g )
        {
            if( set[g] )
            {
                globals.push_back( globals_[g] );
            }
        }
        std::ranges::sort( globals );
        return globals;
    }

    // Those it names, and those its calls write or return into.
    Set owning_in( u32 f ) const
    {
        Set owning = named_[f];
        join( owning, writes_[f] );
        for( const u32 c : callees_[f] )
        {
            join( owning, into_[c] );
        }
        return owning;
    }

    std::vector<Call_effects> calls_of( u32 f ) const
    {
        const Function&           function = functions_[f];
        std::vector<Call_effects> calls;
        for( const Statement& statement : function.statements )
        {
            Set writes( globals_.size(), false );
            Set into( globals_.size(), false );
            for( const u32 c : callees_of( function, statement ) )
            {
                join( writes, writes_[c] );
                join( into, into_[c] );
            }

            Call_effects call { .call = statement.value.id(), .writes = listed( writes ), .returns_into = listed( into ) };
            if( !call.writes.empty() || !call.returns_into.empty() )
            {
                calls.push_back( std::move( call ) );
            }
        }
        std::ranges::sort( calls, {}, &Call_effects::call );
        return calls;
    }

    std::vector<u32> named( Node_id declaration, std::span<const Type_id> arguments ) const
    {
        std::vector<u32> found;
        if( const auto it = by_declaration_.find( declaration.v ); it != by_declaration_.end() )
        {
            for( const u32 f : it->second )
            {
                if( std::ranges::equal( functions_[f].type_arguments, arguments ) )
                {
                    found.push_back( f );
                }
            }
        }
        return found;
    }

    void note_global( Node_id global )
    {
        if( index_.contains( global.v ) )
        {
            return;
        }

        const bool owning = instance_owns( ast_, types_.table(), types_.type_of( global ), types_.recorded() );
        index_[global.v]  = owning ? globals_.size() : k_none;
        if( owning )
        {
            globals_.push_back( global );
        }
    }

    // A folded function-pointer global names its functions only in its value.
    void note_constant( const Constant_value& value )
    {
        if( value.kind == Constant_value::Kind::Address )
        {
            if( const auto it = by_declaration_.find( value.target.v ); it != by_declaration_.end() )
            {
                addressed_.insert( addressed_.end(), it->second.begin(), it->second.end() );
            }
        }
        for( const Constant_value& element : value.elements )
        {
            note_constant( element );
        }
    }

    void note_place( const Place& place, std::vector<Node_id>& named )
    {
        if( !place.is_global() )
        {
            return;
        }
        note_global( place.global );
        named.push_back( place.global );
        if( const std::optional<Constant_value> value = types_.constant_of( place.global ) )
        {
            note_constant( *value );
        }
    }

    void gather()
    {
        std::vector<std::vector<Node_id>> named_by( functions_.size() );
        for( u32 f = 0; f < functions_.size(); ++f )
        {
            const Function& function = functions_[f];
            if( function.declaration.is_valid() && ast_.kind( function.declaration ) == Node_kind::Destructor_decl )
            {
                destructors_.push_back( f );
            }

            for( const Statement& statement : function.statements )
            {
                note_place( statement.place, named_by[f] );
                if( !statement.value.is_valid() )
                {
                    continue;
                }

                const Rvalue& value = function.rvalue( statement );
                note_place( value.a.place, named_by[f] );
                note_place( value.b.place, named_by[f] );
                for( const Operand& argument : value.arguments )
                {
                    note_place( argument.place, named_by[f] );
                }

                if( value.kind == Rvalue_kind::Function_address )
                {
                    std::ranges::copy( named( value.callee, value.type_arguments ), std::back_inserter( addressed_ ) );
                }
            }
        }

        named_.assign( functions_.size(), Set( globals_.size(), false ) );
        for( u32 f = 0; f < functions_.size(); ++f )
        {
            for( const Node_id global : named_by[f] )
            {
                if( const std::size_t g = index_of( global ); g != k_none )
                {
                    named_[f][g] = true;
                }
            }
        }

        for( const Conformance& conformance : types_.conformances().all() )
        {
            for( std::size_t i = 0; i < conformance.requirements.size(); ++i )
            {
                if( const auto it = by_declaration_.find( conformance.methods[i].v ); it != by_declaration_.end() )
                {
                    std::vector<u32>& answers = answering_[conformance.requirements[i].v];
                    answers.insert( answers.end(), it->second.begin(), it->second.end() );
                }
            }
        }

        std::ranges::sort( addressed_ );
        addressed_.erase( std::ranges::unique( addressed_ ).begin(), addressed_.end() );
    }

    // Every function one statement may call: direct, through an interface or a pointer, or the
    // destructors a drop runs. A drop naming no destructor may run any field's.
    std::vector<u32> callees_of( const Function& function, const Statement& statement ) const
    {
        if( !statement.value.is_valid() )
        {
            return {};
        }

        const Rvalue& value = function.rvalue( statement );
        const bool    call  = statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Call;

        if( call && ast_.is_requirement( value.callee ) )
        {
            const auto it = answering_.find( value.callee.v );
            return it == answering_.end() ? std::vector<u32> {} : it->second;
        }
        if( call || ( statement.kind == Statement_kind::Drop && value.callee.is_valid() ) )
        {
            return named( value.callee, value.type_arguments );
        }
        if( statement.kind == Statement_kind::Drop )
        {
            return destructors_;
        }
        if( statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Indirect_call )
        {
            return addressed_;
        }
        return {};
    }

    std::vector<u32> callees_of( u32 f ) const
    {
        std::vector<u32> callees;
        for( const Statement& statement : functions_[f].statements )
        {
            std::ranges::copy( callees_of( functions_[f], statement ), std::back_inserter( callees ) );
        }

        std::ranges::sort( callees );
        callees.erase( std::ranges::unique( callees ).begin(), callees.end() );
        return callees;
    }

    bool returns_address( u32 f ) const
    {
        const Function& function = functions_[f];
        return function.returns_a_value && types_.table().is_pointer( function.locals[Local_id { 0 }].type );
    }

    std::size_t index_of( Node_id global ) const
    {
        return index_.at( global.v );
    }

    bool through_pointer( const Function& function, const Place& place ) const
    {
        return !place.is_global() && place.num_projections != 0 &&
               function.projections_of( place )[0].kind == Projection_kind::Deref;
    }

    // The globals the address of `place`, or with `copy` a pointer read from it, may point into:
    // the global itself or what it owns, or what the pointer it is reached through points into.
    void reached( const std::vector<Set>& pointers, const Function& function, const Place& place, bool copy, Set& into ) const
    {
        if( place.is_global() )
        {
            if( const std::size_t g = index_of( place.global ); g != k_none )
            {
                into[g] = true;
            }
        }
        else if( through_pointer( function, place ) || ( copy && place.local.is_valid() ) )
        {
            join( into, pointers[place.local.v] );
        }
    }

    // Per local, which globals it may point into. Flow-insensitive: every assignment to a whole
    // local, until nothing changes.
    std::vector<Set> pointers_of( u32 f ) const
    {
        const Function&  function = functions_[f];
        const Set        none( globals_.size(), false );
        std::vector<Set> pointers( function.locals.size(), none );

        const auto operand = [&]( const Operand& from, Set& into )
        {
            if( from.kind != Operand_kind::Constant )
            {
                reached( pointers, function, from.place, true, into );
            }
        };

        for( bool changed = true; changed; )
        {
            changed = false;
            for( const Statement& statement : function.statements )
            {
                if( statement.kind != Statement_kind::Assign || statement.place.is_global() || !statement.value.is_valid() )
                {
                    continue;
                }

                const Rvalue& value = function.rvalue( statement );
                Set           into  = none;

                switch( value.kind )
                {
                case Rvalue_kind::Address_of:
                    reached( pointers, function, value.a.place, false, into );
                    break;
                case Rvalue_kind::Call:
                case Rvalue_kind::Indirect_call:
                    // A returned address may point at whatever its arguments reach.
                    for( const Operand& argument : value.arguments )
                    {
                        operand( argument, into );
                    }
                    for( const u32 c : callees_of( function, statement ) )
                    {
                        join( into, into_[c] );
                    }
                    break;
                case Rvalue_kind::Use:
                case Rvalue_kind::Binary:
                case Rvalue_kind::Cast:
                case Rvalue_kind::Run_of:
                case Rvalue_kind::Run_data:
                case Rvalue_kind::Field_read:
                    operand( value.a, into );
                    operand( value.b, into );
                    break;
                default:
                    break;
                }

                // A field path of a local holds what its parts hold.
                if( !through_pointer( function, statement.place ) )
                {
                    changed = join( pointers[statement.place.local.v], into ) || changed;
                }
            }
        }
        return pointers;
    }

    void direct_writes( u32 f, const std::vector<Set>& pointers )
    {
        const Function& function = functions_[f];

        const auto written = [&]( const Place& place ) { reached( pointers, function, place, false, writes_[f] ); };

        for( const Statement& statement : function.statements )
        {
            if( statement.kind == Statement_kind::Assign || statement.kind == Statement_kind::Drop )
            {
                written( statement.place );
            }

            if( statement.value.is_valid() )
            {
                const Rvalue& value = function.rvalue( statement );
                if( value.kind == Rvalue_kind::Address_of && value.address_purpose != Address_purpose::Read )
                {
                    written( value.a.place );
                }
            }
        }
    }

    static constexpr std::size_t k_none = ~std::size_t { 0 };
};

} // namespace

const Call_effects* Global_effects::of( const Function& function, const Rvalue& call ) const
{
    if( calls.empty() || function.rvalues.empty() || &call < &function.rvalues.front() || &call > &function.rvalues.back() )
    {
        return nullptr;
    }

    const Rvalue_id id { narrow_cast<u32>( &call - &function.rvalues.front() ) };
    const auto      it = std::ranges::lower_bound( calls, id, {}, &Call_effects::call );
    return it != calls.end() && it->call == id ? &*it : nullptr;
}

bool Global_effects::owns( Node_id global ) const
{
    return std::ranges::binary_search( owning, global );
}

std::vector<Global_effects> global_effects_of( std::span<const Function> functions, const Ast& ast, Types& types )
{
    return Effects( functions, ast, types ).run();
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <string>
#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

constexpr std::string_view k_buf = "class Buf {\n"
                                   "    u8[*] b;\n"
                                   "    public i32 n;\n"
                                   "    public Buf() { unsafe { b = alloc<u8>( 1 ); } n = 0; }\n"
                                   "    ~Buf() { unsafe { free( b ); } }\n"
                                   "    public void push( i32 v ) { n = n + v; }\n"
                                   "    public i32 size() const { return n; }\n"
                                   "    public const u8* first() const { unsafe { return &b[0]; } }\n"
                                   "};\n"
                                   "Buf G = Buf();\n"
                                   "Buf H = Buf();\n"
                                   "i32 count = 3;\n";

struct Effects_of : Compiled
{
    explicit Effects_of( std::string_view source )
        : Compiled( std::string( k_buf ) + std::string( source ) )
    {
        effects = global_effects_of( all, ast, types );
    }

    std::vector<Global_effects> effects;

    // The globals' names, in order.
    std::string names( std::string_view function, bool returned = false ) const
    {
        for( std::size_t f = 0; f < all.size(); ++f )
        {
            if( all[f].declaration.is_valid() && interner.text( ast.name( all[f].declaration ) ) == function )
            {
                std::string text;
                for( const Node_id global : returned ? effects[f].returns_into : effects[f].writes )
                {
                    text += std::string( text.empty() ? "" : " " ) + std::string( interner.text( ast.name( global ) ) );
                }
                return text;
            }
        }
        return "<none>";
    }
};

} // namespace

TEST_CASE( "global_effects_of finds a direct write and one two calls deep", "[global_writes]" )
{
    const Effects_of p( "void put() { G.push( 1 ); }\n"
                        "void middle() { put(); }\n"
                        "void outer() { middle(); }\n"
                        "i32 main() { outer(); return 0; }\n" );
    INFO( p.rendered() );
    REQUIRE( p.clean() );
    CHECK( p.names( "put" ) == "G" );
    CHECK( p.names( "outer" ) == "G" );
    CHECK( p.names( "main" ) == "G" );
}

TEST_CASE( "global_effects_of reads through a const receiver without writing", "[global_writes]" )
{
    const Effects_of p( "i32 look() { return G.size() + count; }\n"
                        "void bump() { count = count + 1; }\n"
                        "i32 main() { bump(); return look(); }\n" );
    INFO( p.rendered() );
    REQUIRE( p.clean() );
    CHECK( p.names( "look" ).empty() );
    CHECK( p.names( "bump" ).empty() ); // `count` owns nothing
}

TEST_CASE( "global_effects_of joins mutually recursive functions", "[global_writes]" )
{
    const Effects_of p( "void ping( i32 n ) { if( n > 0 ) { pong( n - 1 ); } }\n"
                        "void pong( i32 n ) { H.push( n ); ping( n ); }\n"
                        "void other() { G = Buf(); }\n"
                        "i32 main() { ping( 2 ); other(); return 0; }\n" );
    INFO( p.rendered() );
    REQUIRE( p.clean() );
    CHECK( p.names( "ping" ) == "H" );
    CHECK( p.names( "pong" ) == "H" );
    CHECK( p.names( "other" ) == "G" );
    CHECK( p.names( "main" ) == "G H" );
}

TEST_CASE( "global_effects_of finds a write through a pointer it was given back", "[global_writes]" )
{
    const Effects_of p( "void fill( ref Buf b ) { b.push( 1 ); }\n"
                        "void through() { fill( ref G ); }\n"
                        "i32 main() { through(); return 0; }\n" );
    INFO( p.rendered() );
    REQUIRE( p.clean() );
    CHECK( p.names( "fill" ).empty() );
    CHECK( p.names( "through" ) == "G" );
}

TEST_CASE( "global_effects_of reaches every function whose address is taken", "[global_writes]" )
{
    const Effects_of p( "void put() { G.push( 1 ); }\n"
                        "void idle() {}\n"
                        "void call( fn() -> void f ) { f(); }\n"
                        "i32 main() { call( &idle ); call( kept ); return 0; }\n"
                        "fn() -> void kept = &put;\n" );
    INFO( p.rendered() );
    REQUIRE( p.clean() );
    CHECK( p.names( "call" ) == "G" ); // `put` through `kept`, a folded global
}

TEST_CASE( "global_effects_of reaches every method answering an interface call", "[global_writes]" )
{
    const Effects_of p(
        "interface sink { void take( i32 n ); };\n"
        "class Into implements sink { i32 n; public Into() { n = 0; } public void take( i32 n ) { G.push( n ); } };\n"
        "class Away implements sink { i32 n; public Away() { n = 0; } public void take( i32 n ) {} };\n"
        "void give( ref sink s ) { s.take( 1 ); }\n"
        "i32 main() { Away a = Away(); give( ref a ); Into i = Into(); give( ref i ); return 0; }\n"
    );
    INFO( p.rendered() );
    REQUIRE( p.clean() );
    CHECK( p.names( "give" ) == "G" );
}

TEST_CASE( "global_effects_of finds a write in a destructor a drop runs", "[global_writes]" )
{
    const Effects_of p( "class Logger { i32 n; public Logger() { n = 0; } ~Logger() { G.push( 1 ); } };\n"
                        "class Pair { Logger a; i32 b; public Pair() { a = Logger(); b = 1; } };\n"
                        "void one() { Logger l = Logger(); }\n"
                        "void two() { Pair p = Pair(); }\n"
                        "i32 main() { one(); two(); return 0; }\n" );
    INFO( p.rendered() );
    REQUIRE( p.clean() );
    CHECK( p.names( "one" ) == "G" );
    CHECK( p.names( "two" ) == "G" );
}

TEST_CASE( "global_effects_of finds an address returned into a global", "[global_writes]" )
{
    const Effects_of p( "const u8* raw() { return G.first(); }\n"
                        "const u8* again() { return raw(); }\n"
                        "i32* whole() { return &G.n; }\n"
                        "i32 main() { const u8* a = again(); i32* w = whole(); return 0; }\n" );
    INFO( p.rendered() );
    REQUIRE( p.clean() );
    CHECK( p.names( "raw", true ) == "G" );
    CHECK( p.names( "again", true ) == "G" );
    CHECK( p.names( "whole", true ) == "G" ); // its own storage, which `whole().b` reaches past
    CHECK( p.names( "raw" ).empty() );
}

} // namespace keel
#endif
