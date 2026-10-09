// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/borrow_check.h"

#include <span>
#include "check/borrows.h"

namespace keel
{

namespace
{

// One bit per local: whether a later statement reads it.
using Live = std::vector<bool>;

void use_place( const Place& place, Live& live )
{
    if( place.local.is_valid() && !place.is_global() )
    {
        live[place.local.v] = true;
    }
}

void use_operand( const Operand& operand, Live& live )
{
    if( operand.kind != Operand_kind::Constant )
    {
        use_place( operand.place, live );
    }
}

// Live before `statement`, from live after it: a whole write kills, then every read is a use.
void step_back( const Function& func, const Statement& statement, Live& live )
{
    if( statement.kind == Statement_kind::Assign )
    {
        if( !statement.place.is_global() && statement.place.num_projections == 0 )
        {
            live[statement.place.local.v] = false;
        }
        else if( !statement.place.is_global() && statement.place.num_projections != 0 )
        {
            use_place( statement.place, live );
        }

        use_operand( statement.value.a, live );
        use_operand( statement.value.b, live );

        for( u32 i = statement.value.first_argument; i < statement.value.first_argument + statement.value.argument_count; ++i )
        {
            use_operand( func.operands[i], live );
        }

        if( statement.value.kind == Rvalue_kind::Address_of )
        {
            use_place( statement.value.a.place, live );
        }
    }
    else if( statement.kind == Statement_kind::Drop )
    {
        use_place( statement.place, live );
    }
}

void step_back_terminator( const Terminator& terminator, Live& live )
{
    use_operand( terminator.condition, live );
    use_operand( terminator.message, live );
}

// Live on leaving `block`: live on entering any successor.
Live live_out( const Function& func, Block_id block, const std::vector<Live>& in )
{
    Live out( func.locals.size(), false );

    std::vector<Block_id> next;
    successors( func.blocks[block.v].terminator, next );

    for( std::size_t i = 0; i < next.size(); ++i )
    {
        const Block_id target = next[i];

        for( std::size_t k = 0; k < func.locals.size(); ++k )
        {
            out[k] = out[k] || in[target.v][k];
        }
    }

    return out;
}

// Live on entering each block. Backward, to a fixpoint; ends because a bit is only ever set.
std::vector<Live> live_in( const Function& func )
{
    std::vector<Live> in( func.blocks.size(), Live( func.locals.size(), false ) );

    bool changed = true;
    while( changed )
    {
        changed = false;

        for( u32 b = 0; b < narrow_cast<u32>( func.blocks.size() ); ++b )
        {
            const Block& block = func.blocks[b];

            Live live = live_out( func, Block_id { b }, in );
            step_back_terminator( block.terminator, live );

            for( u32 i = block.statement_count; i > 0; --i )
            {
                step_back( func, func.statements[block.first_statement + i - 1], live );
            }

            if( live != in[b] )
            {
                in[b]   = std::move( live );
                changed = true;
            }
        }
    }

    return in;
}

// One thing a statement does to a place, and where to report it.
struct Effect
{
    Place         place;
    Loan_conflict conflict;
    Span          at;
};

// `(*r)` through a binding of a local is that local's place; anything else is itself.
Place resolve( const Function& func, const std::vector<Borrow>& borrows, const Place& place )
{
    if( !place.is_global() && place.num_projections != 0 &&
        func.projections[place.first_projection].kind == Projection_kind::Deref && borrows[place.local.v].of.is_valid() )
    {
        return borrows[place.local.v].place;
    }

    return place;
}

// Every move, assignment, drop and changing argument in one statement.
void effects_of(
    const Function& func, const std::vector<Borrow>& borrows, const Statement& statement, std::vector<Effect>& out
)
{
    out.clear();

    if( statement.kind == Statement_kind::Assign )
    {
        const Rvalue&                  value = statement.value;
        const std::span<const Operand> arguments( func.operands.data() + value.first_argument, value.argument_count );

        const auto moved = [&]( const Operand& operand )
        {
            if( operand.kind == Operand_kind::Move && !operand.place.is_global() && operand.place.num_projections == 0 )
            {
                out.push_back( Effect {
                    .place = Place { .local = operand.place.local }, .conflict = Loan_conflict::Moved, .at = statement.span
                } );
            }
        };

        moved( value.a );
        moved( value.b );

        for( const Operand& argument : arguments )
        {
            moved( argument );
        }

        if( !statement.place.is_global() )
        {
            out.push_back( Effect {
                .place = resolve( func, borrows, statement.place ), .conflict = Loan_conflict::Assigned, .at = statement.span
            } );
        }

        if( value.kind != Rvalue_kind::Call && value.kind != Rvalue_kind::Indirect_call )
        {
            return;
        }

        // Checked at the call rather than where the address is taken, so an argument read by value
        // in the same call is not still live: `v.push( x )`.
        for( const Operand& argument : arguments )
        {
            if( argument.kind != Operand_kind::Copy || argument.place.is_global() || argument.place.num_projections != 0 ||
                func.locals[argument.place.local.v].name.is_valid() )
            {
                continue;
            }

            const Borrow& borrow = borrows[argument.place.local.v];

            if( borrow.of.is_valid() && borrow.purpose != Address_purpose::Read )
            {
                out.push_back( Effect { .place = borrow.place, .conflict = Loan_conflict::Changed, .at = borrow.at } );
            }
        }
    }
    else if( statement.kind == Statement_kind::Drop )
    {
        out.push_back( Effect {
            .place = resolve( func, borrows, statement.place ), .conflict = Loan_conflict::Assigned, .at = statement.span
        } );
    }
}

// A `ref` binding of a place or of a call's result. An argument's temporary is the one-call rule's,
// and a named pointer is assigned a copy of an address, never the address itself.
bool is_holder( const Function& func, const Borrow& borrow, Local_id local )
{
    return func.locals[local.v].name.is_valid() && borrow.assignments == 1 && borrow.bound &&
           ( borrow.of.is_valid() || borrow.through != nullptr ) && borrow.purpose != Address_purpose::Initialise;
}

// D54: a payload's loan is broken by anything reaching it, since another variant destroys it; a
// direct loan only by a move; an indirect one by anything reaching its object, if that object owns
// something, since otherwise it has nothing to free or relocate.
bool conflicts(
    const Function&            func,
    const std::vector<Borrow>& borrows,
    const std::vector<bool>&   owning,
    const Borrow&              loan,
    const Effect&              effect
)
{
    if( loan.purpose == Address_purpose::Payload )
    {
        return overlaps( func, loan.place, effect.place );
    }

    if( loan.of.is_valid() )
    {
        return effect.conflict == Loan_conflict::Moved && overlaps( func, loan.place, effect.place );
    }

    return loan.through != nullptr && ( effect.conflict == Loan_conflict::Moved || owning[effect.place.local.v] ) &&
           reaches( func, borrows, loan, effect.place );
}

} // namespace

std::vector<bool> owning_locals( const Function& func, const Ast& ast, Types& types )
{
    std::vector<bool> owning( func.locals.size(), false );

    for( std::size_t l = 0; l < func.locals.size(); ++l )
    {
        const Type&   type = types.table().get( func.locals[l].type );
        const Type_id of   = type.kind == Type_kind::Pointer ? type.element : func.locals[l].type;

        owning[l] = instance_owns( ast, types.table(), of, types.recorded() );
    }

    return owning;
}

std::vector<Loan_error> check_loans( const Function& func, const std::vector<bool>& owning )
{
    const std::vector<Borrow> borrows = borrows_of( func );
    const std::vector<Live>   live    = live_in( func );

    std::vector<Local_id> holders;
    for( u32 l = 0; l < narrow_cast<u32>( func.locals.size() ); ++l )
    {
        if( is_holder( func, borrows[l], Local_id { l } ) )
        {
            holders.push_back( Local_id { l } );
        }
    }

    std::vector<bool>       reported( func.locals.size(), false );
    std::vector<Loan_error> errors;
    std::vector<Effect>     effects;

    // Blocks in order, so a holder is reported at its first conflict.
    for( u32 b = 0; b < narrow_cast<u32>( func.blocks.size() ); ++b )
    {
        const Block& block = func.blocks[b];
        Live         out   = live_out( func, Block_id { b }, live );

        step_back_terminator( block.terminator, out );

        // What is live after each statement.
        std::vector<Live> after( block.statement_count );
        for( u32 i = block.statement_count; i > 0; --i )
        {
            after[i - 1] = out;
            step_back( func, func.statements[block.first_statement + i - 1], out );
        }

        for( u32 i = 0; i < block.statement_count; ++i )
        {
            effects_of( func, borrows, func.statements[block.first_statement + i], effects );

            for( const Effect& effect : effects )
            {
                for( const Local_id holder : holders )
                {
                    if( !after[i][holder.v] || reported[holder.v] )
                    {
                        continue;
                    }

                    const Borrow& loan = borrows[holder.v];

                    if( conflicts( func, borrows, owning, loan, effect ) )
                    {
                        errors.push_back( Loan_error {
                            .object   = loan.of.is_valid() ? loan.of : effect.place.local,
                            .holder   = holder,
                            .at       = effect.at,
                            .taken    = loan.at,
                            .conflict = effect.conflict
                        } );
                        reported[holder.v] = true;
                    }
                }
            }
        }
    }

    return errors;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

struct Loaned : Compiled
{
    // By name, since an enum's destructor is lowered after the functions written.
    explicit Loaned( std::string_view source, std::string_view under_test = "main" )
        : Compiled( source )
    {
        for( std::size_t f = 0; f < functions.size(); ++f )
        {
            if( interner.text( ast.name( functions[f].declaration ) ) == under_test )
            {
                tested = f;
                owning = owning_locals( functions[f], ast, types );
            }
        }
    }

    std::vector<Loan_error> errors() const
    {
        return check_loans( functions[tested], owning );
    }

    std::size_t       tested = 0;
    std::vector<bool> owning;

    std::string_view name_of( Local_id local ) const
    {
        return interner.text( functions[tested].locals[local.v].name );
    }
};

struct Refused
{
    const char*   body;
    Loan_conflict conflict;
    const char*   at;     // found in the breaking statement's text
    const char*   holder; // the binding whose loan it breaks
    const char*   object; // what that binding borrows
};

void require_refused( std::string_view prelude, std::string_view locals, const Refused& refused )
{
    const Loaned p( std::string( prelude ) + "i32 main() { " + std::string( locals ) + refused.body + " return 0; }" );

    INFO( refused.body << "\n" << p.rendered() );
    REQUIRE( p.clean() );

    const std::vector<Loan_error> errors = p.errors();

    REQUIRE( errors.size() == 1 );
    REQUIRE( errors[0].conflict == refused.conflict );
    REQUIRE( p.sm.text( errors[0].at ).find( refused.at ) != std::string_view::npos );
    REQUIRE( p.name_of( errors[0].holder ) == refused.holder );
    REQUIRE( p.name_of( errors[0].object ) == refused.object );
}

void require_accepted( std::string_view prelude, std::string_view locals, std::string_view body )
{
    const Loaned p( std::string( prelude ) + "i32 main() { " + std::string( locals ) + std::string( body ) + " return 0; }" );

    INFO( body << "\n" << p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.errors().empty() );
}

constexpr std::string_view k_owned = "class B { public u64 n; B() { n = 0; } ~B() { } };\n"
                                     "void take( move B b ) { }\n"
                                     "void poke( ref B b ) { }\n";

constexpr std::string_view k_owned_locals = "B b = B(); u64 c = 0; ";

constexpr std::string_view k_vector =
    "class V { public i32 x; V() { x = 0; } ~V() { } public i32* operator[]( u64 i ) const { return nullptr; } "
    "public const ref i32 get() const { return x; } public i32 size() const { return 0; } public void push( i32 e ) { } };\n"
    "class W { public V a; public V b; W() { a = V(); b = V(); } };\n"
    "void take( move V v ) { }\n"
    "void poke( ref V v ) { }\n"
    "void look( const ref V v ) { }\n"
    "const ref V pick( const ref V a, const ref V b ) { return a; }\n";

constexpr std::string_view k_vector_locals = "V v = V(); V u = V(); W w = W(); u64 i = 0; ";

constexpr std::string_view k_plain = "struct P { i32 x; i32 y; };\n"
                                     "const ref i32 picks( const ref i32 a, const ref i32 b ) { return a; }\n"
                                     "const ref i32 gets( const ref P p ) { return p.y; }\n"
                                     "void bump( ref P p ) { }\n";

constexpr std::string_view k_plain_locals = "i32 a = 1; i32 c = 2; P p = P { 1, 2 }; ";

constexpr std::string_view k_payload = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                                       "enum H { Full( B b ), Empty };\n"
                                       "void reset( ref H h ) { h = H::Empty; }\n"
                                       "void take( move H h ) { }\n";

constexpr std::string_view k_payload_locals = "H h = H::Full( B( 1 ) ); u64 c = 0; ";

} // namespace

// D54: a direct loan points at its object's own storage, which stays put until its scope ends, so
// only a move breaks it.
TEST_CASE( "borrow_check_refuses_a_move_under_a_direct_loan", "[check][borrow][m9]" )
{
    SECTION( "refused" )
    {
        for( const Refused& refused : {
                 Refused { "ref B r = b; take( move b ); u64 n = r.n;", Loan_conflict::Moved, "move b", "r", "b" },
                 Refused { "ref u64 m = b.n; B d = move b; u64 k = m;", Loan_conflict::Moved, "move b", "m", "b" },
                 Refused { "ref B r = b; ref B s = r; take( move b ); u64 n = s.n;", Loan_conflict::Moved, "move b", "s", "b" },
                 Refused {
                     "ref B r = b; if ( c == 0 ) { take( move b ); } u64 n = r.n;", Loan_conflict::Moved, "move b", "r", "b"
                 },
             } )
        {
            require_refused( k_owned, k_owned_locals, refused );
        }
    }

    // Dead before the move, a write into the same storage, another object, or a pointer.
    SECTION( "accepted" )
    {
        for( const char* body : {
                 "ref B r = b; u64 n = r.n; take( move b );",
                 "ref B r = b; b = B(); poke( ref b ); b.n = 3; u64 n = r.n;",
                 "ref u64 m = b.n; B d = B(); take( move d ); u64 k = m;",
                 "if ( c == 0 ) { ref B r = b; u64 n = r.n; } take( move b );",
                 // A pointer says it can dangle.
                 "B* p = &b; take( move b ); B* q = p;",
             } )
        {
            require_accepted( k_owned, k_owned_locals, body );
        }
    }
}

// D54: an indirect loan reaches storage its object owns elsewhere, so while it is live the object
// is frozen: anything that could free or reallocate that storage is refused.
TEST_CASE( "borrow_check_freezes_the_object_of_an_indirect_loan", "[check][borrow][m9]" )
{
    SECTION( "refused" )
    {
        for( const Refused& refused : {
                 Refused { "ref i32 x = v[0]; v.push( 1 ); i32 y = x;", Loan_conflict::Changed, "v", "x", "v" },
                 Refused { "ref i32 x = v[0]; poke( ref v ); i32 y = x;", Loan_conflict::Changed, "ref v", "x", "v" },
                 Refused { "ref i32 x = v[0]; take( move v ); i32 y = x;", Loan_conflict::Moved, "move v", "x", "v" },
                 Refused { "ref i32 x = v[0]; v = V(); i32 y = x;", Loan_conflict::Assigned, "v", "x", "v" },
                 Refused { "const ref i32 g = v.get(); v.push( 1 ); i32 y = g;", Loan_conflict::Changed, "v", "g", "v" },
                 Refused {
                     "const ref i32 g = v.get(); const ref i32 h = g; v.push( 1 ); i32 y = h;",
                     Loan_conflict::Changed,
                     "v",
                     "h",
                     "v"
                 },
                 Refused {
                     "ref i32 x = v[0]; if ( i == 0 ) { v.push( 1 ); } i32 y = x;", Loan_conflict::Changed, "v", "x", "v"
                 },
                 // The next iteration's read is after the push.
                 Refused {
                     "ref i32 x = v[0]; while ( i < 2 ) { i32 y = x; v.push( 1 ); i = i + 1; }",
                     Loan_conflict::Changed,
                     "v",
                     "x",
                     "v"
                 },
                 // Taken through a `ref` binding, or changed through one: either way it is `v`.
                 Refused { "ref V r = v; ref i32 x = r[0]; v.push( 1 ); i32 y = x;", Loan_conflict::Changed, "v", "x", "v" },
                 Refused { "ref i32 x = v[0]; ref V r = v; r.push( 1 ); i32 y = x;", Loan_conflict::Changed, "r", "x", "v" },
                 Refused { "ref i32 x = w.b[0]; w.b.push( 1 ); i32 y = x;", Loan_conflict::Changed, "w.b", "x", "w" },
                 Refused { "ref i32 x = w.b[0]; w = W(); i32 y = x;", Loan_conflict::Assigned, "w", "x", "w" },
                 // A returned reference borrows every borrowed argument.
                 Refused { "const ref V p = pick( v, u ); u.push( 1 ); i32 z = p.x;", Loan_conflict::Changed, "u", "p", "u" },
             } )
        {
            require_refused( k_vector, k_vector_locals, refused );
        }
    }

    // Dead before the change, only read, another field or object, or a fresh loan each iteration.
    SECTION( "accepted" )
    {
        for( const char* body : {
                 "ref i32 x = v[0]; i32 y = x; v.push( 1 );",
                 // Copied into the call before it runs.
                 "ref i32 x = v[0]; v.push( x );",
                 "ref i32 x = v[0]; i32 n = v.size(); look( v ); i32 z = v[1]; i32 y = x;",
                 "ref i32 x = v[0]; ref i32 y = v[1]; i32 z = x + y;",
                 "ref i32 x = v[0]; x = 5; i32 y = x;",
                 "ref i32 x = v[0]; ref V r = v; i32 y = x; r.push( 1 );",
                 "ref i32 x = w.b[0]; w.a.push( 1 ); i32 y = x;",
                 "ref i32 x = v[0]; u.push( 1 ); i32 y = x;",
                 "while ( i < 2 ) { ref i32 x = v[0]; i32 y = x; v.push( 1 ); i = i + 1; }",
             } )
        {
            require_accepted( k_vector, k_vector_locals, body );
        }
    }

    // The object is the caller's, reached through the parameter.
    SECTION( "through a `ref` parameter" )
    {
        const Loaned p(
            std::string( k_vector ) + "i32 main() { return 0; }\n"
                                      "i32 first( ref V v ) { ref i32 x = v[0]; v.push( 1 ); return x; }\n",
            "first"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::vector<Loan_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].conflict == Loan_conflict::Changed );
        REQUIRE( p.name_of( errors[0].holder ) == "x" );
    }
}

// D54: an object that owns nothing has nothing to free or relocate, so a returned reference into it
// is broken only by a move, as a direct loan is.
TEST_CASE( "borrow_check_freezes_only_an_owner", "[check][borrow][m9]" )
{
    require_refused(
        k_plain,
        k_plain_locals,
        Refused { "const ref i32 m = picks( a, c ); i32 z = move c; i32 y = m;", Loan_conflict::Moved, "move c", "m", "c" }
    );

    for( const char* body : {
             "const ref i32 m = picks( a, c ); c = 50; i32 y = m;",
             "const ref i32 r = gets( p ); p = P { 3, 4 }; i32 y = r;",
             "const ref i32 r = gets( p ); bump( ref p ); i32 y = r;",
         } )
    {
        require_accepted( k_plain, k_plain_locals, body );
    }
}

// D54: a pattern binding borrows its payload, and assigning another variant destroys it, so
// anything reaching what was matched breaks the loan, whichever name it goes through.
TEST_CASE( "borrow_check_holds_what_a_payload_binding_borrows", "[check][borrow][m9]" )
{
    const auto arm = []( std::string_view scrutinee, std::string_view full )
    {
        return std::string( "switch( " ) + std::string( scrutinee ) + " ) { case H::Full( b ): " + std::string( full ) +
               " c = b.n; break; case H::Empty: break; }";
    };

    struct Case
    {
        std::string   body;
        Loan_conflict conflict;
        const char*   at;
    };

    for( const Case& refused : {
             Case { arm( "h", "h = H::Empty;" ), Loan_conflict::Assigned, "h" },
             Case { arm( "h", "reset( ref h );" ), Loan_conflict::Changed, "h" },
             Case { arm( "h", "take( move h );" ), Loan_conflict::Moved, "h" },
             Case { "ref H r = h; " + arm( "r", "h = H::Empty;" ), Loan_conflict::Assigned, "h" },
             Case { "ref H r = h; " + arm( "h", "r = H::Empty;" ), Loan_conflict::Assigned, "r" },
         } )
    {
        require_refused(
            k_payload, k_payload_locals, Refused { refused.body.c_str(), refused.conflict, refused.at, "b", "h" }
        );
    }

    // After the binding's last use, after the switch, or in an arm that borrows nothing.
    for( const std::string& body : {
             std::string( "switch( h ) { case H::Full( b ): c = b.n; h = H::Empty; break; case H::Empty: break; }" ),
             arm( "h", "" ) + " h = H::Empty;",
             std::string( "switch( h ) { case H::Full( b ): break; case H::Empty: h = H::Empty; break; }" ),
         } )
    {
        require_accepted( k_payload, k_payload_locals, body );
    }
}

// One error per loan, however many statements break it.
TEST_CASE( "borrow_check_reports_a_loan_once", "[check][borrow][m9]" )
{
    const Loaned p(
        std::string( k_vector ) + "i32 main() { " + std::string( k_vector_locals ) +
        "ref i32 x = v[0]; v.push( 1 ); poke( ref v ); v = V(); i32 y = x; return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::vector<Loan_error> errors = p.errors();

    REQUIRE( errors.size() == 1 );
    REQUIRE( p.sm.text( errors[0].at ) == "v" );
    REQUIRE( p.sm.text( errors[0].taken ).find( "v[0]" ) != std::string_view::npos );
}

} // namespace keel
#endif
