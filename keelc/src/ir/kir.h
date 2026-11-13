// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <cassert>
#include <utility>
#include <vector>
#include "ast/node.h"
#include "common/id_vector.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/run.h"
#include "common/span.h"
#include "common/types.h"
#include "lex/token.h"
#include "sema/type.h"

namespace keel
{

// PLAN §3.1. A flat CFG rather than a tree: every intermediate result is a named local, and
// control flow is edges between blocks. Deliberately not SSA - the analysis in §8 tracks whether a
// *named local* is initialised or moved, which SSA would rename apart and obscure.
//
// This header is data only. Passes are free functions over it, which is what stops KIR growing
// into another file-local class the size of Checker (§3.2).

// 0xFFFF'FFFF rather than 0, matching Node_id and Symbol_id - index 0 has to stay usable, since
// block 0 is the entry block and local 0 is the return slot.
inline constexpr u32 k_invalid_kir_id = 0xFFFF'FFFFu;

struct Local_id
{
    u32 v = k_invalid_kir_id;

    bool is_valid() const
    {
        return v != k_invalid_kir_id;
    }

    auto operator<=>( const Local_id& other ) const = default;
};

inline constexpr Local_id k_return_slot { 0 };

struct Block_id
{
    u32 v = k_invalid_kir_id;

    bool is_valid() const
    {
        return v != k_invalid_kir_id;
    }

    auto operator<=>( const Block_id& other ) const = default;
};

inline constexpr Block_id k_entry_block { 0 };

struct Rvalue_id
{
    u32 v = k_invalid_kir_id;

    bool is_valid() const
    {
        return v != k_invalid_kir_id;
    }

    auto operator<=>( const Rvalue_id& other ) const = default;
};

// A temporary is a local with no name. Nothing else distinguishes it: the drop analysis asks which
// locals are live, and has no reason to care which ones the author wrote.
struct Local
{
    Type_id   type {};
    Span      span {};
    Symbol_id name {};             // invalid for a temporary
    bool      built_whole = false; // a class with a constructor or destructor: assigned only whole
};

enum class Projection_kind : u8
{
    Field,
    Deref,
    Tag,    // D7: the discriminant of an enum that carries payloads, or of a union; `field` is unused
    Member, // §6.7: one member of a union
    Index   // an element of a `T[N]`; `index` is the `u64` local holding which
};

struct Projection
{
    Projection_kind kind = Projection_kind::Deref;
    Node_id         field {};  // the Field_decl; valid only for kind == Field
    Type_id         member {}; // valid only for kind == Member
    Local_id        index {};  // valid only for kind == Index
};

// Somewhere a value lives: `x`, `p.y`, `(*q).z`. The same split the C emitter found as lower()
// versus lower_place(), first-class here because move checking asks whether `p.y` was moved out
// of - which needs the path, not just the local.
struct Place
{
    Local_id local {};
    Node_id  global {};            // a file-scope Var_decl - exactly one of these is valid
    u32      first_projection = 0; // into Function::projections
    u32      num_projections  = 0;
    Type_id  owner {}; // the instantiation whose static this is, stays invalid for file-scope variables and non-generic statics

    bool is_global() const
    {
        return global.is_valid();
    }

    // A local itself, not a part of one.
    bool is_whole_local() const
    {
        return !is_global() && local.is_valid() && num_projections == 0;
    }
};

enum class Operand_kind : u8
{
    Copy,
    Move, // M4 gives this meaning: the place is dead afterwards. Inert until then.
    Constant,
    Debug // true under --debug; settled after the checks, which see both paths
};

// A value being read.
struct Operand
{
    // Constant by default, so an operand a kind does not use holds nothing that looks like a
    // reference to a local.
    Operand_kind kind = Operand_kind::Constant;
    Place        place {};    // Copy and Move
    Literal_id   constant {}; // Constant: an integer's bits at the type's width, a float's signed value
    Type_id      type {};
    Span         span {}; // the expression it was lowered from; invalid when made by a pass

    bool has_place() const
    {
        return kind == Operand_kind::Copy || kind == Operand_kind::Move;
    }

    Span span_or( Span fallback ) const
    {
        return span.is_valid() ? span : fallback;
    }
};

enum class Rvalue_kind : u8
{
    Use,
    Binary,
    Unary,
    Overflows, // bool: whether `a op b` overflows `a`'s type; `op` is `+`, `-` or `*`
    Cast,
    Bits,     // `a` reinterpreted as the other of a float and an integer of its width
    Run_of,   // `a` is the T[*] data, `b` is the u64 count, and the type is the run pointer
    Run_data, // `a` is the run pointer, and the type is T[*]
    Run_size, // `a` is the run pointer, and the type is u64
    Call,
    Address_of, // reads a.place; the address is not a copy of what lives there
    Allocate,
    Release,
    Function_address,
    Indirect_call,
    Field_offset,   // callee is the Field_decl; the aggregate is argument 0 of the field type
    Field_read,     // a is the object's address, b the offset
    Literal_bytes,  // the address of a string literal's bytes; a is the literal
    Make_interface, // §6.7: `a` is an implementer's address, and the type the interface pointer of it and its vtable
    Vtable_slot,    // §6.7: `a` is an interface pointer, callee the requirement, and the type the slot's function
    Interface_self, // §6.7: `a` is an interface pointer, and the type the `void*` of its object
    Sqrt,           // the correctly rounded square root of the float `a`
};

// D80: a Call of a module's build or drop function, which no declaration has; `callee` is its first global.
enum class Globals_call : u8
{
    None,
    Build,
    Drop
};

enum class Address_purpose : u8
{
    Borrow,
    Read, // a `const ref` or a `const` method's receiver: nothing is written through it
    Initialise,
    Payload // a pattern binding's payload: changing the variant destroys it
};

// How a value is produced. One tagged struct rather than a variant hierarchy, as Node and Type
// already are, so -Wswitch keeps every consumer exhaustive.
struct Rvalue
{
    Rvalue_kind kind = Rvalue_kind::Use;
    Type_id     type {};
    Token_kind  op = Token_kind::Unknown; // Binary and Unary; a Call's to a D41 guard, which no declaration has
    Operand     a {};
    Operand     b {}; // Binary only
    Node_id     callee {};

    std::vector<Operand> arguments {}; // Call and Indirect_call

    // The callee's type arguments, for the same reason Function carries them: a Node_id no longer
    // identifies a function once one declaration can be emitted more than once. Defaulted like
    // every other field here, so the designated-initialiser factories below need not list it.
    std::vector<Type_id> type_arguments {};
    Address_purpose      address_purpose = Address_purpose::Borrow;
    Globals_call         globals         = Globals_call::None;
};

enum class Statement_kind : u8
{
    Assign,
    Drop,         // M3
    Storage_live, // scope entry and exit, which is what tells the drop pass where a scope ended
    Storage_dead,
};

// A statement's rvalue in Function::rvalues. Move-only, and made only by Function, so no two
// statements can hold one rvalue: a second statement gets its own through Function::clone.
class Owned_rvalue
{
public:
    Owned_rvalue() = default; // none: a storage marker

    Owned_rvalue( Owned_rvalue&& other ) noexcept
        : id_( std::exchange( other.id_, Rvalue_id {} ) )
    {
    }

    Owned_rvalue& operator=( Owned_rvalue&& other ) noexcept
    {
        id_ = std::exchange( other.id_, Rvalue_id {} );
        return *this;
    }

    Owned_rvalue( const Owned_rvalue& )            = delete;
    Owned_rvalue& operator=( const Owned_rvalue& ) = delete;

    Rvalue_id id() const
    {
        return id_;
    }

    bool is_valid() const
    {
        return id_.is_valid();
    }

private:
    friend struct Function;
    friend class Statements;

    explicit Owned_rvalue( Rvalue_id id )
        : id_( id )
    {
    }

    Rvalue_id id_ {};
};

struct Statement
{
    Statement_kind kind      = Statement_kind::Assign;
    bool           replacing = false; // Drop only: its place is assigned again.
    Span           span {};
    Place          place {};     // assigned to by Assign, dropped by Drop
    Owned_rvalue   value {};     // Assign; a class's Drop names its destructor in callee and type_arguments
    Local_id       drop_flag {}; // Drop only: invalid means drop unconditionally
};

// `statement` holding `value` instead of its own.
inline Statement with_value( const Statement& statement, Owned_rvalue value )
{
    return Statement {
        .kind      = statement.kind,
        .replacing = statement.replacing,
        .span      = statement.span,
        .place     = statement.place,
        .value     = std::move( value ),
        .drop_flag = statement.drop_flag,
    };
}

// Function::statements. Copied only with its function, whose rvalues come along.
class Statements : public std::vector<Statement>
{
public:
    Statements() = default;

    Statements( std::vector<Statement>&& statements )
        : vector( std::move( statements ) )
    {
    }

    Statements( Statements&& )            = default;
    Statements& operator=( Statements&& ) = default;
    ~Statements()                         = default;

private:
    friend struct Function;

    Statements( const Statements& other )
    {
        reserve( other.size() );
        for( const Statement& statement : other )
        {
            push_back( with_value( statement, Owned_rvalue( statement.value.id() ) ) );
        }
    }

    Statements& operator=( const Statements& other )
    {
        Statements copy( other );
        return *this = std::move( copy );
    }
};

enum class Terminator_kind : u8
{
    Unset, // the default, so a block nobody terminated is a verify error rather than a silent one
    Goto,
    Branch,
    Return,
    Unreachable,
    Panic // ends the program, running nothing
};

// What a Panic reports.
enum class Failure : u8
{
    Assert,     // span: the condition
    Message,    // span: the whole `panic( ... )`
    Cast,       // span: the whole `cast<T>( x )`
    Division,   // span: the operation
    Remainder,  // span: the operation
    Unordered,  // span: the comparison
    Unhashable, // span: the call
    Bounds,     // span: the index or slice
    Overflow,   // span: the operation
};

struct Terminator
{
    Terminator_kind kind    = Terminator_kind::Unset;
    Failure         failure = Failure::Assert; // Assert_failed
    Span            span {};
    Operand         condition {};  // Branch
    Block_id        targets[2] {}; // [0] for Goto; [0] true and [1] false for Branch
    Operand         message {};    // Panic: the message
    Operand         at {};         // Panic: the `location` a message was given, if any
};

// Exactly one terminator, and terminators appear nowhere else. verify() checks this first: it is
// what makes the graph a graph.
struct Block
{
    u32        first_statement = 0; // into Function::statements
    u32        statement_count = 0;
    Terminator terminator {};
};

constexpr u32 k_no_field = ~u32 { 0 };

// One field tracked on its own: of a local, or of an `out` parameter's or constructor's referent.
// A local's fields are one contiguous preorder run; `parent` and `end` index Function::fields.
struct Field_slot
{
    Local_id local {};
    u32      parent = k_no_field; // k_no_field for a top-level field
    u32      end    = 0;          // one past this field's subtree
    Node_id  field {};
    bool     is_owning   = false;
    bool     is_const    = false;
    bool     built_whole = false;
};

// Block 0 is the entry. Local 0 is the return slot, and locals 1..parameter_count are the
// parameters, in declaration order.
struct Function
{
    Node_id declaration {};

    // What this instantiation was emitted for; empty for an ordinary function. Nothing else in KIR
    // separates two instantiations of one generic - they share a declaration, so the declaration
    // alone no longer names a function.
    std::vector<Type_id> type_arguments {};

    u32 parameter_count = 0;
    // Which parameters arrive uninitialised. Nothing else in KIR says so - an `out` parameter's
    // local holds a valid address, and it is the referent that is empty - so the analysis that
    // needs it cannot work it out from the graph.
    std::vector<Local_id> out_parameters;
    std::vector<Local_id> one_path_temporaries;           // built on one path, so dropped under a flag
    std::vector<Local_id> walked;                         // D74: a walk's receiver, freezing what it walks
    std::vector<Local_id> chosen;                         // D75: assigned the address of whichever arm ran
    Local_id              constructed {};                 // a constructor's receiver, invalid in every other function.
    std::vector<Node_id>  defaulted_fields;               // D77: a constructor's fields given their default at entry
    Node_id               globals {};                     // D80: a module's build or drop function, by its first built global
    Token_kind            compares = Token_kind::Unknown; // D41: a guard's comparison; its type arguments are the operands'
    Type_id thunk_of {}; // §6.7: the interface a thunk's vtable slot is in; declared by the requirement, at the implementer
    bool    drops_globals = false;       // which of the two
    bool    entry         = false;       // the program's `main`, which builds the globals and calls the Keel `main`
    std::vector<Field_slot>      fields; // ir/slots reads them
    Id_vector<Local_id, Local>   locals;
    Id_vector<Block_id, Block>   blocks;
    Statements                   statements;
    std::vector<Projection>      projections;
    Id_vector<Rvalue_id, Rvalue> rvalues; // read through rvalue(); a removed statement's stays until simplify compacts

    // Whether local 0 is a value the body must produce. The passes have no Type_table, so this is
    // recorded the same way out_parameters is, and for the same reason: the graph does not say.
    bool returns_a_value = false;

    bool diverges = false; // returns `never`, so no path may reach a Return

    Run<Statement> statements_of( const Block& block )
    {
        return Run<Statement>( statements, block.first_statement, block.statement_count );
    }

    Run<const Statement> statements_of( const Block& block ) const
    {
        return Run<const Statement>( statements, block.first_statement, block.statement_count );
    }

    Run<Statement> statements_of( Block_id block )
    {
        return statements_of( blocks[block] );
    }

    Run<const Statement> statements_of( Block_id block ) const
    {
        return statements_of( blocks[block] );
    }

    Owned_rvalue add_rvalue( Rvalue value )
    {
        return Owned_rvalue( rvalues.push_back( std::move( value ) ) );
    }

    // A storage marker holds none, and reads as an empty Use.
    const Rvalue& rvalue( const Statement& statement ) const
    {
        static const Rvalue none {};
        return statement.value.is_valid() ? rvalues[statement.value.id()] : none;
    }

    Rvalue& rvalue( const Statement& statement )
    {
        assert( statement.value.is_valid() && "a storage marker holds no rvalue" );
        return rvalues[statement.value.id()];
    }

    // `statement` holding its own copy of `value`, or of nothing for a storage marker.
    Statement copy_of( const Statement& statement, Rvalue value )
    {
        return with_value( statement, statement.value.is_valid() ? add_rvalue( std::move( value ) ) : Owned_rvalue {} );
    }

    // A second statement like `statement`, rvalue and arguments its own.
    Statement clone( const Statement& statement )
    {
        return copy_of( statement, std::as_const( *this ).rvalue( statement ) );
    }

    // Drops the rvalues no statement holds; the rest are renumbered in statement order.
    void compact_rvalues()
    {
        Id_vector<Rvalue_id, Rvalue> kept;
        kept.reserve( statements.size() );
        for( Statement& statement : statements )
        {
            if( statement.value.is_valid() )
            {
                statement.value = Owned_rvalue( kept.push_back( std::move( rvalues[statement.value.id()] ) ) );
            }
        }
        rvalues = std::move( kept );
    }

    // Read-only: places share their runs, and projected() is the only writer.
    Run<const Projection> projections_of( const Place& place ) const
    {
        return Run<const Projection>( projections, place.first_projection, place.num_projections );
    }
};

// The same function: a declaration lowers once per instantiation, a guard once per comparison, and
// a requirement once per implementer and interface its thunk answers it for.
inline bool same_function( const Function& a, const Function& b )
{
    return a.declaration == b.declaration && a.compares == b.compares && a.thunk_of == b.thunk_of &&
           a.type_arguments == b.type_arguments;
}

// Whether `value`, a call or a function's address, names `function`. Only a vtable names a thunk.
inline bool names( const Rvalue& value, const Function& function )
{
    if( function.globals.is_valid() )
    {
        return value.callee == function.globals &&
               value.globals == ( function.drops_globals ? Globals_call::Drop : Globals_call::Build );
    }

    return value.globals == Globals_call::None && value.callee == function.declaration && value.op == function.compares &&
           !function.thunk_of.is_valid() && value.type_arguments == function.type_arguments;
}

inline bool has_deref_projection( const Function& func, const Place& place )
{
    for( const Projection& projection : func.projections_of( place ) )
    {
        if( projection.kind == Projection_kind::Deref )
        {
            return true;
        }
    }

    return false;
}

inline bool same_projection( const Projection& a, const Projection& b )
{
    return a.kind == b.kind && a.member == b.member && a.field == b.field && a.index == b.index;
}

// The same root and the same projections.
inline bool same_place( const Function& func, const Place& a, const Place& b )
{
    if( a.local != b.local || a.global != b.global || a.num_projections != b.num_projections )
    {
        return false;
    }

    for( u32 i = 0; i < a.num_projections; ++i )
    {
        if( !same_projection( func.projections_of( a )[i], func.projections_of( b )[i] ) )
        {
            return false;
        }
    }

    return true;
}

// A local followed only by fields, `p` or `p.a.b`: what a field move may name.
inline bool is_field_path( const Function& func, const Place& place )
{
    if( place.is_global() )
    {
        return false;
    }

    for( const Projection& projection : func.projections_of( place ) )
    {
        if( projection.kind != Projection_kind::Field )
        {
            return false;
        }
    }

    return true;
}

// Projections live in one table addressed by (first, count), so a place's own must be contiguous -
// and base's may sit anywhere. Copy base's to the end, append the new one, and return a place over
// the copy. The root comes across with it, which is what keeps `counter.x` rooted in the global.
inline Place projected( Function& func, Place base, Projection projection )
{
    const u32 first = narrow_cast<u32>( func.projections.size() );

    for( Projection copied : func.projections_of( base ) )
    {
        // Read out before appending: push_back from a reference into the same vector is legal but
        // needlessly subtle.

        func.projections.push_back( copied );
    }

    func.projections.push_back( projection );

    Place result = base;

    result.first_projection = first;
    result.num_projections  = base.num_projections + 1;

    return result;
}

// Constructors for the vocabulary above. Designated initialisers throughout, for three reasons:
// they leave every field a kind does not mean at its declared default, they name what is being set
// so a helper that forgets its own argument is visible, and -Wmissing-field-initializers does not
// fire on them - which a partial positional init does, and the release build treats as an error.

// Every operand an rvalue reads: `a` and `b` cover Use, Binary, Unary and Cast, and the arguments
// cover Call. Visiting all of them needs no switch, because an Rvalue leaves the operands its
// kind does not use at their Constant default - which is the point of that default above.
//
// Here rather than in a pass, because more than one pass needs it and "where operands live" is not a
// fact any of them should own privately.
template <typename Fn>
void for_each_operand( const Rvalue& value, Fn fn )
{
    fn( value.a );
    fn( value.b );

    for( const Operand& argument : value.arguments )
    {
        fn( argument );
    }
}

// Whether any operand `value` reads passes `test`.
template <typename Test>
bool any_operand( const Rvalue& value, Test test )
{
    bool any = false;
    for_each_operand( value, [&]( const Operand& operand ) { any = any || test( operand ); } );
    return any;
}

// Every place in the function, with the statement it is the target of, or null. `Func` is `Function` or
// `const Function`, so `fn` may rewrite them. An rvalue's own operands go regardless of kind, since an
// address's carries its place under any kind.
template <typename Func, typename Fn>
void for_each_place( Func& func, Fn fn )
{
    const auto operand = [&]( auto& o )
    {
        if( o.has_place() )
        {
            fn( o.place, nullptr );
        }
    };

    for( auto& statement : func.statements )
    {
        fn( statement.place, &statement );
        if( !statement.value.is_valid() )
        {
            continue;
        }

        auto& value = func.rvalue( statement );
        fn( value.a.place, nullptr );
        fn( value.b.place, nullptr );

        for( auto& argument : value.arguments )
        {
            operand( argument );
        }
    }

    for( auto& block : func.blocks )
    {
        operand( block.terminator.condition );
        operand( block.terminator.message );
        operand( block.terminator.at );
    }
}

// A block's successors, 0, 1 or 2 of them by terminator kind, appended to `out`.
inline void successors( const Terminator& terminator, std::vector<Block_id>& out )
{
    switch( terminator.kind )
    {
    case Terminator_kind::Goto:
        out.push_back( terminator.targets[0] );
        return;
    case Terminator_kind::Branch:
        out.push_back( terminator.targets[0] );
        out.push_back( terminator.targets[1] );
        return;
    case Terminator_kind::Return:
    case Terminator_kind::Unreachable:
    case Terminator_kind::Panic:
    case Terminator_kind::Unset:
        return;
    }
}

// How many edges enter each block; the entry counts one more, for the call that enters it.
inline std::vector<u32> predecessor_counts( const Function& func )
{
    std::vector<u32> counts( func.blocks.size(), 0 );
    counts[0] = 1;

    std::vector<Block_id> targets;
    for( const Block& block : func.blocks )
    {
        targets.clear();
        successors( block.terminator, targets );
        for( const Block_id target : targets )
        {
            counts[target.v]++;
        }
    }

    return counts;
}

// `bits` kept to their low `width`.
inline u64 truncate( u64 bits, u8 width )
{
    return width == 64 ? bits : bits & ( ( 1ull << width ) - 1 );
}

// `bits` read as a `width`-bit signed integer.
inline i64 as_signed( u64 bits, u8 width )
{
    if( width == 64 || !( bits & ( 1ull << ( width - 1 ) ) ) )
    {
        return static_cast<i64>( bits );
    }

    return static_cast<i64>( bits | ( ~0ull << width ) );
}

inline Operand copy( Place place, Type_id type )
{
    return Operand { .kind = Operand_kind::Copy, .place = place, .type = type };
}

inline Operand move( Place place, Type_id type )
{
    return Operand { .kind = Operand_kind::Move, .place = place, .type = type };
}

inline Operand constant( Literal_id literal, Type_id type )
{
    return Operand { .kind = Operand_kind::Constant, .constant = literal, .type = type };
}

// A use has the type of what it reads; there is no second answer for it to disagree with.
inline Rvalue use( Operand a )
{
    return Rvalue { .kind = Rvalue_kind::Use, .type = a.type, .a = a };
}

inline Rvalue binary( Token_kind op, Operand a, Operand b, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Binary, .type = type, .op = op, .a = a, .b = b };
}

inline Rvalue unary( Token_kind op, Operand a, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Unary, .type = type, .op = op, .a = a };
}

inline Rvalue overflows( Token_kind op, Operand a, Operand b, Type_id boolean )
{
    return Rvalue { .kind = Rvalue_kind::Overflows, .type = boolean, .op = op, .a = a, .b = b };
}

inline Rvalue cast_to( Operand a, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Cast, .type = type, .a = a };
}

inline Rvalue bits_of( Operand a, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Bits, .type = type, .a = a };
}

inline Rvalue sqrt_of( Operand a )
{
    return Rvalue { .kind = Rvalue_kind::Sqrt, .type = a.type, .a = a };
}

inline Rvalue run_of( Operand data, Operand count, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Run_of, .type = type, .a = data, .b = count };
}

inline Rvalue run_data( Operand run, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Run_data, .type = type, .a = run };
}

inline Rvalue run_size( Operand run, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Run_size, .type = type, .a = run };
}

inline Rvalue allocate( Type_id type ) // type is the *pointer* type; element comes off it
{
    return Rvalue { .kind = Rvalue_kind::Allocate, .type = type };
}

inline Rvalue allocate( Type_id type, Operand count )
{
    return Rvalue { .kind = Rvalue_kind::Allocate, .type = type, .a = count };
}

inline Rvalue release( Operand a ) // type is void
{
    return Rvalue { .kind = Rvalue_kind::Release, .type = Type_id {}, .a = a };
}

// The operand carries only its place, and `at` the expression borrowed. An address is not a read of
// what lives there, so the operand's kind means nothing here and verify does not look at it.
inline Rvalue address_of( Place place, Type_id type, Address_purpose purpose, Span at = {} )
{
    return Rvalue {
        .kind            = Rvalue_kind::Address_of,
        .type            = type,
        .a               = Operand { .place = place, .span = at },
        .address_purpose = purpose,
    };
}

inline Rvalue function_address( Node_id callee, Type_id type, std::vector<Type_id> type_arguments = {} )
{
    return Rvalue {
        .kind = Rvalue_kind::Function_address, .type = type, .callee = callee, .type_arguments = std::move( type_arguments )
    };
}

inline Rvalue make_interface( Operand address, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Make_interface, .type = type, .a = address };
}

inline Rvalue vtable_slot( Operand reference, Node_id requirement, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Vtable_slot, .type = type, .a = reference, .callee = requirement };
}

inline Rvalue interface_self( Operand reference, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Interface_self, .type = type, .a = reference };
}

inline Rvalue literal_bytes( Literal_id literal, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Literal_bytes, .type = type, .a = constant( literal, type ) };
}

inline Rvalue field_offset( Node_id field, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Field_offset, .type = type, .callee = field };
}

inline Rvalue field_read( Operand object, Operand offset, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Field_read, .type = type, .a = object, .b = offset };
}

inline Rvalue indirect_call( Operand callee, std::vector<Operand> arguments, Type_id type )
{
    return Rvalue {
        .kind      = Rvalue_kind::Indirect_call,
        .type      = type,
        .a         = callee,
        .arguments = std::move( arguments ),
    };
}

// D41: `a op b` where no type holds both, as a call of the guard for the pair; `a` is the integer, and signed if both are.
inline Rvalue guard_call( Token_kind op, Operand a, Operand b, Type_id boolean )
{
    return Rvalue {
        .kind           = Rvalue_kind::Call,
        .type           = boolean,
        .op             = op,
        .arguments      = { a, b },
        .type_arguments = { a.type, b.type },
    };
}

inline Rvalue globals_call( Node_id first, bool drops, Type_id nothing )
{
    return Rvalue {
        .kind    = Rvalue_kind::Call,
        .type    = nothing,
        .callee  = first,
        .globals = drops ? Globals_call::Drop : Globals_call::Build,
    };
}

inline Rvalue call( Node_id callee, std::vector<Operand> arguments, Type_id type, std::vector<Type_id> type_arguments = {} )
{
    return Rvalue {
        .kind           = Rvalue_kind::Call,
        .type           = type,
        .callee         = callee,
        .arguments      = std::move( arguments ),
        .type_arguments = std::move( type_arguments ),
    };
}

} // namespace keel
