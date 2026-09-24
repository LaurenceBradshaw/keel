#pragma once
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/span.h"

#include <string_view>
#include <type_traits>

namespace keel
{

enum class Token_kind : u16
{
    // The lexer emits this at text.size() with a zero-length span, so "unexpected end of input"
    // has somewhere to point.
    End_of_file,

    // Emitted after an unrecoverable character so the parser still sees a token and spans stay
    // contiguous.
    Unknown,

    Identifier,
    Keyword, // static_cast<Keyword>( token.symbol.v ) gives which one

    // Values are not computed here - the span is enough, and deferring keeps overflow out of the
    // lexer. `true` and `false` arrive as Keyword.
    Int_literal,
    Float_literal,
    String_literal,
    Char_literal,

    // --- Grouping and separators ---
    L_paren,     // (
    R_paren,     // )
    L_brace,     // {
    R_brace,     // }
    L_bracket,   // [
    R_bracket,   // ]
    Semicolon,   // ;
    Comma,       // ,
    Colon,       // :
    Colon_colon, // ::
    Dot,         // .
    Dot_dot,     // ..
    Arrow,       // -> lexed only so the parser can reject it by name (D22)

    // --- Arithmetic ---
    Plus,    // +
    Minus,   // -
    Star,    // *  also pointer types and dereference
    Slash,   // /
    Percent, // %

    // --- Comparison ---
    Equal_equal,   // ==
    Bang_equal,    // !=
    Less,          // <  also opens a generic parameter/argument list
    Greater,       // >
    Less_equal,    // <=
    Greater_equal, // >=

    // --- Logical ---
    Amp_amp,   // &&
    Pipe_pipe, // ||
    Bang,      // !

    // --- Bitwise ---
    Amp,             // &  also reference types
    Pipe,            // |
    Caret,           // ^
    Tilde,           // ~  destructor names
    Less_less,       // <<
    Greater_greater, // >>

    // --- Assignment ---
    Equal,                 // =
    Plus_equal,            // +=
    Minus_equal,           // -=
    Star_equal,            // *=
    Slash_equal,           // /=
    Percent_equal,         // %=
    Amp_equal,             // &=
    Pipe_equal,            // |=
    Caret_equal,           // ^=
    Less_less_equal,       // <<=
    Greater_greater_equal, // >>=

    // PLAN §6.3 D6: error propagation. No C++ meaning, so it is a pure addition.
    Question, // ?

    // Statements, never expressions (PLAN §6.3 D12): `i++;` is fine, `x = a[i++]` is an error.
    Plus_plus,   // ++
    Minus_minus, // --

    Count
};

// Stable spelling for --dump-tokens and debugging. Never empty.
std::string_view token_kind_name( Token_kind kind );

// The user-facing spelling, for diagnostics: ";" not "Semicolon". Punctuation returns the
// characters themselves; kinds with no fixed spelling return a lowercase description ("identifier",
// "end of file") that reads inside a sentence. Callers add their own quoting.
std::string_view token_kind_spelling( Token_kind kind );

// The operation a compound assignment performs: `+=` is `+`, applied and stored back. The emitter
// expands it rather than emitting C's own `+=`, so that the conversions stay explicit and one code
// path computes the result - C's compound assignment has conversion rules of its own, and they are
// not §6.4's.
Token_kind base_operator( Token_kind assignment );

// The six operators whose answer is a `bool` about two values rather than a value of their own.
// D41 turns on that distinction, and both the lowering that picks their domain and the backend
// that compares outside one ask the question - so it is answered here rather than twice.
bool is_comparison( Token_kind kind );

struct Token
{
    Token_kind kind;
    Span       span;
    Symbol_id  symbol; // Only valid for identifiers and keywords.

    bool is( Token_kind k ) const
    {
        return kind == k;
    }

    // Valid only when kind == Token_kind::Keyword.
    Keyword keyword() const
    {
        return static_cast<Keyword>( symbol.v );
    }

    // Valid only for Int_literal and Float_literal. The same slot as `symbol`, which those kinds
    // do not use - a literal has no name.
    Literal_id literal() const
    {
        return Literal_id { symbol.v };
    }
};

static_assert( std::is_trivially_copyable_v<Token>, "Token must stay trivially copyable" );

} // namespace keel
