#include "codegen_c/emit_kir.h"
#include <fmt/format.h>
#include <span>
#include "codegen_c/mangle.h"
#include "codegen_c/spelling.h"
#include "common/types.h"

namespace keel
{

namespace
{

class Kir_emitter
{
public:
    Kir_emitter(
        const std::vector<Function>& functions,
        const Ast&                   ast,
        Types&                       types,
        const Literals&              literals,
        const Source_manager&        sm,
        const Interner&              interner,
        std::span<const Type_id>     struct_order
    );

    std::string run();

private:
    void write_line( std::string_view text );
    void line_directive( Span span );

    void emit_prologue();
    void emit_structs();
    void emit_enums();
    void emit_globals();
    void emit_prototypes();
    void emit_runtime_prototypes();
    void emit_externs();
    void emit_functions();
    void emit_main_shim();

    void emit_statement( const Statement& statement );
    void emit_terminator( const Terminator& terminator, const Function& function );
    void emit_function( const Function& function );

    std::string local_name( u32 index ) const;

    bool uses_runtime() const;

    // A void local is never declared: C has no such object, and nothing reads one. Testing the
    // *kind* rather than is_valid() - a void local's Type_id is perfectly valid, it just names the
    // one type C cannot hold.
    bool is_void( Type_id type ) const;
    bool assigns_to_void( const Place& place ) const;
    // A place's type, walked the same way place() walks its text. Drop needs it to name the
    // destructor to call.
    Type_id     type_of( const Place& place ) const;
    std::string place( const Place& place ) const;
    std::string constant( Literal_id literal, Type_id type ) const;
    std::string operand( const Operand& operand ) const;
    std::string rvalue( const Rvalue& rvalue ) const;

    std::string prototype( Node_id decl, std::span<const Type_id> type_arguments = {} ) const;
    std::string prototype( const Function& function ) const;

    std::string     out_;
    u32             indent_    = 0;
    u32             last_line_ = 0;       // so #line is emitted only when it changes
    const Function* current_   = nullptr; // The function currently being emitted.

    const std::vector<Function>& functions_;
    const Ast&                   ast_;
    // Mutable because emit_structs substitutes a field's declared type through its instance, and
    // substituting interns. Everything it names was interned while the order was built, so nothing
    // new appears - but the table has no const way to say so.
    Types&                   types_;
    std::span<const Type_id> struct_order_;
    const Literals&          literals_;
    const Source_manager&    sm_;
    const Interner&          interner_;
    const Spelling           spelling_;
};

Kir_emitter::Kir_emitter(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    Types&                       types,
    const Literals&              literals,
    const Source_manager&        sm,
    const Interner&              interner,
    std::span<const Type_id>     struct_order
)
    : functions_( functions ),
      ast_( ast ),
      types_( types ),
      struct_order_( struct_order ),
      literals_( literals ),
      sm_( sm ),
      interner_( interner ),
      spelling_( Spelling { ast, types, interner } )
{
}

std::string Kir_emitter::run()
{
    emit_prologue();
    emit_structs();
    emit_enums();
    emit_globals();
    emit_runtime_prototypes();
    emit_externs();
    emit_prototypes();
    emit_functions();
    emit_main_shim();

    return out_;
}

void Kir_emitter::write_line( std::string_view text )
{
    if( !text.empty() )
    {
        out_.append( indent_, ' ' );
        out_ += text;
    }

    out_ += '\n';
}

void Kir_emitter::line_directive( Span span )
{
    const Line_col loc = sm_.line_col( span.file, span.start );

    if( loc.line == last_line_ )
    {
        return; // only when it moves, or the directives outnumber the code
    }

    last_line_ = loc.line;

    // Column-zero syntax: never indented, whatever the surrounding block depth.
    out_ += fmt::format( "#line {} \"{}\"\n", loc.line, sm_.file( span.file ).path );
}

void Kir_emitter::emit_prologue()
{
    write_line( "#include <stdint.h>" );
    write_line( "#include <stdbool.h>" );
    write_line( "#include <stddef.h>" ); // NULL
    write_line( "" );
}

// D7. An enum that carries payloads is a struct in C: a tag saying which variant is live, then
// every payload field of every variant side by side.
//
// Side by side rather than in a union, which is what a tagged variant would normally be. The field
// names are already mangled with their node id, so two variants cannot collide, and the only cost
// is the space a union would have saved - invisible, because nothing guarantees an enum's layout
// and no FFI can see one. Moving to a union later is a change to this function and nothing else.
void Kir_emitter::emit_enums()
{
    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( decl ) != Node_kind::Enum_decl || !enum_has_payload( ast_, decl ) )
        {
            continue;
        }

        write_line( spelling_.structure( types_.type_of( decl ) ) );
        write_line( "{" );
        indent_ += 4;

        write_line( fmt::format( "{} tag;", spelling_.type( types_.table().get( types_.type_of( decl ) ).element ) ) );

        for( const Node_id variant : ast_.children( decl ).subspan( 1 ) )
        {
            for( const Node_id field : ast_.children( variant ) )
            {
                write_line( fmt::format( "{} {};", spelling_.type( types_.type_of( field ) ), spelling_.field( field ) ) );
            }
        }

        indent_ -= 4;
        write_line( "};" );
        write_line( "" );
    }
}

void Kir_emitter::emit_structs()
{
    // One entry per instantiation, already ordered so that a type arrives after everything it holds
    // by value. See emitted_struct_order, which is where the walk and the reason for it live.
    if( struct_order_.empty() )
    {
        return;
    }

    // §7.4's forward declarations. Nothing in v0 needs them - the definitions are already ordered
    // - but they cost a line each and become necessary the moment a struct holds a pointer to one,
    // which a generic linked list does.
    for( const Type_id type : struct_order_ )
    {
        write_line( fmt::format( "{};", spelling_.structure( type ) ) );
    }

    write_line( "" );

    for( const Type_id type : struct_order_ )
    {
        const Node_id declaration = types_.table().get( type ).declaration;

        write_line( spelling_.structure( type ) );
        write_line( "{" );
        indent_ += 4;

        for( const Node_id field : ast_.members( declaration ) )
        {
            if( ast_.kind( field ) != Node_kind::Field_decl )
            {
                continue;
            }

            // The field's type through *this* instance: what the declaration says is `T`, which is
            // true of the template and has no C spelling. Every instance was interned while the
            // order was built, so nothing new is named here.
            const Type_id spelled = types_.table().substitute(
                types_.type_of( field ), aggregate_bindings( ast_, types_.table(), type, types_.recorded() )
            );

            write_line( fmt::format( "{} {};", spelling_.type( spelled ), spelling_.field( field ) ) );
        }

        indent_ -= 4;
        write_line( "};" );
        write_line( "" );
    }
}

void Kir_emitter::emit_globals()
{
    bool any = false;

    for( const Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) != Node_kind::Var_decl )
        {
            continue;
        }

        write_line( spelling_.global_definition( child ) );

        any = true;
    }

    if( any )
    {
        write_line( "" );
    }
}

void Kir_emitter::emit_prototypes()
{
    // Every signature ahead of every body: C has no equivalent of D18, so this is what lets
    // `even` call `odd` from above it.
    for( const Function& function : functions_ )
    {

        write_line( prototype( function ) + ";" );
    }

    write_line( "" );
}

void Kir_emitter::emit_runtime_prototypes()
{
    if( !uses_runtime() )
    {
        return;
    }

    write_line( "void* kl_rt_alloc( size_t );" );
    write_line( "void  kl_rt_free( void* );" );
    write_line( "" );
}

void Kir_emitter::emit_externs()
{
    bool any = false;

    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        if( !is_extern( ast_, decl ) )
        {
            continue;
        }

        write_line( prototype( decl ) + ";" );
        any = true;
    }

    if( any )
    {
        write_line( "" );
    }
}

void Kir_emitter::emit_functions()
{
    for( const Function& function : functions_ )
    {
        emit_function( function );
        write_line( "" );
    }
}

void Kir_emitter::emit_main_shim()
{
    Node_id keel_main;

    for( const Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) != Node_kind::Function_decl )
        {
            continue; // aux is only a Symbol_id on a declaration
        }

        if( interner_.text( Symbol_id { ast_.aux( child ) } ) == "main" )
        {
            keel_main = child;
            break;
        }
    }

    // A program with no main is a library, not an error for the emitter to raise.
    if( !keel_main.is_valid() )
    {
        return;
    }

    // The symbol is derived from the node rather than hardcoded, so that a main with a different
    // signature calls the function that actually exists.
    const std::string symbol = spelling_.function( keel_main );

    write_line( "int main( void )" );
    write_line( "{" );
    indent_ += 4;
    write_line( fmt::format( "return (int) {}();", symbol ) );
    indent_ -= 4;
    write_line( "}" );
}

bool Kir_emitter::is_void( Type_id type ) const
{
    return type.is_valid() && types_.table().get( type ).kind == Type_kind::Void;
}

bool Kir_emitter::assigns_to_void( const Place& target ) const
{
    return !target.is_global() && target.num_projections == 0 && is_void( current_->locals[target.local.v].type );
}

void Kir_emitter::emit_statement( const Statement& statement )
{

    switch( statement.kind )
    {
    case Statement_kind::Assign:
    {
        line_directive( statement.span );
        const std::string value = rvalue( statement.value );

        // A void local is not declared, so there is nothing to assign into - the call is the whole
        // statement. Keeping Assign total in KIR is what confines this to one rule, in one place.
        if( assigns_to_void( statement.place ) )
        {
            write_line( fmt::format( "{};", value ) );
            return;
        }

        write_line( fmt::format( "{} = {};", place( statement.place ), value ) );
        return;
    }

    // By address, because a destructor takes the receiver as a pointer.
    case Statement_kind::Drop:
    {

        line_directive( statement.span );
        const std::string call =
            fmt::format( "{}( &{} );", spelling_.destructor_of( type_of( statement.place ) ), place( statement.place ) );

        // The flag is the whole of what a conditional drop costs in C.
        write_line(
            statement.drop_flag.is_valid() ? fmt::format( "if ( {} ) {}", local_name( statement.drop_flag.v ), call ) : call
        );
        return;
    }

    // The storage markers describe scopes for the drop pass rather than instructions - KIR has no
    // scoping, and every local is declared up front. Skipped before the line directive, which would
    // otherwise be left with nothing under it.
    case Statement_kind::Storage_live:
    case Statement_kind::Storage_dead:
        return;
    }
}

void Kir_emitter::emit_terminator( const Terminator& terminator, const Function& function )
{
    switch( terminator.kind )
    {
    case Terminator_kind::Goto:
        write_line( fmt::format( "goto bb{};", terminator.targets[0].v ) );
        return;

    case Terminator_kind::Branch:
        write_line( fmt::format(
            "if ( {} ) goto bb{}; else goto bb{};",
            operand( terminator.condition ),
            terminator.targets[0].v,
            terminator.targets[1].v
        ) );
        return;

    // The terminator carries no value: the lowerer put it in the return slot, which is local 0.
    case Terminator_kind::Return:
        if( is_void( types_.type_of( function.declaration ) ) )
        {
            write_line( "return;" );
            return;
        }

        write_line( fmt::format( "return {};", local_name( 0 ) ) );
        return;

    case Terminator_kind::Unreachable:
        assert( false && "the lowerer emits no unreachable terminator" );
        return;

    case Terminator_kind::Unset:
        assert( false && "block has no terminator; verify rejects this" );
        return;
    }
}

void Kir_emitter::emit_function( const Function& function )
{
    current_   = &function;
    last_line_ = 0;

    // Parameters named from KIR locals 1..parameter_count, NOT from Param_decl nodes: the body
    // names them by Local_id, and prototype() deliberately emits no names, so the two can never
    // disagree - nothing may name them from the Param_decl nodes, which is why parameter_types()
    // spells types only.
    std::string params;

    for( u32 i = 1; i <= function.parameter_count; ++i )
    {
        params += fmt::format( "{}{} {}", i == 1 ? "" : ", ", spelling_.type( function.locals[i].type ), local_name( i ) );
    }

    write_line( fmt::format(
        "{} {}( {} )",
        spelling_.type( function.locals[k_return_slot.v].type ),
        spelling_.function( function.declaration, function.type_arguments ), // matches the prototype
        params.empty() ? "void" : params
    ) );
    write_line( "{" );
    indent_ += 4;

    for( u32 i = 0; i < function.locals.size(); ++i )
    {
        // Parameters are declared by the signature already, and a void local cannot be declared at
        // all. Everything else, including the return slot, needs storage here - KIR has no scoping,
        // so there is one declaration per local at the top.
        if( i >= 1 && i <= function.parameter_count )
        {
            continue;
        }

        if( is_void( function.locals[i].type ) )
        {
            continue;
        }

        write_line( fmt::format( "{} {};", spelling_.type( function.locals[i].type ), local_name( i ) ) );
    }

    for( u32 i = 0; i < function.blocks.size(); ++i )
    {
        const Block& block = function.blocks[i];

        if( i != 0 )
        {
            write_line( fmt::format( "bb{}:", i ) );
        }

        for( u32 j = 0; j < block.statement_count; ++j )
        {
            const Statement& statement = function.statements[block.first_statement + j];
            emit_statement( statement );
        }

        emit_terminator( block.terminator, function );
    }
    indent_ -= 4;
    write_line( "}" );
}

std::string Kir_emitter::prototype( Node_id decl, std::span<const Type_id> type_arguments ) const
{
    const std::string params = spelling_.parameter_types( decl );

    return fmt::format(
        "{} {}( {} )",
        spelling_.return_type( decl ), // on the declaration, and a pointer when it returns a binding
        spelling_.function( decl, type_arguments ),
        params
    );
}

// From the lowered function rather than from its declaration, because an instantiation's types live
// only here: the declaration still says `T`, and asking it would hand the emitter a type with no C
// spelling. Local 0 is the return slot and 1..parameter_count are the parameters, both already
// substituted by the lowerer - which also means this and emit_function cannot disagree.
std::string Kir_emitter::prototype( const Function& function ) const
{
    std::string params;

    for( u32 i = 1; i <= function.parameter_count; ++i )
    {
        params += fmt::format( "{}{}", i == 1 ? "" : ", ", spelling_.type( function.locals[i].type ) );
    }

    return fmt::format(
        "{} {}( {} )",
        spelling_.type( function.locals[k_return_slot.v].type ),
        spelling_.function( function.declaration, function.type_arguments ),
        params.empty() ? "void" : params
    );
}

std::string Kir_emitter::local_name( u32 index ) const
{
    const Local& local = current_->locals[index];

    return local.name.is_valid() ? mangle_local( interner_.text( local.name ), index ) : fmt::format( "kl_t{}", index );
}

// Asked of KIR rather than the tree: the question is whether the C about to be written calls the
// runtime, and KIR is what it is written from. The tree would answer it twice over wrongly - an
// alloc is buried in a function body rather than being a top-level declaration, and the day
// something other than an Alloc_expr lowers to an allocation the scan would quietly stop finding
// it and the emitted C would stop linking.
bool Kir_emitter::uses_runtime() const
{
    for( const Function& function : functions_ )
    {
        for( const Statement& statement : function.statements )
        {
            if( statement.kind != Statement_kind::Assign )
            {
                continue;
            }

            if( statement.value.kind == Rvalue_kind::Allocate || statement.value.kind == Rvalue_kind::Release )
            {
                return true;
            }
        }
    }

    return false;
}

Type_id Kir_emitter::type_of( const Place& place ) const
{
    Type_id type = place.is_global() ? types_.type_of( place.global ) : current_->locals[place.local.v].type;

    for( u32 i = 0; i < place.num_projections; ++i )
    {
        const Projection& proj = current_->projections[place.first_projection + i];

        // Deref is the pointee, Field is the field's own recorded type, and a Tag is the enum's
        // underlying integer - which is also `element` for an enum, so the two share a branch.
        // Mirrors place()'s walk, and has to stay in step with it.
        type = proj.kind == Projection_kind::Field ? types_.type_of( proj.field ) : types_.table().get( type ).element;
    }

    return type;
}

std::string Kir_emitter::place( const Place& place ) const
{
    std::string text = place.is_global()
                           ? mangle_local( interner_.text( Symbol_id { ast_.aux( place.global ) } ), place.global.v )
                           : local_name( place.local.v );

    for( u32 i = 0; i < place.num_projections; ++i )
    {
        const Projection& proj = current_->projections[place.first_projection + i];

        switch( proj.kind )
        {
        case Projection_kind::Deref:
            text = fmt::format( "( *{} )", text );
            break;
        case Projection_kind::Field:
            text = fmt::format( "{}.{}", text, spelling_.field( proj.field ) );
            break;
        case Projection_kind::Tag:
            text = fmt::format( "{}.tag", text );
            break;
        }
    }

    return text;
}

std::string Kir_emitter::constant( Literal_id literal, Type_id type ) const
{
    switch( types_.table().get( type ).kind )
    {
    case Type_kind::Bool:
        return literals_.integer( literal ) != 0 ? "true" : "false";
    case Type_kind::Pointer:
        return "NULL";
    case Type_kind::Float:
        return c_float( literals_.floating( literal ) );
    case Type_kind::Int:
    case Type_kind::Enum: // a variant is its index; the C type is the underlying integer
        return c_integer( literals_.integer( literal ) );

    default:
        assert( false && "no C spelling for this type" );
        return "<unknown>";
    }
}

std::string Kir_emitter::operand( const Operand& operand ) const
{
    switch( operand.kind )
    {
    case Operand_kind::Constant:
        return constant( operand.constant, operand.type );
    case Operand_kind::Copy:
    case Operand_kind::Move:
        return place( operand.place );

    default:
        assert( false && "unknown operand kind" );
        return "<unknown>";
    }
}

std::string Kir_emitter::rvalue( const Rvalue& rvalue ) const
{
    switch( rvalue.kind )
    {
    case Rvalue_kind::Use:
        return operand( rvalue.a );

    case Rvalue_kind::Binary:
        return fmt::format( "{} {} {}", operand( rvalue.a ), token_kind_spelling( rvalue.op ), operand( rvalue.b ) );

    case Rvalue_kind::Unary:
        return fmt::format( "{}{}", token_kind_spelling( rvalue.op ), operand( rvalue.a ) );

    case Rvalue_kind::Cast:
        return fmt::format( "( {} ) {}", spelling_.type( rvalue.type ), operand( rvalue.a ) );

    case Rvalue_kind::Address_of:
        return fmt::format( "&{}", place( rvalue.a.place ) );

    case Rvalue_kind::Call:
    {
        std::string args;
        for( u32 i = 0; i < rvalue.argument_count; ++i )
        {
            if( i != 0 )
            {
                args += ", ";
            }
            args += operand( current_->operands[rvalue.first_argument + i] );
        }
        return fmt::format( "{}( {} )", spelling_.function( rvalue.callee, rvalue.type_arguments ), args );
    }
    case Rvalue_kind::Allocate:
        // The element type gives the size, which is the whole reason `alloc` is a keyword rather
        // than a function: no `sizeof` is written and none can be got wrong.
        return fmt::format(
            "( {} ) kl_rt_alloc( sizeof( {} ) )",
            spelling_.type( rvalue.type ),
            spelling_.type( types_.table().get( rvalue.type ).element )
        );
    case Rvalue_kind::Release:
        return fmt::format( "kl_rt_free( {} )", operand( rvalue.a ) );

    default:
        assert( false && "unknown rvalue kind" );
        return "<unknown>";
    }
}

} // namespace

// Every struct type that has to reach C, in an order where a type arrives after everything it holds
// by value. One entry per *instantiation*: `Box<i32>` and `Box<f64>` are two C structs from one
// declaration, and the open `Box<T>` is none, the way a generic function is no C function.
//
// Built here rather than in the checker because an instantiation can be created during lowering -
// substituting `Box<T>` inside an instance of a generic that holds one - and the checker has gone
// by then. The walk interns as it goes, for the same reason: a field of type `Pair<T>` inside
// `Box<T>` names `Pair<i32>`, which nothing else need ever have written down.
//
// Terminating and acyclic by the checker's own walk: contains_itself rejected every by-value cycle
// before this runs, and a pointer field is skipped here exactly as it is there.
std::vector<Type_id> emitted_struct_order( const Ast& ast, Types& types )
{
    std::vector<Type_id> order;
    std::vector<Type_id> visiting;

    // Recursive rather than a queue: the post-order *is* the answer, and a queue would need the
    // graph built first.
    const auto visit = [&]( auto&& self, Type_id type ) -> void
    {
        if( !type.is_valid() || !types.table().is_struct( type ) || types.table().mentions_parameter( type ) )
        {
            return;
        }

        if( std::find( order.begin(), order.end(), type ) != order.end() ||
            std::find( visiting.begin(), visiting.end(), type ) != visiting.end() )
        {
            return;
        }

        visiting.push_back( type );

        for( const Node_id member : ast.members( types.table().get( type ).declaration ) )
        {
            if( ast.kind( member ) != Node_kind::Field_decl )
            {
                continue;
            }

            self( self, field_type( ast, types.table(), type, member, types.recorded() ) );
        }

        visiting.pop_back();
        order.push_back( type );
    };

    // Indexed, because visiting a type can intern another and grow the table underneath us - which
    // is the point: a `Box<i32>` reached only through a field is discovered exactly here.
    for( std::size_t at = 0; at < types.table().struct_types().size(); ++at )
    {
        visit( visit, types.table().struct_types()[at] );
    }

    return order;
}

std::string emit_c_from_kir(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    Types&                       types,
    const Literals&              literals,
    const Source_manager&        sm,
    const Interner&              interner
)
{
    const std::vector<Type_id> order = emitted_struct_order( ast, types );

    return Kir_emitter( functions, ast, types, literals, sm, interner, order ).run();
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "common/diagnostics.h"
#include "ir/lower.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/resolver.h"

namespace keel
{
namespace
{

// The whole pipeline, so what is emitted came from a real lowering rather than a hand-built
// Function. A backend bug that only shows on real input is the kind worth catching.
struct Generated
{
    Source_manager sm;
    Interner       interner;
    Literals       literals;
    Diagnostics    diags;
    Ast            ast;
    Resolution     resolution;
    Types          types;
    std::string    c;

    explicit Generated( std::string_view source )
    {
        const File_id file = sm.add_file( "t.kl", std::string( source ) );

        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
        resolution = resolve( ast, sm, interner, diags );
        types      = type_check( ast, resolution, literals, sm, interner, diags );

        if( !diags.has_errors() )
        {
            c = emit_c_from_kir( lower( ast, resolution, types, literals, interner ), ast, types, literals, sm, interner );
        }
    }

    bool clean() const
    {
        return !diags.has_errors();
    }

    bool has( std::string_view needle ) const
    {
        return c.find( needle ) != std::string::npos;
    }
};

} // namespace

TEST_CASE( "emit_kir_writes_a_function", "[codegen][kir]" )
{
    Generated g( "i32 main() { return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    // The definition's symbol matches the prototype's, because both come from Spelling::function.
    REQUIRE( g.has( "int32_t kl__main__( void );" ) );
    REQUIRE( g.has( "int32_t kl__main__( void )" ) );

    // The return slot is local 0, declared like any other local. The terminator carries no value.
    REQUIRE( g.has( "int32_t kl_t0;" ) );
    REQUIRE( g.has( "kl_t0 = 0;" ) );
    REQUIRE( g.has( "return kl_t0;" ) );
}

// The body names parameters by Local_id; the prototype names none at all. Naming them from the
// Param_decl nodes instead would declare one identifier and use another, and C would complain at
// the use rather than at the cause.
TEST_CASE( "emit_kir_names_parameters_from_kir_locals", "[codegen][kir]" )
{
    Generated g( "i32 add( i32 a, i32 b ) { return a + b; }\ni32 main() { return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    REQUIRE( g.has( "int32_t kl__add__3i32_3i32( int32_t, int32_t );" ) ); // the prototype, unnamed
    REQUIRE( g.has( "int32_t kl_a_1, int32_t kl_b_2" ) );                  // the definition, named

    // Declared once, by the signature - not again as locals.
    REQUIRE_FALSE( g.has( "int32_t kl_a_1;" ) );
}

// Every block but the entry gets a label, and every edge is an explicit goto - so no label is left
// unused, which -Wall would make an error in the golden runner.
TEST_CASE( "emit_kir_writes_blocks_as_labels_and_gotos", "[codegen][kir]" )
{
    Generated g( "i32 f( i32 n ) { if ( n > 0 ) { return 1; } return 2; }\ni32 main() { return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    REQUIRE( g.has( "if ( " ) );
    REQUIRE( g.has( " ) goto bb" ) );
    REQUIRE( g.has( "else goto bb" ) );
    REQUIRE( g.has( "bb1:" ) );
    REQUIRE_FALSE( g.has( "bb0:" ) ); // the entry falls through from the opening brace
}

TEST_CASE( "emit_kir_writes_loops_as_back_edges", "[codegen][kir]" )
{
    Generated g( "i32 f( i32 n ) { i32 s = 0; while ( s < n ) { s = s + 1; } return s; }\ni32 main() { return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    // A back edge is a goto to a lower-numbered block. No labels are generated for the loop itself:
    // break and continue were already edges by the time this ran.
    REQUIRE( g.has( "goto bb1;" ) );
}

// The lowerer already inserted §6.4's conversions as Cast rvalues, so nothing here re-derives them
// and no operand carries a cast of its own.
TEST_CASE( "emit_kir_leaves_conversions_to_the_lowerer", "[codegen][kir]" )
{
    SECTION( "a widening operand is a cast statement, not a cast expression" )
    {
        Generated g( "i64 f( u8 a, i64 b ) { return a + b; }\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "= ( int64_t ) kl_a_1;" ) );
    }

    SECTION( "and operands already at one type get none" )
    {
        Generated g( "i32 f( i32 a, i32 b ) { return a + b; }\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE_FALSE( g.has( "( int32_t ) kl_a_1" ) );
    }
}

TEST_CASE( "emit_kir_writes_places", "[codegen][kir]" )
{
    SECTION( "a field through a pointer parenthesises the deref" )
    {
        Generated g( "struct N { i32 v; };\ni32 f( N* p ) { return (*p).v; }\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        // `.` binds tighter than unary `*`, so without the parentheses this means *( p.v ).
        REQUIRE( g.has( "( *kl_p_1 )." ) );
    }

    // A global is spelled from its declaration node, exactly as its definition is - which is what
    // makes the two backends agree on the symbol without coordinating.
    SECTION( "a global is spelled by its declaration" )
    {
        Generated g( "i32 counter = 1;\ni32 f() { return counter; }\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        const std::string name = "kl_counter_";

        REQUIRE( g.has( name + "2 = 1;" ) );    // the definition
        REQUIRE( g.has( "= " + name + "2;" ) ); // the read, same symbol
    }
}

// KIR keeps Assign total, so a call returning nothing still assigns - to a local C cannot declare.
// One rule here is the price of not having an optional destination in every consumer.
TEST_CASE( "emit_kir_drops_the_destination_of_a_void_call", "[codegen][kir]" )
{
    Generated g( "void nothing() { return; }\ni32 main() { nothing(); return 0; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    // Two spaces: the argument list is empty and the format keeps its padding. The AST backend
    // spells it the same way, which is what matters - the two need to agree in behaviour, and here
    // they happen to agree in text as well.
    REQUIRE( g.has( "kl__nothing__(  );" ) );
    REQUIRE_FALSE( g.has( "void kl_t" ) ); // no void local is declared
    REQUIRE( g.has( "void kl__nothing__( void )" ) );
}

// §12: `extern` "should mean what C++'s `extern \"C\"` means, including suppressing mangling" -
// `kl__abs__i32` would not link against anything. Spelling::function is the single place a C name
// is produced, so the prototype and the call site cannot disagree.
TEST_CASE( "emit_kir_leaves_an_extern_unmangled", "[codegen][kir][extern]" )
{
    Generated g( "extern i32 abs( i32 v );\ni32 main() { i32 n = 0; unsafe { n = abs( 1 ); } return n; }" );

    INFO( g.c );
    REQUIRE( g.clean() );

    SECTION( "the prototype carries the C name" )
    {
        REQUIRE( g.has( "int32_t abs( int32_t );" ) );
        REQUIRE_FALSE( g.has( "kl__abs" ) );
    }

    SECTION( "and so does the call" )
    {
        REQUIRE( g.has( "= abs( " ) );
    }

    SECTION( "an ordinary function beside it is still mangled" )
    {
        REQUIRE( g.has( "kl__main__" ) );
    }
}

// An extern never becomes a KIR function, so its declaration cannot come from the same loop the
// other prototypes do - it is emitted from the AST, and forgetting it would produce C that does
// not compile rather than C that is wrong.
TEST_CASE( "emit_kir_declares_every_extern", "[codegen][kir][extern]" )
{
    SECTION( "one that is never called is still declared" )
    {
        Generated g( "extern i32 rand();\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "int32_t rand( void );" ) );
    }

    SECTION( "several" )
    {
        Generated g( "extern i32 abs( i32 v );\nextern i32 rand();\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "int32_t abs( int32_t );" ) );
        REQUIRE( g.has( "int32_t rand( void );" ) );
    }

    SECTION( "no body is emitted for it" )
    {
        Generated g( "extern i32 abs( i32 v );\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        // A definition would be `int32_t abs( int32_t kl_... )` followed by a brace. The
        // declaration ends in a semicolon and nothing else mentions the name.
        REQUIRE( g.c.find( "abs" ) == g.c.rfind( "abs" ) );
    }

    SECTION( "a pointer signature spells C pointers" )
    {
        Generated g( "extern u8* reserve( u64 n );\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "uint8_t* reserve( uint64_t );" ) );
    }

    SECTION( "the declarations precede the definitions that call them" )
    {
        Generated g( "extern i32 abs( i32 v );\ni32 main() { i32 n = 0; unsafe { n = abs( 1 ); } return n; }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        // C needs the declaration first, and the emitter's order is what guarantees it.
        REQUIRE( g.c.find( "int32_t abs( int32_t );" ) < g.c.find( "int32_t kl__main__( void )\n{" ) );
    }
}

// A `ref` parameter travels as an address (D32), which is exactly C's `int*` - so the modes are
// how an extern spells the signatures a real C library has. Pinned because the prototype, the call
// site and the KIR return slot all derive it separately and must agree.
TEST_CASE( "emit_kir_spells_an_extern_binding_as_a_pointer", "[codegen][kir][extern]" )
{
    SECTION( "a ref parameter" )
    {
        Generated g( "extern void bump( ref i32 v );\ni32 main() { i32 x = 1; unsafe { bump( ref x ); } return x; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "void bump( int32_t* );" ) );
    }

    SECTION( "an out parameter" )
    {
        Generated g( "extern void init( out i32 v );\ni32 main() { i32 x; unsafe { init( out x ); } return x; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "void init( int32_t* );" ) );
    }

    SECTION( "a const ref return" )
    {
        Generated g( "extern const ref i32 peek();\ni32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "int32_t* peek( void );" ) );
    }
}

// The element type gives the size, which is the whole reason `alloc` is a keyword: no `sizeof` is
// written in Keel and none can be got wrong.
TEST_CASE( "emit_kir_writes_an_allocation", "[codegen][kir][alloc]" )
{
    SECTION( "a struct" )
    {
        Generated g( "struct N { i32 v; };\ni32 main() { unsafe { N* n = alloc<N>(); free( n ); } return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "( struct kl__N* ) kl_rt_alloc( sizeof( struct kl__N ) )" ) );
    }

    SECTION( "a builtin" )
    {
        Generated g( "i32 main() { unsafe { i32* n = alloc<i32>(); free( n ); } return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "( int32_t* ) kl_rt_alloc( sizeof( int32_t ) )" ) );
    }

    SECTION( "a pointer" )
    {
        Generated g( "i32 main() { unsafe { i32** n = alloc<i32*>(); free( n ); } return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "( int32_t** ) kl_rt_alloc( sizeof( int32_t* ) )" ) );
    }

    SECTION( "free is a statement of its own, with no destination" )
    {
        // void is not a type C can declare a local of, so the release has nowhere to be assigned -
        // the same rule a void call already goes through.
        Generated g( "i32 main() { unsafe { i32* n = alloc<i32>(); free( n ); } return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl_rt_free( kl_n_1 );" ) );
        REQUIRE_FALSE( g.has( "= kl_rt_free" ) );
    }

    SECTION( "neither runs a constructor or a destructor" )
    {
        // `alloc` is memory, not an object. Pinned in the backend as well as the checker because
        // this is where a future `Owned<T>` would have to start emitting the calls.
        Generated g( "class C { i32 x; C( i32 v ) { x = v; } ~C() { } };\n"
                     "i32 main() { unsafe { C* p = alloc<C>(); free( p ); } return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        // The destructor is still *defined* - `C` has one - but nothing in main calls it.
        const std::size_t body = g.c.find( "int32_t kl__main__( void )\n{" );

        REQUIRE( body != std::string::npos );
        REQUIRE( g.c.find( "kl__C__dtor(", body ) == std::string::npos );
        REQUIRE( g.c.find( "ctor", body ) == std::string::npos );
    }
}

// The bug this replaced could never fire: it scanned the top-level declarations for an Alloc_expr,
// which lives inside a function body. Every program came out claiming it used no runtime, and the
// emitted C called kl_rt_alloc undeclared.
TEST_CASE( "emit_kir_declares_the_runtime_when_it_is_used", "[codegen][kir][alloc]" )
{
    const std::string_view alloc_prototype = "void* kl_rt_alloc( size_t );";
    const std::string_view free_prototype  = "void  kl_rt_free( void* );";

    SECTION( "a program that allocates gets both" )
    {
        Generated g( "i32 main() { unsafe { i32* n = alloc<i32>(); free( n ); } return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( alloc_prototype ) );
        REQUIRE( g.has( free_prototype ) );
    }

    SECTION( "one that only allocates still gets both" )
    {
        Generated g( "i32 main() { i32* n = nullptr; unsafe { n = alloc<i32>(); } return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( alloc_prototype ) );
        REQUIRE( g.has( free_prototype ) );
    }

    SECTION( "one that only frees still gets both" )
    {
        Generated g( "i32 main() { i32* n = nullptr; unsafe { free( n ); } return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( alloc_prototype ) );
        REQUIRE( g.has( free_prototype ) );
    }

    SECTION( "one that does neither gets neither" )
    {
        // Not cosmetic: emitting them unconditionally would churn every golden in the corpus for
        // programs that never touch the heap.
        Generated g( "i32 main() { i32 x = 1; return x; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE_FALSE( g.has( "kl_rt_" ) );
    }

    SECTION( "an allocation buried several levels down is still found" )
    {
        // The shape the old scan missed. An alloc is never a top-level declaration.
        Generated g( "i32 main()\n"
                     "{\n"
                     "    i32 i = 0;\n"
                     "    unsafe\n"
                     "    {\n"
                     "        while( i < 2 )\n"
                     "        {\n"
                     "            if( i == 0 )\n"
                     "            {\n"
                     "                i32* n = alloc<i32>();\n"
                     "                free( n );\n"
                     "            }\n"
                     "            i = i + 1;\n"
                     "        }\n"
                     "    }\n"
                     "    return 0;\n"
                     "}" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( alloc_prototype ) );
    }

    SECTION( "an allocation in a function other than main is found" )
    {
        Generated g( "i32* make() { i32* n = nullptr; unsafe { n = alloc<i32>(); } return n; }\n"
                     "i32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( alloc_prototype ) );
    }

    SECTION( "one in a method is found" )
    {
        Generated g( "class C { i32* p; C() { unsafe { p = alloc<i32>(); } } ~C() { unsafe { free( p ); } } };\n"
                     "i32 main() { C c = C(); return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( alloc_prototype ) );
    }

    SECTION( "the declarations precede every use" )
    {
        Generated g( "i32 main() { unsafe { i32* n = alloc<i32>(); free( n ); } return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        // C needs them first, and the emitter's ordering in run() is the only thing that says so.
        REQUIRE( g.c.find( alloc_prototype ) < g.c.find( "kl_rt_alloc( sizeof" ) );
        REQUIRE( g.c.find( free_prototype ) < g.c.find( "kl_rt_free( kl_" ) );
    }
}

// One generic is emitted once per set of type arguments, and a `Node_id` no longer identifies a
// function - two call sites resolve to the same declaration and must reach different code.
// A binding mode on a generic parameter. The callee's parameter type is written in *its*
// declaration, so lowering an argument has to substitute with the callee's bindings - a caller has
// no `T` of its own, and reaching for one asserts.
TEST_CASE( "emit_kir_substitutes_a_generic_binding_mode", "[codegen][kir][generic]" )
{
    SECTION( "a `ref` parameter becomes a pointer to the substituted type" )
    {
        Generated g( "void bump<T>( ref T a ) { }\ni32 main() { i32 x = 5; bump<i32>( ref x ); return x; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "int32_t*" ) );
    }

    SECTION( "written through" )
    {
        Generated g( "void set_to<T>( ref T a, T v ) where T : Copyable { a = v; }\n"
                     "i32 main() { i32 x = 1; set_to<i32>( ref x, 12 ); return x; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
    }

    SECTION( "an `out` parameter" )
    {
        Generated g( "void init<T>( out T a, T v ) where T : Copyable { a = v; }\n"
                     "i32 main() { i32 x; init<i32>( out x, 13 ); return x; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
    }

    SECTION( "a `const ref` return" )
    {
        Generated g( "const ref T peek<T>( const ref T a ) { return a; }\n"
                     "i32 main() { i32 x = 5; return peek<i32>( x ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );
    }
}

// A generic is an ordinary function once instantiated, so it may call anything - and what it calls
// is resolved against the call site's own instantiation rather than the enclosing one's.
// A generic aggregate is a family of C structs rather than one, and which of them exist is not
// something any single declaration says. The set is every closed instantiation the table holds by
// the time lowering has finished, and the order is by-value containment - both of which the walk in
// emitted_struct_order answers, because the checker has gone before some of them are even created.
TEST_CASE( "emit_kir_emits_one_struct_per_instantiation", "[codegen][kir][generic][aggregate]" )
{
    const std::string_view box = "struct Box<T> where T : Copyable { T v; };\n";

    SECTION( "two type arguments are two C structs" )
    {
        Generated g( std::string( box ) + "i32 main() { Box<i32> a; Box<f64> b; a.v = 1; b.v = 1.5; return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "struct kl__Box__I3i32E" ) );
        REQUIRE( g.has( "struct kl__Box__I3f64E" ) );
    }

    SECTION( "each field is spelled at its own argument" )
    {
        Generated g( std::string( box ) + "i32 main() { Box<i32> a; Box<f64> b; a.v = 1; b.v = 1.5; return 0; }" );

        INFO( g.c );
        REQUIRE( g.has( "int32_t kl_v_" ) );
        REQUIRE( g.has( "double kl_v_" ) );

        // And never the declared type, which has no C spelling at all.
        REQUIRE_FALSE( g.has( " T kl_v_" ) );
    }

    SECTION( "the template itself is emitted not at all" )
    {
        // The mirror of a generic function: `Box<T>` is what instances are made from, and is no
        // more a C struct than `id<T>` is a C function.
        Generated g( std::string( box ) + "i32 main() { Box<i32> a; a.v = 1; return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE_FALSE( g.has( "struct kl__Box;" ) );
        REQUIRE_FALSE( g.has( "struct kl__Box\n" ) );
    }

    SECTION( "a declared but never instantiated generic emits nothing" )
    {
        Generated g( std::string( box ) + "i32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE_FALSE( g.has( "kl__Box" ) );
    }

    SECTION( "what a struct holds is defined before it" )
    {
        // C needs the inner definition first for a by-value member, which is the whole reason the
        // order is a post-order rather than the order things were interned in.
        Generated g(
            std::string( box ) + "struct Holder { Box<i32> b; };\n"
                                 "i32 main() { Holder h; h.b.v = 1; return 0; }"
        );

        INFO( g.c );
        REQUIRE( g.clean() );

        const std::size_t inner = g.c.find( "struct kl__Box__I3i32E\n{" );
        const std::size_t outer = g.c.find( "struct kl__Holder\n{" );

        REQUIRE( inner != std::string::npos );
        REQUIRE( outer != std::string::npos );
        REQUIRE( inner < outer );
    }

    SECTION( "an instantiation reached only through a field is still emitted" )
    {
        // `Holder<i32>` is what the source names; `Box<i32>` is what its field becomes, and nothing
        // else in the program writes it down. Interned by the order walk itself.
        Generated g(
            std::string( box ) + "struct Holder<T> where T : Copyable { Box<T> b; };\n"
                                 "i32 main() { Holder<i32> h; h.b.v = 1; return 0; }"
        );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "struct kl__Box__I3i32E" ) );
        REQUIRE( g.has( "struct kl__Holder__I3i32E" ) );
    }

    SECTION( "nested instantiations, which need `>>` to have parsed" )
    {
        Generated g( std::string( box ) + "i32 main() { Box<Box<i32>> b; b.v.v = 1; return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "struct kl__Box__I3BoxI3i32EE" ) );
        REQUIRE( g.has( "struct kl__Box__I3i32E" ) );
    }

    SECTION( "a generic list, which is what the forward declarations are for" )
    {
        Generated g( "class Node<T> where T : Copyable { T v; Node<T>* next; };\n"
                     "i32 main() { Node<i32> n; n.v = 1; return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "struct kl__Node__I3i32E;" ) ); // the forward declaration
        REQUIRE( g.has( "struct kl__Node__I3i32E* kl_next_" ) );
    }

    SECTION( "a non-generic aggregate is spelled exactly as it was" )
    {
        Generated g( "struct Point { i32 x; i32 y; };\ni32 main() { Point p; p.x = 1; return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "struct kl__Point" ) );
        REQUIRE_FALSE( g.has( "kl__Point__I" ) );
    }
}

TEST_CASE( "emit_kir_lets_a_generic_call_other_functions", "[codegen][kir][generic]" )
{
    SECTION( "an ordinary function" )
    {
        Generated g( "i32 twice( i32 v ) { return v + v; }\n"
                     "i32 use<T>( T a ) { return twice( 4 ); }\n"
                     "i32 main() { return use<bool>( true ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl__twice__3i32" ) );
    }

    SECTION( "arithmetic on a `T` is emitted at the substituted type" )
    {
        // The operands are recorded as `T`, and the type the operation happens *at* is derived from
        // them - so deriving it without substituting first carries a `T` into the emitter, which
        // has no C spelling for one. Only reachable since a literal could adopt `T`: before that,
        // no generic body could do arithmetic at all.
        Generated g( "T f<T>( T a ) where T : Copyable & Integral { return a + 1; }\n"
                     "i32 main() { return f<i32>( 6 ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "int32_t kl__f__3i32( int32_t" ) );
    }

    SECTION( "an integer literal adopting a floating `T` comes out of the integer pool" )
    {
        // `1` is stored as an integer whatever type it ends up with, and the conversion happens
        // where the checker's decision becomes a value. Reading it as a float instead finds an
        // unrelated entry, silently, because the index is usually in range.
        Generated g( "T f<T>( T a ) where T : Copyable & Floating { return a + 1; }\n"
                     "i32 main() { f64 x = f<f64>( 1.5 ); return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "double kl__f__3f64( double" ) );
    }

    SECTION( "a numeric bound carries `Copyable`, so the parameter travels by value" )
    {
        // Nothing but a builtin can satisfy `Numeric` today and every builtin copies, so a numeric
        // parameter need not be borrowed. Visible only here: the checker's answer is a mangled name
        // and a C signature, and `Tp` is what a borrow looks like in both.
        Generated g( "void f<T>( T a ) where T : Integral { }\ni32 main() { f<i32>( 1 ); return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        // The C signature rather than the symbol: an instantiation is named by its type arguments
        // alone, so the convention is no longer visible in the name - which is the right place for
        // it to be invisible, and the wrong place to test it from.
        REQUIRE( g.has( "void kl__f__3i32( int32_t )" ) );
        REQUIRE_FALSE( g.has( "void kl__f__3i32( int32_t* )" ) );
    }

    SECTION( "a bound that does not carry `Copyable` borrows, and a constant is materialised" )
    {
        // `Equatable` is deliberately left borrowing - a string compares and owns - so this is the
        // shape that has a literal arriving at a by-address parameter. A constant has no address,
        // so it goes into a temporary and that is what travels.
        Generated g( "void f<T>( T a ) where T : Equatable { }\ni32 main() { f<i32>( 1 ); return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "void kl__f__3i32( int32_t* )" ) );
    }

    SECTION( "at its caller's own parameter, which no call site ever wrote" )
    {
        // `f<i32>` needs `g<i32>`, and nothing in the source names it: the call inside `f` writes
        // `g<T>`, which is an edge in the generic call graph rather than an instance. The checker's
        // list is a seed, and the set of functions to emit is its closure under that graph.
        Generated g( "T inner<T>( T a ) where T : Copyable { return a; }\n"
                     "T outer<T>( T a ) where T : Copyable { return inner<T>( a ); }\n"
                     "i32 main() { return outer<i32>( 7 ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl__outer__3i32" ) );
        REQUIRE( g.has( "kl__inner__3i32" ) );

        // And not the template it was written as.
        REQUIRE_FALSE( g.has( "kl__inner__T__T" ) );
    }

    SECTION( "the closure follows each instance separately" )
    {
        // Two instances of the outer generic reach two different instances of the inner one. A
        // single pass over the checker's list would emit neither.
        Generated g( "T inner<T>( T a ) where T : Copyable { return a; }\n"
                     "T outer<T>( T a ) where T : Copyable { return inner<T>( a ); }\n"
                     "i32 main() { f64 d = outer<f64>( 1.5 ); return outer<i32>( 7 ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl__inner__3i32" ) );
        REQUIRE( g.has( "kl__inner__3f64" ) );
    }

    SECTION( "a generic reached only through another is still emitted" )
    {
        // Three deep, so the worklist has to find something the round before also only discovered.
        Generated g( "T third<T>( T a ) where T : Copyable { return a; }\n"
                     "T second<T>( T a ) where T : Copyable { return third<T>( a ); }\n"
                     "T first<T>( T a ) where T : Copyable { return second<T>( a ); }\n"
                     "i32 main() { return first<i32>( 7 ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl__third__3i32" ) );
    }

    SECTION( "an uninstantiated generic contributes nothing" )
    {
        // The edge exists, but nothing seeds it - so neither end is emitted, and asking the emitter
        // for a C spelling of `T` never arises.
        Generated g( "void inner<T>( T a ) where T : Copyable { }\n"
                     "void outer<T>( T a ) where T : Copyable { inner<T>( a ); }\n"
                     "i32 main() { return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE_FALSE( g.has( "kl__inner" ) );
        REQUIRE_FALSE( g.has( "kl__outer" ) );
    }

    SECTION( "a type built from the parameter, one level and no cycle" )
    {
        // `f<i32>` needs `g<i32*>`. Legal because nothing leads back: the set is two instances and
        // it closes.
        Generated g( "void inner<T>( T a ) where T : Copyable { }\n"
                     "void outer<T>( T a ) where T : Copyable { T* p = &a; inner<T*>( p ); }\n"
                     "i32 main() { i32 x = 1; outer<i32>( x ); return 0; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl__inner__P3i32" ) );
    }

    SECTION( "another generic, at a different type" )
    {
        Generated g( "T id<T>( T a ) where T : Copyable { return a; }\n"
                     "i32 relay<T>( T a ) where T : Copyable { return id<i32>( 6 ); }\n"
                     "i32 main() { return relay<bool>( true ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        // The inner call is `id<i32>` whatever `relay` was instantiated at.
        REQUIRE( g.has( "kl__id__3i32" ) );
        REQUIRE( g.has( "kl__relay__4bool" ) );
    }
}

// Substitution is structural, so a parameter reached through a type constructor is bound too.
TEST_CASE( "emit_kir_substitutes_through_type_constructors", "[codegen][kir][generic]" )
{
    SECTION( "a struct type argument" )
    {
        Generated g( "struct P { i32 v; };\nT id<T>( T a ) where T : Copyable { return a; }\n"
                     "i32 main() { P p = P { 7 }; P q = id<P>( p ); return q.v; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl__id__1P" ) );
    }

    SECTION( "a pointer type argument" )
    {
        Generated g( "T id<T>( T a ) where T : Copyable { return a; }\n"
                     "i32 main() { i32 x = 9; i32* p = &x; i32* q = id<i32*>( p ); return *q; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl__id__P3i32" ) );
    }

    SECTION( "an enum type argument" )
    {
        Generated g( "enum E { A, B };\nT id<T>( T a ) where T : Copyable { return a; }\n"
                     "i32 main() { E e = id<E>( E::B ); switch( e ) { case E::A: return 0; case E::B: return 1; } }" );

        INFO( g.c );
        REQUIRE( g.clean() );
    }
}

TEST_CASE( "emit_kir_emits_one_function_per_instantiation", "[codegen][kir][generic]" )
{
    SECTION( "two types, two functions, two names" )
    {
        Generated g( "T id<T>( T a ) where T : Copyable { return a; }\n"
                     "i32 main() { i32 a = id<i32>( 1 ); bool b = id<bool>( true ); return a; }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl__id__3i32" ) );
        REQUIRE( g.has( "kl__id__4bool" ) );
    }

    SECTION( "each call site names the one it meant" )
    {
        Generated g( "T id<T>( T a ) where T : Copyable { return a; }\n"
                     "i32 main() { i32 a = id<i32>( 1 ); bool b = id<bool>( true ); return a; }" );

        INFO( g.c );
        REQUIRE( g.has( "= kl__id__3i32( 1 )" ) );
        REQUIRE( g.has( "= kl__id__4bool( true )" ) );
    }

    SECTION( "the same type twice is one function" )
    {
        Generated g( "T id<T>( T a ) where T : Copyable { return a; }\ni32 main() { return id<i32>( 3 ) + id<i32>( 4 ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );

        // Once as a prototype and once as a definition, and no more.
        std::size_t definitions = 0;
        std::size_t at          = g.c.find( "int32_t kl__id__3i32( int32_t kl_" );

        while( at != std::string::npos )
        {
            definitions += 1;
            at = g.c.find( "int32_t kl__id__3i32( int32_t kl_", at + 1 );
        }

        REQUIRE( definitions == 1 );
    }

    SECTION( "the generic itself is never emitted" )
    {
        // It has no code of its own, and emitting it would ask `T` for a C spelling.
        Generated g( "T id<T>( T a ) where T : Copyable { return a; }\ni32 main() { return id<i32>( 1 ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE_FALSE( g.has( "kl__id__( " ) );
    }

    SECTION( "several parameters" )
    {
        Generated g( "T pick<T, U>( T a, U b ) where T : Copyable, where U : Copyable { return a; }\n"
                     "i32 main() { return pick<i32, bool>( 1, true ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "kl__pick__3i32_4bool" ) );
    }

    SECTION( "the parameter and return types are the substituted ones" )
    {
        // The declaration still says `T`; only the lowered function knows `i32`. A `Parameter`
        // reaching here trips Spelling::type's assert, so this passing is the proof.
        Generated g( "T id<T>( T a ) where T : Copyable { return a; }\ni32 main() { return id<i32>( 1 ); }" );

        INFO( g.c );
        REQUIRE( g.has( "int32_t kl__id__3i32( int32_t );" ) );
    }

    SECTION( "a non-generic program is unchanged" )
    {
        Generated g( "i32 f( i32 a ) { return a; }\ni32 main() { return f( 1 ); }" );

        INFO( g.c );
        REQUIRE( g.clean() );
        REQUIRE( g.has( "int32_t kl__f__3i32( int32_t );" ) );
    }
}

} // namespace keel
#endif
