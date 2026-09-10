#include "sema/type_checker.h"
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "common/source_manager.h"
#include "lex/token.h"

namespace keel
{

namespace
{

// D1. The rejected C++ spellings are ordinary identifiers to the lexer, so this is the first point
// that knows one was written in type position - which is exactly the argument for putting the
// suggestion here rather than in lex/. Empty when there is nothing to suggest.
//
// Only single-word spellings can appear: `unsigned int` and `long long` are two identifiers and
// fail in the parser long before sema sees them.
std::string_view keel_spelling_for( std::string_view cpp_spelling )
{
    struct Alias
    {
        std::string_view from;
        std::string_view to;
    };

    static constexpr Alias aliases[] = {
        { "int", "i32" },       { "signed", "i32" },   { "unsigned", "u32" },  { "short", "i16" },    { "long", "i64" },
        { "char", "i8" },       { "float", "f32" },    { "double", "f64" },    { "size_t", "u64" },   { "ssize_t", "i64" },
        { "ptrdiff_t", "i64" }, { "intptr_t", "i64" }, { "uintptr_t", "u64" }, { "int8_t", "i8" },    { "int16_t", "i16" },
        { "int32_t", "i32" },   { "int64_t", "i64" },  { "uint8_t", "u8" },    { "uint16_t", "u16" }, { "uint32_t", "u32" },
        { "uint64_t", "u64" },
    };

    for( const Alias& alias : aliases )
    {
        if( alias.from == cpp_spelling )
        {
            return alias.to;
        }
    }

    return {};
}

// What an operator accepts, and where its result type comes from. A table rather than nested
// switches, for the same reason the parser keeps binding_power() as one: these are rules, and
// checking them against PLAN §6.4 and D16 should not mean tracing control flow.
enum class Operands : u8
{
    Numeric,    // integers and floats
    Integer,    // integers only
    Bool,       // bools only
    Comparable, // numeric, or two bools
};

enum class Result : u8
{
    Common, // the §6.4 result of the two operands
    Bool,   // a comparison
    Left,   // the left operand's type - C++ takes no common type for a shift
};

struct Binary_rule
{
    Token_kind kind;
    Operands   operands;
    Result     result;
};

constexpr Binary_rule binary_rules[] = {
    { Token_kind::Plus, Operands::Numeric, Result::Common },
    { Token_kind::Minus, Operands::Numeric, Result::Common },
    { Token_kind::Star, Operands::Numeric, Result::Common },
    { Token_kind::Slash, Operands::Numeric, Result::Common },
    { Token_kind::Percent, Operands::Integer, Result::Common },
    { Token_kind::Amp, Operands::Integer, Result::Common },
    { Token_kind::Pipe, Operands::Integer, Result::Common },
    { Token_kind::Caret, Operands::Integer, Result::Common },
    { Token_kind::Less_less, Operands::Integer, Result::Left },
    { Token_kind::Greater_greater, Operands::Integer, Result::Left },
    { Token_kind::Less, Operands::Numeric, Result::Bool },
    { Token_kind::Less_equal, Operands::Numeric, Result::Bool },
    { Token_kind::Greater, Operands::Numeric, Result::Bool },
    { Token_kind::Greater_equal, Operands::Numeric, Result::Bool },
    { Token_kind::Equal_equal, Operands::Comparable, Result::Bool },
    { Token_kind::Bang_equal, Operands::Comparable, Result::Bool },
    { Token_kind::Amp_amp, Operands::Bool, Result::Bool },
    { Token_kind::Pipe_pipe, Operands::Bool, Result::Bool },
};

struct Unary_rule
{
    Token_kind kind;
    Operands   operands;
    bool       signed_only; // negating an unsigned type has no representable answer
};

constexpr Unary_rule unary_rules[] = {
    { Token_kind::Minus, Operands::Numeric, true },
    { Token_kind::Plus, Operands::Numeric, false },
    { Token_kind::Bang, Operands::Bool, false },
    { Token_kind::Tilde, Operands::Integer, false },
};

const Binary_rule* binary_rule_for( Token_kind kind )
{
    for( const Binary_rule& rule : binary_rules )
    {
        if( rule.kind == kind )
        {
            return &rule;
        }
    }

    return nullptr;
}

const Unary_rule* unary_rule_for( Token_kind kind )
{
    for( const Unary_rule& rule : unary_rules )
    {
        if( rule.kind == kind )
        {
            return &rule;
        }
    }

    return nullptr;
}

// What `cast` and `wrap` each mean for a pair of type kinds. They are not one operator with two
// spellings: `cast` preserves the value, `wrap` keeps the low bits. A conversion that is neither
// of those things belongs to neither operator.
//
// Absent pairs are refused, and the omissions are the point: float to int has no one obvious
// rounding, int to bool says less than `x != 0` does, and nothing converts to or from a struct.
enum class Conversion : u8
{
    None,
    Cast_only, // modular arithmetic has no meaning here
    Both,
    Unsafe, // a real conversion, but one that needs a gate Keel does not have yet
};

struct Conversion_rule
{
    Type_kind  from;
    Type_kind  to;
    Conversion allows;
};

constexpr Conversion_rule conversion_rules[] = {
    { Type_kind::Int, Type_kind::Int, Conversion::Both },
    { Type_kind::Int, Type_kind::Float, Conversion::Cast_only },
    { Type_kind::Float, Type_kind::Float, Conversion::Cast_only },
    { Type_kind::Bool, Type_kind::Int, Conversion::Cast_only },
    { Type_kind::Pointer, Type_kind::Pointer, Conversion::Unsafe },
};

// Remove when the KIR can emit the check. `cast` is defined to test the value at run time and
// nothing can do that yet, so a narrowing `cast` would silently truncate - which is `wrap`'s
// behaviour wearing `cast`'s name. Deleting this and the one branch that reads it is the whole
// change; no program that compiles today changes meaning when it goes.
constexpr bool k_narrowing_cast_needs_a_run_time_check = true;

Conversion conversion_for( Type_kind from, Type_kind to )
{
    for( const Conversion_rule& rule : conversion_rules )
    {
        if( rule.from == from && rule.to == to )
        {
            return rule.allows;
        }
    }

    return Conversion::None;
}

// A folded constant, in the sign-magnitude pair fits() already takes. §12 decided that arithmetic
// wraps at run time; a constant is the case where the answer is known before the program runs, and
// wrapping it there would be a wrong answer delivered in silence.
//
// `overflowed` is not the same as not being constant: `18446744073709551615 * 2` is entirely
// constant and has left what u64 can carry, which overflows every Keel type there is. Losing that
// distinction would let the largest constants through in silence, which is backwards.
struct Constant
{
    u64  magnitude = 0;
    bool negative  = false;
};

struct Folded
{
    bool     constant   = false;
    bool     overflowed = false;
    Constant value;
};

constexpr Folded not_constant()
{
    return Folded {};
}

constexpr Folded too_large()
{
    return Folded { true, true, Constant {} };
}

// Zero has no sign. Without this `0 - 0` folds to a negative zero, which no type holds.
constexpr Folded folded( u64 magnitude, bool negative )
{
    return Folded { true, false, Constant { magnitude, magnitude == 0 ? false : negative } };
}

// Bitwise operators are defined on the *representation*, and sign-magnitude has no bit pattern of
// its own - so folding one means converting to two's complement in the type's width, operating,
// and converting back. That is why these need the type where the arithmetic ones do not.
u64 to_bits( Constant value, u8 width )
{
    const u64 mask = width == 64 ? ~0ull : ( 1ull << width ) - 1;

    return ( value.negative ? ~value.magnitude + 1 : value.magnitude ) & mask;
}

Folded from_bits( u64 bits, u8 width, bool is_signed )
{
    const u64 mask = width == 64 ? ~0ull : ( 1ull << width ) - 1;

    bits &= mask;

    // The sign bit is only a sign in a signed type. In an unsigned one the same pattern is just a
    // large positive value, which is why ~0 is -1 as an i32 and the maximum as a u32.
    const u64 sign = width == 64 ? 1ull << 63 : 1ull << ( width - 1 );

    if( is_signed && ( bits & sign ) != 0 )
    {
        return folded( ( ~bits + 1 ) & mask, true );
    }

    return folded( bits, false );
}

// Negative sorts below positive whatever the magnitudes; within one sign the magnitude decides,
// reversed when both are negative.
bool less_than( Constant a, Constant b )
{
    if( a.negative != b.negative )
    {
        return a.negative;
    }

    return a.negative ? a.magnitude > b.magnitude : a.magnitude < b.magnitude;
}

bool equals( Constant a, Constant b )
{
    return a.magnitude == b.magnitude && a.negative == b.negative;
}

Folded add_constants( Constant a, Constant b )
{
    if( a.negative == b.negative )
    {
        if( a.magnitude > std::numeric_limits<u64>::max() - b.magnitude )
        {
            return too_large();
        }

        return folded( a.magnitude + b.magnitude, a.negative );
    }

    // Opposite signs: the larger magnitude decides both the size and the sign of the answer.
    return a.magnitude >= b.magnitude ? folded( a.magnitude - b.magnitude, a.negative )
                                      : folded( b.magnitude - a.magnitude, b.negative );
}

Folded multiply_constants( Constant a, Constant b )
{
    if( a.magnitude != 0 && b.magnitude > std::numeric_limits<u64>::max() / a.magnitude )
    {
        return too_large();
    }

    return folded( a.magnitude * b.magnitude, a.negative != b.negative );
}

// The help line for an operand kind. Empty where the message above it already says enough.
std::string_view operand_requirement( Operands operands )
{
    switch( operands )
    {
    case Operands::Integer:
        return "both operands must be integers";
    case Operands::Bool:
        return "both operands must be `bool`";
    case Operands::Numeric:
    case Operands::Comparable:
        return {};
    }

    return {};
}

namespace
{
// A constructor and a destructor obey the same three rules and differ only in spelling, so the
// rules are written once over a description of the kind rather than twice over the kinds.
struct Member_kind
{
    Node_kind        node;
    std::string_view noun;   // "constructor"
    std::string_view prefix; // written before the name: "" or "~"
    std::string_view remedy; // what to do instead, when a struct declares one
};

constexpr Member_kind k_member_kinds[] = {
    { Node_kind::Constructor_decl, "constructor", "", "use `class`, or build this from a literal" },
    { Node_kind::Destructor_decl, "destructor", "~", "use `class` if this type owns a resource" },
};
} // namespace

class Checker
{
public:
    Checker(
        const Ast&            ast,
        const Interner&       interner,
        const Resolution&     resolution,
        const Source_manager& source_manager,
        const Literals&       literals,
        Diagnostics&          diags
    )
        : ast_( ast ),
          interner_( interner ),
          resolution_( resolution ),
          sm_( source_manager ),
          literals_( literals ),
          diags_( diags )
    {
    }

    Types run();

private:
    // Every top-level signature is typed before any body, so a call can read its callee's
    // parameter and return types straight out of types_. Same reason as the resolver's two-pass
    // file scope: mutual recursion (D18) means `even` needs `odd`'s return type.
    void declare_signatures();
    void declare_signatures_struct_decls();
    void declare_signatures_field_decls();
    void declare_signatures_function_decls();
    void declare_signatures_member_functions();
    void declare_signatures_global_decls();

    void order_structs();
    bool contains_itself( Node_id decl, std::vector<Node_id>& path );
    void report_containment_cycle( Node_id decl, const std::vector<Node_id>& path );

    // D29's rules, split out because check_struct_ownership cannot run before compute_owning has.
    void check_aggregate_members();
    void compute_owning();
    void check_struct_ownership();
    void check_struct_fields_are_not_owning( Node_id decl );
    void check_member_kind( Node_id decl, const Member_kind& kind );

    bool has_destructor( Node_id decl ) const;

    // The first member of a kind, or invalid. Constructors and destructors are both at most one,
    // so "the first" and "the only" coincide once check_aggregate_members has run.
    Node_id find_member( Node_id decl, Node_kind kind ) const;

    // The same question Types::is_owning answers, asked before there is a Types to ask - the result
    // is not assembled until run() returns.
    bool is_owning_type( Type_id type ) const;

    // D31's initialisation and assignment clause: an owning value transfers rather than copies, and
    // the transfer is written down.
    void check_owning_source( Node_id value, Type_id type );

    // Bare is Keyword::Count, which is not a keyword anyone can write - so "no mode" needs no separate
    // answer and every caller compares the same way.
    Keyword parameter_mode( Node_id param ) const;

    std::string previous_declaration_note( Node_id previous ) const;

    // The main entry points for visiting and inferring types.
    void    visit( Node_id id ); // statements and declarations
    Type_id infer( Node_id id ); // expression, no expectation

    // One per construct, dispatched from visit and infer - the shape parse_*() uses next door.
    void visit_function( Node_id id );
    void check_condition( Node_id id ); // if/while/for all want the same message
    bool is_assignable( Node_id id ) const;
    void visit_return( Node_id id );
    void visit_var( Node_id id );
    void visit_assign( Node_id id );
    void visit_if( Node_id id );
    void visit_while( Node_id id );
    void visit_for( Node_id id );
    void visit_increment( Node_id id );
    void visit_global( Node_id id );

    // Int, Float and Bool literals with nothing to give them a type - the fallback defaults.
    // Whether context can give this expression a type, rather than it having one of its own.
    bool is_literal_expression( Node_id id ) const;

    // What may initialise a file-scope variable: what C also accepts as a constant expression.
    bool is_constant_expression( Node_id id ) const;

    Type_id infer_literal( Node_id id );

    // The bidirectional half (L5): a literal adopts `expected` when its value fits, which is what
    // lets `u32 x = 42;` need no suffix. Also where a unary minus must be pushed through, so that
    // `i32 x = -2147483648;` range-checks as a negative rather than as an out-of-range positive.
    Type_id check_literal( Node_id id, Type_id expected );

    Type_id infer_name( Node_id id );
    Type_id infer_call( Node_id id );
    Type_id infer_binary( Node_id id );
    Type_id infer_unary( Node_id id );

    Node_id find_field( Type_id type, Symbol_id name ) const;
    Type_id infer_field( Node_id id );

    // A composite constructor, not a value literal: its type is fixed by its name rather than
    // adopted from context, which is why it has no place in infer_literal.
    Type_id infer_struct_literal( Node_id id );

    Type_id infer_cast( Node_id id );
    Type_id infer_marker( Node_id id );

    // Constant rejection (§12). fold_* answer what an expression's value is when it is made only
    // of literals; check_constant decides whether that value can exist in the type the operation
    // happens in, and record_constant is the two together.
    // `known` is the type the caller already settled on. check_constant runs *before* the node's
    // own type is recorded, so without it the operators that need a width - the bitwise ones, `~`
    // and a cast - fold to nothing exactly where overflow is being checked.
    Folded             fold_integer( Node_id id, Type_id known = Type_id {} ) const;
    std::optional<f64> fold_float( Node_id id ) const;
    bool               check_constant( Node_id id, Type_id type );
    Type_id            record_constant( Node_id id, Type_id type );

    // Walks an expression only for the errors inside it, in a context that has already failed.
    void absorb( Node_id id );

    bool    accepts( Operands operands, Type_id type ) const;
    Type_id check( Node_id id, Type_id expected ); // expression, with one
    Type_id type_of_annotation( Node_id id );      // Named_type/Pointer_type subtree; invalid for auto

    // Writes the node's type and returns it. Every infer branch ends in one of these, so that
    // forgetting to record a type is hard rather than silent.
    Type_id record( Node_id id, Type_id type );

    void error_at( Span span, std::string message, std::string help = {} );

    const Ast&            ast_;
    const Interner&       interner_;
    const Resolution&     resolution_;
    const Source_manager& sm_;
    const Literals&       literals_;
    Diagnostics&          diags_;

    Type_table              table_;
    std::vector<Type_id>    types_;        // sized node_count(), invalid-filled, like bindings_ in Resolver
    std::vector<Node_id>    struct_order_; // dependencies first; also the "already proved acyclic" set
    std::unordered_set<u32> owning_;       // filled by compute_owning, handed to Types
    Type_id                 current_return_;

    u32 loop_depth_ = 0; // for break/continue

    // One per file-scope initialiser. Every accepted one must fold - the rule already requires a
    // constant expression - so a missing entry is an internal error, not a user's mistake.
    std::unordered_map<u32, Constant_value> constants_;
};

Types Checker::run()
{
    types_.assign( ast_.node_count(), Type_id {} );
    declare_signatures();
    visit( ast_.root() );
    return Types(
        std::move( table_ ), std::move( types_ ), std::move( struct_order_ ), std::move( constants_ ), std::move( owning_ )
    );
}

void Checker::declare_signatures()
{
    declare_signatures_struct_decls();
    declare_signatures_member_functions();
    declare_signatures_field_decls();
    order_structs();
    check_aggregate_members();
    compute_owning();
    check_struct_ownership();
    declare_signatures_function_decls();
    declare_signatures_global_decls();
}

void Checker::declare_signatures_struct_decls()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        std::string_view name = interner_.text( Symbol_id { ast_.aux( child ) } );
        record( child, table_.structure( child, name ) );
    }
}

void Checker::declare_signatures_field_decls()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        for( Node_id field : ast_.children( child ) )
        {
            if( ast_.kind( field ) != Node_kind::Field_decl )
            {
                continue;
            }

            record( field, type_of_annotation( ast_.children( field )[0] ) );
        }
    }
}

void Checker::declare_signatures_function_decls()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Function_decl )
        {
            continue;
        }

        const Node_id return_type_node = ast_.children( child )[0];
        const Type_id return_type      = type_of_annotation( return_type_node );
        record( child, return_type );

        const Node_id param_list = ast_.children( child )[1];
        for( Node_id param : ast_.children( param_list ) )
        {
            if( ast_.kind( param ) != Node_kind::Param_decl )
            {
                continue;
            }

            const Node_id param_type_node = ast_.children( param )[0];
            const Type_id param_type      = type_of_annotation( param_type_node );
            record( param, param_type );
        }

        if( interner_.text( Symbol_id { ast_.aux( child ) } ) == "main" )
        {
            // return type
            if( !table_.is_error( return_type ) && return_type != table_.integer( 32, true ) )
            {
                error_at(
                    ast_.span( return_type_node ),
                    fmt::format( "`main` must return `i32`, but got `{}`", table_.name( return_type ) )
                );
            }

            // parameters
            // Keel currently doesn't support command line args, or arrays,
            // so `main` must be parameterless.
            if( !ast_.children( param_list ).empty() )
            {
                error_at( ast_.span( param_list ), "`main` must not take any parameters" );
            }
        }
    }
}

void Checker::declare_signatures_member_functions()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        for( Node_id member : ast_.children( child ) )
        {
            if( !is_function_like( ast_.kind( member ) ) )
            {
                continue;
            }

            // A destructor returns nothing, so unlike a function there is no annotation to read.
            record( member, table_.builtin( Type_kind::Void ) );

            for( Node_id param : ast_.children( ast_.children( member )[1] ) )
            {
                const Node_id param_type_node = ast_.children( param )[0];
                const Type_id param_type      = type_of_annotation( param_type_node );
                record( param, param_type );
            }
        }
    }
}

void Checker::declare_signatures_global_decls()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Var_decl )
        {
            continue;
        }

        const Node_id var_type_node = ast_.children( child )[0];
        const Type_id var_type      = type_of_annotation( var_type_node );
        record( child, var_type );
    }
}

void Checker::order_structs()
{
    std::vector<Node_id> cycle_reported {};

    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        bool already_reported = std::find( cycle_reported.begin(), cycle_reported.end(), child ) != cycle_reported.end();
        if( already_reported )
        {
            continue;
        }

        std::vector<Node_id> path;

        if( !contains_itself( child, path ) )
        {
            continue;
        }

        report_containment_cycle( child, path );

        // Every aggregate on the cycle is reported by that one message. Without marking them all,
        // `A -> B -> A` is found again from B and reported twice for one mistake.
        cycle_reported.insert( cycle_reported.end(), path.begin(), path.end() );
    }
}

void Checker::report_containment_cycle( Node_id decl, const std::vector<Node_id>& path )
{
    // The route, so the message names how the cycle closes rather than only that it does.
    std::string route;

    for( const Node_id node : path )
    {
        if( !route.empty() )
        {
            route += " -> ";
        }

        route += interner_.text( Symbol_id { ast_.aux( node ) } );
    }

    error_at(
        ast_.span( decl ),
        fmt::format( "`{}` contains itself, so it has no size", interner_.text( Symbol_id { ast_.aux( decl ) } ) ),
        route
    );
}

bool Checker::contains_itself( Node_id decl, std::vector<Node_id>& path )
{
    // Being in the order *is* being done: a struct lands there only once every struct it contains
    // has, so membership and "proved acyclic" are the same fact. A linear scan is right here -
    // this is the number of structs in one file.
    if( std::find( struct_order_.begin(), struct_order_.end(), decl ) != struct_order_.end() )
    {
        return false;
    }

    path.push_back( decl );

    for( Node_id field : ast_.children( decl ) )
    {
        if( ast_.kind( field ) != Node_kind::Field_decl )
        {
            continue;
        }

        // Read back what the field pass recorded. Calling type_of_annotation again would report
        // every unknown type and D1 suggestion a second time.
        const Type_id field_type = types_[field.v];

        // A pointer to a struct is finite, so only by-value containment counts - which is what
        // makes `struct Node { Node* next; }` legal once pointers arrive at M3.
        if( !field_type.is_valid() || table_.is_error( field_type ) || !table_.is_struct( field_type ) )
        {
            continue;
        }

        const Node_id field_decl = table_.get( field_type ).declaration;
        if( !field_decl.is_valid() || !is_aggregate( ast_.kind( field_decl ) ) )
        {
            continue;
        }

        if( std::find( path.begin(), path.end(), field_decl ) != path.end() )
        {
            path.push_back( field_decl );
            return true;
        }

        if( contains_itself( field_decl, path ) )
        {
            return true;
        }
    }

    // The DFS post-order: every struct after everything it contains, which is exactly the order C
    // needs for by-value members. A struct on a cycle never reaches here, and never needs to - the
    // driver stops before emission when anything reported.
    struct_order_.push_back( decl );
    path.pop_back();
    return false;
}

bool Checker::is_owning_type( Type_id type ) const
{
    if( !type.is_valid() || table_.is_error( type ) || !table_.is_struct( type ) )
    {
        return false;
    }

    const Node_id declaration = table_.get( type ).declaration;

    return declaration.is_valid() && owning_.contains( declaration.v );
}

void Checker::check_owning_source( Node_id value, Type_id type )
{
    if( !value.is_valid() || !is_owning_type( type ) || ast_.kind( value ) == Node_kind::Marker_expr )
    {
        return;
    }

    // A temporary needs no marker: it has no other owner, so handing it over is the only thing that
    // can happen to it and there is no variable left behind for a reader to wonder about.
    // is_assignable is exactly the "names a place" test, which is what distinguishes the two.
    if( !is_assignable( value ) )
    {
        return;
    }

    error_at(
        ast_.span( value ),
        "an owning value is transferred, not copied",
        fmt::format( "write `move {}`", sm_.text( ast_.span( value ) ) )
    );
}

Node_id Checker::find_member( Node_id decl, Node_kind kind ) const
{
    for( const Node_id member : ast_.children( decl ) )
    {
        if( ast_.kind( member ) == kind )
        {
            return member;
        }
    }

    return Node_id {};
}

Keyword Checker::parameter_mode( Node_id param ) const
{
    const Node_id annotation = ast_.children( param )[0];

    return ast_.kind( annotation ) == Node_kind::Mode_type ? static_cast<Keyword>( ast_.aux( annotation ) ) : Keyword::Count;
}

std::string Checker::previous_declaration_note( Node_id previous ) const
{
    const Span     span = ast_.span( previous );
    const Line_col loc  = sm_.line_col( span.file, span.start );

    return fmt::format( "previous declaration is at: {}:{}", loc.line, loc.col );
}

void Checker::check_aggregate_members()
{
    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        if( !is_aggregate( ast_.kind( decl ) ) )
        {
            continue;
        }

        for( const Member_kind& kind : k_member_kinds )
        {
            check_member_kind( decl, kind );
        }
    }
}

void Checker::check_member_kind( Node_id decl, const Member_kind& kind )
{
    const bool             on_a_struct = ast_.kind( decl ) == Node_kind::Struct_decl;
    const Symbol_id        type_name { ast_.aux( decl ) };
    const std::string_view type_text = interner_.text( type_name );

    Node_id first {};

    for( const Node_id member : ast_.children( decl ) )
    {
        if( ast_.kind( member ) != kind.node )
        {
            continue;
        }

        // One mistake, one diagnostic: declaring one of these on a struct is a single decision to
        // reverse, so the name and duplicate rules stay quiet about it.
        if( on_a_struct )
        {
            error_at( ast_.span( member ), fmt::format( "a struct cannot have a {}", kind.noun ), std::string( kind.remedy ) );
            return;
        }

        if( first.is_valid() )
        {
            // Overloading is outside v0 (§6.6), so a second one has nothing to tell it apart.
            error_at(
                ast_.span( member ),
                fmt::format( "`{}` already has a {}", type_text, kind.noun ),
                previous_declaration_note( first )
            );
            continue;
        }

        first = member;

        const Symbol_id written { ast_.aux( member ) };

        if( written.is_valid() && written != type_name )
        {
            error_at(
                ast_.span( member ),
                fmt::format( "`{}{}` does not name the enclosing type", kind.prefix, interner_.text( written ) ),
                fmt::format( "write `{}{}`", kind.prefix, type_text )
            );
        }
    }
}

void Checker::compute_owning()
{
    // struct_order_ is the DFS post-order, so every aggregate arrives after everything it
    // contains: a member's answer is always already in owning_ by the time its owner is reached.
    // That is what makes one forward pass enough, with no recursion and no memo. A cycle never
    // enters the order, so it is absent here - order_structs already reported it.
    for( const Node_id decl : struct_order_ )
    {
        bool owns = has_destructor( decl );

        for( const Node_id field : ast_.children( decl ) )
        {
            if( owns )
            {
                break;
            }

            if( ast_.kind( field ) != Node_kind::Field_decl )
            {
                continue;
            }

            // Same guards as contains_itself: a pointer to an owning type owns nothing, because
            // an address says nothing about who frees it.
            const Type_id field_type = types_[field.v];

            if( !field_type.is_valid() || table_.is_error( field_type ) || !table_.is_struct( field_type ) )
            {
                continue;
            }

            const Node_id field_decl = table_.get( field_type ).declaration;

            owns = field_decl.is_valid() && owning_.contains( field_decl.v );
        }

        if( owns )
        {
            owning_.insert( decl.v );
        }
    }
}

void Checker::check_struct_ownership()
{
    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        // A struct with a destructor of its own is already reported by check_aggregate_members,
        // and it is one decision to reverse rather than two.
        if( ast_.kind( decl ) == Node_kind::Struct_decl && !has_destructor( decl ) )
        {
            check_struct_fields_are_not_owning( decl );
        }
    }
}

void Checker::check_struct_fields_are_not_owning( Node_id decl )
{
    for( const Node_id field : ast_.children( decl ) )
    {
        if( ast_.kind( field ) != Node_kind::Field_decl )
        {
            continue;
        }

        const Type_id field_type = types_[field.v];

        if( !field_type.is_valid() || table_.is_error( field_type ) )
        {
            continue;
        }

        const Node_id field_decl = table_.get( field_type ).declaration;

        if( !field_decl.is_valid() || !owning_.contains( field_decl.v ) )
        {
            continue;
        }

        // One per field: each is a separate place the author has to change.
        error_at(
            ast_.span( field ),
            fmt::format(
                "a struct cannot contain `{}`, which {}",
                table_.name( field_type ),
                has_destructor( field_decl ) ? "has a destructor" : "owns a resource"
            ),
            fmt::format(
                "a struct is copied freely, so declare `{}` as a class if it owns this",
                interner_.text( Symbol_id { ast_.aux( decl ) } )
            )
        );
    }
}

bool Checker::has_destructor( Node_id decl ) const
{
    for( const Node_id member : ast_.children( decl ) )
    {
        if( ast_.kind( member ) == Node_kind::Destructor_decl )
        {
            return true;
        }
    }

    return false;
}

void Checker::visit( Node_id id )
{
    if( !id.is_valid() )
    {
        return;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Error:
        return;

    case Node_kind::Source_file:
    {
        for( const Node_id child : ast_.children( id ) )
        {
            // A file-scope variable is checked differently from a local: its type came from the
            // signature pass, and its initialiser must be a literal. Everything else - functions
            // above all - takes the ordinary path, which is what this case replaced when it took
            // over from the recursing default.
            if( ast_.kind( child ) == Node_kind::Var_decl )
            {
                visit_global( child );
                continue;
            }

            visit( child );
        }

        return;
    }

    case Node_kind::Expr_stmt:
        infer( ast_.children( id )[0] );
        return; // discard the result. D15 already made effectless expressions a parse error

    case Node_kind::Function_decl:
    case Node_kind::Destructor_decl:
    case Node_kind::Constructor_decl:
        return visit_function( id );

    case Node_kind::Return_stmt:
        return visit_return( id );

    case Node_kind::Var_decl:
        return visit_var( id );

    case Node_kind::Assign_stmt:
        return visit_assign( id );

    case Node_kind::If_stmt:
        return visit_if( id );

    case Node_kind::While_stmt:
        return visit_while( id );

    case Node_kind::For_stmt:
        return visit_for( id );

    case Node_kind::Increment_stmt:
        return visit_increment( id );

    case Node_kind::Break_stmt:
        if( loop_depth_ == 0 )
        {
            error_at( ast_.span( id ), "`break` outside a loop body", "`break` can only appear inside a `while` or `for`" );
        }
        return;
    case Node_kind::Continue_stmt:
        if( loop_depth_ == 0 )
        {
            error_at(
                ast_.span( id ), "`continue` outside a loop body", "`continue` can only appear inside a `while` or `for`"
            );
        }
        return;

    default:
        // Block, and every statement not yet given a case of its own.
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        return;
    }
}

void Checker::visit_function( Node_id id )
{
    // Children are { return_type, param_list, body }. declare_signatures already typed the first
    // two, so only the body is left.
    //
    // Saved and restored rather than plainly assigned: M6 brings lambdas, and a nested one would
    // otherwise leave the enclosing function checking its returns against the wrong type.
    const Type_id enclosing_return = current_return_;

    current_return_ = types_[id.v];
    visit( ast_.children( id )[2] );
    current_return_ = enclosing_return;
}

void Checker::check_condition( Node_id id )
{
    if( !id.is_valid() ) // `for( ; ; )` has no condition, which is not a mistake
    {
        return;
    }

    const Type_id bool_type = table_.builtin( Type_kind::Bool );
    const Type_id actual    = infer( id );

    if( table_.is_error( actual ) || actual == bool_type )
    {
        return;
    }

    // D5's sharpest break from C++ habit, so the message says what to write instead rather than
    // only what is wrong.
    error_at(
        ast_.span( id ),
        fmt::format( "expected `bool`, but got `{}`", table_.name( actual ) ),
        "there is no conversion to `bool`: compare explicitly, as in `x != 0`"
    );
}

bool Checker::is_assignable( Node_id id ) const
{
    // A field is always assignable. `make().x = 2.0;` writes into a temporary and is therefore
    // useless, but it is legal C++ - a member of a class prvalue is an xvalue - and rejecting it
    // would need a notion of value categories that v0 does not otherwise have. D15 does not cover
    // it either: that rule asks whether an expression *kind* has an effect, and an assignment
    // always does. Pointless-but-harmless belongs to a future warning, not to the type checker.
    if( ast_.kind( id ) == Node_kind::Field_expr )
    {
        return true; // whether the object has fields at all is infer_field's question
    }

    if( ast_.kind( id ) == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( id ) ) == Token_kind::Star )
    {
        return true; // `*ptr = 42;` writes through the pointer. Whether ptr *is* one is infer_unary's question
    }

    const Node_id decl = ast_.kind( id ) == Node_kind::Name_expr ? resolution_.declaration_of( id ) : Node_id {};
    if( !decl.is_valid() )
    {
        return false;
    }

    // C++ forbids it, and `this` is an ordinary parameter here - which is what makes everything
    // else free, and is exactly why this one case has to be written down.
    if( ast_.kind( decl ) == Node_kind::Param_decl && Symbol_id { ast_.aux( decl ) } == Interner::keyword( Keyword::This ) )
    {
        return false;
    }

    return ast_.kind( decl ) == Node_kind::Var_decl || ast_.kind( decl ) == Node_kind::Param_decl ||
           ast_.kind( decl ) == Node_kind::Field_decl;
}

void Checker::visit_return( Node_id id )
{
    const Node_id value     = ast_.children( id )[0];
    const Type_id void_type = table_.builtin( Type_kind::Void );

    if( !value.is_valid() ) // a bare `return;`
    {
        if( current_return_.is_valid() && current_return_ != void_type )
        {
            error_at(
                ast_.span( id ),
                fmt::format( "this function returns `{}`, so `return` needs a value", table_.name( current_return_ ) )
            );
        }

        return;
    }

    if( current_return_ == void_type )
    {
        // Typed anyway: a mistake inside the expression is still a mistake worth reporting.
        absorb( value );
        error_at( ast_.span( value ), "a `void` function cannot return a value" );
        return;
    }

    check( value, current_return_ );
}

void Checker::visit_var( Node_id id )
{
    // Children are { type, init }. Either can be invalid: no annotation means `auto`, and no
    // initialiser means the variable is only declared.
    const Node_id annotation = ast_.children( id )[0];
    const Node_id init       = ast_.children( id )[1];

    Type_id type = type_of_annotation( annotation );

    if( type.is_valid() )
    {
        // Annotated. Checking rather than inferring is what lets a literal adopt the declared
        // type, so `u32 x = 42;` needs no suffix. An unknown annotation is the error type, which
        // check() absorbs - one diagnostic, from type_of_annotation.
        if( init.is_valid() )
        {
            check( init, type );
            check_owning_source( init, type );
        }
    }
    else if( init.is_valid() )
    {
        type = infer( init ); // `auto`: L6's one form of inference
        check_owning_source( init, type );
    }
    else
    {
        error_at( ast_.span( id ), "`auto` needs an initialiser to infer from" );
        type = table_.builtin( Type_kind::Error );
    }

    record( id, type );
}

void Checker::visit_assign( Node_id id )
{
    // Children are { target, value }; aux is the operator, which may be `+=` rather than `=`.
    const Token_kind op     = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    target = ast_.children( id )[0];
    const Node_id    value  = ast_.children( id )[1];

    // Asked before infer(), which would otherwise absorb it: a literal target types as an error
    // today, so `1 = 2;` passed in silence. Parameters are assignable too - L12 is
    // mutable-by-default, and a parameter is a local.
    if( !is_assignable( target ) )
    {
        // A name that does not resolve, or resolves to a function, is already reported by
        // infer_name. Anything else - a literal, a call, an arithmetic expression - has nothing
        // else to report it.
        if( ast_.kind( target ) != Node_kind::Name_expr )
        {
            error_at( ast_.span( target ), "cannot assign to this expression" );
        }

        // `this` is the exception to the rule above: it resolves, so infer_name says nothing, and
        // the guard there stays quiet because it is a Name_expr. Nothing else would report it.
        const Node_id decl = resolution_.declaration_of( target );
        if( decl.is_valid() && ast_.kind( decl ) == Node_kind::Param_decl &&
            Symbol_id { ast_.aux( decl ) } == Interner::keyword( Keyword::This ) )
        {
            error_at( ast_.span( target ), "`this` is immutable" );
        }

        absorb( target );
        absorb( value );
        return;
    }

    const Type_id target_type = infer( target );

    if( table_.is_error( target_type ) )
    {
        absorb( value );
        return;
    }

    if( op == Token_kind::Equal )
    {
        check( value, target_type );
        check_owning_source( value, target_type );
        return;
    }

    // A compound assignment is `x = x op y`, so the value is *checked* against the target rather
    // than inferred. Inferring it would settle a literal on its default type first, and `u8 x;
    // x += 3;` would then combine u8 with i32 and have nowhere to put the result.
    //
    // Checking also subsumes the range rule: whatever holds in the target can be combined with it
    // and stored back, and whatever does not is rejected here with a clearer message than
    // "cannot hold the i32 this produces".
    check( value, target_type );

    if( !table_.is_integer( target_type ) && !table_.is_float( target_type ) )
    {
        error_at(
            ast_.span( id ), fmt::format( "no operator `{}` for `{}`", token_kind_spelling( op ), table_.name( target_type ) )
        );
    }
}

void Checker::visit_if( Node_id id )
{
    // Children are { condition, then, else }; else is invalid when there is none, and visit()
    // returns immediately on that.
    check_condition( ast_.children( id )[0] );
    visit( ast_.children( id )[1] );
    visit( ast_.children( id )[2] );
}

void Checker::visit_while( Node_id id )
{
    check_condition( ast_.children( id )[0] );
    loop_depth_ += 1;
    visit( ast_.children( id )[1] );
    loop_depth_ -= 1;
}

void Checker::visit_for( Node_id id )
{
    // Children are { init, condition, update, body }, any of which `for( ; ; )` leaves invalid.
    // init and update are ordinary statements - a declaration, an assignment, an increment - so
    // they go through visit, not infer.
    visit( ast_.children( id )[0] );
    check_condition( ast_.children( id )[1] );
    visit( ast_.children( id )[2] );
    loop_depth_ += 1;
    visit( ast_.children( id )[3] );
    loop_depth_ -= 1;
}

void Checker::visit_increment( Node_id id )
{
    // Children are { operand }; aux is `++` or `--`.
    const Token_kind op      = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    operand = ast_.children( id )[0];

    if( !is_assignable( operand ) )
    {
        // Same split as visit_assign: infer_name already covers an unresolved name and a function.
        if( ast_.kind( operand ) != Node_kind::Name_expr )
        {
            error_at( ast_.span( operand ), fmt::format( "`{}` needs a variable", token_kind_spelling( op ) ) );
        }

        infer( operand );
        return;
    }

    const Type_id operand_type = infer( operand );

    if( table_.is_error( operand_type ) )
    {
        return;
    }

    if( !table_.is_integer( operand_type ) && !table_.is_float( operand_type ) )
    {
        error_at(
            ast_.span( id ), fmt::format( "no operator `{}` for `{}`", token_kind_spelling( op ), table_.name( operand_type ) )
        );
    }
}

void Checker::visit_global( Node_id id )
{
    const Type_id type = types_[id.v];
    const Node_id init = ast_.children( id )[1];

    if( table_.is_error( type ) )
    {
        return;
    }

    // A struct literal lowers to a temporary and field assignments, and there is nowhere at C file
    // scope to put those. Checked before the initialiser so `Point origin;` is caught too.
    if( table_.is_struct( type ) )
    {
        error_at( ast_.span( id ), "a struct cannot be a file-scope variable yet" );
        return;
    }

    if( !init.is_valid() )
    {
        return; // zero, which C guarantees for file-scope storage
    }

    if( !is_constant_expression( init ) )
    {
        error_at(
            ast_.span( init ),
            "a file-scope initialiser must be a constant expression",
            "it may use literals and arithmetic over them, but nothing that has to run"
        );

        return;
    }

    check( init, type ); // range checking, and the literal adopts the declared type

    // Evaluated here rather than printed as an expression by the backend. The rule already says
    // this is a constant expression, so the value exists; computing it once is what lets a backend
    // emit a literal instead of re-deriving §6.4's conversions in its own spelling of the tree.
    if( table_.is_float( type ) )
    {
        if( const std::optional<f64> value = fold_float( init ) )
        {
            constants_.emplace( id.v, Constant_value { Constant_value::Kind::Float, 0, false, *value } );
        }

        return;
    }

    const Folded value = fold_integer( init );

    if( value.constant && !value.overflowed )
    {
        constants_.emplace(
            id.v, Constant_value { Constant_value::Kind::Integer, value.value.magnitude, value.value.negative, 0.0 }
        );
    }
}

Type_id Checker::infer( Node_id id )
{
    if( !id.is_valid() )
    {
        return Type_id {};
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Error:
        return record( id, table_.builtin( Type_kind::Error ) );

    case Node_kind::Name_expr:
        return infer_name( id );

    case Node_kind::Call_expr:
        return infer_call( id );

    case Node_kind::Binary_expr:
        return infer_binary( id );

    case Node_kind::Unary_expr:
        return infer_unary( id );

    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Bool_literal:
    case Node_kind::Char_literal:
    case Node_kind::Null_literal:
        return infer_literal( id );

    case Node_kind::String_literal:
        // Lexed and parsed, but a string has no type until there is a String, which is M7.
        // Silence here would make it look accepted.
        error_at( ast_.span( id ), "string literals are not supported yet" );
        return record( id, table_.builtin( Type_kind::Error ) );

    case Node_kind::Field_expr:
        return infer_field( id );

    case Node_kind::Struct_literal:
        return infer_struct_literal( id );

    case Node_kind::Cast_expr:
        return infer_cast( id );

    case Node_kind::Marker_expr:
        return infer_marker( id );

    default:
        // Every expression not yet given a case of its own - the literals, chiefly, which cannot
        // be typed until their values survive lexing. Children are still typed, so a mistake
        // inside one is not swallowed by the parent being unsupported.
        for( const Node_id child : ast_.children( id ) )
        {
            infer( child );
        }

        return record( id, table_.builtin( Type_kind::Error ) );
    }
}

bool Checker::accepts( Operands operands, Type_id type ) const
{
    switch( operands )
    {
    case Operands::Integer:
        return table_.is_integer( type );
    case Operands::Numeric:
        return table_.is_integer( type ) || table_.is_float( type );
    case Operands::Bool:
        return type == table_.builtin( Type_kind::Bool );
    case Operands::Comparable:
        return accepts( Operands::Numeric, type ) || accepts( Operands::Bool, type );
    }

    return false;
}

bool Checker::is_literal_expression( Node_id id ) const
{
    if( !id.is_valid() )
    {
        return false;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Bool_literal:
    case Node_kind::Char_literal:
    case Node_kind::Null_literal:
        return true;

    case Node_kind::Unary_expr:
    {
        // `-5` and `-1.5` are negations of literals, and check_literal pushes the expectation
        // through the minus.
        if( static_cast<Token_kind>( ast_.aux( id ) ) != Token_kind::Minus )
        {
            return false;
        }

        const Node_kind operand = ast_.kind( ast_.children( id )[0] );

        return operand == Node_kind::Int_literal || operand == Node_kind::Float_literal;
    }

    default:
        return false;
    }
}

// A file-scope initialiser has to be something C will accept as a constant expression too, since
// that is what lets the emitter print it rather than needing code to run before main. Literals and
// arithmetic over them qualify; anything that loads, calls or takes an address does not.
//
// `&&` and `||` are excluded deliberately. Their ordinary lowering emits control flow - the right
// side must not run unless the left demands it - and keeping them out means the file-scope path
// never has to answer whether printing both sides is sound. They buy nothing in an initialiser.
bool Checker::is_constant_expression( Node_id id ) const
{
    if( !id.is_valid() )
    {
        return false;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Bool_literal:
    case Node_kind::Char_literal:
    case Node_kind::Null_literal:
        return true;

    case Node_kind::Unary_expr:
        // `&x` is an address and `*p` a load; neither is a value known here.
        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Minus:
        case Token_kind::Plus:
        case Token_kind::Tilde:
        case Token_kind::Bang:
            return is_constant_expression( ast_.children( id )[0] );

        default:
            return false;
        }

    case Node_kind::Binary_expr:
    {
        const Token_kind op = static_cast<Token_kind>( ast_.aux( id ) );

        if( op == Token_kind::Amp_amp || op == Token_kind::Pipe_pipe )
        {
            return false;
        }

        return is_constant_expression( ast_.children( id )[0] ) && is_constant_expression( ast_.children( id )[1] );
    }

    case Node_kind::Cast_expr:
        return is_constant_expression( ast_.children( id )[1] );

    default:
        return false;
    }
}

Type_id Checker::infer_literal( Node_id id )
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
        return record( id, table_.default_integer() );

    case Node_kind::Float_literal:
        return record( id, table_.default_float() );

    case Node_kind::Bool_literal:
        return record( id, table_.builtin( Type_kind::Bool ) );

    case Node_kind::Char_literal:
        return record( id, table_.integer( 8, false ) ); // D20: a code point, so a u8

    case Node_kind::Null_literal:
        error_at( ast_.span( id ), "cannot infer type of `nullptr`" );
        return record( id, table_.builtin( Type_kind::Error ) );

    default:
        assert( false );
        return record( id, table_.builtin( Type_kind::Error ) );
    }
}

Type_id Checker::check_literal( Node_id id, Type_id expected )
{
    // `-2147483648` parses as a negation of 2147483648, which does not fit an i32 on its own. So
    // the expectation is pushed through the minus and the range check is told the value is
    // negative - exactly the asymmetry Type_table::fits() carries.
    Node_id literal  = id;
    bool    negative = false;

    if( ast_.kind( id ) == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( id ) ) == Token_kind::Minus &&
        is_literal_expression( ast_.children( id )[0] ) )
    {
        literal = ast_.children( id )[0];

        // Only integers have an asymmetric range. A float's negation cannot take it out of range.
        negative = ast_.kind( literal ) == Node_kind::Int_literal;
    }

    switch( ast_.kind( literal ) )
    {
    case Node_kind::Bool_literal:
        if( expected != table_.builtin( Type_kind::Bool ) )
        {
            error_at( ast_.span( id ), fmt::format( "expected `{}`, but got `bool`", table_.name( expected ) ) );
            return expected;
        }

        break;

    case Node_kind::Float_literal:
    {
        if( !table_.is_float( expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format( "expected `{}`, but got a floating-point literal", table_.name( expected ) ),
                table_.is_integer( expected ) ? "a fractional value cannot be an integer" : std::string {}
            );

            return expected;
        }

        const Literal_id value = Literal_id { ast_.aux( literal ) };

        // A literal the lexer could not scan has no value recorded. It reported there.
        if( value.is_valid() && !table_.fits_float( literals_.floating( value ), expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format(
                    "`{}{}` does not fit in `{}`",
                    literal == id ? "" : "-", // the sign lives in the Unary_expr, not in the value
                    literals_.floating( value ),
                    table_.name( expected )
                )
            );

            return expected;
        }

        break;
    }

    case Node_kind::Char_literal: // a code point is an integer value, measured the same way
    case Node_kind::Int_literal:
    {
        if( !table_.is_integer( expected ) && !table_.is_float( expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format(
                    "expected `{}`, but got {}",
                    table_.name( expected ),
                    ast_.kind( literal ) == Node_kind::Char_literal ? "a character literal" : "an integer literal"
                )
            );

            return expected;
        }

        const Literal_id value = Literal_id { ast_.aux( literal ) };

        // A literal the lexer could not scan - one that overflowed a u64, say - has no value
        // recorded. It was reported there; saying so again here helps nobody.
        if( value.is_valid() && !table_.fits( literals_.integer( value ), negative, expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format(
                    "`{}{}` does not fit in `{}`", negative ? "-" : "", literals_.integer( value ), table_.name( expected )
                )
            );

            return expected;
        }

        break;
    }
    case Node_kind::Null_literal:
        if( !table_.is_pointer( expected ) )
        {
            error_at(
                ast_.span( id ),
                fmt::format( "expected `{}`, but got a null literal", table_.name( expected ) ),
                "a null literal can only be assigned to a pointer"
            );

            return expected;
        }

        break;

    default:
        return expected; // not a literal after all
    }

    // The literal adopts the expected type - the whole point of checking rather than inferring,
    // and what lets `u32 x = 42;` need no suffix. The negation, when there is one, takes it too.
    record( literal, expected );
    return record( id, expected );
}

Type_id Checker::infer_name( Node_id id )
{
    const Node_id decl = resolution_.declaration_of( id );

    if( !decl.is_valid() )
    {
        return record( id, table_.builtin( Type_kind::Error ) ); // the resolver already said so
    }

    if( ast_.kind( decl ) == Node_kind::Function_decl )
    {
        // types_[decl] holds the function's *return* type, so without this `i32 x = f;` would
        // quietly succeed whenever f happens to return an i32.
        error_at(
            ast_.span( id ), fmt::format( "`{}` is a function, not a value", interner_.text( Symbol_id { ast_.aux( id ) } ) )
        );

        return record( id, table_.builtin( Type_kind::Error ) );
    }

    return record( id, types_[decl.v] );
}

Type_id Checker::infer_call( Node_id id )
{
    const Node_id callee = ast_.children( id )[0];
    const Node_id args   = ast_.children( id )[1];

    // v0 has no function pointers, so anything but a plain name in call position has no
    // declaration to find.
    const Node_id decl = ast_.kind( callee ) == Node_kind::Name_expr ? resolution_.declaration_of( callee ) : Node_id {};

    // Even on a failed call the arguments must be typed, or later passes meet untyped nodes and a
    // genuine mistake inside one goes unreported.
    const auto type_the_arguments_anyway = [&]()
    {
        for( const Node_id arg : ast_.children( args ) )
        {
            infer( arg );
        }
    };

    if( !decl.is_valid() )
    {
        // An unknown name was already reported by the resolver; say nothing twice.
        if( ast_.kind( callee ) != Node_kind::Name_expr )
        {
            error_at( ast_.span( callee ), "this expression is not callable" );
        }

        type_the_arguments_anyway();
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    const std::string_view name = interner_.text( Symbol_id { ast_.aux( callee ) } );

    // What the arguments are checked against, what the call produces, and how many leading
    // parameters are not the author's to supply. For a plain function all three are the obvious
    // answers; a construction is where they come apart.
    Node_id callable        = decl;
    Type_id result          = types_[decl.v];
    u32     implicit_params = 0;

    if( is_aggregate( ast_.kind( decl ) ) )
    {
        // `Buffer( 16 )` names a type, not a function: the arguments belong to its constructor, but
        // the result is the type itself - a constructor returns nothing and writes through `this`.
        callable = find_member( decl, Node_kind::Constructor_decl );

        if( !callable.is_valid() )
        {
            error_at(
                ast_.span( callee ),
                fmt::format( "`{}` has no constructor", name ),
                fmt::format( "build it from a literal: `{} {{ ... }}`", name )
            );
            type_the_arguments_anyway();
            return record( id, table_.builtin( Type_kind::Error ) );
        }

        // The receiver is an ordinary first parameter, so skipping it here is what stops every
        // call site owing an extra argument.
        implicit_params = 1;
    }
    else if( ast_.kind( decl ) != Node_kind::Function_decl )
    {
        error_at( ast_.span( callee ), fmt::format( "`{}` is not callable", name ) );
        type_the_arguments_anyway();
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    const std::span<const Node_id> declared  = ast_.children( ast_.children( callable )[1] );
    const std::span<const Node_id> params    = declared.subspan( implicit_params );
    const std::span<const Node_id> arguments = ast_.children( args );

    if( params.size() != arguments.size() )
    {
        error_at(
            ast_.span( args ),
            fmt::format(
                "`{}` takes {} argument{}, but {} {} given",
                name,
                params.size(),
                params.size() == 1 ? "" : "s",
                arguments.size(),
                arguments.size() == 1 ? "was" : "were"
            )
        );
    }

    // Check the pairs that do line up even when the count is wrong: one missing argument should
    // not hide a type error in the others. A parameter whose own type failed to resolve holds an
    // invalid Type_id, which check() absorbs.
    const std::size_t shared = std::min( params.size(), arguments.size() );

    for( std::size_t i = 0; i < shared; ++i )
    {
        check( arguments[i], types_[params[i].v] );

        // D2: transfer is visible at the call *and* in the signature, and neither alone is enough -
        // a reader of one should never have to find the other.
        const Keyword wanted = parameter_mode( params[i] );
        const Keyword given  = ast_.kind( arguments[i] ) == Node_kind::Marker_expr
                                   ? static_cast<Keyword>( ast_.aux( arguments[i] ) )
                                   : Keyword::Count;

        // A ref binds to the caller's object itself, so there is no conversion step for a widened
        // copy to live in: `ref u8` and `ref i32` are different bindings, not convertible ones.
        // Above the agreement check, because agreeing is this rule's precondition rather than its
        // exit - and it wants both sides, so one missing marker does not report twice.
        if( wanted == Keyword::Ref && given == Keyword::Ref && !table_.is_error( types_[arguments[i].v] ) &&
            types_[arguments[i].v] != types_[params[i].v] )
        {
            error_at(
                ast_.span( arguments[i] ),
                fmt::format(
                    "cannot borrow `{}` as `ref {}`", table_.name( types_[arguments[i].v] ), table_.name( types_[params[i].v] )
                ),
                "a borrow is the variable itself, so its type must match exactly"
            );
        }

        if( wanted == given )
        {
            continue;
        }

        // The exemption is narrow and belongs only to `move`: on a non-owning type it is the
        // caller's own assertion that the source is dead afterwards, which the callee never sees.
        // `ref` changes what the callee is holding, at every type.
        const bool about_ownership =
            ( wanted == Keyword::Count || wanted == Keyword::Move ) && ( given == Keyword::Count || given == Keyword::Move );

        if( about_ownership && !is_owning_type( types_[params[i].v] ) )
        {
            continue;
        }

        if( wanted == Keyword::Ref )
        {
            error_at( ast_.span( arguments[i] ), fmt::format( "`{}` may modify this argument", name ), "write `ref`" );
        }
        else if( given == Keyword::Ref )
        {
            error_at( ast_.span( arguments[i] ), fmt::format( "`{}` does not modify this argument", name ), "remove `ref`" );
        }
        else if( wanted == Keyword::Move )
        {
            error_at( ast_.span( arguments[i] ), fmt::format( "`{}` takes ownership of this argument", name ), "write `move`" );
        }
        else if( given == Keyword::Move )
        {
            error_at( ast_.span( arguments[i] ), fmt::format( "`{}` borrows this argument", name ), "remove `move`" );
        }
    }

    for( std::size_t i = shared; i < arguments.size(); ++i )
    {
        infer( arguments[i] );
    }

    return record( id, result );
}

Type_id Checker::infer_binary( Node_id id )
{
    const Token_kind op        = static_cast<Token_kind>( ast_.aux( id ) );
    const Node_id    left      = ast_.children( id )[0];
    const Node_id    right     = ast_.children( id )[1];
    const Type_id    bool_type = table_.builtin( Type_kind::Bool );
    const Type_id    error     = table_.builtin( Type_kind::Error );

    // A literal has a value, not a type, so the other operand is what gives it one. Inferring both
    // would settle it on its default first, and `u32 bits; bits != 0` would then compare a u32
    // with an i32 - which §6.4 rejects, and which no suffix exists to write around (D13). That
    // would leave unsigned code very nearly unwritable.
    Type_id lhs_type;
    Type_id rhs_type;

    if( is_literal_expression( left ) != is_literal_expression( right ) )
    {
        const bool literal_on_the_left = is_literal_expression( left );

        const Node_id known_side   = literal_on_the_left ? right : left;
        const Node_id literal_side = literal_on_the_left ? left : right;

        const Type_id known = infer( known_side );

        // A failed operand gives nothing to adopt, and check() absorbs an error expectation - so
        // the literal is carried along rather than asked to invent a type it has no basis for.
        const Type_id adopted = check( literal_side, known );

        lhs_type = literal_on_the_left ? adopted : known;
        rhs_type = literal_on_the_left ? known : adopted;
    }
    else
    {
        lhs_type = infer( left );
        rhs_type = infer( right );
    }

    // An operand that is already wrong was reported where it went wrong. Repeating it here is the
    // cascade the error type exists to prevent - and name() would assert on it besides.
    if( table_.is_error( lhs_type ) || table_.is_error( rhs_type ) )
    {
        return record( id, error );
    }

    const auto reject = [&]( std::string_view help )
    {
        error_at(
            ast_.span( id ),
            fmt::format(
                "no operator `{}` for `{}` and `{}`",
                token_kind_spelling( op ),
                table_.name( lhs_type ),
                table_.name( rhs_type )
            ),
            std::string( help )
        );

        return record( id, error );
    };

    // Pointer equality, which is how a null check is written. Handled before the rule table, whose
    // operand classes are all numeric or bool. Ordering is deliberately absent: comparing pointers
    // into different allocations is meaningless, and §6.4 has no row for them.
    if( table_.is_pointer( lhs_type ) || table_.is_pointer( rhs_type ) )
    {
        if( op != Token_kind::Equal_equal && op != Token_kind::Bang_equal )
        {
            return reject( {} );
        }

        if( lhs_type != rhs_type )
        {
            return reject( "only pointers of the same type can be compared" );
        }

        return record( id, bool_type );
    }

    const Binary_rule* rule = binary_rule_for( op );

    if( rule == nullptr )
    {
        error_at( ast_.span( id ), fmt::format( "operator `{}` is not supported yet", token_kind_spelling( op ) ) );
        return record( id, error );
    }

    if( !accepts( rule->operands, lhs_type ) || !accepts( rule->operands, rhs_type ) )
    {
        return reject( operand_requirement( rule->operands ) );
    }

    // Two bools compare and combine directly: §6.4 has no row for bool at all.
    if( lhs_type == bool_type && rhs_type == bool_type )
    {
        return record( id, bool_type );
    }

    if( rule->result == Result::Left )
    {
        return record_constant( id, lhs_type );
    }

    const Type_id common = table_.arithmetic_result( lhs_type, rhs_type );

    if( !common.is_valid() )
    {
        return reject( {} );
    }

    return record_constant( id, rule->result == Result::Bool ? bool_type : common );
}

Type_id Checker::infer_unary( Node_id id )
{
    const Token_kind op           = static_cast<Token_kind>( ast_.aux( id ) );
    const Type_id    operand_type = infer( ast_.children( id )[0] );
    const Type_id    error        = table_.builtin( Type_kind::Error );

    if( table_.is_error( operand_type ) )
    {
        return record( id, error );
    }

    // Neither of these is in the rule table: that table assumes the result type is the operand's,
    // and both of these change it.
    if( op == Token_kind::Amp )
    {
        // The operand must be somewhere a value lives. `&f()` names the address of a temporary
        // that is about to vanish, and `&1` names nothing at all. Asked after infer() so that
        // errors inside the operand are reported first.
        if( !is_assignable( ast_.children( id )[0] ) )
        {
            error_at( ast_.span( id ), "cannot take the address of this expression", "it does not name a variable" );

            return record( id, error );
        }

        return record( id, table_.pointer_to( operand_type ) );
    }

    if( op == Token_kind::Star )
    {
        if( !table_.is_pointer( operand_type ) )
        {
            error_at( ast_.span( id ), fmt::format( "`{}` cannot be dereferenced", table_.name( operand_type ) ) );

            return record( id, error );
        }

        return record( id, table_.get( operand_type ).element );
    }

    const Unary_rule* rule = unary_rule_for( op );

    if( rule == nullptr )
    {
        error_at( ast_.span( id ), fmt::format( "unary `{}` is not supported yet", token_kind_spelling( op ) ) );
        return record( id, error );
    }

    const auto reject = [&]( std::string_view help )
    {
        error_at(
            ast_.span( id ),
            fmt::format( "no operator `{}` for `{}`", token_kind_spelling( op ), table_.name( operand_type ) ),
            std::string( help )
        );

        return record( id, error );
    };

    if( !accepts( rule->operands, operand_type ) )
    {
        return reject( operand_requirement( rule->operands ) );
    }

    // C++ answers 4294967295 for `-u32(1)`: a silent wrong answer of exactly the kind D5 removes.
    if( rule->signed_only && table_.is_integer( operand_type ) && !table_.get( operand_type ).is_signed )
    {
        return reject( "negation needs a signed type" );
    }

    return record_constant( id, operand_type );
}

Node_id Checker::find_field( Type_id type, Symbol_id name ) const
{
    const Node_id decl = table_.get( type ).declaration;

    if( !decl.is_valid() )
    {
        return Node_id();
    }

    for( const Node_id field : ast_.children( decl ) )
    {
        if( ast_.aux( field ) == name.v )
        {
            return field;
        }
    }

    return Node_id();
}

Type_id Checker::infer_field( Node_id id )
{
    const Node_id base      = ast_.children( id )[0];
    const Type_id base_type = infer( base );

    if( table_.is_error( base_type ) )
    {
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    // D22: `.` reaches through a pointer, so a pointer-to-struct base is the struct.
    const Type_id object_type = table_.is_pointer( base_type ) ? table_.get( base_type ).element : base_type;

    if( !table_.is_struct( object_type ) )
    {
        error_at( ast_.span( base ), fmt::format( "`{}` has no fields", table_.name( object_type ) ) );
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    const Node_id decl = find_field( object_type, Symbol_id { ast_.aux( id ) } );
    if( !decl.is_valid() )
    {
        error_at(
            ast_.span( id ),
            fmt::format( "`{}` has no field `{}`", table_.name( object_type ), interner_.text( Symbol_id { ast_.aux( id ) } ) )
        );

        return record( id, table_.builtin( Type_kind::Error ) );
    }

    return record( id, types_[decl.v] );
}

Type_id Checker::infer_struct_literal( Node_id id )
{
    const Type_id error = table_.builtin( Type_kind::Error );

    // The resolver already bound the type name, so there is no scope lookup here - and if it
    // failed, it reported. Saying so again is the cascade the error type exists to prevent.
    const Node_id decl = resolution_.declaration_of( id );

    if( !decl.is_valid() || !is_aggregate( ast_.kind( decl ) ) )
    {
        for( const Node_id init : ast_.children( id ) )
        {
            infer( ast_.children( init )[0] ); // type the values anyway
        }

        return record( id, error );
    }

    // D29: a class with a constructor is built by calling it. That is what stops the two
    // initialisation syntaxes competing for one type, and it is the rule the struct-literal-on-a-
    // class exception was standing in for until constructors existed.
    if( find_member( decl, Node_kind::Constructor_decl ).is_valid() )
    {
        const std::string_view name = interner_.text( Symbol_id { ast_.aux( decl ) } );

        error_at(
            ast_.span( id ),
            fmt::format( "`{}` has a constructor, so it cannot be built from a literal", name ),
            fmt::format( "write `{}( ... )`", name )
        );

        for( const Node_id init : ast_.children( id ) )
        {
            infer( ast_.children( init )[0] );
        }

        // The declared type rather than the error type: the mistake is how it was built, not what
        // it is, so a cascade at the assignment would say nothing new.
        return record( id, types_[decl.v] );
    }

    const std::span<const Node_id> initialisers = ast_.children( id );

    // Fields, not members: a destructor is a child of the declaration too, and counting it would
    // demand an extra initialiser and misalign every positional one after it.
    std::vector<Node_id> fields;

    for( const Node_id member : ast_.children( decl ) )
    {
        if( ast_.kind( member ) == Node_kind::Field_decl )
        {
            fields.push_back( member );
        }
    }

    const std::string_view struct_name = interner_.text( Symbol_id { ast_.aux( decl ) } );
    const Type_id          result      = types_[decl.v];

    // One convention per literal, as C++20 requires. Two in the same literal is a reader's
    // problem rather than a parser's.
    std::size_t named = 0;

    for( const Node_id init : initialisers )
    {
        named += Symbol_id { ast_.aux( init ) }.is_valid() ? 1 : 0;
    }

    if( named != 0 && named != initialisers.size() )
    {
        error_at(
            ast_.span( id ),
            "an initialiser list is either all positional or all named",
            "give every field a name, or none of them"
        );
    }

    if( named == 0 )
    {
        if( initialisers.size() != fields.size() )
        {
            error_at(
                ast_.span( id ),
                fmt::format(
                    "`{}` has {} field{}, but {} {} given",
                    struct_name,
                    fields.size(),
                    fields.size() == 1 ? "" : "s",
                    initialisers.size(),
                    initialisers.size() == 1 ? "was" : "were"
                )
            );
        }

        // The pairs that line up are checked even when the count is wrong: one missing value
        // should not hide a type error in the others.
        const std::size_t shared = std::min( initialisers.size(), fields.size() );

        for( std::size_t i = 0; i < shared; ++i )
        {
            // check, never infer - `Point { 1, 2 }` has to let those literals become f64.
            check( ast_.children( initialisers[i] )[0], types_[fields[i].v] );
        }

        for( std::size_t i = shared; i < initialisers.size(); ++i )
        {
            infer( ast_.children( initialisers[i] )[0] );
        }

        return record( id, result );
    }

    std::unordered_map<u32, Node_id> seen;

    for( const Node_id init : initialisers )
    {
        const Symbol_id name { ast_.aux( init ) };
        const Node_id   value = ast_.children( init )[0];

        if( !name.is_valid() )
        {
            absorb( value ); // the mixing error above already covered this one
            continue;
        }

        const Node_id field = find_field( result, name );

        if( !field.is_valid() )
        {
            error_at( ast_.span( init ), fmt::format( "`{}` has no field `{}`", struct_name, interner_.text( name ) ) );
            absorb( value );
            continue;
        }

        if( !seen.try_emplace( name.v, init ).second )
        {
            error_at( ast_.span( init ), fmt::format( "field `{}` is given twice", interner_.text( name ) ) );
        }

        check( value, types_[field.v] );
    }

    // Every field that is missing, not just the first: a struct that gained three fields should
    // say so once rather than over three compiles.
    for( const Node_id field : fields )
    {
        const Symbol_id name { ast_.aux( field ) };

        if( name.is_valid() && seen.find( name.v ) == seen.end() )
        {
            error_at( ast_.span( id ), fmt::format( "field `{}` is not initialised", interner_.text( name ) ) );
        }
    }

    return record( id, result );
}

Type_id Checker::infer_cast( Node_id id )
{
    const bool    is_cast = static_cast<Keyword>( ast_.aux( id ) ) == Keyword::Cast;
    const Type_id target  = type_of_annotation( ast_.children( id )[0] );
    const Node_id operand = ast_.children( id )[1];
    const Type_id error   = table_.builtin( Type_kind::Error );

    // A literal has a value and no type, so `cast` is the context that gives it one:
    // `cast<u8>( 300 )` is the ordinary out-of-range error rather than a conversion, and
    // `cast<f32>( 1 )` is simply an f32 literal. `wrap` must not do this - accepting a value the
    // target cannot hold is the entire point of it.
    const bool from_literal = is_cast && !table_.is_error( target ) && is_literal_expression( operand );

    const Type_id value = from_literal ? check( operand, target ) : infer( operand );

    if( table_.is_error( target ) || table_.is_error( value ) )
    {
        return record( id, error );
    }

    // check() has already ruled on the pair, and reported if it was wrong.
    if( from_literal )
    {
        return record( id, target );
    }

    const std::string_view name = is_cast ? "cast" : "wrap";

    const auto reject = [&]( std::string message, std::string help = {} )
    {
        error_at( ast_.span( id ), std::move( message ), std::move( help ) );

        return record( id, error );
    };

    switch( conversion_for( table_.get( value ).kind, table_.get( target ).kind ) )
    {
    case Conversion::None:
    {
        std::string help;

        if( table_.is_float( value ) && table_.is_integer( target ) )
        {
            help = "rounding is not implied; this needs an explicit rounding function";
        }
        else if( table_.is_integer( value ) && target == table_.builtin( Type_kind::Bool ) )
        {
            help = "compare it instead, as in `x != 0`";
        }

        return reject(
            fmt::format( "`{}` cannot convert `{}` to `{}`", name, table_.name( value ), table_.name( target ) ),
            std::move( help )
        );
    }

    case Conversion::Unsafe:
        return reject( "converting between pointer types is not supported yet" );

    case Conversion::Cast_only:
        if( !is_cast )
        {
            return reject(
                fmt::format( "`wrap` cannot convert `{}` to `{}`", table_.name( value ), table_.name( target ) ),
                "`wrap` keeps the low bits of an integer; use `cast` here"
            );
        }

        break;

    case Conversion::Both:
        // Narrowing is the one place the two operators disagree, and the check that makes `cast`
        // safe there does not exist yet.
        if( k_narrowing_cast_needs_a_run_time_check && is_cast && !table_.holds( value, target ) )
        {
            return reject(
                fmt::format( "`cast` cannot narrow `{}` to `{}` yet", table_.name( value ), table_.name( target ) ),
                fmt::format( "the run-time check is unimplemented; `wrap<{}>` truncates instead", table_.name( target ) )
            );
        }

        break;
    }

    return record( id, target );
}

Type_id Checker::infer_marker( Node_id id )
{
    const Keyword marker  = static_cast<Keyword>( ast_.aux( id ) );
    const Node_id operand = ast_.children( id )[0];

    if( marker == Keyword::Out )
    {
        error_at( ast_.span( id ), "`out` is not supported yet" );
        absorb( operand );
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    // `ref` lends a place the callee may write through, so it needs one that is writable.
    // is_assignable is that question already: what may be assigned is exactly what may be lent, and
    // reusing it is what stops the two drifting apart.
    if( marker == Keyword::Ref )
    {
        const Type_id value = infer( operand );

        if( !is_assignable( operand ) )
        {
            error_at( ast_.span( operand ), "`ref` needs a variable to borrow" );
            return record( id, table_.builtin( Type_kind::Error ) );
        }

        return record( id, value );
    }

    const Type_id value = infer( operand );

    // A field on its own is refused rather than lumped in with the rest, because the reason is
    // different and so is the fix: moving one would leave the object partly moved, and its own
    // scope exit would then drop a field that has already gone. Tracking that needs per-field drop
    // flags, so this is a restriction to lift when they exist rather than a rule to keep.
    if( ast_.kind( operand ) == Node_kind::Field_expr )
    {
        error_at(
            ast_.span( operand ),
            "a field cannot be moved on its own",
            "moving it would leave the object partly moved - move the whole object instead"
        );
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    // Everything else must be a plain named local or parameter. Deliberately stricter than
    // is_assignable, which also accepts `*p`: that names something this function does not own, so
    // moving out of it leaves a hole nothing tracks. The rule and the analysis then agree by
    // construction - the dataflow tracks whole locals, and nothing else can be moved.
    const Node_id decl = ast_.kind( operand ) == Node_kind::Name_expr ? resolution_.declaration_of( operand ) : Node_id {};
    const bool    named =
        decl.is_valid() && ( ast_.kind( decl ) == Node_kind::Var_decl || ast_.kind( decl ) == Node_kind::Param_decl );

    // Same reason as the field above: `*p` names something this function does not own, so moving
    // out of it leaves a hole nothing tracks.
    if( ast_.kind( operand ) == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( operand ) ) == Token_kind::Star )
    {
        error_at( ast_.span( operand ), "a pointee cannot be moved", "move the variable it points into instead" );
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    // A named variable, or a temporary this scope owns. The second is what makes
    // `consume( move Buffer( 16 ) )` expressible: the temporary is built in the caller's frame and
    // handed over, so a transfer genuinely happens and D2 wants it marked - even though no variable
    // is left behind for a use-after-move to catch.
    if( !named && !is_owning_type( value ) )
    {
        error_at( ast_.span( operand ), "only a variable or an owned temporary can be moved" );
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    return record( id, value );
}

Type_id Checker::check( Node_id id, Type_id expected )
{
    if( !id.is_valid() )
    {
        return expected;
    }

    if( table_.is_error( expected ) )
    {
        absorb( id );
        return expected;
    }

    // The bidirectional case: a literal has a value, not a type, so context gives it one. Handled
    // before infer(), which would otherwise settle on the default type first.
    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Bool_literal:
    case Node_kind::Char_literal:
    case Node_kind::Null_literal:
        return check_literal( id, expected );

    case Node_kind::Unary_expr:
        // `-2147483648` only fits an i32 as a negation, so the expectation goes through the minus.
        if( is_literal_expression( id ) )
        {
            return check_literal( id, expected );
        }

        break;

    case Node_kind::Binary_expr:
    {
        // Neither operand has a type of its own, so there is nothing beside them to adopt from -
        // the context is all there is. Without this, `u8 x = 'a' + 1;` combines a u8 with an i32
        // and then has nowhere to put the result.
        //
        // Only for operators whose result comes from their operands: a comparison yields bool
        // whatever it is given, and pushing the expectation into it would be nonsense.
        const Binary_rule* rule = binary_rule_for( static_cast<Token_kind>( ast_.aux( id ) ) );

        if( rule != nullptr && rule->result == Result::Common && is_literal_expression( ast_.children( id )[0] ) &&
            is_literal_expression( ast_.children( id )[1] ) )
        {
            const std::size_t before = diags_.error_count();

            check( ast_.children( id )[0], expected );
            check( ast_.children( id )[1], expected );

            // An operand that did not fit has already been reported, and it is the cause. Folding
            // the whole thing would say the same thing again with a different number: `i8 d = 0 -
            // 200;` would complain about `200` and then about `-200`.
            if( diags_.error_count() != before )
            {
                return record( id, expected );
            }

            return record_constant( id, expected );
        }

        // A shift's result type is its *left* operand's (§6.4), so that is the only operand an
        // expectation flows into - the count is a width, not a value in the same type. Without
        // this `u32 d = 1 << 4;` settles the literal on i32 and is then refused for being one.
        if( rule != nullptr && rule->result == Result::Left && is_literal_expression( ast_.children( id )[0] ) )
        {
            const std::size_t before = diags_.error_count();

            check( ast_.children( id )[0], expected );
            infer( ast_.children( id )[1] );

            if( diags_.error_count() != before )
            {
                return record( id, expected );
            }

            return record_constant( id, expected );
        }

        break;
    }

    default:
        break;
    }

    const Type_id actual = infer( id );
    if( table_.is_error( actual ) )
    {
        return expected;
    }

    if( !table_.holds( actual, expected ) )
    {
        error_at(
            ast_.span( id ), fmt::format( "expected `{}`, but got `{}`", table_.name( expected ), table_.name( actual ) )
        );
        return expected;
    }

    return expected;
}

Type_id Checker::type_of_annotation( Node_id id )
{
    // An invalid Node_id is `auto`, not a mistake - the parser writes one deliberately.
    if( !id.is_valid() )
    {
        return Type_id {};
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Named_type:
    {
        // Check for user-defined types first
        const Node_id decl = resolution_.declaration_of( id );

        if( decl.is_valid() )
        {
            if( !is_aggregate( ast_.kind( decl ) ) )
            {
                // Resolved to a function or variable of the same name.
                error_at(
                    ast_.span( id ), fmt::format( "`{}` is not a type", interner_.text( Symbol_id { ast_.aux( id ) } ) )
                );

                return table_.builtin( Type_kind::Error );
            }

            return types_[decl.v];
        }

        // Check for built-in types next

        const std::string_view spelling = interner_.text( Symbol_id { ast_.aux( id ) } );
        const Type_id          type     = table_.from_spelling( spelling );

        if( type.is_valid() )
        {
            return type;
        }

        const std::string_view suggestion = keel_spelling_for( spelling );

        error_at(
            ast_.span( id ),
            fmt::format( "unknown type `{}`", spelling ),
            suggestion.empty() ? std::string {} : fmt::format( "Keel spells this `{}`", suggestion )
        );

        return table_.builtin( Type_kind::Error );
    }

    case Node_kind::Const_type:
        return type_of_annotation( ast_.children( id )[0] );

    case Node_kind::Pointer_type:
        return table_.pointer_to( type_of_annotation( ast_.children( id )[0] ) );

    // D31/D32: a mode is not a type. It is unwrapped here and nowhere else, so what gets recorded
    // on the declaration is the underlying type and nothing downstream meets the wrapper - anything
    // needing the mode reads it back off the annotation, which is what parameter_mode does.
    case Node_kind::Mode_type:
    {
        const Keyword mode = static_cast<Keyword>( ast_.aux( id ) );

        if( mode == Keyword::Out )
        {
            error_at( ast_.span( id ), "`out` is not supported yet" );
            return table_.builtin( Type_kind::Error );
        }

        const Type_id inner = type_of_annotation( ast_.children( id )[0] );

        // D32: the binding is the referent, so the declaration keeps `T` and every use in the body
        // is an ordinary `T`. The address it travels as goes on the annotation, which nothing else
        // types - lowering and the emitter read it back from there.
        if( mode == Keyword::Ref )
        {
            record( id, table_.pointer_to( inner ) );
        }

        return inner;
    }

    case Node_kind::Generic_type:
        // A diagnostic rather than an assert: these parse, so reaching one is bad input, not a
        // broken invariant, and keelc must not abort on a program someone wrote.
        error_at( ast_.span( id ), "this type is not supported yet" );
        return table_.builtin( Type_kind::Error );

    default:
        // Error nodes, and anything the parser puts in type position that is not a type.
        return table_.builtin( Type_kind::Error );
    }
}

// Only + - * / % << >> fold. `&`, `|`, `^` and `~` cannot take a value outside the type their
// operands came from, so there is nothing for them to overflow and nothing here to check - saying
// "not constant" for those is the correct answer, not a shortcut.
Folded Checker::fold_integer( Node_id id, Type_id known ) const
{
    // Only the node being folded needs the hint: its children were checked first, so their own
    // types are recorded by the time the recursion reaches them.
    const auto width_type = [&]() -> Type_id
    { return known.is_valid() ? known : ( id.v < types_.size() ? types_[id.v] : Type_id {} ); };

    if( !id.is_valid() )
    {
        return not_constant();
    }

    // A node already reported as wrong contributes nothing. Folding through it would report the
    // same mistake again from every operation that encloses it.
    //
    // is_valid() first, and it is not redundant: is_error() answers true for an unrecorded type
    // too, and the node being folded is unrecorded by definition - record_constant runs before the
    // record. Without this the guard rejects every node it is asked about.
    if( id.v < types_.size() && types_[id.v].is_valid() && table_.is_error( types_[id.v] ) )
    {
        return not_constant();
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Char_literal: // a code point is an integer value
    case Node_kind::Int_literal:
    {
        const Literal_id value { ast_.aux( id ) };

        // A literal the lexer could not scan has no value recorded. It reported there.
        return value.is_valid() ? folded( literals_.integer( value ), false ) : not_constant();
    }

    // Neither carries a Literal_id: a bool's value is in aux, and `nullptr` records nothing at
    // all. Both are integers here, which is also how the lowerer spells them.
    case Node_kind::Bool_literal:
        return folded( ast_.aux( id ) != 0 ? 1 : 0, false );

    case Node_kind::Null_literal:
        return folded( 0, false );

    case Node_kind::Unary_expr:
    {
        const Folded operand = fold_integer( ast_.children( id )[0] );

        if( !operand.constant || operand.overflowed )
        {
            return operand;
        }

        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Minus:
            return folded( operand.value.magnitude, !operand.value.negative );

        case Token_kind::Plus:
            return operand;

        case Token_kind::Tilde:
        {
            const Type_id operation = width_type();

            if( !operation.is_valid() || !table_.is_integer( operation ) )
            {
                return not_constant();
            }

            const Type& described = table_.get( operation );

            return from_bits( ~to_bits( operand.value, described.width ), described.width, described.is_signed );
        }

        default:
            return not_constant();
        }
    }

    // Either a widening `cast` or a `wrap` - narrowing `cast` is refused until there is something
    // to trap with - so this is a reduction modulo the target's width either way.
    case Node_kind::Cast_expr:
    {
        const Folded operand = fold_integer( ast_.children( id )[1] );

        if( !operand.constant || operand.overflowed )
        {
            return operand;
        }

        const Type_id target = width_type();

        if( !target.is_valid() || !table_.is_integer( target ) )
        {
            return not_constant();
        }

        const Type& described = table_.get( target );

        return from_bits( to_bits( operand.value, described.width ), described.width, described.is_signed );
    }

    case Node_kind::Binary_expr:
    {
        const Folded left  = fold_integer( ast_.children( id )[0] );
        const Folded right = fold_integer( ast_.children( id )[1] );

        if( !left.constant || !right.constant )
        {
            return not_constant();
        }

        if( left.overflowed || right.overflowed )
        {
            return too_large();
        }

        const Constant a = left.value;
        const Constant b = right.value;

        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Plus:
            return add_constants( a, b );

        case Token_kind::Minus:
            return add_constants( a, Constant { b.magnitude, b.magnitude != 0 && !b.negative } );

        case Token_kind::Star:
            return multiply_constants( a, b );

        // Division truncates toward zero, and the remainder takes the sign of the dividend - which
        // is what sign-magnitude does on its own. check_constant reports the zero divisor.
        case Token_kind::Slash:
            return b.magnitude == 0 ? not_constant() : folded( a.magnitude / b.magnitude, a.negative != b.negative );

        case Token_kind::Percent:
            return b.magnitude == 0 ? not_constant() : folded( a.magnitude % b.magnitude, a.negative );

        case Token_kind::Less_less:
            // An out-of-range count is reported by check_constant; 1ull << 64 is undefined here
            // too, so the guard protects the compiler as much as the program.
            if( b.negative || b.magnitude >= 64 )
            {
                return not_constant();
            }

            return multiply_constants( a, Constant { 1ull << b.magnitude, false } );

        case Token_kind::Greater_greater:
            if( b.negative || b.magnitude >= 64 || a.negative )
            {
                return not_constant();
            }

            return folded( a.magnitude >> b.magnitude, false );

        // A comparison yields bool, which is the integer 0 or 1 here.
        case Token_kind::Less:
            return folded( less_than( a, b ) ? 1 : 0, false );

        case Token_kind::Greater:
            return folded( less_than( b, a ) ? 1 : 0, false );

        case Token_kind::Less_equal:
            return folded( less_than( b, a ) ? 0 : 1, false );

        case Token_kind::Greater_equal:
            return folded( less_than( a, b ) ? 0 : 1, false );

        case Token_kind::Equal_equal:
            return folded( equals( a, b ) ? 1 : 0, false );

        case Token_kind::Bang_equal:
            return folded( equals( a, b ) ? 0 : 1, false );

        case Token_kind::Amp:
        case Token_kind::Pipe:
        case Token_kind::Caret:
        {
            // The width the operation happens in, which the node carries. Without it there is no
            // bit pattern to work on.
            const Type_id operation = width_type();

            if( !operation.is_valid() || !table_.is_integer( operation ) )
            {
                return not_constant();
            }

            const Type& described  = table_.get( operation );
            const u64   left_bits  = to_bits( a, described.width );
            const u64   right_bits = to_bits( b, described.width );

            const Token_kind op = static_cast<Token_kind>( ast_.aux( id ) );

            const u64 result = op == Token_kind::Amp    ? left_bits & right_bits
                               : op == Token_kind::Pipe ? left_bits | right_bits
                                                        : left_bits ^ right_bits;

            return from_bits( result, described.width, described.is_signed );
        }

        default:
            return not_constant();
        }
    }

    default:
        return not_constant();
    }
}

std::optional<f64> Checker::fold_float( Node_id id ) const
{
    // is_valid() first, for the same reason as fold_integer: an unrecorded type reads as an error.
    if( !id.is_valid() || ( id.v < types_.size() && types_[id.v].is_valid() && table_.is_error( types_[id.v] ) ) )
    {
        return std::nullopt;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Float_literal:
    {
        const Literal_id value { ast_.aux( id ) };

        return value.is_valid() ? std::optional<f64>( literals_.floating( value ) ) : std::nullopt;
    }

    case Node_kind::Unary_expr:
    {
        const std::optional<f64> operand = fold_float( ast_.children( id )[0] );

        if( !operand )
        {
            return std::nullopt;
        }

        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Minus:
            return -*operand;

        case Token_kind::Plus:
            return operand;

        default:
            return std::nullopt;
        }
    }

    case Node_kind::Binary_expr:
    {
        const std::optional<f64> left  = fold_float( ast_.children( id )[0] );
        const std::optional<f64> right = fold_float( ast_.children( id )[1] );

        if( !left || !right )
        {
            return std::nullopt;
        }

        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Plus:
            return *left + *right;

        case Token_kind::Minus:
            return *left - *right;

        case Token_kind::Star:
            return *left * *right;

        case Token_kind::Slash:
            return *right == 0.0 ? std::nullopt : std::optional<f64>( *left / *right );

        default:
            return std::nullopt;
        }
    }

    default:
        return std::nullopt;
    }
}

bool Checker::check_constant( Node_id id, Type_id type )
{
    const bool integer  = table_.is_integer( type );
    const bool floating = table_.is_float( type );

    if( !integer && !floating )
    {
        return true; // bool, pointers, structs: nothing here can overflow
    }

    // These two are about the operator rather than the value, so they apply even when what is
    // being divided or shifted is not itself a constant.
    if( ast_.kind( id ) == Node_kind::Binary_expr )
    {
        const Token_kind op    = static_cast<Token_kind>( ast_.aux( id ) );
        const Node_id    right = ast_.children( id )[1];

        if( op == Token_kind::Slash || op == Token_kind::Percent )
        {
            const Folded divisor = fold_integer( right );

            const bool zero = integer ? ( divisor.constant && !divisor.overflowed && divisor.value.magnitude == 0 )
                                      : ( fold_float( right ).value_or( 1.0 ) == 0.0 );

            if( zero )
            {
                error_at( ast_.span( id ), op == Token_kind::Percent ? "remainder by zero" : "division by zero" );
                return false;
            }
        }

        if( op == Token_kind::Less_less || op == Token_kind::Greater_greater )
        {
            const Folded count = fold_integer( right );
            const u8     width = table_.get( type ).width;

            if( count.constant && ( count.overflowed || count.value.negative || count.value.magnitude >= width ) )
            {
                error_at(
                    ast_.span( right ),
                    "the shift count is out of range",
                    fmt::format(
                        "`{}` is {} bits wide, so the count must be between 0 and {}", table_.name( type ), width, width - 1
                    )
                );

                return false;
            }
        }
    }

    if( floating )
    {
        const std::optional<f64> value = fold_float( id );

        // An infinity from finite operands is an overflow, and f64 has no range check of its own
        // to catch it.
        if( value && ( !std::isfinite( *value ) || !table_.fits_float( *value, type ) ) )
        {
            error_at( ast_.span( id ), fmt::format( "`{}` does not fit in `{}`", *value, table_.name( type ) ) );
            return false;
        }

        return true;
    }

    // The type is passed in: this runs before the node's own is recorded, and the operators that
    // need a width would otherwise fold to nothing exactly where overflow is being checked.
    const Folded value = fold_integer( id, type );

    if( !value.constant )
    {
        return true;
    }

    if( value.overflowed )
    {
        error_at( ast_.span( id ), fmt::format( "this constant does not fit in `{}`", table_.name( type ) ) );
        return false;
    }

    if( !table_.fits( value.value.magnitude, value.value.negative, type ) )
    {
        error_at(
            ast_.span( id ),
            fmt::format(
                "`{}{}` does not fit in `{}`", value.value.negative ? "-" : "", value.value.magnitude, table_.name( type )
            )
        );

        return false;
    }

    return true;
}

// Recording the error type on a rejected constant is what stops the enclosing operation folding
// through it and reporting the same mistake again.
Type_id Checker::record_constant( Node_id id, Type_id type )
{
    if( !check_constant( id, type ) )
    {
        return record( id, table_.builtin( Type_kind::Error ) );
    }

    return record( id, type );
}

// A literal is skipped: there is nothing inside one to be wrong, and its only complaint is that
// nothing told it what type to be - which is exactly what the error that got us here already
// explains. Inferring it anyway is how one mistake produces two diagnostics.
void Checker::absorb( Node_id id )
{
    if( id.is_valid() && !is_literal_expression( id ) )
    {
        infer( id );
    }
}

Type_id Checker::record( Node_id id, Type_id type )
{
    types_[id.v] = type;
    return type;
}

void Checker::error_at( Span span, std::string message, std::string help )
{
    diags_.error( span, std::move( message ), std::move( help ) );
}

} // namespace

Types type_check(
    const Ast&            ast,
    const Resolution&     resolution,
    const Literals&       literals,
    const Source_manager& sm, // not needed yet; kept so the pass signatures match
    const Interner&       interner,
    Diagnostics&          diags
)
{
    return Checker( ast, interner, resolution, sm, literals, diags ).run();
}

Type_id parameter_type( const Ast& ast, const Types& types, Node_id param )
{
    return is_ref_parameter( ast, param ) ? types.type_of( ast.children( param )[0] ) : types.type_of( param );
}

bool is_ref_parameter( const Ast& ast, Node_id param )
{
    const Node_id annotation = ast.children( param )[0];

    return ast.kind( annotation ) == Node_kind::Mode_type && static_cast<Keyword>( ast.aux( annotation ) ) == Keyword::Ref;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include "common/interner.h"
#include "lex/lexer.h"
#include "parse/parser.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

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
        ast_           = parse( lex( file_, sm_, interner_, literals_, diags_ ), sm_, diags_ );
        const auto res = resolve( ast_, sm_, interner_, diags_ );
        earlier_       = diags_.error_count();
        types_         = type_check( ast_, res, literals_, sm_, interner_, diags_ );
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

    const Types& types() const
    {
        return types_;
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
    Literals       literals_;
    Diagnostics    diags_;
    File_id        file_;
    Ast            ast_;
    Types          types_;
    std::size_t    earlier_ = 0;
};

} // namespace

TEST_CASE( "type_checker_accepts_a_well_typed_program", "[sema][types]" )
{
    const Typed p( "i32 add( i32 a, i32 b )\n"
                   "{\n"
                   "    i32 total = a + b;\n"
                   "    return total;\n"
                   "}\n"
                   "\n"
                   "i32 main()\n"
                   "{\n"
                   "    return add( 1, 2 );\n"
                   "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// The whole point of bidirectional checking (L5): a literal has a value, not a type, until
// something tells it what to be. Neither of these needs a suffix.
TEST_CASE( "type_checker_takes_a_literal_type_from_context", "[sema][types]" )
{
    SECTION( "an annotation supplies it" )
    {
        const Typed p( "i32 main() { u32 x = 42; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u32" );
    }

    SECTION( "so does a wider one" )
    {
        const Typed p( "i32 main() { u64 x = 42; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u64" );
    }

    // Pins the pending D-entry on the default type of an unsuffixed literal.
    SECTION( "with no context it falls back to i32" )
    {
        const Typed p( "i32 main() { auto x = 42; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "i32" );
    }

    SECTION( "a float literal likewise" )
    {
        const Typed p( "i32 main() { f32 x = 1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Float_literal, 0 ) ) == "f32" );
    }
}

TEST_CASE( "type_checker_rejects_a_literal_that_does_not_fit", "[sema][types]" )
{
    static const char* sources[] = {
        "i32 main() { u8 x = 300; return 0; }",
        "i32 main() { i8 x = 200; return 0; }",
        "i32 main() { u32 x = 4294967296; return 0; }",
        "i32 main() { u8 x = -1; return 0; }", // negative never fits an unsigned type
    };

    for( const char* source : sources )
    {
        const Typed p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

TEST_CASE( "type_checker_types_a_name_from_its_declaration", "[sema][types]" )
{
    const Typed p( "i32 f( u16 p )\n"
                   "{\n"
                   "    u64 local = 1;\n"
                   "    return 0;\n"
                   "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 0 ) ) == "u16" );
    REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 0 ) ) == "u64" );
}

// D5 / §6.4 reaching the surface. The table itself is tested in type.cpp; these check that the
// checker consults it, and on the right pair of types.
TEST_CASE( "type_checker_applies_the_conversion_table", "[sema][types]" )
{
    SECTION( "lossless widening needs no cast" )
    {
        const Typed p( "i64 f( i32 a, i64 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "i64" );
    }

    SECTION( "a strictly wider signed type absorbs an unsigned one" )
    {
        const Typed p( "i32 f( i32 a, u8 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "i32" );
    }

    SECTION( "equal-rank mixed signedness is rejected - the case C++ gets wrong" )
    {
        const Typed p( "i64 f( i32 a, u32 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "so is a float that cannot hold the integer exactly" )
    {
        const Typed p( "f64 f( i64 a, f64 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "same-type arithmetic stays in the type" )
    {
        const Typed p( "u8 f( u8 a, u8 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "u8" );
    }
}

// A comparison yields bool, but its operands still have to agree - `i32 < u32` is the original
// bug D5 exists to catch, and it must not slip through just because the result is a bool.
TEST_CASE( "type_checker_types_comparisons_as_bool", "[sema][types]" )
{
    SECTION( "the result is bool, not the operand type" )
    {
        const Typed p( "bool f( i32 a, i32 b ) { return a < b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "bool" );
    }

    SECTION( "the operands are still subject to the table" )
    {
        const Typed p( "bool f( i32 a, u32 b ) { return a < b; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// D5 forbids int -> bool, so C's truthiness is gone. This is the sharpest break from C++ habit.
TEST_CASE( "type_checker_requires_a_bool_condition", "[sema][types]" )
{
    static const char* rejected[] = {
        "i32 f( i32 n ) { if( n ) { return 1; } return 0; }",
        "i32 f( i32 n ) { while( n ) { return 1; } return 0; }",
        "i32 f( i32 n ) { for( ; n; ) { return 1; } return 0; }",
    };

    for( const char* source : rejected )
    {
        const Typed p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    const Typed ok( "i32 f( i32 n ) { if( n != 0 ) { return 1; } return 0; }" );
    INFO( ok.rendered() );
    REQUIRE( ok.clean() );
}

TEST_CASE( "type_checker_checks_return_against_the_signature", "[sema][types]" )
{
    SECTION( "a lossy return is rejected" )
    {
        const Typed p( "u8 f( i32 n ) { return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "a widening return is fine" )
    {
        const Typed p( "i64 f( i32 n ) { return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a literal return takes the signature's type" )
    {
        const Typed p( "u64 f() { return 7; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u64" );
    }
}

TEST_CASE( "type_checker_checks_call_arguments", "[sema][types]" )
{
    SECTION( "a lossy argument is rejected" )
    {
        const Typed p( "i32 g( u8 x ) { return 0; }\ni32 main() { i32 big = 300; return g( big ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "too few arguments" )
    {
        const Typed p( "i32 g( i32 a, i32 b ) { return 0; }\ni32 main() { return g( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "too many arguments" )
    {
        const Typed p( "i32 g( i32 a ) { return 0; }\ni32 main() { return g( 1, 2 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "a call is typed as the return type, and literals adopt the parameter type" )
    {
        const Typed p( "u64 g( u64 x ) { return x; }\ni32 main() { u64 v = g( 7 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Call_expr, 0 ) ) == "u64" );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u64" );
    }
}

// The error type must absorb, the way Node_kind::Error does in the parser: one unresolved name
// produces one diagnostic, not one per enclosing expression.
TEST_CASE( "type_checker_does_not_cascade_from_an_error", "[sema][types]" )
{
    SECTION( "an unknown name is the resolver's error alone" )
    {
        const Typed p( "i32 main() { return unknown + 1 * 2; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "an unknown type is reported once" )
    {
        const Typed p( "i32 main() { Widget w = 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and a parse error does not reach the checker at all" )
    {
        const Typed p( "i32 main() { return 1 +; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

// D1: the rejected C++ spellings are ordinary identifiers to the lexer, and sema is what knows
// they were used in type position.
TEST_CASE( "type_checker_suggests_the_keel_spelling_for_a_c_type", "[sema][types]" )
{
    const Typed p( "i32 main() { int x = 1; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "i32" ) != std::string::npos );
}

TEST_CASE( "type_checker_rejects_a_lossy_assignment", "[sema][types]" )
{
    SECTION( "narrowing" )
    {
        const Typed p( "i32 main() { i64 wide = 1; i32 narrow = wide; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "re-signing" )
    {
        const Typed p( "i32 main() { i32 a = 1; u32 b = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "widening is accepted" )
    {
        const Typed p( "i32 main() { u32 a = 1; i64 b = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an assignment statement is checked like an initialiser" )
    {
        const Typed p( "i32 main() { u8 small = 1; i32 wide = 1; small = wide; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// D1's promise is that a C++ spelling is met with the Keel one, not a bare "unknown type".
TEST_CASE( "d1_suggests_a_keel_spelling_for_every_rejected_c_type", "[sema][types]" )
{
    struct Case
    {
        std::string_view cpp;
        std::string_view keel;
    };

    static const Case cases[] = {
        { "int", "i32" },
        { "unsigned", "u32" },
        { "short", "i16" },
        { "long", "i64" },
        { "char", "i8" },
        { "float", "f32" },
        { "double", "f64" },
        { "size_t", "u64" },
        { "uint8_t", "u8" },
        { "int64_t", "i64" },
    };

    for( const Case& c : cases )
    {
        INFO( c.cpp );
        REQUIRE( keel_spelling_for( c.cpp ) == c.keel );
    }

    SECTION( "and stays quiet when there is nothing to suggest" )
    {
        REQUIRE( keel_spelling_for( "Widget" ).empty() );
        REQUIRE( keel_spelling_for( "" ).empty() );

        // Already Keel spellings: these never reach the suggestion path, but suggesting a type
        // as a replacement for itself would be a bug worth catching if they did.
        REQUIRE( keel_spelling_for( "i32" ).empty() );
        REQUIRE( keel_spelling_for( "bool" ).empty() );
    }
}

// Every other test puts calls on the right of a `return`, which is how an entire category went
// unchecked: visit's default recursed with visit, so a statement-position expression never
// reached infer at all.
TEST_CASE( "type_checker_checks_expressions_in_statement_position", "[sema][types]" )
{
    SECTION( "a call as a statement is checked exactly like one in a return" )
    {
        const Typed as_statement( "i32 f( i32 a ) { return a; }\ni32 main() { f( 1, 2, 3 ); return 0; }\n" );
        const Typed as_value( "i32 f( i32 a ) { return a; }\ni32 main() { return f( 1, 2, 3 ); }\n" );

        INFO( "statement: " << as_statement.rendered() );
        REQUIRE( as_statement.errors() >= 1 );
        REQUIRE( as_statement.errors() == as_value.errors() );
    }

    SECTION( "and so is one inside a loop body" )
    {
        const Typed p( "i32 f( i32 a ) { return a; }\ni32 main() { while( true ) { f( 1, 2 ); } return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

TEST_CASE( "type_checker_checks_increment", "[sema][types]" )
{
    SECTION( "a numeric variable is fine" )
    {
        const Typed p( "i32 main() { i32 i = 0; i++; i--; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a parameter may be incremented" )
    {
        const Typed p( "i32 f( i32 n ) { n++; return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a bool is not numeric" )
    {
        const Typed p( "i32 main() { bool b = true; b++; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Reported before the operand is typed: a literal types as an error today, and absorbing on
    // that would let this pass in silence.
    SECTION( "the operand must be a variable" )
    {
        const Typed p( "i32 main() { 1++; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

TEST_CASE( "type_checker_rejects_assignment_to_a_non_variable", "[sema][types]" )
{
    SECTION( "a literal target" )
    {
        const Typed p( "i32 main() { 1 = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // The three targets below are all unassignable, but two of them are somebody else's error to
    // report. Counting exactly is what keeps the duplicates out.
    SECTION( "a function target reports once, as a function" )
    {
        const Typed p( "i32 f() { return 1; }\ni32 main() { f = 1; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "is a function" ) != std::string::npos );
    }

    SECTION( "an unknown target is the resolver's error alone" )
    {
        const Typed p( "i32 main() { unknown = 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "a parameter is assignable" )
    {
        const Typed p( "i32 f( i32 a ) { a = 1; return a; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D20: a code point is a u8, so it goes through exactly the machinery integer literals do.
TEST_CASE( "type_checker_types_a_character_literal_as_an_integer", "[sema][types]" )
{
    SECTION( "it adopts the annotated type, like any literal" )
    {
        const Typed p( "i32 main() { u8 c = 'A'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Char_literal, 0 ) ) == "u8" );
    }

    SECTION( "a wider integer works too" )
    {
        const Typed p( "i32 main() { u32 c = 'z'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Char_literal, 0 ) ) == "u32" );
    }

    SECTION( "with no context it falls back to u8" )
    {
        const Typed p( "i32 main() { auto c = 'A'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Char_literal, 0 ) ) == "u8" );
    }

    SECTION( "an escape past 127 still fits a u8" )
    {
        const Typed p( "i32 main() { u8 c = '\\xFF'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "but a bool is not an integer" )
    {
        const Typed p( "i32 main() { bool c = 'a'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "character literal" ) != std::string::npos );
    }

    SECTION( "and arithmetic on one is arithmetic on a u8" )
    {
        const Typed p( "u8 f( u8 c ) { return c; }\ni32 main() { u8 x = f( 'a' ); return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A string has no type until String exists (M7), so it must say so rather than vanish.
TEST_CASE( "type_checker_rejects_a_string_literal", "[sema][types]" )
{
    const Typed p( "i32 main() { u8 s = \"hello\"; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "string literals are not supported yet" ) != std::string::npos );
}

// A compound assignment is `x = x op y`, so the value is checked against the target rather than
// inferred. Getting that wrong is invisible until something is emitted: `u8 x; x += 3;` reports
// nothing suspicious, it just settles the literal on i32 and then refuses to store it back.
TEST_CASE( "type_checker_checks_compound_assignment_against_the_target", "[sema][types]" )
{
    SECTION( "a literal adopts the target's type" )
    {
        const Typed p( "i32 main() { u8 x = 1; x += 3; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "including at the edge of the target's range" )
    {
        const Typed p( "i32 main() { u8 x = 1; x += 255; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a value that does not fit the target is still rejected" )
    {
        const Typed p( "i32 main() { u8 x = 1; x += 256; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and so is a wider variable" )
    {
        const Typed p( "i32 main() { u8 x = 1; i32 y = 2; x += y; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "widening the other way is fine" )
    {
        const Typed p( "i32 main() { i64 w = 0; i32 a = 2; w += a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a bool has no arithmetic at all" )
    {
        const Typed p( "i32 main() { bool b = true; b += 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// A literal takes its type from the operand beside it. Without this, `u32 bits; bits != 0` compares
// a u32 with an i32 - which §6.4 rejects, and which no suffix exists to write around (D13). Almost
// no unsigned code would compile.
TEST_CASE( "type_checker_lets_a_literal_adopt_the_other_operand", "[sema][types]" )
{
    SECTION( "comparison against an unsigned variable" )
    {
        const Typed p( "i32 main() { u32 bits = 1; while( bits != 0 ) { bits = bits >> 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "on either side" )
    {
        const Typed p( "i32 main() { u8 b = 1; if( 0 < b ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and in arithmetic, not just comparison" )
    {
        const Typed p( "i32 main() { u64 n = 1; n = n + 1; n = n & 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Adopting is not the same as ignoring: the value still has to fit what it adopted.
    SECTION( "a literal that does not fit the adopted type is still rejected" )
    {
        const Typed p( "i32 main() { u8 b = 1; if( b == 300 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a negative literal cannot adopt an unsigned type" )
    {
        const Typed p( "i32 main() { u32 n = 1; if( n == -1 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Two literals have nothing to adopt from, so both take their defaults.
    SECTION( "two literals still take the default type" )
    {
        const Typed p( "i32 main() { if( 1 < 2 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a mismatch between two real types is still an error" )
    {
        const Typed p( "i32 main() { i32 a = 1; u32 b = 2; if( a < b ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// The same family as adopting from the operand beside it: a literal has a value, not a type, and
// every path that can supply one has to. These two were found by writing a program, not by
// inspecting the checker.
TEST_CASE( "type_checker_pushes_the_expected_type_through_to_literals", "[sema][types]" )
{
    SECTION( "a negated float literal adopts, like a negated integer one" )
    {
        const Typed p( "i32 main() { f32 a = -1.5; f64 b = -1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Float_literal, 0 ) ) == "f32" );
    }

    // Both operands are literals, so there is nothing beside them to adopt from and the context
    // is all there is.
    SECTION( "an operation between two literals takes the context's type" )
    {
        const Typed p( "i32 main() { u8 x = 'a' + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "u8" );
    }

    SECTION( "including when the context is a float" )
    {
        const Typed p( "i32 main() { f32 x = 1 + 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Adopting is not ignoring: each operand still has to fit what it adopted.
    SECTION( "an operand that does not fit the adopted type is rejected" )
    {
        const Typed p( "i32 main() { u8 x = 'a' + 300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // A comparison yields bool whatever it is handed, so pushing the expectation into it would be
    // nonsense - the operands must still meet each other, not the context.
    SECTION( "a comparison does not take the context's type" )
    {
        const Typed p( "i32 main() { bool b = 1 < 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a non-literal operand still forces a real match" )
    {
        const Typed p( "i32 main() { i32 a = 1; u8 x = a + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// C's main returns int and the emitted shim calls it with no arguments, so anything else either
// truncates silently or generates C that will not compile - a cc error pointing at generated code
// rather than a diagnostic pointing at the program.
TEST_CASE( "type_checker_constrains_the_signature_of_main", "[sema][types]" )
{
    SECTION( "i32 and no parameters is the one accepted form" )
    {
        const Typed p( "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "another integer type is rejected" )
    {
        const Typed p( "u64 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`main` must return `i32`" ) != std::string::npos );
    }

    SECTION( "so is void" )
    {
        const Typed p( "void main() { }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "parameters are rejected too - the shim passes none" )
    {
        const Typed p( "i32 main( i32 n ) { return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "parameters" ) != std::string::npos );
    }

    SECTION( "both wrong reports both" )
    {
        const Typed p( "u8 main( i32 n ) { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }

    // The unknown type is reported by type_of_annotation; saying `main` must return i32 on top of
    // that would be two messages for one mistake.
    SECTION( "an unresolved return type reports once, as the unknown type" )
    {
        const Typed p( "Widget main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "unknown type" ) != std::string::npos );
    }

    SECTION( "the rule is about main alone" )
    {
        const Typed p( "u64 helper( i32 a, i32 b ) { return 0; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A program without one is a library. The emitter writes no shim, and nothing here objects.
    SECTION( "no main at all is not an error" )
    {
        const Typed p( "u64 helper() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_types_struct_declarations", "[sema][types]" )
{
    SECTION( "a struct is a type, and a field carries its own" )
    {
        const Typed p( "struct Point { f64 x; u32 n; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Struct_decl, 0 ) ) == "Point" );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 0 ) ) == "f64" );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 1 ) ) == "u32" );
    }

    // The field pass has to reach into each struct: fields are children of the Struct_decl, not of
    // the root, so a loop over the root's children finds none of them and a bad field type becomes
    // invisible.
    SECTION( "an unknown field type is reported" )
    {
        const Typed p( "struct Point { Widget w; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "unknown type `Widget`" ) != std::string::npos );
    }

    SECTION( "D1 applies to a field type too" )
    {
        const Typed p( "struct Point { double x; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "f64" ) != std::string::npos );
    }

    // Struct types must all exist before any field type resolves: `Line` names `Point`, which is
    // declared below it.
    SECTION( "a field may name a struct declared later" )
    {
        const Typed p( "struct Line { Point a; Point b; };\nstruct Point { f64 x; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a struct is usable in a signature" )
    {
        const Typed p( "struct Point { f64 x; };\nPoint make( Point p ) { return p; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The resolver binds the name; if it bound to something that is not a struct, that is not a
    // type however good the spelling looks.
    SECTION( "a function name is not a type" )
    {
        const Typed p( "i32 Point() { return 0; }\ni32 f( Point p ) { return 0; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// A struct containing itself by value has infinite size. C's error for this is incomprehensible,
// and the emitter's topological sort would not terminate.
TEST_CASE( "type_checker_rejects_recursive_structs", "[sema][types]" )
{
    SECTION( "directly" )
    {
        const Typed p( "struct Node { Node next; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    // Exactly one message: every struct on the cycle is marked when it is found, or the same cycle
    // is rediscovered from B and reported twice for one mistake.
    SECTION( "and through another struct, reported once" )
    {
        const Typed p( "struct A { B b; };\nstruct B { A a; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a three-struct cycle is also one message" )
    {
        const Typed p( "struct A { B b; };\nstruct B { C c; };\nstruct C { A a; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Containment is what makes it infinite. A chain that does not close is fine however deep.
    SECTION( "a deep chain that does not close is fine" )
    {
        const Typed p( "struct A { f64 x; };\nstruct B { A a; };\nstruct C { B b; };\n"
                       "struct D { C c; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a struct used twice is not a cycle" )
    {
        const Typed p( "struct Point { f64 x; };\nstruct Line { Point a; Point b; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_types_field_access", "[sema][types]" )
{
    SECTION( "a field access has the field's type" )
    {
        const Typed p( "struct Point { f64 x; u32 n; };\n"
                       "f64 get( Point p ) { return p.x; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 0 ) ) == "f64" );
    }

    SECTION( "and is checked against its context" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "u32 get( Point p ) { return p.x; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "an unknown field is reported" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "f64 get( Point p ) { return p.z; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "z" ) != std::string::npos );
    }

    SECTION( "so is a field on something that is not a struct" )
    {
        const Typed p( "i32 f( i32 n ) { return n.x; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "access chains" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "struct Line { Point a; };\n"
                       "f64 get( Line l ) { return l.a.x; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        // The inner access is built first, so nth() sees `l.a` before `l.a.x`.
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 0 ) ) == "Point" );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 1 ) ) == "f64" );
    }

    // One diagnostic for one mistake: an object that failed to type has nothing to look a field up
    // in, and saying so again would be the cascade the error type exists to prevent.
    SECTION( "a field on an unresolved name reports once" )
    {
        const Typed p( "i32 main() { auto v = missing.x; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 ); // the resolver already reported the name
    }

    // Useless - the temporary is discarded - but legal, as it is in C++. Rejecting it would need
    // value categories, which v0 does not have.
    SECTION( "a field of a temporary is still assignable" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "Point make() { return Point { 1.0 }; }\n"
                       "i32 main() { make().x = 2.0; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a field is assignable" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "f64 bump( Point p ) { p.x = 1.0; return p.x; }\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_types_struct_literals", "[sema][types]" )
{
    constexpr std::string_view point = "struct Point { f64 x; f64 y; };\n";

    SECTION( "positional initialisers match the fields in order" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { 1.0, 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Struct_literal, 0 ) ) == "Point" );
    }

    SECTION( "designated initialisers match by name" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { .x = 1.0, .y = 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Struct_literal, 0 ) ) == "Point" );
    }

    // The values are *checked*, not inferred, so a literal adopts the field's type. Inferring
    // would settle `1` on i32 and then refuse to store it in an f64 field.
    SECTION( "a value adopts the field's type" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { 1, 2 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "f64" );
    }

    SECTION( "a value that does not fit the field is rejected" )
    {
        const Typed p( "struct Small { u8 v; };\ni32 main() { auto q = Small { 300 }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and so is one of the wrong type" )
    {
        const Typed p( std::string( point ) + "i32 main() { bool b = true; auto q = Point { b, 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "too few and too many both report" )
    {
        for( const char* tail :
             { "i32 main() { auto q = Point { 1.0 }; return 0; }", "i32 main() { auto q = Point { 1.0, 2.0, 3.0 }; return 0; }"
             } )
        {
            const Typed p( std::string( point ) + tail );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() >= 1 );
        }
    }

    SECTION( "an unknown field name is reported" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { .z = 1.0, .y = 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "a repeated field name is reported" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { .x = 1.0, .x = 2.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    // Every field must be given, so adding one to a struct errors at each construction site rather
    // than silently zeroing.
    SECTION( "a missing field is reported" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { .x = 1.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    // As C++20 forbids. Two conventions in one literal is a reader's problem, not a parser's.
    SECTION( "positional and designated may not be mixed" )
    {
        const Typed p( std::string( point ) + "i32 main() { auto q = Point { 1.0, .y = 2.0 }; return 0; }" );

        INFO( p.rendered() );

        // The message, not just a count: with the mixing rule removed the literal is read as
        // named, `x` goes uninitialised, and a different error keeps the count above zero.
        REQUIRE( p.rendered().find( "all positional or all named" ) != std::string::npos );
    }

    SECTION( "an empty struct takes an empty literal" )
    {
        const Typed p( "struct Empty { };\ni32 main() { auto q = Empty { }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "literals nest" )
    {
        const Typed p( "struct Point { f64 x; };\nstruct Line { Point a; Point b; };\n"
                       "i32 main() { auto l = Line { Point { 1.0 }, Point { 2.0 } }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and can be passed, returned and assigned" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "Point echo( Point p ) { return p; }\n"
                       "i32 main() { auto a = Point { 1.0 }; auto b = echo( a ); a = b; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "but there is no arithmetic on one" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "i32 main() { auto a = Point { 1.0 }; auto b = Point { 2.0 }; auto c = a + b; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// The same rule integer literals get: a value is measured against the type it is being given.
TEST_CASE( "type_checker_range_checks_float_literals", "[sema][types]" )
{
    SECTION( "a value beyond f32's range is rejected" )
    {
        const Typed p( "i32 main() { f32 x = 1e40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit in `f32`" ) != std::string::npos );
    }

    // The sign lives in the Unary_expr, not in the recorded value, so the message has to put it
    // back or it names a number the author did not write.
    SECTION( "and the message keeps the sign" )
    {
        const Typed p( "i32 main() { f32 x = -1e40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`-1e+40`" ) != std::string::npos );
    }

    SECTION( "a value at the edge is accepted" )
    {
        const Typed p( "i32 main() { f32 x = 3.4e38; f32 y = -3.4e38; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an inexact value is accepted - range is not representability" )
    {
        const Typed p( "i32 main() { f32 x = 0.1; f32 y = 1e-50; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "f64 takes anything the lexer let through" )
    {
        const Typed p( "i32 main() { f64 x = 1e300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Reported by the lexer, which is the only place the digits still exist. Sema records no value
    // for it and must not report a second time.
    SECTION( "a value beyond f64 reports exactly once, from the lexer" )
    {
        const Typed p( "i32 main() { f64 x = 1e400; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.rendered().find( "out of range" ) != std::string::npos );
    }

    // This regressed when fits_float replaced the is_float() guard: every float-into-integer
    // assignment silently compiled.
    SECTION( "a float literal still cannot be given to an integer" )
    {
        const Typed p( "i32 main() { i32 x = 1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "floating-point literal" ) != std::string::npos );
        REQUIRE( p.rendered().find( "cannot be an integer" ) != std::string::npos );
    }

    SECTION( "nor to a bool" )
    {
        const Typed p( "i32 main() { bool b = 1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

TEST_CASE( "type_checker_types_pointers", "[sema][types]" )
{
    SECTION( "an annotation, an address and a dereference" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32* q = &v; i32 w = *q; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 1 ) ) == "i32*" );
        REQUIRE( p.type_name( p.nth( Node_kind::Unary_expr, 0 ) ) == "i32*" ); // &v
        REQUIRE( p.type_name( p.nth( Node_kind::Unary_expr, 1 ) ) == "i32" );  // *q
    }

    SECTION( "pointers nest" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32* q = &v; i32** r = &q; i32 w = **r; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 2 ) ) == "i32**" );
    }

    SECTION( "and point at structs" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "i32 main() { auto s = Point { 1.0 }; Point* q = &s; f64 v = (*q).x; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The address of a temporary names storage that is about to vanish, and a literal has no
    // storage at all.
    SECTION( "the address of something that is not a place is rejected" )
    {
        for( const char* tail : { "i32 v = &f(); return 0;", "i32 v = &1; return 0;", "i32 v = &( 1 + 2 ); return 0;" } )
        {
            const Typed p( std::string( "i32 f() { return 1; }\ni32 main() { " ) + tail + " }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() >= 1 );
            REQUIRE( p.rendered().find( "cannot take the address" ) != std::string::npos );
        }
    }

    SECTION( "the address of a field is fine - a field is a place" )
    {
        const Typed p( "struct Point { f64 x; };\n"
                       "i32 main() { auto s = Point { 1.0 }; f64* q = &s.x; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "dereferencing something that is not a pointer is rejected" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32 w = *v; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot be dereferenced" ) != std::string::npos );
    }

    SECTION( "writing through a pointer" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32* q = &v; *q = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Identity only: no void*, no conversion between pointee types.
    SECTION( "pointer types do not convert" )
    {
        const Typed p( "i32 main() { i32 v = 1; i32* q = &v; u8* r = q; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "nor to or from an integer" )
    {
        const Typed a( "i32 main() { i32 v = 1; i32* q = &v; i64 n = q; return 0; }" );
        const Typed b( "i32 main() { i64 n = 1; i32* q = n; return 0; }" );

        INFO( a.rendered() << b.rendered() );
        REQUIRE( a.errors() == 1 );
        REQUIRE( b.errors() == 1 );
    }

    // D27. Arithmetic belongs to a many-item pointer, which v0 does not have.
    SECTION( "there is no pointer arithmetic" )
    {
        for( const char* tail : { "i32* r = q + 1;", "i32* r = q - 1;", "i32 n = q * 2;" } )
        {
            const Typed p( std::string( "i32 main() { i32 v = 1; i32* q = &v; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() >= 1 );
        }
    }

    SECTION( "a pointer can be passed and returned" )
    {
        const Typed p( "i32 read( i32* q ) { return *q; }\n"
                       "i32 main() { i32 v = 1; return read( &v ); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D26: a literal whose type comes from context, exactly like an integer literal.
TEST_CASE( "type_checker_types_nullptr", "[sema][types]" )
{
    SECTION( "it adopts the annotated pointer type" )
    {
        const Typed p( "i32 main() { i32* q = nullptr; u8* r = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Null_literal, 0 ) ) == "i32*" );
        REQUIRE( p.type_name( p.nth( Node_kind::Null_literal, 1 ) ) == "u8*" );
    }

    SECTION( "with no context there is nothing to adopt" )
    {
        const Typed p( "i32 main() { auto q = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and it is not a value of any other type" )
    {
        for( const char* tail : { "i32 v = nullptr;", "bool b = nullptr;", "f64 d = nullptr;" } )
        {
            const Typed p( std::string( "i32 main() { " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    SECTION( "it can be passed to a pointer parameter" )
    {
        const Typed p( "i32 take( i32* q ) { return 0; }\ni32 main() { return take( nullptr ); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The conversion table. `cast` preserves the value and `wrap` keeps the low bits, so the two
// accept genuinely different sets - a cell that both allow, or that neither does, is the
// interesting part rather than an accident.
TEST_CASE( "type_checker_allows_the_conversions_in_the_table", "[sema][types][cast]" )
{
    SECTION( "cast widens an integer" )
    {
        const Typed p( "i32 main() { i32 x = 1; i64 y = cast<i64>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Cast_expr, 0 ) ) == "i64" );
    }

    SECTION( "cast converts an integer to a float" )
    {
        const Typed p( "i32 main() { i32 x = 1; f32 y = cast<f32>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Precision loss, but a float that cannot hold the value gives an infinity rather than a
    // plausible wrong number, so this is not the case narrowing is held back for.
    SECTION( "cast narrows a float" )
    {
        const Typed p( "i32 main() { f64 d = 1.5; f32 y = cast<f32>( d ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "cast converts a bool to an integer" )
    {
        const Typed p( "i32 main() { bool b = true; i32 y = cast<i32>( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "wrap narrows an integer" )
    {
        const Typed p( "i32 main() { i32 x = 300; u8 y = wrap<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Cast_expr, 0 ) ) == "u8" );
    }

    // Losing the sign is exactly what wrap is for, and exactly what cast must refuse.
    SECTION( "wrap reinterprets the sign" )
    {
        const Typed p( "i32 main() { i32 x = 0 - 1; u32 y = wrap<u32>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "wrap widens too, and simply never wraps" )
    {
        const Typed p( "i32 main() { i32 x = 1; i64 y = wrap<i64>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_rejects_the_conversions_outside_the_table", "[sema][types][cast]" )
{
    // No one obvious rounding, so neither operator picks one.
    SECTION( "neither converts a float to an integer" )
    {
        for( const char* tail : { "i32 y = cast<i32>( d );", "i32 y = wrap<i32>( d );" } )
        {
            const Typed p( std::string( "i32 main() { f64 d = 1.5; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "rounding" ) != std::string::npos );
        }
    }

    // `x != 0` says it better, and modular arithmetic down to one bit says something else again.
    SECTION( "neither converts an integer to a bool" )
    {
        for( const char* tail : { "bool y = cast<bool>( x );", "bool y = wrap<bool>( x );" } )
        {
            const Typed p( std::string( "i32 main() { i32 x = 1; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    SECTION( "wrap refuses anything that is not integer to integer" )
    {
        for( const char* source :
             { "i32 main() { i32 x = 1; f32 y = wrap<f32>( x ); return 0; }",
               "i32 main() { bool b = true; i32 y = wrap<i32>( b ); return 0; }",
               "i32 main() { f64 d = 1.5; f32 y = wrap<f32>( d ); return 0; }" } )
        {
            const Typed p( source );

            INFO( source << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "`wrap` cannot convert" ) != std::string::npos );
        }
    }

    SECTION( "neither touches a struct" )
    {
        for( const char* tail : { "i32 y = cast<i32>( p );", "i32 y = wrap<i32>( p );" } )
        {
            const Typed p( std::string( "struct P { i32 x; };\ni32 main() { P p = P { 1 }; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    SECTION( "converting to a struct is refused too" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { i32 x = 1; P y = cast<P>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // A real conversion, held back for the unsafe gate rather than rejected as nonsense - so it
    // gets its own message.
    SECTION( "a pointer conversion is not supported yet" )
    {
        const Typed p( "i32 main() { i32 x = 1; i32* q = &x; u8* r = cast<u8*>( q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "not supported yet" ) != std::string::npos );
    }

    SECTION( "an unknown target type is reported once" )
    {
        const Typed p( "i32 main() { i32 x = 1; return cast<Nope>( x ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// Narrowing is the one cell where the two operators disagree, and `cast` is defined to check the
// value at run time. Nothing can do that yet, so it is refused rather than silently truncating -
// which would be `wrap`'s behaviour under `cast`'s name. Delete these when the check exists.
TEST_CASE( "type_checker_holds_back_a_narrowing_cast", "[sema][types][cast]" )
{
    SECTION( "narrowing the width" )
    {
        const Typed p( "i32 main() { i32 x = 300; u8 y = cast<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot narrow" ) != std::string::npos );
        REQUIRE( p.rendered().find( "wrap<u8>" ) != std::string::npos );
    }

    // u32 cannot hold a negative i32, so this narrows even though the widths match.
    SECTION( "changing the signedness" )
    {
        const Typed p( "i32 main() { i32 x = 1; u32 y = cast<u32>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot narrow" ) != std::string::npos );
    }

    SECTION( "but wrap says the same thing and is allowed" )
    {
        const Typed p( "i32 main() { i32 x = 300; u8 y = wrap<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A literal has a value and no type, so `cast` is the context that gives it one rather than a
// conversion applied after the fact. `wrap` must not do this: taking a value the target cannot
// hold is the whole point of it.
TEST_CASE( "type_checker_gives_a_literal_its_type_from_a_cast", "[sema][types][cast]" )
{
    SECTION( "an in-range literal simply becomes the target type" )
    {
        const Typed p( "i32 main() { u8 y = cast<u8>( 200 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u8" );
    }

    // Not "cast cannot narrow": there is nothing to narrow, the literal never had a wider type.
    SECTION( "an out-of-range literal is a range error" )
    {
        const Typed p( "i32 main() { u8 y = cast<u8>( 300 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit" ) != std::string::npos );
    }

    SECTION( "an integer literal can be cast to a float" )
    {
        const Typed p( "i32 main() { f32 y = cast<f32>( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "f32" );
    }

    SECTION( "a negated literal keeps the asymmetric range" )
    {
        const Typed p( "i32 main() { i8 y = cast<i8>( -128 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The literal is inferred first, then wrapped - so this is 44, not an error.
    SECTION( "wrap takes the literal at its default type instead" )
    {
        const Typed p( "i32 main() { u8 y = wrap<u8>( 300 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "i32" );
    }

    SECTION( "a float literal cannot be cast to an integer either" )
    {
        const Typed p( "i32 main() { i32 y = cast<i32>( 1.5 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// The result has a type of its own, so it composes like any other expression - and must not be
// mistaken for a literal by the code that gives literals their type from context.
TEST_CASE( "type_checker_uses_a_conversion_as_an_ordinary_expression", "[sema][types][cast]" )
{
    SECTION( "as an operand" )
    {
        const Typed p( "i32 main() { i32 x = 1; i64 y = cast<i64>( x ) + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "i64" );
    }

    SECTION( "as an argument" )
    {
        const Typed p( "i32 take( u8 v ) { return 0; }\n"
                       "i32 main() { i32 x = 300; return take( wrap<u8>( x ) ); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The result is a value, not a place, so it cannot be assigned through.
    SECTION( "but it is not assignable" )
    {
        const Typed p( "i32 main() { i32 x = 1; cast<i64>( x ) = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot assign to this expression" ) != std::string::npos );
    }

    SECTION( "the result type is what the annotation says, not the operand's" )
    {
        const Typed p( "i32 main() { i32 x = 1; u8 y = wrap<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Cast_expr, 0 ) ) == "u8" );
    }
}

// One mistake, one diagnostic. A literal has a value and no type, so in a context that has already
// failed there is nothing for it to adopt from - and asking it anyway makes it report that, on top
// of the error that is the actual cause. `nullptr` is the one that shows this, because it is the
// only literal with no default type to fall back on.
TEST_CASE( "type_checker_absorbs_a_literal_beside_a_failed_operand", "[sema][types]" )
{
    SECTION( "comparison, literal on the right" )
    {
        const Typed p( "i32 main() { if ( nope() != nullptr ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
        REQUIRE( p.rendered().find( "`nope` is not declared" ) != std::string::npos );
    }

    // The operand order must not change which diagnostics appear.
    SECTION( "comparison, literal on the left" )
    {
        const Typed p( "i32 main() { if ( nullptr != nope() ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
    }

    SECTION( "assigning to something that is not a place" )
    {
        const Typed p( "i32* get( i32* q ) { return q; }\n"
                       "i32 main() { i32 x = 1; get( &x ) = nullptr; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot assign to this expression" ) != std::string::npos );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
    }

    SECTION( "assigning to a name that does not resolve" )
    {
        const Typed p( "i32 main() { nope = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
    }

    SECTION( "returning a value from a void function" )
    {
        const Typed p( "void f() { return nullptr; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );
    }

    SECTION( "an initialiser for a field that does not exist" )
    {
        const Typed p( "struct P { i32* q; };\ni32 main() { auto p = P { .bad = nullptr }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "cannot infer type of" ) == std::string::npos );

        // Two errors, but two *facts*: the name is wrong, and so the real field went uninitialised.
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "has no field `bad`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "field `q` is not initialised" ) != std::string::npos );
    }
}

// The other half of absorbing: a literal that genuinely has nothing to adopt from still has to say
// so, and one that is simply out of range must not be waved through. Absorbing too eagerly would
// silence both.
TEST_CASE( "type_checker_still_reports_a_literal_with_no_context", "[sema][types]" )
{
    SECTION( "auto has nothing to give a null literal" )
    {
        const Typed p( "i32 main() { auto p = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot infer type of `nullptr`" ) != std::string::npos );
    }

    SECTION( "a null literal against a real type is still wrong" )
    {
        const Typed p( "i32 main() { i32 v = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "range checking survives" )
    {
        const Typed p( "i32 main() { u8 x = 300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit" ) != std::string::npos );
    }

    SECTION( "a literal target is still not assignable" )
    {
        const Typed p( "i32 main() { 1 = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot assign to this expression" ) != std::string::npos );
    }

    // The reason infer_binary pushes the expectation at all: without it these compare a u32 or a u8
    // against an i32, which §6.4 rejects and no suffix can write around.
    SECTION( "and a literal beside a good operand still adopts from it" )
    {
        const Typed a( "i32 main() { u32 bits = 3; if ( bits != 0 ) { return 1; } return 0; }" );
        const Typed b( "i32 main() { u8 i = 97 + 1; return 0; }" );

        INFO( a.rendered() << b.rendered() );
        REQUIRE( a.clean() );
        REQUIRE( b.clean() );
    }
}

// `break` and `continue` are only meaningful inside a loop, and the depth has to be scoped to the
// *body*: visit()'s default recurses into children rather than asserting, so a missing case here
// would let `break;` compile anywhere at all rather than failing loudly.
TEST_CASE( "type_checker_accepts_break_and_continue_inside_a_loop", "[sema][types][loops]" )
{
    SECTION( "directly in a while" )
    {
        const Typed p( "i32 main() { while ( true ) { break; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "directly in a for" )
    {
        const Typed p( "i32 main() { for ( i32 i = 0; i < 3; i++ ) { continue; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The case a naive check misses: the enclosing statement is an if, not a loop.
    SECTION( "nested inside an if inside a loop" )
    {
        const Typed p( "i32 main() { while ( true ) { if ( true ) { break; } } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "in a block inside a loop" )
    {
        const Typed p( "i32 main() { while ( true ) { { continue; } } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "nested loops" )
    {
        const Typed p( "i32 main() { while ( true ) { for ( i32 i = 0; i < 3; i++ ) { continue; } break; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_rejects_break_and_continue_outside_a_loop", "[sema][types][loops]" )
{
    SECTION( "at the top of a function" )
    {
        const Typed b( "i32 main() { break; return 0; }" );
        const Typed c( "i32 main() { continue; return 0; }" );

        INFO( b.rendered() << c.rendered() );
        REQUIRE( b.errors() == 1 );
        REQUIRE( c.errors() == 1 );
        REQUIRE( b.rendered().find( "`break` outside a loop" ) != std::string::npos );
        REQUIRE( c.rendered().find( "`continue` outside a loop" ) != std::string::npos );
    }

    SECTION( "in an if that is not inside a loop" )
    {
        const Typed p( "i32 main() { if ( true ) { break; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "in a bare block" )
    {
        const Typed p( "i32 main() { { break; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // The depth has to come back down, or everything after a loop is wrongly inside one.
    SECTION( "after the loop has closed" )
    {
        const Typed p( "i32 main() { while ( true ) { break; } break; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "after a nested loop has closed" )
    {
        const Typed p( "i32 main() { while ( true ) { for ( i32 i = 0; i < 3; i++ ) { break; } } continue; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Every loop in a chain must restore the depth, not just the last one.
    SECTION( "after several loops in sequence" )
    {
        const Typed p( "i32 main() {\n"
                       "  while ( true ) { break; }\n"
                       "  for ( i32 i = 0; i < 3; i++ ) { break; }\n"
                       "  while ( true ) { break; }\n"
                       "  break;\n"
                       "  return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// A file-scope initialiser must be a literal. Not because anything cannot be computed, but because
// there is no constant folder: the restriction is the honest statement of what the compiler can
// actually evaluate, and widening it later breaks nothing that compiles under it.
//
// It also removes two problems outright. A global cannot name another global, so initialisation
// order never exists as a question; and no global can hold a type with a destructor, so M3 never
// has to sequence global teardown.
TEST_CASE( "type_checker_accepts_a_literal_initialised_global", "[sema][types][globals]" )
{
    SECTION( "an integer, taking its type from the annotation" )
    {
        const Typed p( "u8 small = 200;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u8" );
    }

    SECTION( "the other literal kinds" )
    {
        for( const char* head : { "f64 ratio = 1.5;", "bool ready = true;", "u8 letter = 'a';", "i32* address = nullptr;" } )
        {
            const Typed p( std::string( head ) + "\ni32 main() { return 0; }\n" );

            INFO( head << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    SECTION( "a negated literal" )
    {
        const Typed p( "i32 below = -1;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "with no initialiser at all" )
    {
        const Typed p( "i32 counter;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The type has to be recorded before any body is checked, or a function using a global sees
    // nothing there.
    SECTION( "a function may use it, and gets its type" )
    {
        const Typed p( "u8 small = 200;\nu8 read() { return small; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Name_expr, 0 ) ) == "u8" );
    }

    SECTION( "including one written above it" )
    {
        const Typed p( "u8 read() { return small; }\nu8 small = 200;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "range checking still applies" )
    {
        const Typed p( "u8 small = 300;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit" ) != std::string::npos );
    }

    SECTION( "and so does the type of the literal" )
    {
        const Typed p( "i32 whole = 1.5;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// The rule is what the compiler can actually evaluate, and since the constant folder landed that
// is more than a bare literal. C accepts arithmetic constant expressions at file scope too, so the
// emitter can print the expression rather than needing a value computed for it.
TEST_CASE( "type_checker_accepts_a_constant_expression_global", "[sema][types][globals][constants]" )
{
    SECTION( "arithmetic" )
    {
        const Typed p( "i32 limit = 60 * 60;\ni32 main() { return limit; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the operators that fold, and the ones that cannot overflow" )
    {
        for( const char* head :
             { "i32 a = 1 + 2 * 3;",
               "i32 b = ( 1 + 2 ) * 3;",
               "u8  c = 255 & 15;",
               "u32 d = 1 << 4;",
               "i32 e = ~0;",
               "i32 f = -( 3 * 3 );",
               "i32 g = 7 % 3;",
               "f64 h = 1.5 * 2.0;",
               "bool i = 1 < 2;" } )
        {
            const Typed p( std::string( head ) + "\ni32 main() { return 0; }\n" );

            INFO( head << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // The idiom for a deliberately wrapped constant, now writable where it is most wanted.
    SECTION( "a wrap, which is how an all-ones mask is spelled" )
    {
        const Typed p( "u32 mask = wrap<u32>( 0 - 1 );\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Constant rejection applies at file scope exactly as it does anywhere else.
    SECTION( "and it is still measured against the type" )
    {
        for( const char* head : { "u8 over = 200 + 100;", "i32 wide = 2000000000 * 2;", "i32 bad = 1 / 0;" } )
        {
            const Typed p( std::string( head ) + "\ni32 main() { return 0; }\n" );

            INFO( head << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    // Short-circuit operators emit control flow, which file scope has nowhere to put - so they are
    // outside the rule however constant their operands are.
    SECTION( "but not the short-circuit operators" )
    {
        const Typed p( "bool ready = true && false;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

TEST_CASE( "type_checker_rejects_a_computed_global_initialiser", "[sema][types][globals]" )
{
    SECTION( "a call" )
    {
        const Typed p( "i32 make() { return 1; }\ni32 total = make();\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // This is the one the rule exists for: allowing it is the static initialisation order fiasco.
    SECTION( "another global" )
    {
        const Typed p( "i32 first = 1;\ni32 second = first;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "the address of another global" )
    {
        const Typed p( "i32 first = 1;\ni32* second = &first;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Not a constant-folding question: a struct literal lowers to a temporary and field
    // assignments, which cannot appear at C file scope.
    SECTION( "a struct literal" )
    {
        const Typed p( "struct Point { i32 x; };\nPoint origin = Point { 0 };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "and a struct-typed global at all" )
    {
        const Typed p( "struct Point { i32 x; };\nPoint origin;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// §12 decided that arithmetic *wraps* at run time. That leaves the constant case, where the answer
// is known and wrapping it is a wrong answer delivered in silence - so a constant that does not fit
// the type its operation happens in is rejected. The value is folded only to decide whether to
// complain; nothing about what gets emitted changes.
TEST_CASE( "type_checker_rejects_constant_overflow", "[sema][types][constants]" )
{
    SECTION( "unsigned subtraction below zero" )
    {
        const Typed p( "i32 main() { u32 d = 1 - 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`-1` does not fit in `u32`" ) != std::string::npos );
    }

    SECTION( "addition past the top" )
    {
        const Typed p( "i32 main() { u8 d = 200 + 100; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`300` does not fit in `u8`" ) != std::string::npos );
    }

    SECTION( "multiplication past the top" )
    {
        const Typed p( "i32 main() { i32 d = 2000000000 * 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`4000000000` does not fit in `i32`" ) != std::string::npos );
    }

    SECTION( "and below the bottom of a signed type" )
    {
        const Typed p( "i32 main() { i8 d = 0 - 200; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // The value has to be measured at every step, not only at the end: this one fits an i32
    // comfortably, and the addition on the way does not. §6.4 says the operation happens in the
    // common type, so that is the type each step is measured against.
    SECTION( "an intermediate result that does not fit" )
    {
        const Typed p( "i32 main() { i32 d = 2000000000 + 2000000000 - 2000000000; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`4000000000` does not fit in `i32`" ) != std::string::npos );
    }

    SECTION( "a negated constant" )
    {
        const Typed p( "i32 main() { u8 d = -( 1 + 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a float constant that overflows to infinity" )
    {
        const Typed p( "i32 main() { f32 d = 1e30 * 1e30; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit in `f32`" ) != std::string::npos );
    }
}

// Undefined behaviour in C rather than merely a wrong answer, and trivially known here.
TEST_CASE( "type_checker_rejects_a_constant_divide_by_zero", "[sema][types][constants]" )
{
    SECTION( "division" )
    {
        const Typed p( "i32 main() { i32 d = 1 / 0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "division by zero" ) != std::string::npos );
    }

    SECTION( "remainder" )
    {
        const Typed p( "i32 main() { i32 d = 1 % 0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "remainder by zero" ) != std::string::npos );
    }

    SECTION( "a divisor that computes to zero" )
    {
        const Typed p( "i32 main() { i32 d = 1 / ( 3 - 3 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // IEEE would make this an infinity rather than undefined, but v0 has no way to write an
    // infinity deliberately, so a constant zero divisor is a mistake whatever the type. One rule
    // rather than an exception that exists only to admit a value nothing can name.
    SECTION( "float division by zero as well" )
    {
        const Typed p( "i32 main() { f64 d = 1.0 / 0.0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "division by zero" ) != std::string::npos );
    }

    SECTION( "and a runtime divisor is not the checker's business" )
    {
        const Typed p( "i32 main() { i32 z = 0; i32 d = 1 / z; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Shifting by the width or more has no defined result in C. The count is usually a constant, so
// this is usually knowable.
TEST_CASE( "type_checker_rejects_a_constant_shift_past_the_width", "[sema][types][constants]" )
{
    SECTION( "wider than the type" )
    {
        const Typed p( "i32 main() { i32 d = 1 << 40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "shift" ) != std::string::npos );
    }

    SECTION( "exactly the width" )
    {
        const Typed p( "i32 main() { u32 d = 1 << 32; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a negative count" )
    {
        const Typed p( "i32 main() { i32 d = 1 << -1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "one less than the width is fine" )
    {
        const Typed p( "i32 main() { u32 d = 1 << 31; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // 1 << 31 does not fit a signed i32, which is the overflow rule rather than the shift rule.
    SECTION( "but it must still fit the type" )
    {
        const Typed p( "i32 main() { i32 d = 1 << 31; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a runtime count is not rejected" )
    {
        const Typed p( "i32 main() { u32 n = 40; u32 d = 1 << n; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The divisor and the count are constants even when the value being divided or shifted is not, and
// these are the cases worth catching most: a mistake in an expression that otherwise looks like
// ordinary running code. They reach infer_binary rather than check()'s literal branches, which is a
// separate path through the same rule.
TEST_CASE( "type_checker_rejects_a_constant_divisor_of_a_runtime_value", "[sema][types][constants]" )
{
    SECTION( "division" )
    {
        const Typed p( "i32 main() { i32 f = 3; i32 d = f / 0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "division by zero" ) != std::string::npos );
    }

    SECTION( "remainder" )
    {
        const Typed p( "i32 main() { u32 f = 3; u32 d = f % 0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "remainder by zero" ) != std::string::npos );
    }

    SECTION( "a shift count past the width" )
    {
        const Typed p( "i32 main() { u32 f = 3; u32 d = f << 40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "shift count" ) != std::string::npos );
    }

    // The width is the *left* operand's, so a narrow type has a correspondingly small limit.
    SECTION( "measured against the type being shifted, not i32" )
    {
        const Typed p( "i32 main() { u8 f = 3; u8 d = f << 9; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and a count inside the width is fine" )
    {
        const Typed p( "i32 main() { u32 f = 3; u32 d = f << 4; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A shift's result type is its *left* operand's (§6.4), so that is where an expectation has to
// flow. Without this `u32 d = 1 << 4;` combines an i32 with a u32 and is refused - the fourth
// instance of inferring where checking belonged.
TEST_CASE( "type_checker_pushes_an_expectation_through_a_shift", "[sema][types][constants]" )
{
    SECTION( "the left operand adopts the target type" )
    {
        const Typed p( "i32 main() { u32 d = 1 << 4; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u32" );
    }

    SECTION( "for narrow types too" )
    {
        const Typed p( "i32 main() { u8 d = 1 << 4; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The count is independent of the shifted type, so it keeps inferring on its own.
    SECTION( "the count is not forced to the same type" )
    {
        const Typed p( "i32 main() { u8 n = 4; u8 d = 1 << n; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a right shift behaves the same" )
    {
        const Typed p( "i32 main() { u32 d = 256 >> 4; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The rule must not reach past constants, and must not refuse arithmetic that is simply correct.
TEST_CASE( "type_checker_allows_constants_that_fit", "[sema][types][constants]" )
{
    SECTION( "ordinary arithmetic" )
    {
        for( const char* body :
             { "i32 d = 2 + 3;",
               "u8 d = 200 + 55;",
               "i32 d = 1 - 2;",
               "i32 d = 6 / 3;",
               "i32 d = 7 % 3;",
               "u8 d = 255 & 15;",
               "i32 d = ~0;",
               "i32 d = -2147483648;",
               "u64 d = 4294967295 * 2;" } )
        {
            const Typed p( std::string( "i32 main() { " ) + body + " return 0; }" );

            INFO( body << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // A value that leaves u64 entirely is an overflow of every Keel type, so it is still refused -
    // the folder running out of room is not a reason to stay quiet.
    SECTION( "past what u64 can hold" )
    {
        const Typed p( "i32 main() { u64 d = 18446744073709551615 * 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "nothing involving a variable is folded" )
    {
        for( const char* body :
             { "u8 a = 200; u8 d = a + a;", "i32 a = 2000000000; i32 d = a * 2;", "u32 a = 1; u32 d = a - 2;" } )
        {
            const Typed p( std::string( "i32 main() { " ) + body + " return 0; }" );

            INFO( body << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // `wrap` infers its operand rather than pushing the target type in, so the fold happens in i32
    // where -1 fits. That is the opt-out for deliberate wrapping, and it falls out of D28 rather
    // than being a special case here.
    SECTION( "wrap is the way to ask for it deliberately" )
    {
        const Typed p( "i32 main() { u32 mask = wrap<u32>( 0 - 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // cast pushes the type in, so the constant is measured against u32 and refused.
    SECTION( "and cast is not" )
    {
        const Typed p( "i32 main() { u32 mask = cast<u32>( 0 - 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "comparisons cannot overflow and are left alone" )
    {
        const Typed p( "i32 main() { if ( 200 + 100 > 0 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The folder was written to *detect* overflow, so it answered "not constant" for every operator
// that cannot overflow. Evaluating a global's initialiser needs it to answer properly, and the
// constant-overflow rule gets wider for free: `~0 + 1` is not checked today because the `~` stops
// the fold before the addition is reached.
TEST_CASE( "type_checker_folds_bitwise_operators", "[sema][types][constants][fold]" )
{
    // Bitwise operators are defined on the representation, so folding one means going to two's
    // complement in the type's width and back. Sign-magnitude has no bit pattern of its own.
    SECTION( "and, or, xor on positive values" )
    {
        for( const char* body :
             { "u8 d = 255 & 15; if ( d != 15 ) { return 1; }",
               "u8 d = 240 | 15; if ( d != 255 ) { return 1; }",
               "u8 d = 255 ^ 15; if ( d != 240 ) { return 1; }" } )
        {
            const Typed p( std::string( "i32 main() { " ) + body + " return 0; }" );

            INFO( body << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // The round trip a sign bug breaks: ~0 is -1 in every signed width. Asserted on the *value*
    // rather than on cleanliness, because ignoring the sign bit still produces a clean program -
    // just one holding 4294967295.
    SECTION( "complement of zero is negative one" )
    {
        const Typed p( "i32 flipped = ~0;\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::optional<Constant_value> value = p.constant_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE( value.has_value() );
        REQUIRE( value->negative );
        REQUIRE( value->magnitude == 1 );
    }

    // The same pattern in an unsigned type is the maximum, not minus one - which is the whole
    // reason from_bits needs the signedness and not just the width.
    SECTION( "and the maximum in an unsigned one" )
    {
        const Typed p( "u32 all_ones = wrap<u32>( ~0 );\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::optional<Constant_value> value = p.constant_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE( value.has_value() );
        REQUIRE_FALSE( value->negative );
        REQUIRE( value->magnitude == 4294967295ull );
    }

    // ~0 does not fit an unsigned type as -1, but as a bit pattern it is the maximum. The width is
    // what decides, which is why the fold needs the type rather than just the value.
    SECTION( "complement in an unsigned type is the maximum" )
    {
        const Typed p( "i32 main() { u8 d = ~0; return 0; }" );

        INFO( p.rendered() );

        // -1 does not fit u8, so this is refused - and it is the *fold* that knows, not the parser.
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and on negative values, through the sign conversion" )
    {
        const Typed p( "i32 main() { i32 d = -1 & 255; if ( d != 255 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The point of folding these at all: a bitwise operand no longer stops the fold, so the value
    // reaches the enclosing operation.
    SECTION( "a bitwise operand no longer stops the fold" )
    {
        const Typed p( "i32 main() { u8 d = 255 & 255; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Nested here is still refused for an unrelated reason: check() pushes an expectation into a
    // binary only when both operands are literal expressions, and a Binary_expr is not one - so
    // `( 255 & 255 )` settles on i32 before the addition. That is the inferring-where-checking-
    // belonged family again, and widening is_literal_expression would fix it.
    SECTION( "though a nested one is still refused, for a different reason" )
    {
        const Typed p( "i32 main() { u8 d = ( 255 & 255 ) + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

TEST_CASE( "type_checker_folds_comparisons", "[sema][types][constants][fold]" )
{
    SECTION( "producing a bool" )
    {
        for( const char* body :
             { "bool d = 1 < 2; if ( !d ) { return 1; }",
               "bool d = 2 < 1; if ( d ) { return 1; }",
               "bool d = 2 == 2; if ( !d ) { return 1; }",
               "bool d = 2 != 2; if ( d ) { return 1; }" } )
        {
            const Typed p( std::string( "i32 main() { " ) + body + " return 0; }" );

            INFO( body << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // Negative sorts below positive whatever the magnitudes, which sign-magnitude does not give
    // for free.
    SECTION( "with a negative operand" )
    {
        const Typed p( "i32 main() { bool d = -5 < 1; if ( !d ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A cast reaching the folder is either a widening `cast` or a `wrap` - narrowing `cast` is refused
// by the checker until there is something to trap with - so folding one is reducing modulo the
// target's width, which is the same machinery the bitwise operators need.
TEST_CASE( "type_checker_folds_casts", "[sema][types][constants][fold]" )
{
    SECTION( "a widening cast keeps the value" )
    {
        const Typed p( "i32 main() { i64 d = cast<i64>( 7 ); if ( d != 7 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The all-ones idiom, and the one that exercises the sign conversion in both directions.
    SECTION( "a wrap reduces modulo the width" )
    {
        const Typed p( "i32 main() { u32 d = wrap<u32>( 0 - 1 ); if ( d != 4294967295 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a narrowing wrap keeps the low bits" )
    {
        const Typed p( "i32 main() { u8 d = wrap<u8>( 300 ); if ( d != 44 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Every accepted file-scope initialiser must fold, because the rule already requires it to be a
// constant expression. A failure here is an internal error rather than a user one, which is a
// stronger invariant than the expression printer it replaces.
TEST_CASE( "type_checker_records_a_value_for_every_global", "[sema][types][constants][fold]" )
{
    const Typed p( "i32  counter = 1;\n"
                   "f64  ratio = 1.5;\n"
                   "bool ready = true;\n"
                   "i32  below = -1;\n"
                   "i32  computed = 60 * 60;\n"
                   "u8   masked = 255 & 15;\n"
                   "u32  shifted = 1 << 4;\n"
                   "i32  flipped = ~0;\n"
                   "bool compared = 1 < 2;\n"
                   "u32  all_ones = wrap<u32>( 0 - 1 );\n"
                   "i32* nothing = nullptr;\n"
                   "i32  blank;\n"
                   "i32 main() { return 0; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    // One per global with an initialiser; `blank` has none and needs no value.
    std::size_t recorded = 0;

    for( u32 i = 0; i < 12; ++i )
    {
        const Node_id global = p.nth( Node_kind::Var_decl, i );

        if( !global.is_valid() )
        {
            break;
        }

        const Node_id init = p.child( global, 1 );

        if( init.is_valid() )
        {
            INFO( "global " << i );
            REQUIRE( p.constant_of( global ).has_value() );
            recorded += 1;
        }
    }

    REQUIRE( recorded == 11 );
}

// D29 draws the struct/class line at trivial copyability, and a destructor is what breaks it: copy
// plus destructor is a double free. The parser accepts one on either kind so that the message can
// name `class` as the fix rather than being a syntax error.
TEST_CASE( "type_checker_rejects_a_destructor_on_a_struct", "[sema][aggregates]" )
{
    SECTION( "a struct with a destructor is rejected" )
    {
        const Typed p( "struct Point { i32 x; ~Point() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "cannot have a destructor" ) != std::string::npos );
    }

    // One mistake, one diagnostic - the rest of the declaration is well formed and must not be
    // re-reported as a consequence of the destructor.
    SECTION( "and reported once" )
    {
        const Typed p( "struct Point { i32 x; i32 y; ~Point() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a class with one is accepted" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a class without one is accepted" )
    {
        const Typed p( "class Handle { u64 value; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// aux carries what was written after the `~`, so a mismatch is a comparison rather than a parse
// failure - which is what lets the message name the type that was meant.
TEST_CASE( "type_checker_checks_destructor_names", "[sema][aggregates]" )
{
    SECTION( "a name that does not match the enclosing type is rejected" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Wrong() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a matching name is accepted" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The resolver's duplicate-member check skips everything that is not a Field_decl, so a second
    // destructor reaches here unreported and needs its own rule.
    SECTION( "two destructors are rejected once" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }
}

// D2: a type is owning exactly when it has a destructor, directly or through a by-value member.
// The answer is recorded rather than recomputed because drop elaboration runs on KIR, long after
// the checker has finished.
TEST_CASE( "type_checker_computes_the_owning_query", "[sema][aggregates][owning]" )
{
    const auto owning = []( const Typed& p, std::size_t nth_decl )
    { return p.types().is_owning( p.types().type_of( p.nth( Node_kind::Class_decl, nth_decl ) ) ); };

    SECTION( "a class with a destructor owns; one without does not" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Handle { u64 value; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( owning( p, 0 ) );
        REQUIRE_FALSE( owning( p, 1 ) );
    }

    SECTION( "a struct never owns - it cannot hold anything that does" )
    {
        const Typed p( "struct Point { i32 x; i32 y; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE_FALSE( p.types().is_owning( p.types().type_of( p.nth( Node_kind::Struct_decl, 0 ) ) ) );
    }

    // The transitivity is forced rather than chosen: destroying a Wrapper destroys its Buffer, so
    // there is no way for it not to own.
    SECTION( "owning is transitive through a by-value member" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Wrapper { Buffer inner; };\n"
                       "class Outer { Wrapper w; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( owning( p, 1 ) );
        REQUIRE( owning( p, 2 ) );
    }

    // An address says nothing about who frees it, so a pointer breaks the chain.
    SECTION( "a pointer to an owning type does not own" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Holder { Buffer* p; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE_FALSE( owning( p, 1 ) );
    }

    SECTION( "a builtin never owns" )
    {
        const Typed p( "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.types().is_owning( p.types().table().integer( 32, true ) ) );
    }
}

// The rule that makes `struct` mean something: it is legal exactly when D2's owning query says no,
// so the check costs one call rather than any new machinery.
TEST_CASE( "type_checker_rejects_an_owning_member_in_a_struct", "[sema][aggregates][owning]" )
{
    SECTION( "a struct holding a class with a destructor is rejected" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer b; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "transitively, through a class that only contains one" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Wrapper { Buffer inner; };\n"
                       "struct Holder { Wrapper w; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a pointer to one is fine - it owns nothing" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer* p; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A destructor is a child of the declaration alongside the fields, so anything counting
    // children counts it too - which demanded an extra initialiser and misaligned every positional
    // one after it. The literal form on a class is D29's temporary exception until constructors
    // exist, so it is exactly the path with no other coverage.
    SECTION( "a destructor is not counted as a field by a struct literal" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "i32 main() { Buffer b = Buffer { nullptr }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a genuinely wrong count still reports the field count, not the member count" )
    {
        const Typed p( "class Buffer { u8* ptr; u64 len; ~Buffer() { } };\n"
                       "i32 main() { Buffer b = Buffer { nullptr }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "has 2 fields" ) != std::string::npos );
    }

    SECTION( "a class holding one is fine" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Wrapper { Buffer inner; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a struct holding a non-owning class is fine" )
    {
        const Typed p( "class Handle { u64 value; };\n"
                       "struct Holder { Handle h; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Every field is reported, because each is a separate place the author has to change.
    SECTION( "two owning fields are two diagnostics" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer a; Buffer b; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }
}

// The destructor body is typed like any function body. `this` needs no rule of its own: it is a
// parameter with an annotation, so the ordinary path types it.
TEST_CASE( "type_checker_types_a_destructor_body", "[sema][types][aggregates]" )
{
    SECTION( "a bare field has the field's type" )
    {
        const Typed p( "class Buffer { u8* ptr; u64 len; ~Buffer() { len = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Name_expr, 0 ) ) == "u64" );
    }

    SECTION( "`this` is a pointer to the enclosing type" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { this.ptr = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Name_expr, 0 ) ) == "Buffer*" );
    }

    // D22 already makes `.` reach through a pointer, so `this.ptr` needs no rule either.
    SECTION( "`this.field` has the field's type" )
    {
        const Typed p( "class Buffer { u64 len; ~Buffer() { this.len = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 0 ) ) == "u64" );
    }

    SECTION( "a field's type is still checked against what is assigned" )
    {
        const Typed p( "class Buffer { u64 len; ~Buffer() { len = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // C++ forbids it, and a pointer parameter would otherwise accept it silently.
    SECTION( "`this` cannot be assigned" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { this = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a destructor returns nothing" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { return 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a bare `return` is fine" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { return; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "calling a free function from a destructor works" )
    {
        const Typed p( "void release( u8* p ) { }\n"
                       "class Buffer { u8* ptr; ~Buffer() { release( ptr ); } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D29: a class is built by a constructor and a struct from a literal, which is what stops the two
// initialisation syntaxes competing. The parser accepts one on either kind so the message can name
// the fix rather than being a syntax error.
TEST_CASE( "type_checker_checks_constructor_declarations", "[sema][aggregates]" )
{
    SECTION( "a struct with a constructor is rejected" )
    {
        const Typed p( "struct Point { i32 x; Point( i32 a ) { x = a; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a class with one is accepted" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a name that does not match the enclosing type is rejected" )
    {
        const Typed p( "class Buffer { u64 len; Wrong( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    // §6.6 puts overloading outside v0, so a second one has nothing to distinguish it.
    SECTION( "two constructors are rejected once" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { } Buffer( i32 m ) { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a constructor beside a destructor is fine" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the body is type checked" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a constructor returns nothing" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { return 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// `Buffer( 16 )` is a Call_expr whose callee names a type rather than a function. Its result is the
// type itself, and its parameters are the constructor's with the receiver skipped.
TEST_CASE( "type_checker_types_a_constructor_call", "[sema][aggregates]" )
{
    SECTION( "the call has the class's type" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer( 16 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Call_expr, 0 ) ) == "Buffer" );
    }

    // The receiver is a parameter like any other, so forgetting to skip it would demand an extra
    // argument at every call site.
    SECTION( "the receiver is not one of the arguments" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer( 16, 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "too few arguments is an error" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer(); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "an argument is checked against the parameter" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer( nullptr ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a class without a constructor is not callable" )
    {
        const Typed p( "class Handle { u64 value; };\ni32 main() { Handle h = Handle( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// The other half of D29's rule, and the entry that comes out of the debts list: with a constructor
// to build it, the literal form stops being a class's only way in.
TEST_CASE( "type_checker_rejects_a_literal_for_a_constructed_class", "[sema][aggregates]" )
{
    SECTION( "a literal is rejected when the class has a constructor" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                       "i32 main() { Buffer b = Buffer { 16 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and accepted when it has none" )
    {
        const Typed p( "class Handle { u64 value; };\ni32 main() { Handle h = Handle { 1 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a struct is unaffected" )
    {
        const Typed p( "struct Point { i32 x; i32 y; };\ni32 main() { Point q = Point { 1, 2 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D2's marker. It records that the author asked for a transfer; what that *means* is the dataflow's,
// and until that exists a moved-from value is still readable. The keyword is legal on any type -
// D31 makes it an assertion on a struct rather than a transfer - and in any expression position,
// because the same rule governs initialisation and assignment as well as arguments.
TEST_CASE( "type_checker_types_a_move", "[sema][move]" )
{
    SECTION( "the marker has its operand's type" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( move a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Marker_expr, 0 ) ) == "i32" );
    }

    SECTION( "in an initialiser" )
    {
        const Typed p( "i32 main() { i32 a = 1; i32 b = move a; return b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "in an assignment" )
    {
        const Typed p( "i32 main() { i32 a = 1; i32 b = 2; b = move a; return b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A field on its own is refused: moving it would leave the object partly moved, and the
    // object's own scope exit would then drop a field that has already gone. A restriction to lift
    // when per-field drop flags exist, not a rule to keep.
    SECTION( "a field cannot be moved on its own" )
    {
        const Typed p( "struct Point { i32 x; i32 y; };\n"
                       "i32 main() { Point q = Point { 1, 2 }; i32 a = move q.x; return a; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "a field cannot be moved on its own" ) != std::string::npos );
    }

    // Nor through a pointer: `*p` names something this function does not own, so moving out of it
    // leaves a hole nothing tracks.
    SECTION( "nor a pointee" )
    {
        const Typed p( "i32 main() { i32 a = 1; i32* p = &a; i32 b = move *p; return b; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "moving the whole object is how you do it" )
    {
        const Typed p( "struct Point { i32 x; i32 y; };\n"
                       "void f( Point p ) { }\n"
                       "i32 main() { Point q = Point { 1, 2 }; f( move q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Legal on a struct, where it is an assertion rather than a transfer: the bytes are copied and
    // the source is marked dead. One user-visible meaning across both kinds.
    SECTION( "a non-owning type can be moved" )
    {
        const Typed p( "struct Point { i32 x; };\n"
                       "void f( Point p ) { }\n"
                       "i32 main() { Point q = Point { 1 }; f( move q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an owning type can be moved" )
    {
        const Typed p( "class Buffer { u64 len; ~Buffer() { } };\n"
                       "void f( move Buffer b ) { }\n"
                       "i32 main() { Buffer b = Buffer { 1 }; f( move b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// `move` makes its source dead, so it needs a source to kill. is_assignable already answers "is
// this a place", which keeps move and assignment from drifting apart on what counts as one.
TEST_CASE( "type_checker_rejects_moving_a_non_place", "[sema][move]" )
{
    SECTION( "a literal has nowhere to be moved from" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { f( move 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "nor does a call result" )
    {
        const Typed p( "i32 g() { return 1; }\nvoid f( i32 x ) { }\ni32 main() { f( move g() ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "nor an arithmetic expression" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( move ( a + 1 ) ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // A marker annotates a whole argument rather than binding as an operator, so it takes the
    // expression to the boundary and this is `move ( a + 1 )`. Under unary binding it would be
    // `( move a ) + 1`, which passes the *sum* by copy - leaving a `move` at the call site saying
    // nothing about what the callee receives, which is the one thing D2 exists to guarantee.
    SECTION( "a marker takes the whole argument, not just the first operand" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( move a + 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "only a variable or an owned temporary can be moved" ) != std::string::npos );
    }

    // The old reading stays available, and has to be written down.
    SECTION( "parentheses restore it" )
    {
        const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( ( move a ) + 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// `out` waits on definite assignment in the callee. It should say so in its own words rather than
// share `move`'s path, and one error rather than two - the marker is the only thing wrong here.
TEST_CASE( "type_checker_rejects_out_for_now", "[sema][move]" )
{
    const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( out a ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "not supported yet" ) != std::string::npos );
}

// D31: an owning value is transferred, not copied, and the transfer is written down - in an
// initialiser and an assignment as much as at a call. Without this the lowerer moves it anyway,
// which frees exactly once but does it silently, and a silent transfer is the one thing D2 exists
// to prevent.
TEST_CASE( "type_checker_requires_move_when_copying_an_owning_value", "[sema][move]" )
{
    constexpr std::string_view owning = "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n";

    SECTION( "an initialiser from a named variable" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); B c = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "write `move a`" ) != std::string::npos );
    }

    SECTION( "an assignment from a named variable" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); B c = B( 2 ); c = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "with the marker it is accepted" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); B c = move a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A temporary has no other owner, so handing it over is the only thing that can happen to it -
    // and there is no variable left behind for a reader to wonder about.
    SECTION( "a temporary needs no marker" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); a = B( 2 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and `auto` is checked the same way" )
    {
        const Typed p( std::string( owning ) + "i32 main() { B a = B( 1 ); auto c = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // Nothing changes for a type that owns nothing: copying one is what it is for.
    SECTION( "a non-owning value copies freely" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { P a = P { 1 }; P c = a; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// PLAN D32. `ref` is a binding mode, not a type: the declaration keeps `T`, so every use inside the
// body is an ordinary `T`. The address it travels as goes on the annotation, which nothing else
// records a type on - and which is what lowering and the emitter read back.
TEST_CASE( "type_checker_types_a_ref_parameter_as_its_referent", "[sema][ref]" )
{
    const Typed p( "void bump( ref i32 n ) { n = n + 1; }\ni32 main() { i32 x = 1; bump( ref x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    // An i32 and not a pointer - `n = n + 1` above would not have typed otherwise.
    REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 0 ) ) == "i32" );
    REQUIRE( p.type_name( p.nth( Node_kind::Mode_type, 0 ) ) == "i32*" );
}

// The same mechanism reaches a class, which is what makes it worth having: a borrow is the only
// thing §6.6 leaves available for passing one without giving it away.
TEST_CASE( "type_checker_lends_a_class_by_ref", "[sema][ref]" )
{
    const Typed p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                   "void grow( ref B b ) { b.n = b.n + 1; }\n"
                   "i32 main() { B a = B( 1 ); grow( ref a ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.type_name( p.nth( Node_kind::Mode_type, 0 ) ) == "B*" );
}

TEST_CASE( "type_checker_requires_the_ref_marker_at_the_call", "[sema][ref]" )
{
    constexpr std::string_view bump = "void bump( ref i32 n ) { n = n + 1; }\n";

    // D2: a reader of the call site should never have to find the declaration to learn that the
    // callee may write through the argument. Note the type owns nothing - unlike `move`, `ref` is
    // not exempt there, because it changes what the callee is holding whatever the type is.
    SECTION( "a bare argument is refused" )
    {
        const Typed p( std::string( bump ) + "i32 main() { i32 x = 1; bump( x ); return x; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "write `ref`" ) != std::string::npos );
    }

    SECTION( "a marker the signature does not ask for is refused" )
    {
        const Typed p( "void plain( i32 n ) { }\ni32 main() { i32 x = 1; plain( ref x ); return x; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "remove `ref`" ) != std::string::npos );
    }

    // The exemption that remains is `move` on a non-owning type: it is the caller's own assertion
    // that the source is dead afterwards, which the callee neither sees nor cares about.
    SECTION( "move on a non-owning type is still exempt" )
    {
        const Typed p( "void plain( i32 n ) { }\ni32 main() { i32 x = 1; plain( move x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "with the marker it is accepted" )
    {
        const Typed p( std::string( bump ) + "i32 main() { i32 x = 1; bump( ref x ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_requires_a_place_for_a_ref_argument", "[sema][ref]" )
{
    constexpr std::string_view bump = "void bump( ref i32 n ) { n = n + 1; }\n";

    SECTION( "a literal has no place to lend" )
    {
        const Typed p( std::string( bump ) + "i32 main() { bump( ref 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "`ref` needs a variable to borrow" ) != std::string::npos );
    }

    // Unlike a move, a borrow leaves nothing partly gone, so a field needs no restriction of its
    // own - which is the whole reason is_assignable is the test rather than a stricter one.
    SECTION( "a field can be lent" )
    {
        const Typed p(
            "struct P { i32 x; };\n" + std::string( bump ) + "i32 main() { P p = P { 1 }; bump( ref p.x ); return p.x; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and so can a pointee" )
    {
        const Typed p( std::string( bump ) + "i32 main() { i32 x = 1; i32* q = &x; bump( ref *q ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A binding is the caller's object itself, so there is no conversion step for a widened copy to
    // live in. §6.4 would happily widen the argument, and the callee would then write into a
    // temporary nobody reads.
    SECTION( "a ref argument is not widened to reach the parameter" )
    {
        const Typed p( std::string( bump ) + "i32 main() { u8 x = 1; bump( ref x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// `out` needs definite assignment in the callee, which is §8's lattice read from the Uninitialised
// end rather than the Moved one. Until that exists it is refused rather than half-supported.
TEST_CASE( "type_checker_still_refuses_out", "[sema][ref]" )
{
    const Typed p( "void init( out i32 n ) { n = 1; }\ni32 main() { i32 x = 0; init( out x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE_FALSE( p.clean() );
}

} // namespace keel
#endif
