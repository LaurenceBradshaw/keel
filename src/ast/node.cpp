#include "ast/node.h"

namespace keel
{

std::string_view node_kind_name( Node_kind kind )
{
    switch( kind )
    {
    case Node_kind::Error:
        return "Error";
    case Node_kind::Source_file:
        return "Source_file";
    case Node_kind::Function_decl:
        return "Function_decl";
    case Node_kind::Param_decl:
        return "Param_decl";
    case Node_kind::Named_type:
        return "Named_type";
    case Node_kind::Param_list:
        return "Param_list";
    case Node_kind::Block:
        return "Block";
    case Node_kind::Return_stmt:
        return "Return_stmt";
    case Node_kind::Int_literal:
        return "Int_literal";
    case Node_kind::Binary_expr:
        return "Binary_expr";

    case Node_kind::Count:
        break;
    }

    return "Unknown";
}

} // namespace keel
