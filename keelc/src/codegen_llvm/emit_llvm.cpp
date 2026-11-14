// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/emit_llvm.h"
#include <fmt/format.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <algorithm>
#include <array>
#include <memory>
#include <unordered_map>
#include "codegen/failure.h"
#include "codegen/layout.h"
#include "codegen_c/spelling.h"
#include "ir/call_graph.h"
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

const char* triple( Platform platform )
{
    return platform == Platform::Windows ? "x86_64-pc-windows-msvc" : "x86_64-pc-linux-gnu";
}

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
        const Module_graph&          modules,
        const Llvm_options&          options
    );

    llvm::Module& run();

private:
    const Type_table& table() const;
    bool              defines( Node_id declaration ) const;

    Held        held( Type_id type ) const;
    bool        is_void( Type_id type ) const;
    bool        is_signed( Type_id type ) const;
    llvm::Type* value_type( Type_id type ) const;
    llvm::Type* memory_type( Type_id type ) const;
    llvm::Align align( Type_id type ) const;

    // The target's C ABI, for Keel's own calls too (D70). SysV: an aggregate of at most 16 bytes
    // travels as its eightbytes while registers last, else by `byval`, and returns through `sret`
    // when larger. Win64: one of 1, 2, 4 or 8 bytes travels as an integer that wide; any other, an
    // interface reference included, by a pointer to the caller's copy, and returns through `sret`,
    // as a two-word reference does. A run parameter is two words on both.
    void                      classify( Type_id type, u64 at, std::array<bool, 2>& integer ) const;
    std::vector<llvm::Type*>  eightbytes( Type_id type ) const;
    bool                      by_reference( Type_id type ) const;
    std::vector<llvm::Value*> split( Type_id type, llvm::Value* from, const std::vector<llvm::Type*>& parts );
    void                      join( Type_id type, std::span<llvm::Value* const> parts, llvm::Value* to );
    std::pair<llvm::FunctionType*, llvm::AttributeList>
                    signature( Type_id result, std::span<const Type_id> parameters, bool diverges );
    llvm::Function* declare( std::string_view name, Type_id result, std::span<const Type_id> parameters, bool diverges );
    llvm::Function* declare( const Function& function );
    llvm::Function* callee( const Rvalue& value );
    llvm::Function* runtime( std::string_view name, llvm::Type* result, std::vector<llvm::Type*> parameters, bool diverges );

    void define( const Function& function, llvm::Function* defined );
    void check_stack( llvm::Function* defined );

    // DWARF under --debug: Keel's names for functions, locals, types and fields, and its lines.
    llvm::DIFile* file( File_id id );
    llvm::DIType* debug_type( Type_id type );
    void          describe( const Function& function, llvm::Function* defined );
    void          describe_local( Local_id local, llvm::Value* slot );
    void          locate( Span span );

    // Braun et al.'s SSA construction (§2.2) for the locals `promote` chooses. A block is KIR's, and
    // `blocks_.size()` stands for the entry block, where the parameters arrive.
    void           promote( const Function& function );
    llvm::Value*   value_of( Local_id local ); // a promoted local's value, or a load of its slot
    llvm::Value*   read( Local_id local, u32 block );
    llvm::Value*   read_above( Local_id local, u32 block );
    llvm::PHINode* phi( Local_id local, u32 block );
    llvm::Value*   fill( Local_id local, u32 block, llvm::PHINode* phi );
    llvm::Value*   remove_if_trivial( llvm::PHINode* phi );
    void           seal_if_ready( u32 block );
    void           statement( const Statement& statement );
    void           terminator( const Terminator& terminator );
    void           panic( const Terminator& terminator );

    Type_id      type_of( const Place& place ) const;
    Type_id      operand_type( const Operand& operand ) const;
    Type_id      field_type( Type_id aggregate, Node_id field ) const;
    llvm::Value* address( const Place& place );
    llvm::Value* offset( llvm::Value* base, u64 bytes );
    llvm::Value* load( Type_id type, llvm::Value* from );
    void         store( Type_id type, llvm::Value* value, llvm::Value* to );
    void         copy( Type_id type, llvm::Value* to, llvm::Value* from );
    void         copy_parts( Type_id type, llvm::Value* to, llvm::Value* from, u64 at, u64 size );
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
    const std::vector<bool>      deep_;  // D76: each function's stack check at entry
    const Parts*                 parts_; // null emits the whole compile
    const File_id                part_;
    const bool                   klo_;
    const bool                   debug_;
    const u32                    level_;
    const bool                   windows_;

    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module>      module_;
    mutable llvm::IRBuilder<>          builder_; // mutable for its type getters

    std::unordered_map<std::string, llvm::Constant*> strings_;

    std::unique_ptr<llvm::DIBuilder>                   di_; // null without --debug
    llvm::DICompileUnit*                               unit_ = nullptr;
    std::unordered_map<u32, llvm::DIFile*>             files_;
    std::unordered_map<u32, llvm::DIType*>             debug_types_;
    llvm::DISubprogram*                                subprogram_ = nullptr;
    File_id                                            subprogram_file_ {};
    std::unordered_map<u32, llvm::DILexicalBlockFile*> elsewhere_; // the subprogram's lines in another file

    // The function being defined.
    const Function*                current_ = nullptr;
    llvm::Function*                defined_ = nullptr;
    llvm::BasicBlock*              entry_   = nullptr; // its allocas
    std::vector<llvm::BasicBlock*> blocks_;
    std::vector<llvm::Value*>      locals_;  // each local's address, null for one holding nothing or promoted
    Span                           span_ {}; // the statement being emitted

    // Its promotion, by block.
    std::vector<bool>                                             promoted_;
    std::vector<bool>                                             addressed_; // a pointer may reach it
    std::vector<std::vector<u32>>                                 predecessors_;
    std::vector<llvm::BasicBlock*>                                exits_; // where a filled block's terminator is
    std::vector<bool>                                             filled_;
    std::vector<bool>                                             sealed_; // every predecessor filled
    std::vector<std::unordered_map<u32, llvm::WeakTrackingVH>>    current_values_;
    std::vector<std::vector<std::pair<Local_id, llvm::PHINode*>>> incomplete_; // in a block not yet sealed
    std::vector<llvm::PHINode*>                                   filling_;    // phis taking operands now
    u32                                                           block_ = 0;  // the block being filled
};

Llvm_emitter::Llvm_emitter(
    const std::vector<Function>& functions,
    const Description&           program,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner,
    const Module_graph&          modules,
    const Llvm_options&          options
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
      deep_( deep_drops( functions ) ),
      parts_( options.parts ),
      part_( options.part ),
      klo_( options.klo ),
      debug_( options.debug ),
      level_( options.level ),
      windows_( options.platform == Platform::Windows ),
      context_( std::make_unique<llvm::LLVMContext>() ),
      module_( std::make_unique<llvm::Module>( "keel", *context_ ) ),
      builder_( *context_ )
{
    module_->setTargetTriple( triple( options.platform ) );
}

llvm::Module& Llvm_emitter::run()
{
    if( debug_ )
    {
        di_           = std::make_unique<llvm::DIBuilder>( *module_ );
        File_id first = part_;
        for( const Function& function : functions_ )
        {
            if( !first.is_valid() && function.declaration.is_valid() )
            {
                first = program_[function.declaration].span.file;
            }
        }
        unit_ = di_->createCompileUnit(
            llvm::dwarf::DW_LANG_C_plus_plus_14,
            first.is_valid() ? file( first ) : di_->createFile( "keel", "." ),
            "keelc",
            false,
            "",
            0,
            "",
            llvm::DICompileUnit::FullDebug,
            0,
            true,
            false,
            llvm::DICompileUnit::DebugNameTableKind::None // lld's merged index is one gdb rejects
        );
        module_->addModuleFlag( llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION );
        if( windows_ )
        {
            module_->addModuleFlag( llvm::Module::Warning, "CodeView", 1 ); // for a .pdb
        }
        else
        {
            module_->addModuleFlag( llvm::Module::Warning, "Dwarf Version", 4 ); // valgrind reads no DWARF 5 string forms
        }
    }

    // Every signature first, so a body may call one defined below it.
    std::vector<llvm::Function*> declared;
    for( const Function& function : functions_ )
    {
        declared.push_back( declare( function ) );
    }

    // This part's globals, named here or not, as another part may.
    for( const Node_id global : program_.globals() )
    {
        if( defines( global ) )
        {
            this->global( global, {} );
        }
    }

    for( std::size_t i = 0; i < functions_.size(); ++i )
    {
        const Function& function = functions_[i];
        if( parts_ != nullptr && parts_->of( function ) != part_ )
        {
            continue;
        }
        if( klo_ && placed_by_reach( function ) )
        {
            declared[i]->setLinkage( llvm::GlobalValue::WeakODRLinkage );
            declared[i]->setComdat( module_->getOrInsertComdat( declared[i]->getName() ) );
        }
        define( function, declared[i] );
    }

    if( di_ != nullptr )
    {
        di_->finalize();
    }
    return *module_;
}

bool Llvm_emitter::defines( Node_id declaration ) const
{
    return parts_ == nullptr || parts_->of( declaration ) == part_;
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

    // C widens a narrow integer or `bool` it passes; Win64 C only a `bool`.
    const auto widened = [&]( llvm::AttrBuilder& attributes, Type_id type )
    {
        const llvm::Type* value = value_type( type );
        if( value->isIntegerTy() && value->getIntegerBitWidth() < ( windows_ ? 2 : 32 ) )
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
    case Held::Memory:
    {
        if( held( result ) == Held::Pair && !windows_ )
        {
            returned = value_type( result );
            break;
        }
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
        case Held::Memory:
        {
            if( by_reference( parameter ) )
            {
                types.push_back( builder_.getPtrTy() ); // the callee's to change
                break;
            }
            if( held( parameter ) == Held::Pair )
            {
                types.push_back( value_type( parameter ) );
                integers -= std::min<u64>( integers, 2 );
                break;
            }
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
    // Windows probes every large frame already, through __chkstk.
    if( !windows_ )
    {
        defined->addFnAttr( "probe-stack", "inline-asm" );
    }
    defined->setUWTableKind( llvm::UWTableKind::Async );
    if( debug_ )
    {
        defined->addFnAttr( "frame-pointer", "all" ); // gdb finds a prologue's end by its frame setup
    }

    entry_ = llvm::BasicBlock::Create( *context_, "entry", defined );
    blocks_.clear();
    for( const Block_id i : function.blocks.ids() )
    {
        blocks_.push_back( llvm::BasicBlock::Create( *context_, fmt::format( "bb{}", i.v ), defined ) );
    }

    builder_.SetInsertPoint( entry_ );
    // Contraction is D86's permission alone: elsewhere a * b + c rounds twice (§7.1).
    llvm::FastMathFlags flags;
    flags.setAllowContract( function.real_arithmetic );
    builder_.setFastMathFlags( flags );
    describe( function, defined );
    locals_.assign( function.locals.size(), nullptr );

    llvm::Function::arg_iterator argument = defined->arg_begin();
    if( defined->hasStructRetAttr() )
    {
        locals_[k_return_slot.v] = &*argument++;
    }

    promote( function );
    const u32 entry = static_cast<u32>( blocks_.size() );
    predecessors_.assign( entry + 1, {} );
    predecessors_[0].push_back( entry );
    std::vector<Block_id> targets;
    for( const Block_id i : function.blocks.ids() )
    {
        targets.clear();
        successors( function.blocks[i].terminator, targets );
        for( const Block_id target : targets )
        {
            predecessors_[target.v].push_back( i.v );
        }
    }
    exits_.assign( entry + 1, nullptr );
    exits_[entry] = entry_;
    filled_.assign( entry + 1, false );
    filled_[entry] = true;
    sealed_.assign( entry + 1, false );
    sealed_[entry] = true;
    current_values_.assign( entry + 1, {} );
    incomplete_.assign( entry + 1, {} );
    for( u32 i = 0; i < entry; ++i )
    {
        seal_if_ready( i );
    }
    block_ = entry;

    // Every other local an alloca in the entry block; a parameter's arrives into its own.
    for( const Local_id i : function.locals.ids() )
    {
        const Local& local = function.locals[i];
        const Held   kind  = held( local.type );
        if( kind == Held::Nothing || locals_[i.v] != nullptr )
        {
            continue;
        }

        const bool parameter = i.v >= 1 && i.v <= function.parameter_count;
        if( promoted_[i.v] )
        {
            if( parameter )
            {
                current_values_[entry][i.v] = &*argument++;
            }
            continue;
        }
        if( parameter && ( ( kind == Held::Memory && argument->hasByValAttr() ) || by_reference( local.type ) ) )
        {
            locals_[i.v] = &*argument++; // the callee's own copy
            describe_local( i, locals_[i.v] );
            continue;
        }

        llvm::AllocaInst* slot = builder_.CreateAlloca(
            memory_type( local.type ),
            nullptr,
            local.name.is_valid() ? std::string( interner_.text( local.name ) ) : fmt::format( "t{}", i.v )
        );
        slot->setAlignment( align( local.type ) );
        locals_[i.v] = slot;
        describe_local( i, slot );

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
    const bool pointer = function.parameter_count == 1 && table().is_pointer( function.locals[Local_id { 1 }].type );
    if( deep_[static_cast<std::size_t>( &function - functions_.data() )] && pointer &&
        is_void( function.locals[k_return_slot].type ) )
    {
        check_stack( defined );
    }
    else
    {
        builder_.CreateBr( blocks_[0] );
    }

    for( const Block_id i : function.blocks.ids() )
    {
        block_ = i.v;
        builder_.SetInsertPoint( blocks_[i.v] );
        for( const Statement& each : function.statements_of( i ) )
        {
            statement( each );
        }
        locate( function.blocks[i].terminator.span );
        terminator( function.blocks[i].terminator );
        exits_[i.v]  = builder_.GetInsertBlock();
        filled_[i.v] = true;

        targets.clear();
        successors( function.blocks[i].terminator, targets );
        for( const Block_id target : targets )
        {
            seal_if_ready( target.v );
        }
    }
    builder_.SetCurrentDebugLocation( {} );
}

// Under kl_rt_stack_low, the drop runs again on a fresh segment, from its start.
void Llvm_emitter::check_stack( llvm::Function* defined )
{
    llvm::Type*           word = builder_.getInt64Ty();
    llvm::GlobalVariable* low  = module_->getGlobalVariable( "kl_rt_stack_low" );
    if( low == nullptr )
    {
        low = new llvm::GlobalVariable(
            *module_,
            word,
            false,
            llvm::GlobalValue::ExternalLinkage,
            nullptr,
            "kl_rt_stack_low",
            nullptr,
            llvm::GlobalValue::InitialExecTLSModel
        );
    }

    llvm::Value*      here   = builder_.CreatePtrToInt( builder_.CreateStackSave(), word );
    llvm::BasicBlock* deeper = llvm::BasicBlock::Create( *context_, "deeper", defined );
    builder_.CreateCondBr(
        builder_.CreateICmpULT( here, builder_.CreateLoad( word, low ) ),
        deeper,
        blocks_[0],
        llvm::MDBuilder( *context_ ).createBranchWeights( 1, 1 << 20 )
    );

    builder_.SetInsertPoint( deeper );
    llvm::Type* pointer = builder_.getPtrTy();
    builder_.CreateCall(
        runtime( "kl_rt_deeper", builder_.getVoidTy(), { pointer, pointer }, false ), { defined, defined->getArg( 0 ) }
    );
    builder_.CreateRetVoid();
}

llvm::DIFile* Llvm_emitter::file( File_id id )
{
    llvm::DIFile*& found = files_[id.v];
    if( found == nullptr )
    {
        const std::filesystem::path path = std::filesystem::absolute( sm_.file( id ).path ).lexically_normal();
        found = di_->createFile( path.filename().generic_string(), path.parent_path().generic_string() );
    }
    return found;
}

// A function the user did not write (drop glue, a thunk, a guard, a module's globals, the `main`
// that calls Keel's) is the compiler's own, under its symbol.
void Llvm_emitter::describe( const Function& function, llvm::Function* defined )
{
    subprogram_ = nullptr;
    elsewhere_.clear();
    if( di_ == nullptr )
    {
        return;
    }

    std::string    name   = defined->getName().str();
    Span           span   = {};
    llvm::DIScope* scope  = unit_;
    auto           flags  = llvm::DISubprogram::SPFlagDefinition;
    const bool     source = function.declaration.is_valid() && !function.entry && !function.globals.is_valid() &&
                        function.compares == Token_kind::Unknown && !function.thunk_of.is_valid();
    if( source )
    {
        const Described& described = program_[function.declaration];
        const bool       ends      = described.kind == Declared::Enum || described.kind == Declared::Destructor;
        name =
            ( described.aggregate_name.empty() ? "" : described.aggregate_name + "::" ) + ( ends ? "~" : "" ) + described.name;
        span = described.span;
        if( const std::string package = spelling_.owner( function.declaration ).package;
            !package.empty() && package != "prelude" )
        {
            scope = di_->createNameSpace( unit_, package, false );
        }
    }
    if( !source || !span.is_valid() )
    {
        flags |= llvm::DISubprogram::SPFlagLocalToUnit;
    }

    subprogram_file_   = span.is_valid() ? span.file : File_id {};
    llvm::DIFile* in   = span.is_valid() ? file( span.file ) : unit_->getFile();
    const u32     line = span.is_valid() ? sm_.line_col( span.file, span.start ).line : 0;
    subprogram_        = di_->createFunction(
        scope,
        name,
        "", // a mangled name would be what gdb shows
        in,
        line,
        di_->createSubroutineType( di_->getOrCreateTypeArray( {} ) ),
        line,
        source ? llvm::DINode::FlagPrototyped : llvm::DINode::FlagArtificial,
        flags
    );
    defined->setSubprogram( subprogram_ );

    builder_.SetCurrentDebugLocation( {} ); // the entry block's work is the prologue, as clang's is
}

// A named local, parameters numbered from 1.
void Llvm_emitter::describe_local( Local_id local, llvm::Value* slot )
{
    const Local& described = current_->locals[local];
    if( subprogram_ == nullptr || !described.name.is_valid() || current_->entry )
    {
        return;
    }
    llvm::DIType* type = debug_type( described.type );
    if( type == nullptr )
    {
        return;
    }

    const Span        span = described.span.is_valid() ? described.span : Span {};
    const u32         line = span.is_valid() ? sm_.line_col( span.file, span.start ).line : 0;
    llvm::DIFile*     in   = span.is_valid() ? file( span.file ) : subprogram_->getFile();
    const std::string name( interner_.text( described.name ) );

    llvm::DILocalVariable* variable = local.v >= 1 && local.v <= current_->parameter_count
                                          ? di_->createParameterVariable( subprogram_, name, local.v, in, line, type, true )
                                          : di_->createAutoVariable( subprogram_, name, in, line, type, true );
    di_->insertDeclare(
        slot, variable, di_->createExpression(), llvm::DILocation::get( *context_, line, 0, subprogram_ ), entry_
    );
}

// A span outside the subprogram's file is in a lexical block of that file; an invalid one keeps the line before it.
void Llvm_emitter::locate( Span span )
{
    if( subprogram_ == nullptr || !span.is_valid() )
    {
        return;
    }
    llvm::DIScope* scope = subprogram_;
    if( span.file != subprogram_file_ )
    {
        llvm::DILexicalBlockFile*& block = elsewhere_[span.file.v];
        if( block == nullptr )
        {
            block = di_->createLexicalBlockFile( subprogram_, file( span.file ) );
        }
        scope = block;
    }
    const Line_col at = sm_.line_col( span.file, span.start );
    builder_.SetCurrentDebugLocation( llvm::DILocation::get( *context_, at.line, at.col, scope ) );
}

// Keel's name for each type; a struct's fields by name and offset. A payload enum shows its tag,
// a union only its size, and a scalar enum its integer.
llvm::DIType* Llvm_emitter::debug_type( Type_id type )
{
    if( const auto found = debug_types_.find( type.v ); found != debug_types_.end() )
    {
        return found->second;
    }

    const Type&       described = table().get( type );
    const std::string name      = std::string( table().name( type ) );
    const u64         bits      = is_void( type ) ? 0 : layout_.size( type ) * 8;
    const u32         aligned   = is_void( type ) ? 0 : static_cast<u32>( layout_.align( type ) * 8 );
    const auto        basic     = [&]( u64 width, unsigned encoding ) { return di_->createBasicType( name, width, encoding ); };
    const auto        pointer   = [&]( llvm::DIType* to ) { return di_->createPointerType( to, 64, 64, std::nullopt, name ); };
    const auto        pointee   = [&]( Type_id element ) { return is_void( element ) ? nullptr : debug_type( element ); };

    // Members of a struct that may reach itself through a pointer, so it is a placeholder until they are built.
    const auto composite = [&]( auto members )
    {
        llvm::DICompositeType* placeholder = di_->createReplaceableCompositeType(
            llvm::dwarf::DW_TAG_structure_type, name, unit_, nullptr, 0, 0, bits, aligned
        );
        debug_types_[type.v] = placeholder;
        std::vector<llvm::Metadata*> elements;
        const auto                   member = [&]( std::string_view field, llvm::DIType* of, u64 size, u64 at )
        {
            if( of != nullptr )
            {
                elements.push_back(
                    di_->createMemberType( placeholder, field, nullptr, 0, size * 8, 0, at * 8, llvm::DINode::FlagZero, of )
                );
            }
        };
        members( member );
        llvm::DICompositeType* whole = di_->createStructType(
            unit_, name, nullptr, 0, bits, aligned, llvm::DINode::FlagZero, nullptr, di_->getOrCreateArray( elements )
        );
        return di_->replaceTemporary( llvm::TempDIType( placeholder ), whole );
    };

    llvm::DIType* made = nullptr;
    switch( described.kind )
    {
    case Type_kind::Bool:
        made = basic( 8, llvm::dwarf::DW_ATE_boolean );
        break;
    case Type_kind::Int:
        made = basic( described.width, described.is_signed ? llvm::dwarf::DW_ATE_signed : llvm::dwarf::DW_ATE_unsigned );
        break;
    case Type_kind::Float:
        made = basic( described.width, llvm::dwarf::DW_ATE_float );
        break;
    case Type_kind::Field:
        made = basic( 64, llvm::dwarf::DW_ATE_unsigned );
        break;
    case Type_kind::Function:
        made = pointer( nullptr );
        break;
    case Type_kind::Many_pointer:
        made = pointer( pointee( described.element ) );
        break;
    case Type_kind::Pointer:
        if( held( type ) != Held::Pair )
        {
            made = pointer( pointee( described.element ) );
            break;
        }
        made = composite(
            [&]( auto member )
            {
                const bool run = table().is_run( described.element );
                member( run ? "data" : "object", di_->createPointerType( nullptr, 64 ), 8, 0 );
                if( run )
                {
                    member( "size", di_->createBasicType( "u64", 64, llvm::dwarf::DW_ATE_unsigned ), 8, 8 );
                }
                else
                {
                    member( "vtable", di_->createPointerType( nullptr, 64 ), 8, 8 );
                }
            }
        );
        break;
    case Type_kind::Enum:
        if( held( type ) == Held::Scalar )
        {
            made = di_->createTypedef( debug_type( described.element ), name, nullptr, 0, unit_ );
            break;
        }
        made = composite( [&]( auto member )
                          { member( "tag", debug_type( described.element ), layout_.size( described.element ), 0 ); } );
        break;
    case Type_kind::Closure:
        if( held( type ) == Held::Scalar )
        {
            made = basic( 8, llvm::dwarf::DW_ATE_unsigned );
            break;
        }
        [[fallthrough]];
    case Type_kind::Struct:
        made = composite(
            [&]( auto member )
            {
                for( const Held_field& field : program_.fields( type ) )
                {
                    member(
                        program_[field.declaration].name,
                        is_void( field.type ) ? nullptr : debug_type( field.type ),
                        layout_.size( field.type ),
                        layout_.offset( type, field.declaration )
                    );
                }
            }
        );
        break;
    case Type_kind::Union:
        made = composite( []( auto ) {} );
        break;
    case Type_kind::Array:
        if( llvm::DIType* element = pointee( described.element ) )
        {
            made = di_->createArrayType(
                bits,
                aligned,
                element,
                di_->getOrCreateArray( { di_->getOrCreateSubrange( 0, static_cast<int64_t>( described.count ) ) } )
            );
        }
        break;
    default: // nothing a named local holds
        break;
    }

    debug_types_[type.v] = made;
    return made;
}

// A scalar nothing takes the address of, though what it points at may be; under --debug, only a
// temporary, so a debugger finds every named local.
void Llvm_emitter::promote( const Function& function )
{
    promoted_.assign( function.locals.size(), false );
    addressed_.assign( function.locals.size(), false );
    for( const Local_id i : function.locals.ids() )
    {
        const Local& local = function.locals[i];
        promoted_[i.v]     = held( local.type ) == Held::Scalar && !( debug_ && level_ <= 1 && local.name.is_valid() );
    }

    const auto check = [&]( const Place& place, bool addressed )
    {
        if( place.is_global() || !place.local.is_valid() )
        {
            return;
        }
        if( place.num_projections == 0 ? addressed : function.projections_of( place )[0].kind != Projection_kind::Deref )
        {
            promoted_[place.local.v] = false;
        }
    };
    const auto check_operand = [&]( const Operand& operand )
    {
        if( operand.has_place() )
        {
            check( operand.place, false );
        }
    };

    for( const Statement& statement : function.statements )
    {
        check( statement.place, statement.kind == Statement_kind::Drop );
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }
        const Rvalue& value = function.rvalue( statement );
        if( value.kind == Rvalue_kind::Address_of ) // whatever its operand's kind
        {
            check( value.a.place, true );
            if( !value.a.place.is_global() && value.a.place.local.is_valid() &&
                ( value.a.place.num_projections == 0 ||
                  function.projections_of( value.a.place )[0].kind != Projection_kind::Deref ) )
            {
                addressed_[value.a.place.local.v] = true;
            }
        }
        check_operand( value.a );
        check_operand( value.b );
        for( const Operand& argument : value.arguments )
        {
            check_operand( argument );
        }
    }
}

llvm::Value* Llvm_emitter::value_of( Local_id local )
{
    return promoted_[local.v] ? read( local, block_ ) : load( current_->locals[local].type, locals_[local.v] );
}

llvm::Value* Llvm_emitter::read( Local_id local, u32 block )
{
    const auto found = current_values_[block].find( local.v );
    return found != current_values_[block].end() ? static_cast<llvm::Value*>( found->second ) : read_above( local, block );
}

// None reaching it reads as zero, as only a block no path enters can see.
llvm::Value* Llvm_emitter::read_above( Local_id local, u32 block )
{
    const std::vector<u32>& above = predecessors_[block];
    llvm::Value*            value = nullptr;
    if( !sealed_[block] )
    {
        llvm::PHINode* waiting = phi( local, block );
        incomplete_[block].emplace_back( local, waiting );
        value = waiting;
    }
    else if( above.empty() )
    {
        value = llvm::Constant::getNullValue( value_type( current_->locals[local].type ) );
    }
    else if( above.size() == 1 )
    {
        value = read( local, above[0] );
    }
    else
    {
        llvm::PHINode* joined           = phi( local, block );
        current_values_[block][local.v] = joined; // ends a cycle back here
        value                           = fill( local, block, joined );
    }
    current_values_[block][local.v] = value;
    return value;
}

llvm::PHINode* Llvm_emitter::phi( Local_id local, u32 block )
{
    llvm::IRBuilder<> at( blocks_[block], blocks_[block]->begin() );
    return at.CreatePHI( value_type( current_->locals[local].type ), static_cast<u32>( predecessors_[block].size() ) );
}

llvm::Value* Llvm_emitter::fill( Local_id local, u32 block, llvm::PHINode* phi )
{
    filling_.push_back( phi );
    for( const u32 above : predecessors_[block] )
    {
        phi->addIncoming( read( local, above ), exits_[above] );
    }
    filling_.pop_back();
    return remove_if_trivial( phi );
}

// One that joins a single value is that value, and so may be a phi that took this one, the value
// itself among them, so it is tracked.
llvm::Value* Llvm_emitter::remove_if_trivial( llvm::PHINode* phi )
{
    llvm::Value* same = nullptr;
    for( llvm::Value* incoming : phi->incoming_values() )
    {
        if( incoming == same || incoming == phi )
        {
            continue;
        }
        if( same != nullptr )
        {
            return phi;
        }
        same = incoming;
    }
    if( same == nullptr )
    {
        same = llvm::Constant::getNullValue( phi->getType() );
    }

    std::vector<llvm::WeakVH> users;
    for( llvm::User* user : phi->users() )
    {
        if( user != phi && llvm::isa<llvm::PHINode>( user ) )
        {
            users.emplace_back( user );
        }
    }
    const llvm::WeakTrackingVH result = same;
    phi->replaceAllUsesWith( same );
    phi->eraseFromParent();

    for( const llvm::WeakVH& user : users )
    {
        llvm::Value* still = user;
        if( still != nullptr && std::ranges::find( filling_, still ) == filling_.end() )
        {
            remove_if_trivial( llvm::cast<llvm::PHINode>( still ) );
        }
    }
    return result;
}

void Llvm_emitter::seal_if_ready( u32 block )
{
    if( sealed_[block] ||
        !std::ranges::all_of( predecessors_[block], [&]( u32 above ) { return static_cast<bool>( filled_[above] ); } ) )
    {
        return;
    }
    const std::vector<std::pair<Local_id, llvm::PHINode*>> waiting = std::move( incomplete_[block] );
    incomplete_[block].clear();
    for( const auto& [local, phi] : waiting )
    {
        fill( local, block, phi );
    }
    sealed_[block] = true;
}

void Llvm_emitter::statement( const Statement& statement )
{
    locate( statement.span );
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
        if( result != nullptr && statement.place.is_whole_local() && promoted_[statement.place.local.v] )
        {
            current_values_[block_][statement.place.local.v] = result;
        }
        else if( result != nullptr && held( target ) != Held::Nothing )
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
            llvm::BasicBlock* drop = llvm::BasicBlock::Create( *context_, "drop", defined_ );
            dropped                = llvm::BasicBlock::Create( *context_, "dropped", defined_ );
            builder_.CreateCondBr( value_of( statement.drop_flag ), drop, dropped );
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
        builder_.CreateRet( value_of( k_return_slot ) );
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
    Type_id                     type = place.is_global() ? types_.type_of( place.global ) : current_->locals[place.local].type;
    llvm::Value*                at   = place.is_global() ? global( place.global, place.owner ) : locals_[place.local.v];
    const Run<const Projection> projections = current_->projections_of( place );
    u32                         first       = 0;

    // A promoted pointer is its value, and `promote` let only a deref reach through it.
    if( !place.is_global() && promoted_[place.local.v] )
    {
        if( projections.size() == 0 )
        {
            malformed( "the address of a local held as a value" );
        }
        at    = read( place.local, block_ );
        type  = table().get( type ).element;
        first = 1;
    }
    if( at == nullptr )
    {
        malformed( "a place in a local that holds nothing" );
    }

    for( u32 i = first; i < projections.size(); ++i )
    {
        const Projection& projection = projections[i];
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
            llvm::Value*  index   = value_of( projection.index );
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

// A small aggregate by its parts, so each load reads what one store wrote and the store forwards.
void Llvm_emitter::copy( Type_id type, llvm::Value* to, llvm::Value* from )
{
    if( layout_.size( type ) > 32 )
    {
        builder_.CreateMemCpy( to, align( type ), from, align( type ), layout_.size( type ) );
        return;
    }
    copy_parts( type, to, from, 0, layout_.size( type ) );
}

// `size` bytes of `type` at `at`: a field at a time, a payload's bytes a word at a time.
void Llvm_emitter::copy_parts( Type_id type, llvm::Value* to, llvm::Value* from, u64 at, u64 size )
{
    const Type& described = table().get( type );
    switch( held( type ) )
    {
    case Held::Nothing:
        return;
    case Held::Scalar:
    case Held::Pair:
    {
        llvm::Type*       part  = memory_type( type );
        const llvm::Align place = align( type );
        builder_.CreateAlignedStore( builder_.CreateAlignedLoad( part, offset( from, at ), place ), offset( to, at ), place );
        return;
    }
    case Held::Memory:
        break;
    }

    switch( described.kind )
    {
    case Type_kind::Struct:
    case Type_kind::Closure:
        for( const Held_field& field : program_.fields( type ) )
        {
            copy_parts( field.type, to, from, at + layout_.offset( type, field.declaration ), layout_.size( field.type ) );
        }
        return;
    case Type_kind::Array:
        for( u64 i = 0; i < described.count; ++i )
        {
            const u64 each = layout_.size( described.element );
            copy_parts( described.element, to, from, at + i * each, each );
        }
        return;
    default: // the tag, then the union a word at a time
    {
        copy_parts( described.element, to, from, at, layout_.size( described.element ) );
        for( u64 byte = at + layout_.payload( type ); byte < at + size; )
        {
            u64 width = 8;
            while( byte % width != 0 || byte + width > at + size )
            {
                width /= 2;
            }
            llvm::Type*       part  = builder_.getIntNTy( static_cast<u32>( 8 * width ) );
            const llvm::Align place = llvm::Align( std::min<u64>( width, layout_.align( type ) ) );
            builder_.CreateAlignedStore(
                builder_.CreateAlignedLoad( part, offset( from, byte ), place ), offset( to, byte ), place
            );
            byte += width;
        }
    }
    }
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

// An `i64` or `double` per eightbyte of an aggregate passed in registers, or Win64's one integer;
// none when it is in memory.
std::vector<llvm::Type*> Llvm_emitter::eightbytes( Type_id type ) const
{
    const u64 size = layout_.size( type );
    if( windows_ )
    {
        if( held( type ) == Held::Memory && ( size == 1 || size == 2 || size == 4 || size == 8 ) )
        {
            return { builder_.getIntNTy( narrow_cast<u32>( 8 * size ) ) };
        }
        return {};
    }
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

// Win64 passes an interface reference, or an aggregate not of a register's width, by a pointer to a
// copy. A run stays two parameters, data then size, as C spells it (D70).
bool Llvm_emitter::by_reference( Type_id type ) const
{
    if( !windows_ )
    {
        return false;
    }
    if( held( type ) == Held::Pair )
    {
        return !table().is_run( table().get( type ).element );
    }
    return held( type ) == Held::Memory && eightbytes( type ).empty();
}

u64 bytes_of( const std::vector<llvm::Type*>& parts )
{
    u64 bytes = 0;
    for( const llvm::Type* part : parts )
    {
        bytes += part->getPrimitiveSizeInBits() / 8;
    }
    return bytes;
}

// Through a padded copy when the parts are wider, so the last never reads or writes past the aggregate.
std::vector<llvm::Value*> Llvm_emitter::split( Type_id type, llvm::Value* from, const std::vector<llvm::Type*>& parts )
{
    if( layout_.size( type ) < bytes_of( parts ) )
    {
        llvm::Value* padded = temporary( type, bytes_of( parts ) );
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
    std::vector<llvm::Type*> types;
    for( llvm::Value* part : parts )
    {
        types.push_back( part->getType() );
    }
    llvm::Value* into = layout_.size( type ) < bytes_of( types ) ? temporary( type, bytes_of( types ) ) : to;
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
        if( operand.place.is_whole_local() && promoted_[operand.place.local.v] )
        {
            return read( operand.place.local, block_ );
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
    case Rvalue_kind::Sqrt:
    {
        llvm::Value* a = operand( value.a );
        return builder_.CreateIntrinsic( llvm::Intrinsic::sqrt, { a->getType() }, { a } );
    }
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
        if( by_reference( type ) ) // a fresh copy each call, as the callee may change it
        {
            llvm::Value* copied = temporary( type );
            if( held( type ) == Held::Pair )
            {
                store( type, operand( argument ), copied );
            }
            else
            {
                copy( type, copied, address( argument.place ) );
            }
            arguments.push_back( copied );
        }
        else if( held( type ) != Held::Memory )
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
    if( returns_memory && held( value.type ) == Held::Pair )
    {
        return load( value.type, result );
    }
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
    // Through a temporary unless no pointer reaches the place: an argument may borrow it.
    case Rvalue_kind::Call:
    case Rvalue_kind::Indirect_call:
    {
        if( place.is_whole_local() && !addressed_[place.local.v] )
        {
            call( value, address( place ) );
            return;
        }
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

// Each global once, at its layout's alignment; a generic's static is weak, as in C, and another
// part's is only declared.
llvm::GlobalVariable* Llvm_emitter::global( Node_id declaration, Type_id owner )
{
    const std::string name = spelling_.global( declaration, owner );
    if( llvm::GlobalVariable* found = module_->getNamedGlobal( name ) )
    {
        return found;
    }

    const Type_id type = types_.type_of( declaration );
    if( !owner.is_valid() && !defines( declaration ) )
    {
        auto* declared =
            new llvm::GlobalVariable( *module_, memory_type( type ), false, llvm::GlobalValue::ExternalLinkage, nullptr, name );
        declared->setAlignment( align( type ) );
        return declared;
    }

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
    if( owner.is_valid() && windows_ ) // COFF keeps one of several weak definitions only in a COMDAT
    {
        made->setComdat( module_->getOrInsertComdat( name ) );
    }
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

// Set by -march; x86-64-v2 (SSE4.2) unless asked, which every x86-64 since about 2010 has.
std::string g_cpu      = "x86-64-v2";
std::string g_features = "";

// x86-64, COFF or ELF by the module's triple, at the machine passes of its level, and none of LLVM's IR passes (§2.2):
// Keel optimises its own IR. None at -O0 and at a debug -O1, so each line keeps its code and every store.
std::string write_object( llvm::Module& module, const std::filesystem::path& object, bool debug, u32 level )
{
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();

    const std::string   triple = module.getTargetTriple();
    std::string         error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget( triple, error );
    if( target == nullptr )
    {
        return "internal error: no x86-64 target in this keelc: " + error;
    }

    const std::unique_ptr<llvm::TargetMachine> machine( target->createTargetMachine(
        triple,
        g_cpu,
        g_features,
        llvm::TargetOptions {},
        llvm::Reloc::PIC_,
        std::nullopt,
        level == 0 || ( debug && level == 1 ) ? llvm::CodeGenOptLevel::None
        : level == 1                          ? llvm::CodeGenOptLevel::Less
                                              : llvm::CodeGenOptLevel::Aggressive
    ) );
    module.setDataLayout( machine->createDataLayout() );
    module.setPICLevel( llvm::PICLevel::BigPIC );
    if( machine->getTargetTriple().isOSLinux() )
    {
        module.setPIELevel( llvm::PIELevel::Large );
    }

    std::error_code      failed;
    llvm::raw_fd_ostream out( object.generic_string(), failed, llvm::sys::fs::OF_None );
    if( failed )
    {
        return fmt::format( "cannot write {}: {}", object.generic_string(), failed.message() );
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

std::string target_cpu()
{
    return g_features.empty() ? g_cpu : g_cpu + " " + g_features;
}

std::string set_target_cpu( const std::string& cpu )
{
    if( cpu != "native" )
    {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        std::string         error;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget( "x86_64-unknown-linux-gnu", error );
        const std::unique_ptr<llvm::MCSubtargetInfo> info(
            target == nullptr ? nullptr : target->createMCSubtargetInfo( "x86_64-unknown-linux-gnu", "", "" )
        );
        if( info == nullptr || !info->isCPUStringValid( cpu ) )
        {
            return fmt::format( "there is no -march={}; try x86-64, x86-64-v2, x86-64-v3, x86-64-v4 or native", cpu );
        }
        g_cpu = cpu;
        g_features.clear();
        return {};
    }
    // The host's name and the features it has, as clang's -march=native: a CPU's name alone may promise
    // features a virtual machine has turned off.
    g_cpu = llvm::sys::getHostCPUName().str();
    llvm::StringMap<bool> features;
    llvm::sys::getHostCPUFeatures( features );
    g_features.clear();
    for( const auto& feature : features )
    {
        g_features +=
            fmt::format( "{}{}{}", g_features.empty() ? "" : ",", feature.getValue() ? '+' : '-', feature.getKey().str() );
    }
    return {};
}

std::string set_llvm_options( const std::vector<std::string>& options )
{
    std::vector<const char*> argv { "keelc --llvm-option" }; // how LLVM's errors name us
    for( const std::string& option : options )
    {
        argv.push_back( option.c_str() );
    }
    std::string              error;
    llvm::raw_string_ostream errors( error );
    if( !llvm::cl::ParseCommandLineOptions( static_cast<int>( argv.size() ), argv.data(), "", &errors ) )
    {
        return error;
    }
    return {};
}

Llvm_output emit_llvm(
    const std::vector<Function>& functions,
    const Description&           program,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner,
    const Module_graph&          modules,
    const Llvm_options&          options
)
{
    Llvm_emitter emitter( functions, program, types, literals, sm, interner, modules, options );
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

        for( const llvm::GlobalValue& value : module.global_values() )
        {
            if( !value.isDeclaration() && !value.hasLocalLinkage() )
            {
                output.defined.push_back( value.getName().str() );
            }
        }

        if( options.print )
        {
            llvm::raw_string_ostream text( output.ir );
            module.print( text, nullptr );
        }

        if( output.error.empty() && !options.object.empty() )
        {
            output.error = write_object( module, options.object, options.debug, options.level );
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
#include <fstream>
#include "check/pipeline_test_support.h"
#include "common/temp_dir.h"

namespace keel
{

TEST_CASE( "emit_llvm_writes_a_function_the_verifier_accepts", "[codegen][llvm]" )
{
    Compiled p( "i32 twice( i32 a ) { return a * 2; }\ni32 main() { return twice( 3 ) - 6; }\n", Stage::Elaborated );
    REQUIRE( p.clean() );

    const Description program = describe( p.ast, p.types, p.interner );
    const Llvm_output out =
        emit_llvm( p.functions, program, p.types, p.literals, p.sm, p.interner, p.modules, { .print = true } );

    INFO( out.error );
    INFO( out.ir );
    REQUIRE( out.error.empty() );
    REQUIRE( out.ir.find( "define i32 @kl__5twice__3i32(i32 %0)" ) != std::string::npos );
    REQUIRE( out.ir.find( "mul i32" ) != std::string::npos );
    REQUIRE( out.ir.find( " nsw " ) == std::string::npos );
}

TEST_CASE( "emit_llvm_contracts_only_in_a_real_arithmetic_function", "[codegen][llvm]" )
{
    Compiled p(
        "@real_arithmetic\nf64 fused( f64 a, f64 b, f64 c ) { return a * b + c; }\n"
        "f64 exact( f64 a, f64 b, f64 c ) { return a * b + c; }\n"
        "i32 main() { return fused( 1.0, 2.0, 3.0 ) == exact( 1.0, 2.0, 3.0 ) ? 0 : 1; }\n",
        Stage::Elaborated
    );
    REQUIRE( p.clean() );

    const Description program = describe( p.ast, p.types, p.interner );
    const Llvm_output out =
        emit_llvm( p.functions, program, p.types, p.literals, p.sm, p.interner, p.modules, { .print = true } );

    INFO( out.ir );
    REQUIRE( out.error.empty() );
    const u64 fused = out.ir.find( "@kl__5fused" );
    const u64 exact = out.ir.find( "@kl__5exact" );
    REQUIRE( fused < exact );
    REQUIRE( out.ir.find( "fmul contract double" ) < exact );
    REQUIRE( out.ir.find( "fadd contract double" ) < exact );
    REQUIRE( out.ir.find( "contract", exact ) == std::string::npos );
}

TEST_CASE( "emit_llvm_promotes_a_scalar_local_unless_debug_at_O1_names_it", "[codegen][llvm]" )
{
    Compiled p(
        "i32 count( i32 n ) { i32 i = 0; while( i < n ) { i = i + 1; } return i; }\n"
        "i32 main() { return count( 3 ) - 3; }\n",
        Stage::Elaborated
    );
    REQUIRE( p.clean() );

    const Description program = describe( p.ast, p.types, p.interner );
    const Llvm_output promoted =
        emit_llvm( p.functions, program, p.types, p.literals, p.sm, p.interner, p.modules, { .print = true } );
    const Llvm_output kept = emit_llvm(
        p.functions, program, p.types, p.literals, p.sm, p.interner, p.modules, { .print = true, .debug = true, .level = 1 }
    );

    INFO( promoted.ir );
    REQUIRE( promoted.error.empty() );
    REQUIRE( promoted.ir.find( "phi i32" ) != std::string::npos );
    REQUIRE( promoted.ir.find( "alloca" ) == std::string::npos );
    REQUIRE( kept.error.empty() );
    REQUIRE( kept.ir.find( "%i = alloca i32" ) != std::string::npos );

    // Above -O1 the KIR no longer keeps a named local's stores, so neither does its alloca.
    const Llvm_output optimised = emit_llvm(
        p.functions, program, p.types, p.literals, p.sm, p.interner, p.modules, { .print = true, .debug = true, .level = 2 }
    );
    REQUIRE( optimised.ir.find( "alloca" ) == std::string::npos );
}

TEST_CASE( "emit_llvm_describes_functions_and_locals_only_under_debug", "[codegen][llvm]" )
{
    Compiled p( "i32 count( i32 n ) { i32 i = n; return i; }\ni32 main() { return count( 0 ); }\n", Stage::Elaborated );
    REQUIRE( p.clean() );

    const Description program = describe( p.ast, p.types, p.interner );
    const Llvm_output release =
        emit_llvm( p.functions, program, p.types, p.literals, p.sm, p.interner, p.modules, { .print = true } );
    const Llvm_output debug = emit_llvm(
        p.functions, program, p.types, p.literals, p.sm, p.interner, p.modules, { .print = true, .debug = true, .level = 1 }
    );

    INFO( debug.ir );
    REQUIRE( debug.error.empty() );
    REQUIRE( debug.ir.find( "distinct !DISubprogram(name: \"count\"" ) != std::string::npos );
    REQUIRE( debug.ir.find( "!DILocalVariable(name: \"n\", arg: 1" ) != std::string::npos );
    REQUIRE( debug.ir.find( "!DILocalVariable(name: \"i\"" ) != std::string::npos );
    REQUIRE( debug.ir.find( "name: \"i32\", size: 32, encoding: DW_ATE_signed" ) != std::string::npos );
    REQUIRE( release.ir.find( "!dbg" ) == std::string::npos );
}

TEST_CASE( "emit_llvm_follows_each_targets_c_abi", "[codegen][llvm]" )
{
    Compiled p(
        "struct Tiny { u8 a; i16 b; };\n"
        "struct Three { f32 x; f32 y; f32 z; };\n"
        "Tiny tiny( Tiny t ) { return t; }\n"
        "Three three( Three t ) { return t; }\n"
        "bool flip( bool b, i8 c ) { return !b; }\n"
        "u64 count( const ref i32[] values ) { return values.size; }\n"
        "const ref i32[] echo( const ref i32[] values ) { return values; }\n"
        "interface Shape { f64 area() const; };\n"
        "struct Square implements Shape { f64 side; f64 area() const { return side * side; } };\n"
        "f64 measure( const ref Shape s ) { return s.area(); }\n"
        "i32 main()\n"
        "{\n"
        "    Tiny s = tiny( Tiny { 7, -300 } );\n"
        "    Three t = three( Three { 1.0, 2.0, 3.0 } );\n"
        "    i32[3] a = [ 1, 2, 3 ];\n"
        "    Square q = Square { 2.0 };\n"
        "    return flip( true, 1 ) && count( a ) == 3 && echo( a ).size == 3 && measure( q ) == 4.0 ? 0 : 1;\n"
        "}\n",
        Stage::Elaborated
    );
    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Description program = describe( p.ast, p.types, p.interner );
    const auto        emit    = [&]( Platform platform, bool debug, const std::filesystem::path& object = {} )
    {
        return emit_llvm(
            p.functions,
            program,
            p.types,
            p.literals,
            p.sm,
            p.interner,
            p.modules,
            { .object = object, .print = true, .debug = debug, .level = debug ? 1u : 2u, .platform = platform }
        );
    };
    // The signature of the function whose name holds `name`.
    const auto defined = []( const std::string& ir, std::string_view name )
    {
        for( std::size_t at = ir.find( "define " ); at != std::string::npos; at = ir.find( "define ", at + 1 ) )
        {
            const std::string line = ir.substr( at, ir.find( '\n', at ) - at );
            if( line.find( name ) != std::string::npos )
            {
                return line;
            }
        }
        return std::string();
    };

    const Llvm_output sysv  = emit( Platform::Linux, false );
    const Llvm_output win64 = emit( Platform::Windows, false );
    INFO( sysv.ir );
    INFO( win64.ir );
    REQUIRE( sysv.error.empty() );
    REQUIRE( win64.error.empty() );

    REQUIRE( sysv.ir.find( "target triple = \"x86_64-pc-linux-gnu\"" ) != std::string::npos );
    REQUIRE( win64.ir.find( "target triple = \"x86_64-pc-windows-msvc\"" ) != std::string::npos );

    // SysV: eightbytes in registers, every narrow integer widened.
    REQUIRE( defined( sysv.ir, "tiny" ).find( "i64 @" ) != std::string::npos );
    REQUIRE( defined( sysv.ir, "three" ).find( "{ double, double } @" ) != std::string::npos );
    REQUIRE( defined( sysv.ir, "flip" ).find( "(i1 zeroext %0, i8 signext %1)" ) != std::string::npos );
    REQUIRE( defined( sysv.ir, "count" ).find( "({ ptr, i64 } %0)" ) != std::string::npos );
    REQUIRE( defined( sysv.ir, "echo" ).find( "{ ptr, i64 } @" ) != std::string::npos );
    REQUIRE( defined( sysv.ir, "measure" ).find( "({ ptr, ptr } %0)" ) != std::string::npos );

    // Win64: one integer of a register's width, else a pointer to a copy; only a `bool` widened; a
    // run parameter still two words, as C's two parameters.
    REQUIRE( defined( win64.ir, "tiny" ).find( "i32 @" ) != std::string::npos );
    REQUIRE( defined( win64.ir, "tiny" ).find( "(i32 %0)" ) != std::string::npos );
    REQUIRE( defined( win64.ir, "three" ).find( "void @" ) != std::string::npos );
    REQUIRE( defined( win64.ir, "three" ).find( "(ptr sret([12 x i8]) align 4 %0, ptr %1)" ) != std::string::npos );
    REQUIRE( defined( win64.ir, "flip" ).find( "(i1 zeroext %0, i8 %1)" ) != std::string::npos );
    REQUIRE( defined( win64.ir, "count" ).find( "({ ptr, i64 } %0)" ) != std::string::npos );
    REQUIRE( defined( win64.ir, "echo" ).find( "(ptr sret({ ptr, i64 }) align 8 %0, { ptr, i64 } %1)" ) != std::string::npos );
    REQUIRE( defined( win64.ir, "measure" ).find( "(ptr %0)" ) != std::string::npos );

    // COFF objects, with CodeView under --debug.
    const Temp_dir    dir;
    const Llvm_output object = emit( Platform::Windows, true, dir.path / "main.obj" );
    INFO( object.error );
    REQUIRE( object.error.empty() );
    REQUIRE( object.ir.find( "\"CodeView\"" ) != std::string::npos );
    REQUIRE( object.ir.find( "\"Dwarf Version\"" ) == std::string::npos );

    std::ifstream     in( dir.path / "main.obj", std::ios::binary );
    const std::string bytes( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    REQUIRE( bytes.substr( 0, 2 ) == std::string( "\x64\x86", 2 ) ); // IMAGE_FILE_MACHINE_AMD64
    REQUIRE( bytes.find( ".debug$S" ) != std::string::npos );
}

} // namespace keel
#endif
