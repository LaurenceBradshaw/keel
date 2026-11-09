// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/emit_llvm.h"
#include <fmt/format.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <memory>
#include <unordered_map>
#include "codegen/failure.h"
#include "codegen/layout.h"
#include "codegen_c/spelling.h"

namespace keel
{
namespace
{

// What the backend cannot emit, as the sentence the driver prints.
struct Refusal
{
    std::string what;
};

[[noreturn]] void not_yet( std::string_view what )
{
    throw Refusal { fmt::format( "the LLVM backend cannot emit {} yet", what ) };
}

[[noreturn]] void malformed( std::string_view what )
{
    throw Refusal { fmt::format( "internal error: the LLVM backend met {}", what ) };
}

// How a value of a type is held: nowhere, as one LLVM value, as a two-word reference, or only in
// memory, where an aggregate is its layout's bytes.
enum class Held : u8
{
    Nothing,
    Scalar,
    Pair,
    Memory
};

class Llvm_emitter
{
public:
    Llvm_emitter(
        const std::vector<Function>& functions,
        const Description&           program,
        Types&                       types,
        const Literal_pool&          literals,
        const Source_manager&        sm,
        const Interner&              interner,
        const Module_graph&          modules
    );

    llvm::Module& run();

private:
    const Type_table& table() const;

    Held        held( Type_id type ) const;
    bool        is_void( Type_id type ) const;
    bool        is_signed( Type_id type ) const;
    llvm::Type* value_type( Type_id type ) const;
    llvm::Type* memory_type( Type_id type ) const;
    llvm::Align align( Type_id type ) const;

    // Keel's convention: an aggregate travels by `byval` and returns through `sret`.
    llvm::Function* declare( std::string_view name, Type_id result, std::span<const Type_id> parameters, bool diverges );
    llvm::Function* declare( const Function& function );
    llvm::Function* callee( const Rvalue& value );
    llvm::Function* runtime( std::string_view name, llvm::Type* result, std::vector<llvm::Type*> parameters, bool diverges );

    void define( const Function& function, llvm::Function* defined );
    void statement( const Statement& statement );
    void terminator( const Terminator& terminator );
    void panic( const Terminator& terminator );

    Type_id      type_of( const Place& place ) const;
    Type_id      operand_type( const Operand& operand ) const;
    Type_id      field_type( Type_id aggregate, Node_id field ) const;
    llvm::Value* address( const Place& place );
    llvm::Value* offset( llvm::Value* base, u64 bytes );
    llvm::Value* load( Type_id type, llvm::Value* from );
    void         store( Type_id type, llvm::Value* value, llvm::Value* to );
    void         copy( Type_id type, llvm::Value* to, llvm::Value* from );
    llvm::Value* temporary( Type_id type );

    llvm::Value* operand( const Operand& operand );
    llvm::Value* constant( Literal_id literal, Type_id type ) const;
    llvm::Value* rvalue( const Rvalue& value );
    llvm::Value* binary( const Rvalue& value );
    llvm::Value* unary( const Rvalue& value );
    llvm::Value* cast( const Rvalue& value );
    llvm::Value* address_of( const Rvalue& value );
    llvm::Value* call( const Rvalue& value, llvm::Value* result );
    void         assign_memory( const Rvalue& value, const Place& place );

    llvm::Constant* c_string( const std::string& text );

    const std::vector<Function>& functions_;
    const Description&           program_;
    Types&                       types_;
    const Literal_pool&          literals_;
    const Source_manager&        sm_;
    const Interner&              interner_;
    const Spelling               spelling_;
    const Layout                 layout_;

    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module>      module_;
    mutable llvm::IRBuilder<>          builder_; // mutable for its type getters

    std::unordered_map<std::string, llvm::Constant*> strings_;

    // The function being defined.
    const Function*                current_ = nullptr;
    llvm::Function*                defined_ = nullptr;
    llvm::BasicBlock*              entry_   = nullptr; // its allocas
    std::vector<llvm::BasicBlock*> blocks_;
    std::vector<llvm::Value*>      locals_; // each local's address, null for one holding nothing
};

Llvm_emitter::Llvm_emitter(
    const std::vector<Function>& functions,
    const Description&           program,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner,
    const Module_graph&          modules
)
    : functions_( functions ),
      program_( program ),
      types_( types ),
      literals_( literals ),
      sm_( sm ),
      interner_( interner ),
      spelling_( Spelling { program, types, interner, modules, sm } ),
      layout_( program, types.table() ),
      context_( std::make_unique<llvm::LLVMContext>() ),
      module_( std::make_unique<llvm::Module>( "keel", *context_ ) ),
      builder_( *context_ )
{
}

llvm::Module& Llvm_emitter::run()
{
    // Every signature first, so a body may call one defined below it.
    std::vector<llvm::Function*> declared;
    for( const Function& function : functions_ )
    {
        declared.push_back( declare( function ) );
    }

    for( std::size_t i = 0; i < functions_.size(); ++i )
    {
        define( functions_[i], declared[i] );
    }

    return *module_;
}

const Type_table& Llvm_emitter::table() const
{
    return types_.table();
}

Held Llvm_emitter::held( Type_id type ) const
{
    const Type& described = table().get( type );
    switch( described.kind )
    {
    case Type_kind::Void:
    case Type_kind::Never:
        return Held::Nothing;
    case Type_kind::Bool:
    case Type_kind::Int:
    case Type_kind::Float:
    case Type_kind::Many_pointer:
    case Type_kind::Function:
    case Type_kind::Field:
        return Held::Scalar;
    case Type_kind::Pointer:
        return table().is_interface( described.element ) || table().is_run( described.element ) ? Held::Pair : Held::Scalar;
    case Type_kind::Enum:
        return program_[described.declaration].payload ? Held::Memory : Held::Scalar;
    case Type_kind::Closure: // one of no captures is a byte
        return program_.fields( type ).empty() ? Held::Scalar : Held::Memory;
    case Type_kind::Struct:
    case Type_kind::Union:
    case Type_kind::Array:
        return Held::Memory;
    default:
        malformed( fmt::format( "a value of type `{}`", table().name( type ) ) );
    }
}

bool Llvm_emitter::is_void( Type_id type ) const
{
    return table().is_void( type ) || table().is_never( type );
}

bool Llvm_emitter::is_signed( Type_id type ) const
{
    const Type& described = table().get( type );
    return described.kind == Type_kind::Int                                    ? described.is_signed
           : described.kind == Type_kind::Enum && held( type ) == Held::Scalar ? is_signed( described.element )
                                                                               : false;
}

// As a register holds it: `bool` is an `i1`.
llvm::Type* Llvm_emitter::value_type( Type_id type ) const
{
    const Type& described = table().get( type );
    switch( described.kind )
    {
    case Type_kind::Bool:
        return builder_.getInt1Ty();
    case Type_kind::Int:
        return builder_.getIntNTy( described.width );
    case Type_kind::Float:
        return described.width == 32 ? builder_.getFloatTy() : builder_.getDoubleTy();
    case Type_kind::Field:
        return builder_.getInt64Ty();
    case Type_kind::Enum:
        return value_type( described.element );
    case Type_kind::Closure:
        return builder_.getInt8Ty();
    case Type_kind::Pointer:
        if( table().is_interface( described.element ) )
        {
            return llvm::StructType::get( *context_, { builder_.getPtrTy(), builder_.getPtrTy() } );
        }
        if( table().is_run( described.element ) )
        {
            return llvm::StructType::get( *context_, { builder_.getPtrTy(), builder_.getInt64Ty() } );
        }
        return builder_.getPtrTy();
    case Type_kind::Many_pointer:
    case Type_kind::Function:
        return builder_.getPtrTy();
    default:
        malformed( fmt::format( "no register type for `{}`", table().name( type ) ) );
    }
}

// As memory holds it: `bool` is a byte, an aggregate its layout's bytes.
llvm::Type* Llvm_emitter::memory_type( Type_id type ) const
{
    if( table().get( type ).kind == Type_kind::Bool )
    {
        return builder_.getInt8Ty();
    }

    return held( type ) == Held::Memory ? llvm::ArrayType::get( builder_.getInt8Ty(), layout_.size( type ) )
                                        : value_type( type );
}

llvm::Align Llvm_emitter::align( Type_id type ) const
{
    return llvm::Align( layout_.align( type ) );
}

llvm::Function*
Llvm_emitter::declare( std::string_view name, Type_id result, std::span<const Type_id> parameters, bool diverges )
{
    llvm::LLVMContext& context = *context_;

    // C widens a narrow integer or `bool` it passes.
    const auto widened = [&]( llvm::AttrBuilder& attributes, Type_id type )
    {
        const llvm::Type* value = value_type( type );
        if( value->isIntegerTy() && value->getIntegerBitWidth() < 32 )
        {
            attributes.addAttribute( is_signed( type ) ? llvm::Attribute::SExt : llvm::Attribute::ZExt );
        }
    };

    std::vector<llvm::Type*>        types;
    std::vector<llvm::AttributeSet> attributes;
    llvm::Type*                     returned = builder_.getVoidTy();
    llvm::AttrBuilder               returns( context );

    switch( held( result ) )
    {
    case Held::Nothing:
        break;
    case Held::Scalar:
        returned = value_type( result );
        widened( returns, result );
        break;
    case Held::Pair:
        returned = value_type( result );
        break;
    case Held::Memory:
    {
        llvm::AttrBuilder into( context );
        into.addStructRetAttr( memory_type( result ) );
        into.addAlignmentAttr( align( result ) );
        types.push_back( builder_.getPtrTy() );
        attributes.push_back( llvm::AttributeSet::get( context, into ) );
        break;
    }
    }

    for( const Type_id parameter : parameters )
    {
        llvm::AttrBuilder passed( context );
        switch( held( parameter ) )
        {
        case Held::Nothing:
            continue;
        case Held::Scalar:
            types.push_back( value_type( parameter ) );
            widened( passed, parameter );
            break;
        case Held::Pair:
            types.push_back( value_type( parameter ) );
            break;
        case Held::Memory:
            types.push_back( builder_.getPtrTy() );
            passed.addByValAttr( memory_type( parameter ) );
            passed.addAlignmentAttr( align( parameter ) );
            break;
        }
        attributes.push_back( llvm::AttributeSet::get( context, passed ) );
    }

    llvm::AttrBuilder whole( context );
    whole.addAttribute( llvm::Attribute::NoUnwind );
    if( diverges )
    {
        whole.addAttribute( llvm::Attribute::NoReturn );
    }

    llvm::Function* function = llvm::Function::Create(
        llvm::FunctionType::get( returned, types, false ), llvm::GlobalValue::ExternalLinkage, name, *module_
    );
    if( function->getName() != llvm::StringRef( name.data(), name.size() ) )
    {
        malformed( fmt::format( "two functions named `{}`", name ) );
    }

    function->setAttributes( llvm::AttributeList::get(
        context, llvm::AttributeSet::get( context, whole ), llvm::AttributeSet::get( context, returns ), attributes
    ) );
    return function;
}

// From the lowered function, whose locals hold an instance's types already.
llvm::Function* Llvm_emitter::declare( const Function& function )
{
    std::vector<Type_id> parameters;
    for( u32 i = 1; i <= function.parameter_count; ++i )
    {
        parameters.push_back( function.locals[Local_id { i }].type );
    }

    return declare( spelling_.function( function ), function.locals[k_return_slot].type, parameters, function.diverges );
}

llvm::Function* Llvm_emitter::callee( const Rvalue& value )
{
    const bool declared = value.op == Token_kind::Unknown && value.globals == Globals_call::None;
    if( declared && program_[value.callee].requirement )
    {
        malformed( "a call of a requirement, which lower_dispatch makes indirect" );
    }

    const std::string name = spelling_.callee( value );
    if( llvm::Function* found = module_->getFunction( name ) )
    {
        return found;
    }

    if( !declared || !program_[value.callee].is_extern )
    {
        malformed( fmt::format( "a call of `{}`, which has no body here", name ) );
    }

    const Described& described = program_[value.callee];
    for( const Type_id type : described.passed )
    {
        if( held( type ) == Held::Memory )
        {
            not_yet( "an `extern` call passing a struct by value" );
        }
    }
    if( held( described.type ) == Held::Memory )
    {
        not_yet( "an `extern` call returning a struct by value" );
    }

    return declare( name, described.type, described.passed, table().is_never( described.type ) );
}

llvm::Function*
Llvm_emitter::runtime( std::string_view name, llvm::Type* result, std::vector<llvm::Type*> parameters, bool diverges )
{
    if( llvm::Function* found = module_->getFunction( name ) )
    {
        return found;
    }

    llvm::Function* function = llvm::Function::Create(
        llvm::FunctionType::get( result, parameters, false ), llvm::GlobalValue::ExternalLinkage, name, *module_
    );
    function->addFnAttr( llvm::Attribute::NoUnwind );
    if( diverges )
    {
        function->addFnAttr( llvm::Attribute::NoReturn );
    }
    return function;
}

void Llvm_emitter::define( const Function& function, llvm::Function* defined )
{
    current_ = &function;
    defined_ = defined;

    // As the C is compiled: stack probes (§7 rule 7), and unwind tables for a debugger's backtrace.
    defined->addFnAttr( "probe-stack", "inline-asm" );
    defined->setUWTableKind( llvm::UWTableKind::Async );

    entry_ = llvm::BasicBlock::Create( *context_, "entry", defined );
    blocks_.clear();
    for( const Block_id i : function.blocks.ids() )
    {
        blocks_.push_back( llvm::BasicBlock::Create( *context_, fmt::format( "bb{}", i.v ), defined ) );
    }

    builder_.SetInsertPoint( entry_ );
    locals_.assign( function.locals.size(), nullptr );

    llvm::Function::arg_iterator argument = defined->arg_begin();
    if( defined->hasStructRetAttr() )
    {
        locals_[k_return_slot.v] = &*argument++;
    }

    // Every local an alloca in the entry block; a parameter's arrives into its own.
    for( const Local_id i : function.locals.ids() )
    {
        const Local& local = function.locals[i];
        const Held   kind  = held( local.type );
        if( kind == Held::Nothing || locals_[i.v] != nullptr )
        {
            continue;
        }

        const bool parameter = i.v >= 1 && i.v <= function.parameter_count;
        if( parameter && kind == Held::Memory )
        {
            locals_[i.v] = &*argument++; // `byval`: the callee's own copy
            continue;
        }

        llvm::AllocaInst* slot = builder_.CreateAlloca(
            memory_type( local.type ),
            nullptr,
            local.name.is_valid() ? std::string( interner_.text( local.name ) ) : fmt::format( "t{}", i.v )
        );
        slot->setAlignment( align( local.type ) );
        locals_[i.v] = slot;

        if( parameter )
        {
            store( local.type, &*argument++, slot );
        }
    }
    builder_.CreateBr( blocks_[0] );

    for( const Block_id i : function.blocks.ids() )
    {
        builder_.SetInsertPoint( blocks_[i.v] );
        for( const Statement& each : function.statements_of( i ) )
        {
            statement( each );
        }
        terminator( function.blocks[i].terminator );
    }
}

void Llvm_emitter::statement( const Statement& statement )
{
    switch( statement.kind )
    {
    case Statement_kind::Assign:
    {
        const Rvalue& value  = current_->rvalue( statement );
        const Type_id target = type_of( statement.place );

        if( held( target ) == Held::Memory )
        {
            assign_memory( value, statement.place );
            return;
        }

        // Into a `void` place, only what the rvalue does.
        llvm::Value* result = rvalue( value );
        if( result != nullptr && held( target ) != Held::Nothing )
        {
            store( target, result, address( statement.place ) );
        }
        return;
    }

    case Statement_kind::Drop:
    {
        llvm::BasicBlock* dropped = nullptr;
        if( statement.drop_flag.is_valid() )
        {
            const Type_id     flag = current_->locals[statement.drop_flag].type;
            llvm::BasicBlock* drop = llvm::BasicBlock::Create( *context_, "drop", defined_ );
            dropped                = llvm::BasicBlock::Create( *context_, "dropped", defined_ );
            builder_.CreateCondBr( load( flag, locals_[statement.drop_flag.v] ), drop, dropped );
            builder_.SetInsertPoint( drop );
        }

        const Type_id type = type_of( statement.place );

        // `*s` of a `Shape*` has no place of its own: its vtable's first slot drops the object.
        if( table().is_interface( type ) )
        {
            Place reference = statement.place;
            reference.num_projections -= 1;
            llvm::Value* pair   = load( type_of( reference ), address( reference ) );
            llvm::Value* self   = builder_.CreateExtractValue( pair, 0 );
            llvm::Value* vtable = builder_.CreateExtractValue( pair, 1 );
            llvm::Value* slot   = builder_.CreateAlignedLoad( builder_.getPtrTy(), vtable, llvm::Align( 8 ) );
            builder_.CreateCall(
                llvm::FunctionType::get( builder_.getVoidTy(), { builder_.getPtrTy() }, false ), slot, { self }
            );
        }
        else
        {
            const std::string name       = spelling_.destructor_of( type );
            llvm::Function*   destructor = module_->getFunction( name );
            if( destructor == nullptr )
            {
                malformed( fmt::format( "a drop by `{}`, which has no body here", name ) );
            }
            builder_.CreateCall( destructor, { address( statement.place ) } )->setAttributes( destructor->getAttributes() );
        }

        if( dropped != nullptr )
        {
            builder_.CreateBr( dropped );
            builder_.SetInsertPoint( dropped );
        }
        return;
    }

    // Scopes for the drop pass, not instructions.
    case Statement_kind::Storage_live:
    case Statement_kind::Storage_dead:
        return;
    }
}

void Llvm_emitter::terminator( const Terminator& terminator )
{
    switch( terminator.kind )
    {
    case Terminator_kind::Goto:
        builder_.CreateBr( blocks_[terminator.targets[0].v] );
        return;

    case Terminator_kind::Branch:
        builder_.CreateCondBr(
            operand( terminator.condition ), blocks_[terminator.targets[0].v], blocks_[terminator.targets[1].v]
        );
        return;

    // The lowerer put the value in local 0; an aggregate's is the caller's `sret` already.
    case Terminator_kind::Return:
    {
        const Type_id type = current_->locals[k_return_slot].type;
        if( held( type ) == Held::Nothing || held( type ) == Held::Memory )
        {
            builder_.CreateRetVoid();
            return;
        }
        builder_.CreateRet( load( type, locals_[k_return_slot.v] ) );
        return;
    }

    case Terminator_kind::Unreachable:
        builder_.CreateCall( runtime( "kl_rt_unreachable", builder_.getVoidTy(), {}, true ) );
        builder_.CreateUnreachable();
        return;

    case Terminator_kind::Panic:
        panic( terminator );
        builder_.CreateUnreachable();
        return;

    case Terminator_kind::Unset:
        malformed( "a block with no terminator" );
    }
}

// A `str` crosses as its bytes and count, and a `location` as its file's and its line.
void Llvm_emitter::panic( const Terminator& terminator )
{
    llvm::Type* const pointer = builder_.getPtrTy();
    llvm::Type* const word    = builder_.getInt64Ty();
    llvm::Type* const line    = builder_.getInt32Ty();

    const auto field = [&]( llvm::Value* object, Type_id type, std::string_view name, Type_id& held_type )
    {
        const Held_field* found = program_.field_named( type, name );
        if( found == nullptr )
        {
            malformed( fmt::format( "a `{}` with no `{}`", table().name( type ), name ) );
        }
        held_type = found->type;
        return offset( object, layout_.offset( type, found->declaration ) );
    };
    const auto read = [&]( llvm::Value* object, Type_id type, std::string_view name )
    {
        Type_id      held_type {};
        llvm::Value* at = field( object, type, name, held_type );
        return load( held_type, at );
    };

    if( terminator.failure == Failure::Message )
    {
        const Type_id str     = terminator.message.type;
        llvm::Value*  message = address( terminator.message.place );
        llvm::Value*  data    = read( message, str, "data" );
        llvm::Value*  size    = read( message, str, "size" );

        if( terminator.at.type.is_valid() )
        {
            llvm::Value* at = address( terminator.at.place );
            Type_id      file_type {};
            llvm::Value* file = field( at, terminator.at.type, "file", file_type );
            builder_.CreateCall(
                runtime( "kl_rt_panic_at", builder_.getVoidTy(), { pointer, word, line, pointer, word }, true ),
                { read( file, file_type, "data" ),
                  read( file, file_type, "size" ),
                  builder_.CreateIntCast( read( at, terminator.at.type, "line" ), line, false ),
                  data,
                  size }
            );
            return;
        }

        builder_.CreateCall(
            runtime( "kl_rt_panic_message", builder_.getVoidTy(), { pointer, line, pointer, word }, true ),
            { c_string( sm_.file( terminator.span.file ).path ),
              builder_.getInt32( sm_.line_col( terminator.span.file, terminator.span.start ).line ),
              data,
              size }
        );
        return;
    }

    builder_.CreateCall(
        runtime( "kl_rt_panic", builder_.getVoidTy(), { pointer, line, pointer }, true ),
        { c_string( sm_.file( terminator.span.file ).path ),
          builder_.getInt32( sm_.line_col( terminator.span.file, terminator.span.start ).line ),
          c_string( std::string( failure_prefix( terminator.failure ) ) + as_written( sm_, terminator.span ) ) }
    );
}

Type_id Llvm_emitter::type_of( const Place& place ) const
{
    return program_.type_of( table(), *current_, place );
}

Type_id Llvm_emitter::operand_type( const Operand& operand ) const
{
    return operand.has_place() ? type_of( operand.place ) : operand.type;
}

Type_id Llvm_emitter::field_type( Type_id aggregate, Node_id field ) const
{
    for( const Held_field& held_field : program_.fields( aggregate ) )
    {
        if( held_field.declaration == field )
        {
            return held_field.type;
        }
    }
    malformed( fmt::format( "a field `{}` lacks", table().name( aggregate ) ) );
}

// A place's address, walked as Description::type_of walks its type.
llvm::Value* Llvm_emitter::address( const Place& place )
{
    if( place.is_global() )
    {
        not_yet( "a global" );
    }

    Type_id      type = current_->locals[place.local].type;
    llvm::Value* at   = locals_[place.local.v];
    if( at == nullptr )
    {
        malformed( "a place in a local that holds nothing" );
    }

    for( const Projection& projection : current_->projections_of( place ) )
    {
        switch( projection.kind )
        {
        case Projection_kind::Field:
            at   = offset( at, layout_.offset( type, projection.field ) );
            type = field_type( type, projection.field );
            break;
        case Projection_kind::Deref:
            if( held( type ) == Held::Pair )
            {
                malformed( "a place through an interface or run reference" );
            }
            at   = builder_.CreateAlignedLoad( builder_.getPtrTy(), at, align( type ) );
            type = table().get( type ).element;
            break;
        case Projection_kind::Tag: // first, at 0
            type = table().get( type ).element;
            break;
        case Projection_kind::Member:
            at   = offset( at, layout_.payload( type ) );
            type = projection.member;
            break;
        case Projection_kind::Index:
        {
            const Type_id element = table().get( type ).element;
            llvm::Value*  index   = load( current_->locals[projection.index].type, locals_[projection.index.v] );
            at                    = builder_.CreateGEP(
                builder_.getInt8Ty(), at, builder_.CreateMul( index, builder_.getInt64( layout_.size( element ) ) )
            );
            type = element;
            break;
        }
        }
    }

    return at;
}

llvm::Value* Llvm_emitter::offset( llvm::Value* base, u64 bytes )
{
    return bytes == 0 ? base : builder_.CreateConstGEP1_64( builder_.getInt8Ty(), base, bytes );
}

llvm::Value* Llvm_emitter::load( Type_id type, llvm::Value* from )
{
    llvm::Value* value = builder_.CreateAlignedLoad( memory_type( type ), from, align( type ) );
    return table().get( type ).kind == Type_kind::Bool ? builder_.CreateTrunc( value, builder_.getInt1Ty() ) : value;
}

void Llvm_emitter::store( Type_id type, llvm::Value* value, llvm::Value* to )
{
    if( table().get( type ).kind == Type_kind::Bool )
    {
        value = builder_.CreateZExt( value, builder_.getInt8Ty() );
    }
    builder_.CreateAlignedStore( value, to, align( type ) );
}

void Llvm_emitter::copy( Type_id type, llvm::Value* to, llvm::Value* from )
{
    builder_.CreateMemCpy( to, align( type ), from, align( type ), layout_.size( type ) );
}

// Storage for one statement's aggregate, in the entry block.
llvm::Value* Llvm_emitter::temporary( Type_id type )
{
    llvm::IRBuilder<> at( entry_, entry_->begin() );
    llvm::AllocaInst* slot = at.CreateAlloca( memory_type( type ) );
    slot->setAlignment( align( type ) );
    return slot;
}

// A scalar or a pair; null for a `void`.
llvm::Value* Llvm_emitter::operand( const Operand& operand )
{
    switch( operand.kind )
    {
    case Operand_kind::Constant:
        return is_void( operand.type ) ? nullptr : constant( operand.constant, operand.type );
    case Operand_kind::Copy:
    case Operand_kind::Move:
    {
        const Type_id type = type_of( operand.place );
        if( held( type ) == Held::Nothing )
        {
            return nullptr;
        }
        if( held( type ) == Held::Memory )
        {
            malformed( "an aggregate read as one value" );
        }
        return load( type, address( operand.place ) );
    }
    case Operand_kind::Debug:
        malformed( "a --debug operand left unsettled" );
    }
    return nullptr;
}

// The bits as the C emitter writes them, so both backends hold the same value.
llvm::Value* Llvm_emitter::constant( Literal_id literal, Type_id type ) const
{
    const Type& described = table().get( type );
    switch( described.kind )
    {
    case Type_kind::Bool:
        return builder_.getInt1( literals_.integer( literal ) != 0 );
    case Type_kind::Float:
        return described.width == 32 ? llvm::ConstantFP::get( builder_.getFloatTy(), literals_.narrow( literal ) )
                                     : llvm::ConstantFP::get( builder_.getDoubleTy(), literals_.floating( literal ) );
    case Type_kind::Int:
    case Type_kind::Enum: // a variant is its index
    {
        llvm::Type* integer = value_type( type );
        return llvm::ConstantInt::get(
            integer, truncate( literals_.integer( literal ), narrow_cast<u8>( integer->getIntegerBitWidth() ) )
        );
    }
    case Type_kind::Pointer: // null, a `Shape*`'s object and vtable both
    case Type_kind::Many_pointer:
    case Type_kind::Closure: // one of no captures
        return llvm::Constant::getNullValue( value_type( type ) );
    default:
        malformed( fmt::format( "a constant of type `{}`", table().name( type ) ) );
    }
}

llvm::Value* Llvm_emitter::rvalue( const Rvalue& value )
{
    switch( value.kind )
    {
    case Rvalue_kind::Use:
        return operand( value.a );
    case Rvalue_kind::Binary:
        return binary( value );
    case Rvalue_kind::Unary:
        return unary( value );
    case Rvalue_kind::Cast:
        return cast( value );
    case Rvalue_kind::Address_of:
        return address_of( value );
    case Rvalue_kind::Call:
    {
        llvm::Value* result = call( value, nullptr );
        return result->getType()->isVoidTy() ? nullptr : result;
    }
    case Rvalue_kind::Overflows:
        not_yet( "an overflow check" );
    case Rvalue_kind::Bits:
        not_yet( "a float's bits" );
    case Rvalue_kind::Run_of:
    case Rvalue_kind::Run_data:
    case Rvalue_kind::Run_size:
        not_yet( "a run" );
    case Rvalue_kind::Allocate:
        not_yet( "an allocation" );
    case Rvalue_kind::Release:
        not_yet( "a release" );
    case Rvalue_kind::Function_address:
        not_yet( "a function's address" );
    case Rvalue_kind::Indirect_call:
        not_yet( "a call through a pointer" );
    case Rvalue_kind::Field_offset:
    case Rvalue_kind::Field_read:
        not_yet( "a field offset" );
    case Rvalue_kind::Literal_bytes:
        not_yet( "a string literal" );
    case Rvalue_kind::Make_interface:
    case Rvalue_kind::Vtable_slot:
    case Rvalue_kind::Interface_self:
        not_yet( "an interface" );
    }
    return nullptr;
}

// C's operator at the operands' type: no `nsw` or `nuw` (§7.1), and KIR has guarded the divisor and the count.
llvm::Value* Llvm_emitter::binary( const Rvalue& value )
{
    const Type_id type = operand_type( value.a );
    llvm::Value*  a    = operand( value.a );
    llvm::Value*  b    = operand( value.b );

    // Two `Shape*` are the same pointer when they hold the same object.
    if( held( type ) == Held::Pair )
    {
        a = builder_.CreateExtractValue( a, 0 );
        b = builder_.CreateExtractValue( b, 0 );
    }

    // A `T[*]` moves by whole elements; a `void[*]`'s are all at its start.
    if( table().is_many_pointer( type ) && ( value.op == Token_kind::Plus || value.op == Token_kind::Minus ) )
    {
        llvm::Value* count = builder_.CreateIntCast( b, builder_.getInt64Ty(), is_signed( operand_type( value.b ) ) );
        if( value.op == Token_kind::Minus )
        {
            count = builder_.CreateNeg( count );
        }
        const u64 size = layout_.size( table().get( type ).element );
        return builder_.CreateGEP( builder_.getInt8Ty(), a, builder_.CreateMul( count, builder_.getInt64( size ) ) );
    }

    const bool real    = table().is_float( type );
    const bool signed_ = is_signed( type );
    switch( value.op )
    {
    case Token_kind::Plus:
        return real ? builder_.CreateFAdd( a, b ) : builder_.CreateAdd( a, b );
    case Token_kind::Minus:
        return real ? builder_.CreateFSub( a, b ) : builder_.CreateSub( a, b );
    case Token_kind::Star:
        return real ? builder_.CreateFMul( a, b ) : builder_.CreateMul( a, b );
    case Token_kind::Slash:
        return real ? builder_.CreateFDiv( a, b ) : signed_ ? builder_.CreateSDiv( a, b ) : builder_.CreateUDiv( a, b );
    case Token_kind::Percent:
        return real ? builder_.CreateFRem( a, b ) : signed_ ? builder_.CreateSRem( a, b ) : builder_.CreateURem( a, b );
    case Token_kind::Amp:
        return builder_.CreateAnd( a, b );
    case Token_kind::Pipe:
        return builder_.CreateOr( a, b );
    case Token_kind::Caret:
        return builder_.CreateXor( a, b );
    case Token_kind::Less_less:
        return builder_.CreateShl( a, builder_.CreateZExtOrTrunc( b, a->getType() ) );
    case Token_kind::Greater_greater:
        b = builder_.CreateZExtOrTrunc( b, a->getType() );
        return signed_ ? builder_.CreateAShr( a, b ) : builder_.CreateLShr( a, b );
    case Token_kind::Equal_equal:
        return real ? builder_.CreateFCmpOEQ( a, b ) : builder_.CreateICmpEQ( a, b );
    case Token_kind::Bang_equal:
        return real ? builder_.CreateFCmpUNE( a, b ) : builder_.CreateICmpNE( a, b );
    case Token_kind::Less:
        return real      ? builder_.CreateFCmpOLT( a, b )
               : signed_ ? builder_.CreateICmpSLT( a, b )
                         : builder_.CreateICmpULT( a, b );
    case Token_kind::Less_equal:
        return real      ? builder_.CreateFCmpOLE( a, b )
               : signed_ ? builder_.CreateICmpSLE( a, b )
                         : builder_.CreateICmpULE( a, b );
    case Token_kind::Greater:
        return real      ? builder_.CreateFCmpOGT( a, b )
               : signed_ ? builder_.CreateICmpSGT( a, b )
                         : builder_.CreateICmpUGT( a, b );
    case Token_kind::Greater_equal:
        return real      ? builder_.CreateFCmpOGE( a, b )
               : signed_ ? builder_.CreateICmpSGE( a, b )
                         : builder_.CreateICmpUGE( a, b );
    default:
        malformed( fmt::format( "the binary operator `{}`", token_kind_spelling( value.op ) ) );
    }
}

llvm::Value* Llvm_emitter::unary( const Rvalue& value )
{
    llvm::Value* a = operand( value.a );
    switch( value.op )
    {
    case Token_kind::Minus:
        return table().is_float( operand_type( value.a ) ) ? builder_.CreateFNeg( a ) : builder_.CreateNeg( a );
    case Token_kind::Bang:
    case Token_kind::Tilde:
        return builder_.CreateNot( a );
    default:
        malformed( fmt::format( "the unary operator `{}`", token_kind_spelling( value.op ) ) );
    }
}

// C's `( T ) x`: to `bool` is `!= 0`, and an integer widens by its own signedness.
llvm::Value* Llvm_emitter::cast( const Rvalue& value )
{
    const Type_id from = operand_type( value.a );
    llvm::Value*  x    = operand( value.a );
    llvm::Type*   to   = value_type( value.type );
    llvm::Type*   was  = x->getType();

    if( was == to )
    {
        return x;
    }

    const bool to_bool = table().get( value.type ).kind == Type_kind::Bool;
    if( was->isIntegerTy() )
    {
        if( to_bool )
        {
            return builder_.CreateICmpNE( x, llvm::ConstantInt::get( was, 0 ) );
        }
        if( to->isIntegerTy() )
        {
            return builder_.CreateIntCast( x, to, is_signed( from ) );
        }
        if( to->isFloatingPointTy() )
        {
            return is_signed( from ) ? builder_.CreateSIToFP( x, to ) : builder_.CreateUIToFP( x, to );
        }
        if( to->isPointerTy() )
        {
            return builder_.CreateIntToPtr( x, to );
        }
    }

    if( was->isFloatingPointTy() )
    {
        if( to_bool )
        {
            return builder_.CreateFCmpUNE( x, llvm::ConstantFP::get( was, 0.0 ) );
        }
        if( to->isIntegerTy() )
        {
            return is_signed( value.type ) ? builder_.CreateFPToSI( x, to ) : builder_.CreateFPToUI( x, to );
        }
        if( to->isFloatingPointTy() )
        {
            return builder_.CreateFPCast( x, to );
        }
    }

    if( was->isPointerTy() && to->isIntegerTy() )
    {
        return to_bool ? builder_.CreateIsNotNull( x ) : builder_.CreatePtrToInt( x, to );
    }

    malformed( fmt::format( "a cast from `{}` to `{}`", table().name( from ), table().name( value.type ) ) );
}

llvm::Value* Llvm_emitter::address_of( const Rvalue& value )
{
    // `&(*s)` of a `Shape*` or a `u8[]*` is `s` itself: neither an interface nor a run is a place of its own.
    if( held( value.type ) == Held::Pair )
    {
        Place reference = value.a.place;
        if( reference.num_projections == 0 )
        {
            malformed( "an interface or run reached other than through a pointer" );
        }
        reference.num_projections -= 1;
        return load( type_of( reference ), address( reference ) );
    }

    // Nothing is there to point at, and nothing reads a `void` through it.
    if( is_void( type_of( value.a.place ) ) )
    {
        return llvm::Constant::getNullValue( builder_.getPtrTy() );
    }

    return address( value.a.place );
}

// An aggregate result goes into `result`, which nothing the callee reads may alias.
llvm::Value* Llvm_emitter::call( const Rvalue& value, llvm::Value* result )
{
    llvm::Function* function = callee( value );

    // A discarded aggregate still needs somewhere to land.
    if( result == nullptr && function->hasStructRetAttr() )
    {
        llvm::IRBuilder<> at( entry_, entry_->begin() );
        llvm::AllocaInst* slot = at.CreateAlloca( function->getParamStructRetType( 0 ) );
        slot->setAlignment( function->getParamAlign( 0 ).valueOrOne() );
        result = slot;
    }

    std::vector<llvm::Value*> arguments;
    if( result != nullptr )
    {
        arguments.push_back( result );
    }
    for( const Operand& argument : value.arguments )
    {
        const Type_id type = operand_type( argument );
        if( held( type ) == Held::Nothing )
        {
            continue;
        }
        arguments.push_back( held( type ) == Held::Memory ? address( argument.place ) : operand( argument ) );
    }

    if( arguments.size() != function->arg_size() )
    {
        malformed( fmt::format( "a call of `{}` with {} arguments", function->getName().str(), arguments.size() ) );
    }

    llvm::CallInst* made = builder_.CreateCall( function, arguments );
    made->setAttributes( function->getAttributes() );
    return made;
}

void Llvm_emitter::assign_memory( const Rvalue& value, const Place& place )
{
    const Type_id type = type_of( place );
    switch( value.kind )
    {
    case Rvalue_kind::Use:
    {
        llvm::Value* from = address( value.a.place );
        copy( type, address( place ), from );
        return;
    }
    // Through a temporary: an argument may borrow the place being assigned.
    case Rvalue_kind::Call:
    {
        llvm::Value* result = temporary( type );
        call( value, result );
        copy( type, address( place ), result );
        return;
    }
    default:
        rvalue( value );
        malformed( "an aggregate made by this rvalue" );
    }
}

// A NUL-terminated string for the runtime, each text once.
llvm::Constant* Llvm_emitter::c_string( const std::string& text )
{
    const auto [found, added] = strings_.try_emplace( text, nullptr );
    if( added )
    {
        found->second = builder_.CreateGlobalStringPtr( text, "", 0, module_.get() );
    }
    return found->second;
}

// x86-64 Linux at -O3's machine passes, and none of LLVM's IR passes (§2.2): Keel optimises its own IR.
std::string write_object( llvm::Module& module, const std::filesystem::path& object )
{
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();

    const std::string   triple = "x86_64-pc-linux-gnu";
    std::string         error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget( triple, error );
    if( target == nullptr )
    {
        return "internal error: no x86-64 target in this keelc: " + error;
    }

    const std::unique_ptr<llvm::TargetMachine> machine( target->createTargetMachine(
        triple, "x86-64", "", llvm::TargetOptions {}, llvm::Reloc::PIC_, std::nullopt, llvm::CodeGenOptLevel::Aggressive
    ) );
    module.setTargetTriple( triple );
    module.setDataLayout( machine->createDataLayout() );
    module.setPICLevel( llvm::PICLevel::BigPIC );
    module.setPIELevel( llvm::PIELevel::Large );

    std::error_code      failed;
    llvm::raw_fd_ostream out( object.string(), failed, llvm::sys::fs::OF_None );
    if( failed )
    {
        return fmt::format( "cannot write {}: {}", object.string(), failed.message() );
    }

    llvm::legacy::PassManager passes;
    if( machine->addPassesToEmitFile( passes, out, nullptr, llvm::CodeGenFileType::ObjectFile ) )
    {
        return "internal error: this LLVM cannot write an object file";
    }
    passes.run( module );
    return {};
}

} // namespace

Llvm_output emit_llvm(
    const std::vector<Function>& functions,
    const Description&           program,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner,
    const Module_graph&          modules,
    const std::filesystem::path& object,
    bool                         print
)
{
    Llvm_emitter emitter( functions, program, types, literals, sm, interner, modules );
    Llvm_output  output;

    try
    {
        llvm::Module& module = emitter.run();

        std::string              problems;
        llvm::raw_string_ostream report( problems );
        if( llvm::verifyModule( module, &report ) )
        {
            output.error = "internal error: the LLVM module is malformed:\n" + problems;
        }

        if( print )
        {
            llvm::raw_string_ostream text( output.ir );
            module.print( text, nullptr );
        }

        if( output.error.empty() && !object.empty() )
        {
            output.error = write_object( module, object );
        }
    }
    catch( const Refusal& refusal )
    {
        output.error = refusal.what;
    }

    return output;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "check/pipeline_test_support.h"

namespace keel
{

TEST_CASE( "emit_llvm_writes_a_function_the_verifier_accepts", "[codegen][llvm]" )
{
    Compiled p( "i32 twice( i32 a ) { return a * 2; }\ni32 main() { return twice( 3 ) - 6; }\n", Stage::Elaborated );
    REQUIRE( p.clean() );

    const Description program = describe( p.ast, p.types, p.interner );
    const Llvm_output out     = emit_llvm( p.functions, program, p.types, p.literals, p.sm, p.interner, p.modules, {}, true );

    INFO( out.error );
    INFO( out.ir );
    REQUIRE( out.error.empty() );
    REQUIRE( out.ir.find( "define i32 @kl__5twice__3i32(i32 %0)" ) != std::string::npos );
    REQUIRE( out.ir.find( "mul i32" ) != std::string::npos );
    REQUIRE( out.ir.find( " nsw " ) == std::string::npos );
}

} // namespace keel
#endif
