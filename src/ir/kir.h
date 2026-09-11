#pragma once
#include <vector>
#include "ast/node.h"
#include "common/interner.h"
#include "common/literals.h"
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

// A temporary is a local with no name. Nothing else distinguishes it: the drop analysis asks which
// locals are live, and has no reason to care which ones the author wrote.
struct Local
{
    Type_id   type {};
    Span      span {};
    Symbol_id name {}; // invalid for a temporary
};

enum class Projection_kind : u8
{
    Field,
    Deref,
    Tag // D7: the discriminant of an enum that carries payloads; `field` is unused
};

struct Projection
{
    Projection_kind kind = Projection_kind::Deref;
    Node_id         field {}; // the Field_decl; valid only for kind == Field
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

    bool is_global() const
    {
        return global.is_valid();
    }
};

enum class Operand_kind : u8
{
    Copy,
    Move, // M4 gives this meaning: the place is dead afterwards. Inert until then.
    Constant
};

// A value being read.
struct Operand
{
    // Constant by default, so an operand a kind does not use holds nothing that looks like a
    // reference to a local.
    Operand_kind kind = Operand_kind::Constant;
    Place        place {};    // Copy and Move
    Literal_id   constant {}; // Constant
    Type_id      type {};
};

enum class Rvalue_kind : u8
{
    Use,
    Binary,
    Unary,
    Cast,
    Call,
    Address_of // reads a.place; the address is not a copy of what lives there
};

// How a value is produced. One tagged struct rather than a variant hierarchy, as Node and Type
// already are, so -Wswitch keeps every consumer exhaustive.
struct Rvalue
{
    Rvalue_kind kind = Rvalue_kind::Use;
    Type_id     type {};
    Token_kind  op = Token_kind::Unknown; // Binary and Unary
    Operand     a {};
    Operand     b {};               // Binary only
    u32         first_argument = 0; // into Function::operands, for Call
    u32         argument_count = 0;
    Node_id     callee {};
};

enum class Statement_kind : u8
{
    Assign,
    Drop,         // M3
    Storage_live, // scope entry and exit, which is what tells the drop pass where a scope ended
    Storage_dead,
};

// TODO: Currently 100 bytes, of which 72 are rvalue. Move rvalues to a side table, and give
// `Operand` a `Span` in the same change: operands carry none today, so every argument of a call
// reports at the statement - `two( a, b )` after `two( move a, move b )` gives two move errors with
// identical carets, told apart only by the name in the message. One field on Operand fixes all four
// places one appears (Rvalue::a, Rvalue::b, Terminator::condition, Function::operands), but it costs
// 12 bytes there and 24 here, which is worth paying only once 72 are leaving anyway.
struct Statement
{
    Statement_kind kind = Statement_kind::Assign;
    Span           span {};
    Place          place {};     // assigned to by Assign, dropped by Drop
    Rvalue         value {};     // Assign only
    Local_id       drop_flag {}; // Drop only: invalid means drop unconditionally
};

enum class Terminator_kind : u8
{
    Unset, // the default, so a block nobody terminated is a verify error rather than a silent one
    Goto,
    Branch,
    Return,
    Unreachable
};

struct Terminator
{
    Terminator_kind kind = Terminator_kind::Unset;
    Span            span {};
    Operand         condition {};  // Branch
    Block_id        targets[2] {}; // [0] for Goto; [0] true and [1] false for Branch
};

// Exactly one terminator, and terminators appear nowhere else. verify() checks this first: it is
// what makes the graph a graph.
struct Block
{
    u32        first_statement = 0; // into Function::statements
    u32        statement_count = 0;
    Terminator terminator {};
};

// Block 0 is the entry. Local 0 is the return slot, and locals 1..parameter_count are the
// parameters, in declaration order.
struct Function
{
    Node_id declaration {};
    u32     parameter_count = 0;
    // Which parameters arrive uninitialised. Nothing else in KIR says so - an `out` parameter's
    // local holds a valid address, and it is the referent that is empty - so the analysis that
    // needs it cannot work it out from the graph.
    std::vector<Local_id>   out_parameters;
    std::vector<Local>      locals;
    std::vector<Block>      blocks;
    std::vector<Statement>  statements;
    std::vector<Projection> projections;
    std::vector<Operand>    operands;
};

// Constructors for the vocabulary above. Designated initialisers throughout, for three reasons:
// they leave every field a kind does not mean at its declared default, they name what is being set
// so a helper that forgets its own argument is visible, and -Wmissing-field-initializers does not
// fire on them - which a partial positional init does, and the release build treats as an error.

// Every operand an rvalue reads: `a` and `b` cover Use, Binary, Unary and Cast, and the argument
// range covers Call. Visiting all of them needs no switch, because an Rvalue leaves the operands its
// kind does not use at their Constant default - which is the point of that default above.
//
// Here rather than in a pass, because more than one pass needs it and "where operands live" is not a
// fact any of them should own privately.
template <typename Fn>
void for_each_operand( const Function& func, const Rvalue& value, Fn fn )
{
    fn( value.a );
    fn( value.b );

    for( u32 i = 0; i < value.argument_count; ++i )
    {
        fn( func.operands[value.first_argument + i] );
    }
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

inline Rvalue cast_to( Operand a, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Cast, .type = type, .a = a };
}

// The operand carries only its place. An address is not a read of what lives there, so the
// operand's kind means nothing here and verify does not look at it.
inline Rvalue address_of( Place place, Type_id type )
{
    return Rvalue { .kind = Rvalue_kind::Address_of, .type = type, .a = Operand { .place = place } };
}

inline Rvalue call( Node_id callee, u32 first_argument, u32 argument_count, Type_id type )
{
    return Rvalue {
        .kind           = Rvalue_kind::Call,
        .type           = type,
        .first_argument = first_argument,
        .argument_count = argument_count,
        .callee         = callee,
    };
}

} // namespace keel
