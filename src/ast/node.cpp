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
    case Node_kind::Pointer_type:
        return "Pointer_type";
    case Node_kind::Ref_type:
        return "Ref_type";
    case Node_kind::Const_type:
        return "Const_type";
    case Node_kind::Generic_type:
        return "Generic_type";
    case Node_kind::Type_arg_list:
        return "Type_arg_list";
    case Node_kind::Param_list:
        return "Param_list";
    case Node_kind::Block:
        return "Block";
    case Node_kind::Return_stmt:
        return "Return_stmt";
    case Node_kind::Int_literal:
        return "Int_literal";
    case Node_kind::Float_literal:
        return "Float_literal";
    case Node_kind::String_literal:
        return "String_literal";
    case Node_kind::Char_literal:
        return "Char_literal";
    case Node_kind::Bool_literal:
        return "Bool_literal";
    case Node_kind::Name_expr:
        return "Name_expr";
    case Node_kind::Binary_expr:
        return "Binary_expr";
    case Node_kind::Unary_expr:
        return "Unary_expr";
    case Node_kind::Call_expr:
        return "Call_expr";
    case Node_kind::Arg_list:
        return "Arg_list";
    case Node_kind::Var_decl:
        return "Var_decl";
    case Node_kind::Assign_stmt:
        return "Assign_stmt";
    case Node_kind::Increment_stmt:
        return "Increment_stmt";
    case Node_kind::Expr_stmt:
        return "Expr_stmt";
    case Node_kind::If_stmt:
        return "If_stmt";
    case Node_kind::While_stmt:
        return "While_stmt";
    case Node_kind::For_stmt:
        return "For_stmt";
    case Node_kind::Struct_decl:
        return "Struct_decl";
    case Node_kind::Field_decl:
        return "Field_decl";
    case Node_kind::Field_expr:
        return "Field_expr";
    case Node_kind::Struct_literal:
        return "Struct_literal";
    case Node_kind::Field_init:
        return "Field_init";

    case Node_kind::Count:
        break;
    }

    return "Unknown";
}

} // namespace keel
