#include "lex/token.h"

namespace keel
{

// The enum spelling, for --dump-tokens and debugging. Not for diagnostics: those want the source
// spelling (";", not "Semicolon").
//
// A switch rather than a table: each case names its own enumerator, so a mis-ordered entry is not
// expressible, and -Wswitch turns a forgotten one into a release build failure.
std::string_view token_kind_name( Token_kind kind )
{
    switch( kind )
    {
    case Token_kind::End_of_file:
        return "End_of_file";
    case Token_kind::Unknown:
        return "Unknown";
    case Token_kind::Identifier:
        return "Identifier";
    case Token_kind::Keyword:
        return "Keyword";
    case Token_kind::Int_literal:
        return "Int_literal";
    case Token_kind::Float_literal:
        return "Float_literal";
    case Token_kind::String_literal:
        return "String_literal";
    case Token_kind::Char_literal:
        return "Char_literal";
    case Token_kind::L_paren:
        return "L_paren";
    case Token_kind::R_paren:
        return "R_paren";
    case Token_kind::L_brace:
        return "L_brace";
    case Token_kind::R_brace:
        return "R_brace";
    case Token_kind::L_bracket:
        return "L_bracket";
    case Token_kind::R_bracket:
        return "R_bracket";
    case Token_kind::Semicolon:
        return "Semicolon";
    case Token_kind::Comma:
        return "Comma";
    case Token_kind::Colon:
        return "Colon";
    case Token_kind::Colon_colon:
        return "Colon_colon";
    case Token_kind::Dot:
        return "Dot";
    case Token_kind::Arrow:
        return "Arrow";
    case Token_kind::Plus:
        return "Plus";
    case Token_kind::Minus:
        return "Minus";
    case Token_kind::Star:
        return "Star";
    case Token_kind::Slash:
        return "Slash";
    case Token_kind::Percent:
        return "Percent";
    case Token_kind::Equal_equal:
        return "Equal_equal";
    case Token_kind::Bang_equal:
        return "Bang_equal";
    case Token_kind::Less:
        return "Less";
    case Token_kind::Greater:
        return "Greater";
    case Token_kind::Less_equal:
        return "Less_equal";
    case Token_kind::Greater_equal:
        return "Greater_equal";
    case Token_kind::Amp_amp:
        return "Amp_amp";
    case Token_kind::Pipe_pipe:
        return "Pipe_pipe";
    case Token_kind::Bang:
        return "Bang";
    case Token_kind::Amp:
        return "Amp";
    case Token_kind::Pipe:
        return "Pipe";
    case Token_kind::Caret:
        return "Caret";
    case Token_kind::Tilde:
        return "Tilde";
    case Token_kind::Less_less:
        return "Less_less";
    case Token_kind::Greater_greater:
        return "Greater_greater";
    case Token_kind::Equal:
        return "Equal";
    case Token_kind::Plus_equal:
        return "Plus_equal";
    case Token_kind::Minus_equal:
        return "Minus_equal";
    case Token_kind::Star_equal:
        return "Star_equal";
    case Token_kind::Slash_equal:
        return "Slash_equal";
    case Token_kind::Percent_equal:
        return "Percent_equal";
    case Token_kind::Amp_equal:
        return "Amp_equal";
    case Token_kind::Pipe_equal:
        return "Pipe_equal";
    case Token_kind::Caret_equal:
        return "Caret_equal";
    case Token_kind::Less_less_equal:
        return "Less_less_equal";
    case Token_kind::Greater_greater_equal:
        return "Greater_greater_equal";
    case Token_kind::Question:
        return "Question";
    case Token_kind::Plus_plus:
        return "Plus_plus";
    case Token_kind::Minus_minus:
        return "Minus_minus";

    case Token_kind::Count:
        break;
    }

    return "Unknown";
}

} // namespace keel
