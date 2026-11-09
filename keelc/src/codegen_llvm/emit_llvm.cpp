// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/emit_llvm.h"
#include <fmt/format.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
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
#include <array>
#include <memory>
#include <unordered_map>
#include "codegen/failure.h"
#include "codegen/layout.h"
#include "codegen_c/spelling.h"
#include "ir/vtables.h"

namespace keel
{
namespace
{

// What the backend cannot emit, as the sentence the driver prints.
struct Refusal
{
    std::string what;
};

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

    // The SysV x86-64 C ABI, for Keel's own calls too: an aggregate of at most 16 bytes travels as
    // its eightbytes while registers last, else by `byval`, and returns through `sret` when larger.
    void                      classify( Type_id type, u64 at, std::array<bool, 2>& integer ) const;
    std::vector<llvm::Type*>  eightbytes( Type_id type ) const;
    std::vector<llvm::Value*> split( Type_id type, llvm::Value* from, const std::vector<llvm::Type*>& parts );
    void                      join( Type_id type, std::span<llvm::Value* const> parts, llvm::Value* to );
    std::pair<llvm::FunctionType*, llvm::AttributeList>
                    signature( Type_id result, std::span<const Type_id> parameters, bool diverges );
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
    llvm::Value* temporary( Type_id type, u64 bytes = 0 ); // `bytes`: a padded size instead

    llvm::Value* operand( const Operand& operand );
    llvm::Value* constant( Literal_id literal, Type_id type ) const;
    llvm::Value* rvalue( const Rvalue& value );
    llvm::Value* binary( const Rvalue& value );
    llvm::Value* unary( const Rvalue& value );
    llvm::Value* cast( const Rvalue& value );
    llvm::Value* address_of( const Rvalue& value );
    llvm::Value* call( const Rvalue& value, llvm::Value* result );
    llvm::Value* allocate( const Rvalue& value, Span span );
    void         assign_memory( const Rvalue& value, const Place& place );

    llvm::Constant*       c_string( const std::string& text );
    llvm::GlobalVariable* global( Node_id declaration, Type_id owner );
    llvm::Constant*       initial( const Constant_value& value, Type_id type );
    llvm::Function*       defined( const std::string& name );
    llvm::Constant*       vtable( Type_id implementer, Type_id interface );

    const std::vector<Function>& functions_;
    const Description&           program_;
    Types&                       types_;
    const Literal_pool&          literals_;
    const Source_manager&        sm_;
    const Interner&              interner_;
    const Spelling               spelling_;
    const Layout                 layout_;
    const std::vector<Vtable>    vtables_;

    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module>      module_;
    mutable llvm::IRBuilder<>          builder_; // mutable for its type getters

    std::unordered_map<std::string, llvm::Constant*> strings_;

    // The function being defined.
    const Function*                current_ = nullptr;
    llvm::Function*                defined_ = nullptr;
    llvm::BasicBlock*              entry_   = nullptr; // its allocas
    std::vector<llvm::BasicBlock*> blocks_;
    std::vector<llvm::Value*>      locals_;  // each local's address, null for one holding nothing
    Span                           span_ {}; // the statement being emitted
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
      vtables_( vtables_needed( types, functions ) ),
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

std::pair<llvm::FunctionType*, llvm::AttributeList>
Llvm_emitter::signature( Type_id result, std::span<const Type_id> parameters, bool diverges )
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
    u64                             integers = 6;
    u64                             floats   = 8;

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
        const std::vector<llvm::Type*> parts = eightbytes( result );
        if( !parts.empty() )
        {
            returned = parts.size() == 1 ? parts[0] : llvm::StructType::get( context, parts );
            break;
        }
        --integers;
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
            if( u64& left = types.back()->isFloatingPointTy() ? floats : integers; left > 0 )
            {
                --left;
            }
            break;
        case Held::Pair:
            types.push_back( value_type( parameter ) );
            integers -= std::min<u64>( integers, 2 );
            break;
        case Held::Memory:
        {
            const std::vector<llvm::Type*> parts = eightbytes( parameter );
            const u64                      wide =
                static_cast<u64>( std::ranges::count_if( parts, []( llvm::Type* part ) { return part->isIntegerTy(); } ) );
            if( !parts.empty() && wide <= integers && parts.size() - wide <= floats )
            {
                integers -= wide;
                floats -= parts.size() - wide;
                for( llvm::Type* part : parts )
                {
                    types.push_back( part );
                    attributes.emplace_back();
                }
                continue;
            }
            types.push_back( builder_.getPtrTy() );
            passed.addByValAttr( memory_type( parameter ) );
            passed.addAlignmentAttr( align( parameter ) );
            break;
        }
        }
        attributes.push_back( llvm::AttributeSet::get( context, passed ) );
    }

    llvm::AttrBuilder whole( context );
    whole.addAttribute( llvm::Attribute::NoUnwind );
    if( diverges )
    {
        whole.addAttribute( llvm::Attribute::NoReturn );
    }

    return {
        llvm::FunctionType::get( returned, types, false ),
        llvm::AttributeList::get(
            context, llvm::AttributeSet::get( context, whole ), llvm::AttributeSet::get( context, returns ), attributes
        )
    };
}

llvm::Function*
Llvm_emitter::declare( std::string_view name, Type_id result, std::span<const Type_id> parameters, bool diverges )
{
    const auto [type, attributes] = signature( result, parameters, diverges );
    llvm::Function* function      = llvm::Function::Create( type, llvm::GlobalValue::ExternalLinkage, name, *module_ );
    if( function->getName() != llvm::StringRef( name.data(), name.size() ) )
    {
        malformed( fmt::format( "two functions named `{}`", name ) );
    }
    function->setAttributes( attributes );
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
        if( parameter && kind == Held::Memory && argument->hasByValAttr() )
        {
            locals_[i.v] = &*argument++; // the callee's own copy
            continue;
        }

        llvm::AllocaInst* slot = builder_.CreateAlloca(
            memory_type( local.type ),
            nullptr,
            local.name.is_valid() ? std::string( interner_.text( local.name ) ) : fmt::format( "t{}", i.v )
        );
        slot->setAlignment( align( local.type ) );
        locals_[i.v] = slot;

        if( parameter && kind == Held::Memory )
        {
            std::vector<llvm::Value*> parts;
            for( u64 n = eightbytes( local.type ).size(); n > 0; --n )
            {
                parts.push_back( &*argument++ );
            }
            join( local.type, parts, slot );
        }
        else if( parameter )
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
        span_                = statement.span;

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
        if( held( type ) == Held::Nothing || defined_->hasStructRetAttr() )
        {
            builder_.CreateRetVoid();
            return;
        }
        if( held( type ) == Held::Memory )
        {
            const std::vector<llvm::Value*> parts = split( type, locals_[k_return_slot.v], eightbytes( type ) );
            llvm::Value*                    whole = parts[0];
            if( parts.size() > 1 )
            {
                whole = llvm::PoisonValue::get( defined_->getReturnType() );
                for( u32 i = 0; i < parts.size(); ++i )
                {
                    whole = builder_.CreateInsertValue( whole, parts[i], i );
                }
            }
            builder_.CreateRet( whole );
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
    Type_id      type = place.is_global() ? types_.type_of( place.global ) : current_->locals[place.local].type;
    llvm::Value* at   = place.is_global() ? global( place.global, place.owner ) : locals_[place.local.v];
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

// Marks each eightbyte that holds anything but a float.
void Llvm_emitter::classify( Type_id type, u64 at, std::array<bool, 2>& integer ) const
{
    const Type& described = table().get( type );
    const u64   size      = layout_.size( type );
    switch( described.kind )
    {
    case Type_kind::Float:
        return;
    case Type_kind::Struct:
    case Type_kind::Closure:
        for( const Held_field& field : program_.fields( type ) )
        {
            classify( field.type, at + layout_.offset( type, field.declaration ), integer );
        }
        return;
    case Type_kind::Array:
        for( u64 i = 0; i < described.count; ++i )
        {
            classify( described.element, at + i * layout_.size( described.element ), integer );
        }
        return;
    default: // a payload enum or union is refused at an extern, so all of it counts as integer
        for( u64 byte = at; byte < at + size; byte += 8 - byte % 8 )
        {
            integer[byte / 8] = true;
        }
    }
}

// An `i64` or `double` per eightbyte of an aggregate passed in registers; none when it is in memory.
std::vector<llvm::Type*> Llvm_emitter::eightbytes( Type_id type ) const
{
    const u64 size = layout_.size( type );
    if( size == 0 || size > 16 )
    {
        return {};
    }
    std::array<bool, 2> integer {};
    classify( type, 0, integer );
    std::vector<llvm::Type*> parts;
    for( u64 i = 0; i * 8 < size; ++i )
    {
        parts.push_back( integer[i] ? builder_.getInt64Ty() : builder_.getDoubleTy() );
    }
    return parts;
}

// A whole number of eightbytes, so the last part never reads or writes past the aggregate.
std::vector<llvm::Value*> Llvm_emitter::split( Type_id type, llvm::Value* from, const std::vector<llvm::Type*>& parts )
{
    if( layout_.size( type ) % 8 != 0 )
    {
        llvm::Value* padded = temporary( type, 8 * parts.size() );
        copy( type, padded, from );
        from = padded;
    }
    std::vector<llvm::Value*> values;
    for( u64 i = 0; i < parts.size(); ++i )
    {
        values.push_back( builder_.CreateAlignedLoad(
            parts[i], offset( from, 8 * i ), llvm::Align( std::min<u64>( 8, layout_.align( type ) ) )
        ) );
    }
    return values;
}

void Llvm_emitter::join( Type_id type, std::span<llvm::Value* const> parts, llvm::Value* to )
{
    llvm::Value* into = layout_.size( type ) % 8 != 0 ? temporary( type, 8 * parts.size() ) : to;
    for( u64 i = 0; i < parts.size(); ++i )
    {
        builder_.CreateAlignedStore(
            parts[i], offset( into, 8 * i ), llvm::Align( std::min<u64>( 8, layout_.align( type ) ) )
        );
    }
    if( into != to )
    {
        copy( type, to, into );
    }
}

// Storage for one statement's aggregate, in the entry block.
llvm::Value* Llvm_emitter::temporary( Type_id type, u64 bytes )
{
    llvm::IRBuilder<> at( entry_, entry_->begin() );
    llvm::AllocaInst* slot =
        at.CreateAlloca( bytes == 0 ? memory_type( type ) : llvm::ArrayType::get( builder_.getInt8Ty(), bytes ) );
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
    case Rvalue_kind::Indirect_call:
    {
        llvm::Value* result = call( value, nullptr );
        return result->getType()->isVoidTy() ? nullptr : result;
    }
    case Rvalue_kind::Overflows:
    {
        const bool   signed_ = is_signed( value.a.type );
        const auto   id      = value.op == Token_kind::Plus
                                   ? ( signed_ ? llvm::Intrinsic::sadd_with_overflow : llvm::Intrinsic::uadd_with_overflow )
                               : value.op == Token_kind::Minus
                                   ? ( signed_ ? llvm::Intrinsic::ssub_with_overflow : llvm::Intrinsic::usub_with_overflow )
                                   : ( signed_ ? llvm::Intrinsic::smul_with_overflow : llvm::Intrinsic::umul_with_overflow );
        llvm::Value* a       = operand( value.a );
        return builder_.CreateExtractValue( builder_.CreateIntrinsic( id, { a->getType() }, { a, operand( value.b ) } ), 1 );
    }
    case Rvalue_kind::Bits:
        return builder_.CreateBitCast( operand( value.a ), value_type( value.type ) );
    case Rvalue_kind::Run_of:
    {
        llvm::Value* run = llvm::PoisonValue::get( value_type( value.type ) );
        run              = builder_.CreateInsertValue( run, operand( value.a ), 0 );
        return builder_.CreateInsertValue( run, operand( value.b ), 1 );
    }
    case Rvalue_kind::Run_data:
    case Rvalue_kind::Interface_self:
        return builder_.CreateExtractValue( operand( value.a ), 0 );
    case Rvalue_kind::Run_size:
        return builder_.CreateExtractValue( operand( value.a ), 1 );
    case Rvalue_kind::Allocate:
        return allocate( value, span_ );
    // A `Shape*` frees the object it carries, not the reference.
    case Rvalue_kind::Release:
    {
        llvm::Value* freed = operand( value.a );
        if( table().is_interface_pointer( value.a.type ) )
        {
            freed = builder_.CreateExtractValue( freed, 0 );
        }
        builder_.CreateCall( runtime( "kl_rt_free", builder_.getVoidTy(), { builder_.getPtrTy() }, false ), { freed } );
        return nullptr;
    }
    case Rvalue_kind::Function_address:
        return defined( spelling_.function( value.callee, value.type_arguments ) );
    case Rvalue_kind::Field_offset:
        return llvm::ConstantInt::get(
            value_type( value.type ), layout_.offset( table().get( value.type ).arguments[0], value.callee )
        );
    case Rvalue_kind::Field_read:
        return load( value.type, builder_.CreateGEP( builder_.getInt8Ty(), operand( value.a ), operand( value.b ) ) );
    case Rvalue_kind::Literal_bytes:
        return c_string( std::string( literals_.string( value.a.constant ) ) );
    case Rvalue_kind::Make_interface:
    {
        llvm::Value* reference = llvm::PoisonValue::get( value_type( value.type ) );
        reference              = builder_.CreateInsertValue( reference, operand( value.a ), 0 );
        return builder_.CreateInsertValue(
            reference, vtable( table().get( value.a.type ).element, table().get( value.type ).element ), 1
        );
    }
    // Slot 0 drops; each requirement follows in its interface's order.
    case Rvalue_kind::Vtable_slot:
    {
        const Type_id               interface = table().get( value.a.type ).element;
        const std::vector<Node_id>& members   = program_[table().get( interface ).declaration].members;
        const auto                  slot     = static_cast<u64>( std::ranges::find( members, value.callee ) - members.begin() );
        llvm::Value*                table_at = builder_.CreateExtractValue( operand( value.a ), 1 );
        return builder_.CreateAlignedLoad( builder_.getPtrTy(), offset( table_at, 8 * ( 1 + slot ) ), llvm::Align( 8 ) );
    }
    }
    return nullptr;
}

// kl_rt_alloc( size, file, line, message ), or kl_rt_alloc_many with the count first.
llvm::Value* Llvm_emitter::allocate( const Rvalue& value, Span span )
{
    llvm::Type* const         pointer = builder_.getPtrTy();
    llvm::Type* const         word    = builder_.getInt64Ty();
    llvm::Type* const         line    = builder_.getInt32Ty();
    std::vector<llvm::Value*> arguments {
        builder_.getInt64( layout_.size( table().get( value.type ).element ) ),
        c_string( sm_.file( span.file ).path ),
        builder_.getInt32( sm_.line_col( span.file, span.start ).line ),
        c_string( "out of memory: " + as_written( sm_, span ) ),
    };
    if( !value.a.type.is_valid() )
    {
        return builder_.CreateCall( runtime( "kl_rt_alloc", pointer, { word, pointer, line, pointer }, false ), arguments );
    }
    arguments.insert( arguments.begin(), builder_.CreateIntCast( operand( value.a ), word, is_signed( value.a.type ) ) );
    return builder_.CreateCall(
        runtime( "kl_rt_alloc_many", pointer, { word, word, pointer, line, pointer }, false ), arguments
    );
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
    llvm::FunctionType* signed_as = nullptr;
    llvm::AttributeList attributes;
    llvm::Value*        target = nullptr;
    if( value.kind == Rvalue_kind::Call )
    {
        llvm::Function* function = callee( value );
        signed_as                = function->getFunctionType();
        attributes               = function->getAttributes();
        target                   = function;
    }
    else
    {
        std::vector<Type_id> parameters;
        for( const Operand& argument : value.arguments )
        {
            parameters.push_back( operand_type( argument ) );
        }
        std::tie( signed_as, attributes ) = signature( value.type, parameters, false );
        target                            = operand( value.a );
    }
    const bool returns_memory = attributes.hasParamAttr( 0, llvm::Attribute::StructRet );

    // A discarded aggregate still needs somewhere to land.
    if( result == nullptr && returns_memory )
    {
        llvm::IRBuilder<> at( entry_, entry_->begin() );
        llvm::AllocaInst* slot = at.CreateAlloca( attributes.getParamStructRetType( 0 ) );
        slot->setAlignment( attributes.getParamAlignment( 0 ).valueOrOne() );
        result = slot;
    }

    std::vector<llvm::Value*> arguments;
    if( returns_memory )
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
        if( held( type ) != Held::Memory )
        {
            arguments.push_back( operand( argument ) );
        }
        else if( attributes.hasParamAttr( static_cast<u32>( arguments.size() ), llvm::Attribute::ByVal ) )
        {
            arguments.push_back( address( argument.place ) );
        }
        else
        {
            for( llvm::Value* part : split( type, address( argument.place ), eightbytes( type ) ) )
            {
                arguments.push_back( part );
            }
        }
    }

    if( arguments.size() != signed_as->getNumParams() )
    {
        malformed( fmt::format( "a call of `{}` with {} arguments", target->getName().str(), arguments.size() ) );
    }

    llvm::CallInst* made = builder_.CreateCall( signed_as, target, arguments );
    made->setAttributes( attributes );
    if( result != nullptr && !returns_memory )
    {
        std::vector<llvm::Value*> parts = { made };
        if( auto* pair = llvm::dyn_cast<llvm::StructType>( made->getType() ) )
        {
            parts.clear();
            for( u32 i = 0; i < pair->getNumElements(); ++i )
            {
                parts.push_back( builder_.CreateExtractValue( made, i ) );
            }
        }
        join( value.type, parts, result );
    }
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
    case Rvalue_kind::Indirect_call:
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

// A function this module defines or declares, by name.
llvm::Function* Llvm_emitter::defined( const std::string& name )
{
    llvm::Function* function = module_->getFunction( name );
    if( function == nullptr )
    {
        malformed( fmt::format( "the address of `{}`, which has no body here", name ) );
    }
    return function;
}

// Each global once, at its layout's alignment; a generic's static is weak, as in C.
llvm::GlobalVariable* Llvm_emitter::global( Node_id declaration, Type_id owner )
{
    const std::string name = spelling_.global( declaration, owner );
    if( llvm::GlobalVariable* found = module_->getNamedGlobal( name ) )
    {
        return found;
    }

    const Type_id                       type  = types_.type_of( declaration );
    const std::optional<Constant_value> value = types_.constant_of( declaration );
    llvm::Constant* const init = value ? initial( *value, type ) : llvm::Constant::getNullValue( memory_type( type ) );
    auto*                 made = new llvm::GlobalVariable(
        *module_,
        init->getType(),
        false,
        owner.is_valid() ? llvm::GlobalValue::WeakAnyLinkage : llvm::GlobalValue::ExternalLinkage,
        init,
        name
    );
    made->setAlignment( align( type ) );
    return made;
}

// The checker's value as bytes at `type`'s layout: an aggregate is a packed struct of its parts and
// zero padding, sized as the layout says.
llvm::Constant* Llvm_emitter::initial( const Constant_value& value, Type_id type )
{
    const Type& described = table().get( type );
    const auto  packed    = [&]( std::vector<std::pair<u64, llvm::Constant*>> parts )
    {
        const llvm::DataLayout&      layout = module_->getDataLayout();
        std::vector<llvm::Constant*> elements;
        u64                          at     = 0;
        const auto                   pad_to = [&]( u64 to )
        {
            if( to > at )
            {
                elements.push_back( llvm::Constant::getNullValue( llvm::ArrayType::get( builder_.getInt8Ty(), to - at ) ) );
                at = to;
            }
        };
        for( const auto& [offset, part] : parts )
        {
            pad_to( offset );
            elements.push_back( part );
            at += layout.getTypeStoreSize( part->getType() );
        }
        pad_to( layout_.size( type ) );
        return llvm::ConstantStruct::getAnon( *context_, elements, true );
    };

    switch( value.kind )
    {
    case Constant_value::Kind::Array:
    case Constant_value::Kind::Fill:
    {
        const u64                                    size = layout_.size( described.element );
        std::vector<std::pair<u64, llvm::Constant*>> parts;
        for( u64 i = 0; i < described.count; ++i )
        {
            const Constant_value& part = value.kind == Constant_value::Kind::Fill ? value.elements[0] : value.elements[i];
            parts.emplace_back( i * size, initial( part, described.element ) );
        }
        return packed( std::move( parts ) );
    }
    // `str`'s fields by name: its bytes and their count.
    case Constant_value::Kind::Text:
    {
        std::vector<std::pair<u64, llvm::Constant*>> parts;
        for( const Held_field& field : program_.fields( type ) )
        {
            parts.emplace_back(
                layout_.offset( type, field.declaration ),
                program_[field.declaration].name == "data"
                    ? c_string( value.text )
                    : llvm::ConstantInt::get( memory_type( field.type ), value.text.size() )
            );
        }
        return packed( std::move( parts ) );
    }
    case Constant_value::Kind::Float:
        return llvm::ConstantFP::get( value_type( type ), value.floating );
    case Constant_value::Kind::Address:
        return program_[value.target].kind == Declared::Global ? static_cast<llvm::Constant*>( global( value.target, {} ) )
                                                               : defined( spelling_.function( value.target ) );
    case Constant_value::Kind::Integer:
        break;
    }

    if( held( type ) != Held::Scalar || !memory_type( type )->isIntegerTy() )
    {
        return llvm::Constant::getNullValue( memory_type( type ) ); // a null pointer, the only one there is
    }
    const auto magnitude = llvm::ConstantInt::get( memory_type( type ), value.magnitude );
    return value.negative ? llvm::ConstantExpr::getNeg( magnitude ) : magnitude;
}

// The implementer's drop function, then its thunk for each requirement.
llvm::Constant* Llvm_emitter::vtable( Type_id implementer, Type_id interface )
{
    const std::string name = spelling_.vtable( implementer, interface );
    if( llvm::GlobalVariable* found = module_->getNamedGlobal( name ) )
    {
        return found;
    }

    const auto made = std::ranges::find_if(
        vtables_, [&]( const Vtable& v ) { return v.implementer == implementer && v.interface == interface; }
    );
    if( made == vtables_.end() )
    {
        malformed( fmt::format( "`{}`, a vtable no function needs", name ) );
    }

    std::vector<llvm::Constant*> slots { defined( spelling_.drop_function( implementer ) ) };
    for( const Node_id requirement : made->requirements )
    {
        slots.push_back( defined( spelling_.thunk( implementer, interface, requirement ) ) );
    }
    llvm::Constant* init = llvm::ConstantArray::get( llvm::ArrayType::get( builder_.getPtrTy(), slots.size() ), slots );
    auto*           table_global =
        new llvm::GlobalVariable( *module_, init->getType(), true, llvm::GlobalValue::InternalLinkage, init, name );
    table_global->setAlignment( llvm::Align( 8 ) );
    return table_global;
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
