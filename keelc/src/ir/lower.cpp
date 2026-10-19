// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/lower.h"
#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <vector>
#include "ast/node.h"
#include "ir/builder.h"
#include "ir/drop_scopes.h"
#include "ir/instances.h"

namespace keel
{

namespace
{

class Lowering
{
public:
    Lowering(
        Node_id           declaration,
        const Ast&        ast,
        const Resolution& resolution,
        Types&            types,
        Literal_pool&     literals,
        bool              overflow_checks,
        Bindings          bindings = {}
    );

    Function run();

private:
    // A node's type, and a declaration's binding type, with this instantiation's parameters bound.
    // Every type read goes through these: an escaped `Parameter` trips Spelling::type's assert.
    Type_id type_of( Node_id id );
    Type_id type_under( Node_id id, const Bindings& bindings );
    Type_id binding_type_of( Node_id decl );

    // A binding type under explicit bindings, and the bindings a call is made under: the callee's,
    // since a `main` calling `id<i32>` has none of its own.
    Type_id              binding_type_under( Node_id decl, const Bindings& bindings );
    Bindings             bindings_for_call( Node_id call );
    std::vector<Type_id> type_arguments_for_call( Node_id call );

    // A call of a method at the instance `held` names: its bindings and type arguments.
    struct Instance_target
    {
        Bindings             bound;
        std::vector<Type_id> type_arguments;
    };
    Instance_target at_instance( Type_id held ) const;
    // Who answers `requirement` on `held`: the requirement itself, through the vtable an interface
    // carries; otherwise the implementer's method at `held`'s instance, invalid where the compiler
    // supplies the conformance.
    struct Requirement_target
    {
        Node_id         method;
        Instance_target at;
        bool            through_vtable = false;
    };
    Requirement_target requirement_target( Node_id requirement, Type_id held, Type_id interface ) const;

    Place   place_for( Node_id declaration, Type_id owner = {} );
    Type_id static_owner( Node_id named, Node_id decl );
    Node_id field_of( Type_id type, Symbol_id name ) const;

    Operand lower_expression( Node_id id ); // produces a value

    // One per construct, dispatched from lower_expression. The small cases stay inline there:
    // extracting a two-line literal would cost a name and buy nothing.
    Operand lower_struct_literal( Node_id id );
    Operand lower_array_literal( Node_id id );
    Operand lower_string_literal( Node_id id );
    Operand lower_binary( Node_id id );
    Operand lower_unary( Node_id id );
    Operand lower_call( Node_id id );
    Operand lower_indirect_call( Node_id id );
    Operand lower_field_application( Node_id id );
    Operand lower_method_call( Node_id id );
    Operand lower_method_call_on(
        Node_id                  id,
        Node_id                  method,
        Operand                  receiver,
        std::span<const Node_id> arguments,
        Type_id                  type,
        const Bindings&          callee_bound,
        std::vector<Type_id>     type_arguments
    );
    Operand              lower_interface_call( Node_id id, Node_id requirement );
    Operand              lower_static_interface_call( Node_id id, Node_id requirement );
    std::vector<Operand> lower_call_arguments(
        std::span<const Node_id> arguments, std::span<const Node_id> parameters, const Bindings& callee_bound
    );
    // `type` is the call's language type, `type_of( id )` when not given.
    Operand emit_call(
        Node_id                  id,
        Node_id                  callee,
        std::span<const Operand> operands,
        const Bindings&          callee_bound,
        std::vector<Type_id>     type_arguments,
        Type_id                  type = {}
    );
    Operand lower_operator_call( Node_id id, Node_id method, const Bindings& bindings, std::vector<Type_id> type_arguments );
    // `object.method( arguments )` for an operator, with `object` as the receiver.
    Operand call_operator(
        Node_id                  id,
        Node_id                  method,
        Place                    object,
        std::span<const Node_id> arguments,
        const Bindings&          bindings,
        std::vector<Type_id>     type_arguments
    );
    // In an instance, the method of `held`'s own conformance that `op` calls, or invalid where the
    // operator is the compiler's.
    Requirement_target bound_operator( Token_kind op, Type_id held ) const;
    Operand            lower_operator_index( Node_id id, Node_id method );
    Operand            lower_variant_construction( Node_id id );
    Operand            lower_try( Node_id id );
    Operand            lower_empty_variant( Node_id id );
    void               bind_variant_pattern( Place matched, Type_id matched_type, Node_id label, bool consumes );
    Place              lower_place( Node_id id );     // names where a value lives
    void               lower_statement( Node_id id ); // emits, produces nothing
    // `bindings` are the *callee's*: the parameter's type is written in its declaration, so
    // substituting with this function's would ask it to bind a parameter it has never heard of.
    Operand lower_argument( Node_id argument, Node_id parameter, const Bindings& bindings );
    Operand borrowed_argument( Node_id argument, Type_id address );

    Operand address_operand( Place place, Type_id type, Span span, Address_purpose purpose );
    // address_of, but a `T[N]` borrowed as a `T[]` is lent as a run of its elements.
    Rvalue borrow_of( Place place, Type_id held, Type_id address, Address_purpose purpose, Span span );

    // One per construct, dispatched from lower_statement - the shape visit_* uses next door.
    void lower_block( Node_id id );
    void lower_return( Node_id id );
    void lower_var( Node_id id );
    void lower_assign( Node_id id );
    void store( Place target, Operand value, Type_id type, Node_id target_node, Span span );
    void lower_increment( Node_id id );
    void lower_if( Node_id id );
    void lower_switch( Node_id id );
    void lower_case_test( Operand scrutinee, Place matched, Type_id matched_type, Node_id label, Block_id body );
    void lower_arm_body( Node_id arm, Place matched, Type_id matched_type, Block_id fallthrough_to, bool consumes );
    void lower_while( Node_id id );
    void lower_for( Node_id id );

    Operand temporary( Rvalue value, Type_id type, Span span );

    // Where a `fallthrough` in the current arm goes. Saved and restored around each arm, so a nested
    // switch gives the outer target back.
    Block_id fallthrough_target_;

    // Where `break` and `continue` jump to.
    struct Loop_targets
    {
        Block_id break_target;    // the exit block
        Block_id continue_target; // a while re-tests at the header; a for runs its update first
        u32      depth;
        bool     is_switch = false; // `continue` looks past these; `break` stops at the first
    };

    // Allocated on demand, because a block nothing jumps to is unreachable and verify rejects one.
    // `for( ; ; )` with no break needs no exit, and a body that always returns needs no latch.
    Block_id break_target();
    Block_id continue_target();

    // `&&` and `||` are not operators over two operands: the right side must not run unless the
    // left demands it, so they lower to control flow.
    Operand lower_short_circuit( Node_id id );

    Operand lower_conditional( Node_id id );

    // The type an operation happens in, and the conversion that puts an operand there. See §6.4.
    Type_id operation_type( Node_id node );
    Type_id comparison_width( Type_id type ) const;
    Operand converted( Operand operand, Type_id to, Span span );
    // §6.7: a member into a union holding it, in a temporary of the union; anything else as it is.
    Operand united( Operand operand, Type_id to, Span span );
    // An error written into `target` as it is, into a union holding it, or into a wider union
    // keeping its tag. `source` is storage nothing drops.
    void    write_error( Place target, Type_id to, Place source, Type_id from, Span span );
    Operand checked_cast( Operand operand, Type_id to, Span span );
    // A variable shift count is masked to the width; a variable divisor panics at zero, and MIN / -1 wraps.
    // Under --overflow-checks, + - * and MIN / -1 panic unless `wraps`.
    Rvalue checked_arithmetic( Token_kind op, Operand left, Operand right, Type_id type, Span span, bool wraps );
    // Under --overflow-checks, panics before `left op right` would leave `type`.
    void    check_overflow( Token_kind op, Operand left, Operand right, Type_id type, Span span );
    void    mark_wrapping( Node_id id );
    Operand three_way( Operand left, Operand right, Span span );
    Operand supplied_hash( Operand value, Span span );
    Operand moved_if_owning( Operand operand );

    Operand run_pointer( Node_id run );
    Place   lower_slice_or_run_index( Node_id id );
    Place   lower_array_index( Node_id id );

    bool is_move_parameter( Node_id param ) const;

    void drop_place( Place place, Type_id type, Span span, bool replacing = false );
    // A call whose callee names a type rather than a function.
    bool is_construction( Node_id id ) const;

    bool initialises_field( const Place& target ) const;
    bool writes_a_slot( Node_id target ) const;

    void lower_destroy( Node_id id );

    // Runs a construct with `target`'s address as its receiver. Not an expression: a constructor
    // returns nothing and writes through the pointer it is handed.
    void lower_construction( Place target, Node_id call_expr );

    // `break` binds to the nearest loop or switch, so it is loops_.back(). `continue` looks past
    // switches, and unwinds to the loop's scope depth.
    Loop_targets& enclosing_loop();

    Local_id call_result( Node_id id, Rvalue call, Type_id result_type, Span span );

    // D2 for this instance rather than for the declaration: `Box<i32>` and `Box<Buffer>` are two
    // answers, and the drop being elaborated here belongs to one of them.
    bool owns( Type_id type )
    {
        return instance_owns( ast_, types_.table(), type, types_.recorded() );
    }

    std::vector<Node_id> paired_payload( Type_id instance, Node_id variant, std::size_t count );

    const Ast& ast_;

    // Read only for `&f`, whose callee is a declaration rather than a place. Everywhere else a
    // name arrives with its local already.
    const Resolution& resolution_;
    Types&            types_;

    // This instantiation's type parameters, bound. Empty for an ordinary function, which is
    // what makes the substitution below the identity and lets one path serve both.
    Bindings      bindings_;
    Literal_pool& literal_pool_;

    const Node_id declaration_;
    const bool    overflow_checks_;

    Builder                           builder_;
    std::unordered_map<u32, Local_id> locals_;
    Local_id                          receiver_ {}; // A destructor's synthesised `this`. Invalid in a plain function.
    // Parameters the callee owns, and therefore drops. Collected in the constructor and put in a
    // scope by run(), because drops_ has no scope to put them in until then.
    std::vector<Local_id>   owned_parameters_;
    std::unordered_set<u32> borrowed_bindings_; // declarations whose local holds an address
    // Operator nodes in a `wrap`'s operand, reached only through operators: they wrap, unchecked.
    std::unordered_set<u32> wrapping_;
    Node_id                 receiver_declaration_ {}; // the Param_decl, so place_for can be reused for it

    Drop_scopes drops_;
    Node_id     discarded_ {};

    std::vector<Loop_targets> loops_; // innermost last, so break and continue bind to it
};

Lowering::Lowering(
    Node_id           declaration,
    const Ast&        ast,
    const Resolution& resolution,
    Types&            types,
    Literal_pool&     literals,
    bool              overflow_checks,
    Bindings          bindings
)
    : ast_( ast ),
      resolution_( resolution ),
      types_( types ),
      bindings_( std::move( bindings ) ),
      literal_pool_( literals ),
      declaration_( declaration ),
      overflow_checks_( overflow_checks ),
      builder_(
          declaration, types.table().substitute( binding_type( ast, types, declaration ), bindings_ ), ast_.span( declaration )
      ),
      locals_(),
      drops_( builder_, ast_, types_ )
{
    for( const Node_id param : ast_.params( declaration ) )
    {
        if( ast_.kind( param ) == Node_kind::Param_decl )
        {
            const Type_id   type  = binding_type_of( param );
            const Span      span  = ast_.span( param );
            const Symbol_id name  = ast_.name( param );
            const Local_id  local = builder_.add_parameter( type, span, name );
            locals_.emplace( param.v, local );

            if( is_borrowed_binding( ast_, types_, param ) )
            {
                borrowed_bindings_.insert( param.v );
            }

            if( ast_.parameter_mode( param ) == Keyword::Out )
            {
                builder_.mark_out_parameter( local );
            }

            if( is_move_parameter( param ) )
            {
                owned_parameters_.push_back( local );
            }

            // The receiver, which a bare field name is reached through: the first parameter, captured here
            // rather than assumed to be local 1.
            if( !receiver_.is_valid() && ast_.has_receiver( declaration ) )
            {
                receiver_             = local;
                receiver_declaration_ = param;
            }
        }
    }
}

Function Lowering::run()
{
    const Span span = ast_.span( declaration_ );

    // Owned parameters sit in a scope enclosing the body, so `return`'s unwind_to( 0 ) drops them.
    drops_.push_scope();

    for( const Local_id parameter : owned_parameters_ )
    {
        drops_.add_to_scope( parameter );
    }

    lower_statement( ast_.body( declaration_ ) );

    // The fall-off-the-end path: a `return` has already unwound to 0, and unwind_to is a no-op on a
    // terminated block, so this only fires where nothing returned.
    drops_.pop_scope( span );

    if( !builder_.is_terminated() )
    {
        builder_.terminate_return( span );
    }

    Function function        = builder_.finish();
    function.diverges        = types_.table().is_never( function.locals[k_return_slot.v].type );
    function.returns_a_value = !types_.table().is_void( function.locals[k_return_slot.v].type ) && !function.diverges;

    if( ast_.kind( declaration_ ) == Node_kind::Constructor_decl )
    {
        function.constructed   = receiver_;
        const Type_id instance = types_.table().get( function.locals[receiver_.v].type ).element;

        for( const Node_id member : ast_.members( ast_.enclosing_aggregate( declaration_ ) ) )
        {
            if( ast_.kind( member ) != Node_kind::Field_decl )
            {
                continue;
            }

            function.owed_fields.push_back( Owed_field {
                member,
                owns( field_type( ast_, types_.table(), instance, member, types_.recorded() ) ),
                ast_.is_const_field( member )
            } );
        }
    }

    return function;
}

Type_id Lowering::type_of( Node_id id )
{
    return types_.table().substitute( types_.type_of( id ), bindings_ );
}

Type_id Lowering::type_under( Node_id id, const Bindings& bindings )
{
    return types_.table().substitute( types_.type_of( id ), bindings );
}

Type_id Lowering::binding_type_of( Node_id decl )
{
    return binding_type_under( decl, bindings_ );
}

Type_id Lowering::binding_type_under( Node_id decl, const Bindings& bindings )
{
    return types_.table().substitute( binding_type( ast_, types_, decl ), bindings );
}

// What the symbol is mangled with: the recorded instantiation's arguments, substituted, because a
// call inside a generic wrote `T`.
std::vector<Type_id> Lowering::type_arguments_for_call( Node_id call )
{
    std::vector<Type_id> type_arguments;

    if( const std::optional<std::size_t> instance = types_.instantiation_of( call ) )
    {
        for( const Type_id argument : types_.instantiations()[*instance].arguments )
        {
            type_arguments.push_back( types_.table().substitute( argument, bindings_ ) );
        }
    }

    return type_arguments;
}

Bindings Lowering::bindings_for_call( Node_id call )
{
    const std::optional<std::size_t> instance = types_.instantiation_of( call );

    if( !instance.has_value() )
    {
        return {};
    }

    const Instantiation&       chosen     = types_.instantiations()[*instance];
    const std::vector<Node_id> parameters = ast_.type_parameters( ast_.type_param_list( chosen.declaration ) );

    Bindings bindings;

    for( std::size_t i = 0; i < parameters.size() && i < chosen.arguments.size(); ++i )
    {
        // The recorded arguments are what the call wrote, `T` inside a generic, so substitute through
        // this instance's bindings: `g<T>( a )` in `f<i32>` is `g<i32>`.
        bindings.emplace( types_.type_of( parameters[i] ).v, types_.table().substitute( chosen.arguments[i], bindings_ ) );
    }

    return bindings;
}

Lowering::Instance_target Lowering::at_instance( Type_id held ) const
{
    const std::span<const Type_id> arguments = types_.table().get( held ).arguments;

    return {
        aggregate_bindings( ast_, types_.table(), held, types_.recorded() ),
        std::vector<Type_id>( arguments.begin(), arguments.end() )
    };
}

Lowering::Requirement_target Lowering::requirement_target( Node_id requirement, Type_id held, Type_id interface ) const
{
    const Type_table& table = types_.table();

    if( table.is_interface( held ) )
    {
        return { requirement, { table.instance_bindings( held ), {} }, true };
    }

    return { types_.conformances().method_for( table, held, interface, requirement ), at_instance( held ) };
}

Place Lowering::place_for( Node_id declaration, Type_id owner )
{
    const auto local = locals_.find( declaration.v );

    if( local != locals_.end() )
    {
        const Place base = builder_.place( local->second );

        // D32: the name means the referent, not the address. Same deref the receiver already goes
        // through, which is why no consumer of KIR needs to know a ref binding exists.
        return borrowed_bindings_.contains( declaration.v ) ? builder_.deref( base ) : base;
    }

    // A bare field is `this.field` - D22's reach through the receiver, written implicitly - so it
    // lowers to the same two projections the explicit spelling produces.
    if( ast_.kind( declaration ) == Node_kind::Field_decl )
    {
        return builder_.field( place_for( receiver_declaration_ ), declaration );
    }

    // Anything else that resolves to a Var_decl is at file scope: a function-local one is in the
    // map by the time any name can refer to it, because use-before-declaration is a resolver error.
    assert( ast_.kind( declaration ) == Node_kind::Var_decl && "a name resolves to a local or a global" );
    return builder_.global( declaration, owner );
}

Type_id Lowering::static_owner( Node_id named, Node_id decl )
{
    const Node_id aggregate = ast_.enclosing_aggregate( decl );

    if( !aggregate.is_valid() || !ast_.is_generic( aggregate ) || ast_.kind( decl ) != Node_kind::Var_decl )
    {
        return Type_id {};
    }

    if( ast_.kind( named ) == Node_kind::Path_expr )
    {
        return type_of( ast_.qualifier( named ) );
    }
    else
    {
        return types_.table().substitute( types_.type_of( aggregate ), bindings_ );
    }
}

Node_id Lowering::field_of( Type_id type, Symbol_id name ) const
{
    for( const Node_id field : ast_.members( types_.table().get( type ).declaration ) )
    {
        if( ast_.kind( field ) == Node_kind::Field_decl && ast_.name( field ) == name )
        {
            return field;
        }
    }

    return Node_id {};
}

// Not always the result type: a comparison yields bool while its operands meet at their common type.
Type_id Lowering::operation_type( Node_id node )
{
    // type_of, not types_.type_of: inside an instance the operands are recorded as `T`.
    const Type_id left  = type_of( ast_.lhs( node ) );
    const Type_id right = type_of( ast_.rhs( node ) );

    // D41: a comparison meets in the narrowest type holding both exactly. Invalid means none does,
    // `i64` against `u64` or either against a float, and lower_binary answers that by cases.
    if( is_comparison( ast_.op( node ) ) )
    {
        return types_.table().common( left, right );
    }

    const Type_id common = types_.table().arithmetic_result( left, right );

    // A shift takes no common type: its result is the left operand's, which is why
    // arithmetic_result has no answer for one.
    return common.is_valid() ? common : left;
}

// How far a comparison operand widens on its own, so every pair with no common type reduces to
// i64/u64, i64/f64 or u64/f64, and the backend writes three guards.
Type_id Lowering::comparison_width( Type_id type ) const
{
    const Type_table& table = types_.table();

    return table.is_float( type ) ? table.floating( 64 ) : table.integer( 64, table.get( type ).is_signed );
}

// Converts to `to`, so a binary's operands share a type in KIR and no backend re-derives §6.4.
// D41's exception: a comparison with no common type keeps its operands unequal.
Operand Lowering::converted( Operand operand, Type_id to, Span span )
{
    if( operand.type == to )
    {
        return operand;
    }

    if( types_.table().is_union( to ) || types_.table().is_box( to ) )
    {
        return united( operand, to, span );
    }

    return temporary( cast_to( operand, to ), to, span );
}

Operand Lowering::united( Operand operand, Type_id to, Span span )
{
    // An implementer's address becomes a `Shape*` where one is wanted, as a member becomes a union.
    if( operand.type != to && types_.table().is_interface_pointer( to ) && types_.table().is_pointer( operand.type ) )
    {
        return temporary( cast_to( operand, to ), to, span );
    }

    if( operand.type != to && types_.table().is_box( to ) && types_.table().is_box( operand.type ) )
    {
        return move( builder_.place( builder_.into_temp( cast_to( moved_if_owning( operand ), to ), to, span ) ), to );
    }

    if( operand.type == to || !types_.table().is_union( to ) )
    {
        return operand;
    }

    const Type_id  tag_type = types_.table().get( to ).element;
    const Local_id local    = builder_.add_local( to, span );
    const Place    place    = builder_.place( local );

    builder_.assign(
        builder_.tag( place ),
        use( constant( literal_pool_.add_integer( types_.table().error_tag( operand.type ) ), tag_type ) ),
        span
    );
    builder_.assign( builder_.member( place, operand.type ), use( moved_if_owning( operand ) ), span );

    drops_.own_temporary( local );

    return copy( place, to );
}

void Lowering::write_error( Place target, Type_id to, Place source, Type_id from, Span span )
{
    const Type_table& table = types_.table();

    if( from == to )
    {
        builder_.assign( target, use( copy( source, from ) ), span );
        return;
    }

    const Type_id tag_type = table.get( to ).element;

    if( !table.is_union( from ) )
    {
        builder_.assign(
            builder_.tag( target ), use( constant( literal_pool_.add_integer( table.error_tag( from ) ), tag_type ) ), span
        );
        builder_.assign( builder_.member( target, from ), use( copy( source, from ) ), span );
        return;
    }

    // The tag is kept, and the live member copied across; the last needs no test.
    builder_.assign( builder_.tag( target ), use( copy( builder_.tag( source ), tag_type ) ), span );

    const Type_id                  boolean = table.builtin( Type_kind::Bool );
    const Block_id                 done    = builder_.add_block();
    const std::span<const Type_id> members = table.get( from ).arguments;

    for( std::size_t i = 0; i < members.size(); ++i )
    {
        const Type_id member = members[i];

        if( i + 1 < members.size() )
        {
            const Local_id live = builder_.into_temp(
                binary(
                    Token_kind::Equal_equal,
                    copy( builder_.tag( source ), tag_type ),
                    constant( literal_pool_.add_integer( table.error_tag( member ) ), tag_type ),
                    boolean
                ),
                boolean,
                span
            );

            const Block_id body = builder_.add_block();
            const Block_id next = builder_.add_block();

            builder_.terminate_branch( copy( builder_.place( live ), boolean ), body, next, span );
            builder_.switch_to( body );
            builder_.assign(
                builder_.member( target, member ), use( copy( builder_.member( source, member ), member ) ), span
            );
            builder_.terminate_goto( done, span );
            builder_.switch_to( next );
            continue;
        }

        builder_.assign( builder_.member( target, member ), use( copy( builder_.member( source, member ), member ) ), span );
        builder_.terminate_goto( done, span );
    }

    builder_.switch_to( done );
}

// D28: a narrowing `cast` ends the program unless the value comes back unchanged, sign included.
Operand Lowering::checked_cast( Operand operand, Type_id to, Span span )
{
    const Type_table& table = types_.table();
    const Type_id     from  = operand.type;
    const Operand     value = converted( operand, to, span );

    if( !table.is_integer( from ) || !table.is_integer( to ) )
    {
        return value;
    }

    const Type& source = table.get( from );
    const Type& target = table.get( to );

    const bool holds =
        source.is_signed == target.is_signed ? target.width >= source.width : target.is_signed && target.width > source.width;
    if( holds )
    {
        return value;
    }

    const Type_id  boolean = table.builtin( Type_kind::Bool );
    const Block_id fail    = builder_.add_block();

    const auto require = [&]( Rvalue condition )
    {
        const Block_id pass = builder_.add_block();
        builder_.terminate_branch( temporary( condition, boolean, span ), pass, fail, span );
        builder_.switch_to( pass );
    };

    // A round trip misses a change of sign: -1 as a u32 comes back as -1.
    require( binary( Token_kind::Equal_equal, converted( value, from, span ), operand, boolean ) );

    if( source.is_signed != target.is_signed )
    {
        const Operand signed_side = source.is_signed ? operand : value;
        require( binary(
            Token_kind::Greater_equal, signed_side, constant( literal_pool_.add_integer( 0 ), signed_side.type ), boolean
        ) );
    }

    const Block_id pass = builder_.current();
    builder_.switch_to( fail );
    builder_.terminate_panic( span, Failure::Cast );
    builder_.switch_to( pass );

    return value;
}

Rvalue Lowering::checked_arithmetic( Token_kind op, Operand left, Operand right, Type_id type, Span span, bool wraps )
{
    const Type_table& table   = types_.table();
    const Type_id     boolean = table.builtin( Type_kind::Bool );

    if( !wraps )
    {
        check_overflow( op, left, right, type, span );
    }

    if( !table.is_integer( type ) || right.kind == Operand_kind::Constant ||
        ( op != Token_kind::Slash && op != Token_kind::Percent && op != Token_kind::Less_less &&
          op != Token_kind::Greater_greater ) )
    {
        return binary( op, left, right, type );
    }

    if( op == Token_kind::Less_less || op == Token_kind::Greater_greater )
    {
        Operand masked = temporary(
            binary(
                Token_kind::Amp,
                right,
                constant( literal_pool_.add_integer( table.get( type ).width - 1u ), right.type ),
                right.type
            ),
            right.type,
            span
        );

        return binary( op, left, masked, type );
    }

    const Block_id fail = builder_.add_block();
    const Block_id pass = builder_.add_block();

    builder_.terminate_branch(
        temporary(
            binary( Token_kind::Bang_equal, right, constant( literal_pool_.add_integer( 0 ), right.type ), boolean ),
            boolean,
            span
        ),
        pass,
        fail,
        span
    );

    builder_.switch_to( fail );
    builder_.terminate_panic( span, op == Token_kind::Slash ? Failure::Division : Failure::Remainder );
    builder_.switch_to( pass );

    if( !table.get( type ).is_signed )
    {
        return binary( op, left, right, type );
    }

    const Operand minus_one =
        temporary( unary( Token_kind::Minus, constant( literal_pool_.add_integer( 1 ), type ), type ), type, span );
    const Local_id result = builder_.add_local( type, span );

    const Block_id negate = builder_.add_block();
    const Block_id divide = builder_.add_block();
    const Block_id join   = builder_.add_block();

    builder_.terminate_branch(
        temporary( binary( Token_kind::Equal_equal, right, minus_one, boolean ), boolean, span ), negate, divide, span
    );

    builder_.switch_to( negate );

    if( op == Token_kind::Slash && !wraps )
    {
        check_overflow( Token_kind::Minus, constant( literal_pool_.add_integer( 0 ), type ), left, type, span );
    }

    builder_.assign(
        builder_.place( result ),
        op == Token_kind::Slash ? unary( Token_kind::Minus, left, type )
                                : use( constant( literal_pool_.add_integer( 0 ), type ) ),
        span
    );
    builder_.terminate_goto( join, span );

    builder_.switch_to( divide );
    builder_.assign( builder_.place( result ), binary( op, left, right, type ), span );
    builder_.terminate_goto( join, span );

    builder_.switch_to( join );
    return use( copy( builder_.place( result ), type ) );
}

void Lowering::check_overflow( Token_kind op, Operand left, Operand right, Type_id type, Span span )
{
    const Type_table& table = types_.table();

    if( !overflow_checks_ || !table.is_integer( type ) ||
        ( op != Token_kind::Plus && op != Token_kind::Minus && op != Token_kind::Star ) ||
        ( left.kind == Operand_kind::Constant && right.kind == Operand_kind::Constant ) )
    {
        return;
    }

    const Type_id  boolean = table.builtin( Type_kind::Bool );
    const Block_id fail    = builder_.add_block();
    const Block_id pass    = builder_.add_block();

    builder_.terminate_branch( temporary( overflows( op, left, right, boolean ), boolean, span ), fail, pass, span );

    builder_.switch_to( fail );
    builder_.terminate_panic( span, Failure::Overflow );
    builder_.switch_to( pass );
}

void Lowering::mark_wrapping( Node_id id )
{
    if( ast_.kind( id ) == Node_kind::Binary_expr )
    {
        wrapping_.insert( id.v );
        mark_wrapping( ast_.lhs( id ) );
        mark_wrapping( ast_.rhs( id ) );
    }
    else if( ast_.kind( id ) == Node_kind::Unary_expr )
    {
        wrapping_.insert( id.v );
        mark_wrapping( ast_.operand( id ) );
    }
}

// `<=>` is two comparisons and a choice of variant, and a NaN has no answer.
Operand Lowering::three_way( Operand left, Operand right, Span span )
{
    const Type_table& table    = types_.table();
    const Type_id     boolean  = table.builtin( Type_kind::Bool );
    const Type_id     ordering = table.declared( table.prelude().ordering );

    const auto test = [&]( Token_kind op, Operand a, Operand b ) -> Operand
    { return temporary( binary( op, a, b, boolean ), boolean, span ); };

    // A float literal is never a NaN.
    const auto may_be_nan = [&]( Operand side ) { return table.is_float( side.type ) && side.kind != Operand_kind::Constant; };

    if( may_be_nan( left ) || may_be_nan( right ) )
    {
        const Block_id fail = builder_.add_block();

        for( const Operand side : { left, right } )
        {
            if( !may_be_nan( side ) )
            {
                continue;
            }

            const Block_id pass = builder_.add_block();

            builder_.terminate_branch( test( Token_kind::Equal_equal, side, side ), pass, fail, span );
            builder_.switch_to( pass );
        }

        const Block_id pass = builder_.current();
        builder_.switch_to( fail );
        builder_.terminate_panic( span, Failure::Unordered );
        builder_.switch_to( pass );
    }

    const Local_id result = builder_.add_local( ordering, span );

    const Block_id less     = builder_.add_block();
    const Block_id not_less = builder_.add_block();
    const Block_id equal    = builder_.add_block();
    const Block_id greater  = builder_.add_block();
    const Block_id join     = builder_.add_block();

    const auto choose = [&]( Block_id block, u64 ordinal )
    {
        builder_.switch_to( block );
        builder_.assign( builder_.place( result ), use( constant( literal_pool_.add_integer( ordinal ), ordering ) ), span );
        builder_.terminate_goto( join, span );
    };

    builder_.terminate_branch( test( Token_kind::Less, left, right ), less, not_less, span );

    builder_.switch_to( not_less );
    builder_.terminate_branch( test( Token_kind::Equal_equal, left, right ), equal, greater, span );

    choose( less, 0 );
    choose( equal, 1 );
    choose( greater, 2 );

    builder_.switch_to( join );
    return copy( builder_.place( result ), ordering );
}

// The word the compiler's hash feeds the hasher: the value widened. A float gives its bits, with
// `-0.0` made `0.0` so the two that are `==` agree, and a NaN, which is `==` to nothing, panics.
Operand Lowering::supplied_hash( Operand value, Span span )
{
    const Type_table& table = types_.table();
    const Type_id     u64   = table.integer( 64, false );

    if( !table.is_float( value.type ) )
    {
        return converted( value, u64, span );
    }

    const Type_id boolean = table.builtin( Type_kind::Bool );
    const Type_id number  = table.get( value.type ).width == 32 ? table.integer( 32, false ) : u64;

    const Block_id pass = builder_.add_block();
    const Block_id fail = builder_.add_block();

    const Operand ordered = temporary( binary( Token_kind::Equal_equal, value, value, boolean ), boolean, span );

    builder_.terminate_branch( ordered, pass, fail, span );
    builder_.switch_to( fail );
    builder_.terminate_panic( span, Failure::Unhashable );
    builder_.switch_to( pass );

    const Operand zero     = constant( literal_pool_.add_float( 0.0 ), value.type );
    const Operand positive = temporary( binary( Token_kind::Plus, value, zero, value.type ), value.type, span );
    const Operand bits     = temporary( bits_of( positive, number ), number, span );

    return converted( bits, u64, span );
}

Operand Lowering::moved_if_owning( Operand operand )
{
    if( operand.kind != Operand_kind::Copy || !owns( operand.type ) )
    {
        return operand;
    }

    return move( operand.place, operand.type );
}

Operand Lowering::run_pointer( Node_id run )
{
    const Place place = lower_place( run );
    assert( place.local.is_valid() && place.num_projections == 1 );
    return copy( builder_.place( place.local ), builder_.type_of( place.local ) );
}

Place Lowering::lower_slice_or_run_index( Node_id id )
{
    const Node_id object   = ast_.object( id );
    const Node_id range    = ast_.index( id );
    const bool    is_range = ast_.kind( range ) == Node_kind::Range_expr;
    const bool    on_run   = types_.table().is_run( type_of( object ) );
    const Span    span     = ast_.span( id );
    const Type_id u64      = types_.table().integer( 64, false );
    const Type_id boolean  = types_.table().builtin( Type_kind::Bool );

    // The object first, then the bounds; a run's data is its start, a `T[*]` is its own.
    Operand run {};
    Operand size {};
    Operand data {};

    if( on_run )
    {
        run                           = run_pointer( object );
        size                          = temporary( run_size( run, u64 ), u64, span );
        const Type_id element_pointer = types_.table().many_pointer_to( types_.table().get( type_of( object ) ).element );
        data                          = temporary( run_data( run, element_pointer ), element_pointer, span );
    }
    else
    {
        data = lower_expression( object );
    }

    // Unsigned, so a negative bound fails the same compare a too-large one does.
    const auto bound = [&]( Node_id at ) { return converted( lower_expression( at ), u64, span ); };

    const Operand low  = bound( is_range ? ast_.lower( range ) : range );
    const Operand high = is_range ? bound( ast_.upper( range ) ) : Operand {};

    const Block_id fail    = builder_.add_block();
    const auto     require = [&]( Token_kind op, Operand left, Operand right )
    {
        const Block_id pass = builder_.add_block();
        builder_.terminate_branch( temporary( binary( op, left, right, boolean ), boolean, span ), pass, fail, span );
        builder_.switch_to( pass );
    };

    if( !is_range )
    {
        require( Token_kind::Less, low, size );
    }
    else
    {
        require( Token_kind::Less_equal, low, high );

        if( on_run )
        {
            require( Token_kind::Less_equal, high, size );
        }
    }

    const Block_id pass = builder_.current();
    builder_.switch_to( fail );
    builder_.terminate_panic( span, Failure::Bounds );
    builder_.switch_to( pass );

    const Operand start = temporary( binary( Token_kind::Plus, data, low, data.type ), data.type, span );

    if( !is_range )
    {
        return builder_.deref( start.place );
    }

    // `low <= high` holds, so the count cannot wrap. A reference carries no `const` in KIR.
    const Operand  count    = temporary( binary( Token_kind::Minus, high, low, u64 ), u64, span );
    const Type_id  run_type = types_.table().pointer_to( type_of( id ) );
    const Local_id slice    = builder_.into_temp( run_of( start, count, run_type ), run_type, span );

    return builder_.deref( builder_.place( slice ) );
}

// A constant index was checked against N by sema, so only a run-time one is checked here.
Place Lowering::lower_array_index( Node_id id )
{
    const Node_id object = ast_.object( id );
    const Span    span   = ast_.span( id );
    const Type_id u64    = types_.table().integer( 64, false );

    const Place    base  = lower_place( object );
    const Operand  index = converted( lower_expression( ast_.index( id ) ), u64, span );
    const Local_id local = builder_.into_temp( use( index ), u64, span ); // ponytail: one per constant index too

    if( index.kind != Operand_kind::Constant )
    {
        const Type_id  boolean = types_.table().builtin( Type_kind::Bool );
        const Operand  count   = constant( literal_pool_.add_integer( types_.table().get( type_of( object ) ).count ), u64 );
        const Block_id pass    = builder_.add_block();
        const Block_id fail    = builder_.add_block();

        builder_.terminate_branch(
            temporary( binary( Token_kind::Less, copy( builder_.place( local ), u64 ), count, boolean ), boolean, span ),
            pass,
            fail,
            span
        );
        builder_.switch_to( fail );
        builder_.terminate_panic( span, Failure::Bounds );
        builder_.switch_to( pass );
    }

    return builder_.index( base, local );
}

bool Lowering::is_move_parameter( Node_id param ) const
{
    const Node_id annotation = ast_.annotation( param );

    return ast_.kind( annotation ) == Node_kind::Mode_type && ast_.keyword( annotation ) == Keyword::Move;
}

void Lowering::drop_place( Place place, Type_id type, Span span, bool replacing )
{
    keel::drop_place( builder_, ast_, types_, place, type, span, replacing );
}

bool Lowering::is_construction( Node_id id ) const
{
    if( ast_.kind( id ) != Node_kind::Call_expr )
    {
        return false;
    }

    const Node_id decl = resolution_.declaration_of( ast_.callee( id ) );

    return decl.is_valid() && is_aggregate( ast_.kind( decl ) );
}

bool Lowering::initialises_field( const Place& target ) const
{
    return ast_.kind( declaration_ ) == Node_kind::Constructor_decl && target.local == receiver_ && target.num_projections == 2;
}

bool Lowering::writes_a_slot( Node_id target ) const
{
    // An array's element is part of it, so writing one replaces what was there.
    if( ast_.kind( target ) == Node_kind::Index_expr && !types_.callee_of( target ).is_valid() &&
        !types_.table().is_array( types_.type_of( ast_.object( target ) ) ) )
    {
        return true;
    }

    return false;
}

// A loop dropping `p[0]` to `p[n - 1]`, or only the operands for a `T` that owns nothing.
void Lowering::lower_destroy( Node_id id )
{
    const Span span = ast_.span( id );

    Operand pointer = lower_expression( ast_.pointer( id ) );

    // Evaluated whether or not anything owns: the count may be a call.
    const bool has_count = ast_.count( id ).is_valid();
    Operand    count     = has_count ? lower_expression( ast_.count( id ) ) : Operand {};

    const Type_id element_type = types_.table().get( pointer.type ).element;

    if( !owns( element_type ) )
    {
        return;
    }

    // Read once: a destructor may write the field either came from.
    const Local_id pointer_temp = builder_.into_temp( use( pointer ), pointer.type, span );

    if( !has_count )
    {
        drop_place( builder_.deref( builder_.place( pointer_temp ) ), element_type, span );
        return;
    }

    const Local_id count_temp = builder_.into_temp( use( count ), count.type, span );

    const Literal_id zero  = literal_pool_.add_integer( 0 );
    const Literal_id one   = literal_pool_.add_integer( 1 );
    const Local_id   index = builder_.into_temp( use( constant( zero, count.type ) ), count.type, span );

    const Block_id header = builder_.add_block();
    const Block_id body   = builder_.add_block();
    const Block_id exit   = builder_.add_block();

    builder_.terminate_goto( header, span );
    builder_.switch_to( header );

    const Type_id  boolean = types_.table().builtin( Type_kind::Bool );
    const Local_id more    = builder_.into_temp(
        binary(
            Token_kind::Less,
            copy( builder_.place( index ), count.type ),
            copy( builder_.place( count_temp ), count.type ),
            boolean
        ),
        boolean,
        span
    );

    builder_.terminate_branch( copy( builder_.place( more ), boolean ), body, exit, span );
    builder_.switch_to( body );

    const Local_id slot = builder_.into_temp(
        binary(
            Token_kind::Plus,
            copy( builder_.place( pointer_temp ), pointer.type ),
            copy( builder_.place( index ), count.type ),
            pointer.type
        ),
        pointer.type,
        span
    );

    drop_place( builder_.deref( builder_.place( slot ) ), element_type, span );

    builder_.assign(
        builder_.place( index ),
        binary( Token_kind::Plus, copy( builder_.place( index ), count.type ), constant( one, count.type ), count.type ),
        span
    );
    builder_.terminate_goto( header, span );

    builder_.switch_to( exit );
}

void Lowering::lower_construction( Place target, Node_id call_expr )
{
    const Span span = ast_.span( call_expr );
    // Which constructor, not just which type: a class may declare several, and the checker is
    // where the choice between them was made.
    const Node_id constructor = types_.callee_of( call_expr );

    assert(
        constructor.is_valid() && ast_.kind( constructor ) == Node_kind::Constructor_decl &&
        "the checker rejects constructing a type that has none"
    );

    const std::span<const Node_id> parameters = ast_.params( constructor );
    const std::span<const Node_id> arguments  = ast_.arguments( call_expr );

    // The instance being built, not the enclosing function: `Box<i32>( 7 )` inside a non-generic
    // `main` has no bindings of its own, and the constructor's parameters are written in `T`.
    const Bindings callee_bound = bindings_for_call( call_expr );

    const Type_id receiver_type = binding_type_under( parameters[0], callee_bound );

    std::vector<Operand> operands;

    operands.reserve( arguments.size() + 1 );
    operands.push_back( copy(
        builder_.place(
            builder_.into_temp( address_of( target, receiver_type, Address_purpose::Initialise ), receiver_type, span )
        ),
        receiver_type
    ) );

    // Two loops, as a plain call has, and for the same reason: every argument is lowered before any
    // is converted, so the statements come out in the order they were written (§7.1).
    for( std::size_t i = 0; i < arguments.size(); ++i )
    {
        operands.push_back( lower_argument( arguments[i], parameters[i + 1], callee_bound ) );
    }

    for( std::size_t i = 0; i < arguments.size(); ++i )
    {
        operands[i + 1] =
            converted( operands[i + 1], binding_type_under( parameters[i + 1], callee_bound ), ast_.span( arguments[i] ) );

        if( is_move_parameter( parameters[i + 1] ) )
        {
            operands[i + 1] = moved_if_owning( operands[i + 1] );
        }
    }

    const u32     first = builder_.add_operands( operands );
    const Type_id type  = type_of( constructor ); // void

    // The result is discarded, but Assign stays total: a void local is what the backend drops the
    // assignment from, leaving the bare call.
    builder_.into_temp(
        call( constructor, first, narrow_cast<u32>( operands.size() ), type, type_arguments_for_call( call_expr ) ), type, span
    );
}

Block_id Lowering::break_target()
{
    assert( !loops_.empty() && "the checker rejects a break outside a loop or switch" );

    Loop_targets& loop = loops_.back();

    if( !loop.break_target.is_valid() )
    {
        loop.break_target = builder_.add_block();
    }

    return loop.break_target;
}

Lowering::Loop_targets& Lowering::enclosing_loop()
{
    for( std::size_t i = loops_.size(); i > 0; --i )
    {
        if( !loops_[i - 1].is_switch )
        {
            return loops_[i - 1];
        }
    }

    assert( false && "the checker rejects a continue outside a loop" );
    return loops_.back();
}

Local_id Lowering::call_result( Node_id id, Rvalue call, Type_id result_type, Span span )
{
    // A `never` call is a statement (§6.7), so nothing reads past it.
    if( types_.table().is_never( result_type ) )
    {
        const Local_id result = builder_.into_temp( call, types_.table().builtin( Type_kind::Void ), span );
        builder_.terminate_unreachable( span );
        return result;
    }

    if( id == discarded_ && !owns( result_type ) )
    {
        return builder_.into_temp( call, types_.table().builtin( Type_kind::Void ), span );
    }
    else
    {
        const Local_id result_temp = builder_.into_temp( call, result_type, span );

        drops_.own_temporary( result_temp );

        return result_temp;
    }
}

std::vector<Node_id> Lowering::paired_payload( Type_id instance, Node_id variant, std::size_t count )
{
    std::span<const Node_id> payload = ast_.payload( variant );

    if( count == payload.size() )
    {
        return std::vector<Node_id>( payload.begin(), payload.end() );
    }

    return carried_payload( ast_, types_.table(), instance, variant, types_.recorded() );
}

Block_id Lowering::continue_target()
{
    assert( !loops_.empty() && "the checker rejects a continue outside a loop" );

    Loop_targets& loop = enclosing_loop();

    // A while sets this to its header up front, so only a for's latch is ever allocated here.
    if( !loop.continue_target.is_valid() )
    {
        loop.continue_target = builder_.add_block();
    }

    return loop.continue_target;
}

// A local rather than an operand: the result is read after the join, and KIR has no phi (§3.1).
Operand Lowering::lower_short_circuit( Node_id id )
{
    const Token_kind op   = ast_.op( id );
    const Span       span = ast_.span( id );
    const Type_id    type = type_of( id );

    const Local_id result = builder_.add_local( type, span );

    builder_.assign( builder_.place( result ), use( lower_expression( ast_.lhs( id ) ) ), span );

    const Block_id right_block = builder_.add_block();
    const Block_id join        = builder_.add_block();

    // `&&` evaluates the right side when the left is true, `||` when it is false. That is the only
    // difference between them.
    const bool is_and = op == Token_kind::Amp_amp;

    builder_.terminate_branch(
        copy( builder_.place( result ), type ), is_and ? right_block : join, is_and ? join : right_block, span
    );

    const std::size_t first = drops_.mark();
    builder_.switch_to( right_block );
    builder_.assign( builder_.place( result ), use( lower_expression( ast_.rhs( id ) ) ), span );

    // From wherever lowering ended up, not from right_block: a nested `&&` on the right leaves the
    // cursor in its own join.
    builder_.terminate_goto( join, span );
    drops_.one_path_since( first );

    builder_.switch_to( join );

    return copy( builder_.place( result ), type );
}

Operand Lowering::lower_conditional( Node_id id )
{
    const Span    span = ast_.span( id );
    const Type_id type = type_of( id );

    const Local_id result = builder_.add_local( type, span );

    const Block_id then_block = builder_.add_block();
    const Block_id else_block = builder_.add_block();
    const Block_id join       = builder_.add_block();

    builder_.terminate_branch( lower_expression( ast_.condition( id ) ), then_block, else_block, span );

    // Assigned from wherever lowering the arm ended, since a nested conditional leaves the cursor in
    // its own join.
    const auto lower_arm = [&]( Node_id arm )
    {
        // Widened, since an expectation reaches an arm without retyping it, and moved when it owns.
        const std::size_t first = drops_.mark();
        const Operand     value = converted( lower_expression( arm ), type, ast_.span( arm ) );

        builder_.assign( builder_.place( result ), use( moved_if_owning( value ) ), ast_.span( arm ) );
        builder_.terminate_goto( join, span );
        drops_.one_path_since( first );
    };

    builder_.switch_to( then_block );
    lower_arm( ast_.then_branch( id ) );

    builder_.switch_to( else_block );
    lower_arm( ast_.else_branch( id ) );

    builder_.switch_to( join );

    // Nothing else drops the result when the arms own. Listed after the join, once an arm has
    // filled it.
    drops_.own_temporary( result );

    return copy( builder_.place( result ), type );
}

Operand Lowering::lower_struct_literal( Node_id id )
{
    // A temporary, then one assignment per field. Every value is lowered before any field is
    // written, so a `try` that leaves between them finds nothing half-built.
    const Type_id  type = type_of( id );
    const Span     span = ast_.span( id );
    const Local_id temp = builder_.add_local( type, span );

    builder_.storage_live( temp, span );

    // Positional form names no field, so the i-th initialiser fills the i-th field. The two
    // forms cannot be mixed - the checker rejects that - so an index is enough here. Fields only,
    // as the checker counts them: a method declared before them would misalign every one.
    std::vector<Node_id> fields;

    for( const Node_id member : ast_.members( types_.table().get( type ).declaration ) )
    {
        if( ast_.kind( member ) == Node_kind::Field_decl )
        {
            fields.push_back( member );
        }
    }

    std::vector<std::pair<Node_id, Operand>> values;
    std::size_t                              index = 0;

    for( const Node_id initialiser : ast_.initialisers( id ) )
    {
        const Symbol_id name  = ast_.name( initialiser );
        const Node_id   field = name.is_valid() ? field_of( type, name ) : fields[index];

        assert( field.is_valid() && "checker should have rejected an unknown field" );

        // §6.4 may have widened the value to reach the field, the same as an argument reaching
        // a parameter. Saying so here is what keeps a backend from re-deriving it.
        values.emplace_back(
            initialiser,
            converted(
                lower_expression( ast_.value( initialiser ) ),
                field_type( ast_, types_.table(), type, field, types_.recorded() ),
                ast_.span( initialiser )
            )
        );

        index += 1;
    }

    index = 0;

    for( const auto& [initialiser, value] : values )
    {
        const Symbol_id name  = ast_.name( initialiser );
        const Node_id   field = name.is_valid() ? field_of( type, name ) : fields[index];

        // Moved, not copied, when the field owns something: a copy would leave the temporary and
        // the field holding one resource between them, and both would be dropped.
        builder_.assign(
            builder_.field( builder_.place( temp ), field ), use( moved_if_owning( value ) ), ast_.span( initialiser )
        );

        index += 1;
    }

    drops_.own_temporary( temp );

    return copy( builder_.place( temp ), type );
}

// As lower_struct_literal: every value is lowered before any element is written.
Operand Lowering::lower_array_literal( Node_id id )
{
    const Type_id  type    = type_of( id );
    const Type_id  element = types_.table().get( type ).element;
    const Type_id  u64     = types_.table().integer( 64, false );
    const Span     span    = ast_.span( id );
    const Local_id temp    = builder_.add_local( type, span );

    builder_.storage_live( temp, span );

    const std::span<const Node_id> elements = ast_.children( id );
    std::vector<Operand>           values;

    for( const Node_id value : elements )
    {
        values.push_back( converted( lower_expression( value ), element, ast_.span( value ) ) );
    }

    for( std::size_t k = 0; k < values.size(); ++k )
    {
        const Local_id at = builder_.into_temp( use( constant( literal_pool_.add_integer( k ), u64 ) ), u64, span );

        builder_.assign(
            builder_.index( builder_.place( temp ), at ), use( moved_if_owning( values[k] ) ), ast_.span( elements[k] )
        );
    }

    drops_.own_temporary( temp );

    return copy( builder_.place( temp ), type );
}

Operand Lowering::lower_string_literal( Node_id id )
{
    const Span    span        = ast_.span( id );
    const Type_id type        = type_of( id );
    const Node_id constructor = types_.callee_of( id );

    // `str( this, data, size )`, called as the checker lets no program call it.
    const std::span<const Node_id> parameters = ast_.params( constructor );

    const Type_id    receiver_type = binding_type_under( parameters[0], {} );
    const Type_id    data_type     = type_under( parameters[1], {} );
    const Type_id    size_type     = type_under( parameters[2], {} );
    const Literal_id bytes         = ast_.literal( id );

    // Built into `str` itself, which an adopted literal then hands to its type's `from`.
    const Type_id  text  = types_.type_of( ast_.enclosing_aggregate( constructor ) );
    const Local_id local = builder_.add_local( text, span );

    const Operand operands[] = {
        temporary( address_of( builder_.place( local ), receiver_type, Address_purpose::Initialise ), receiver_type, span ),
        temporary( literal_bytes( bytes, data_type ), data_type, span ),
        constant( literal_pool_.add_integer( literal_pool_.string( bytes ).size() ), size_type ),
    };

    const u32     first = builder_.add_operands( operands );
    const Type_id none  = type_of( constructor );

    builder_.into_temp( call( constructor, first, 3, none ), none, span );

    if( type == text )
    {
        return copy( builder_.place( local ), type );
    }

    const Node_id      from = types_.table().prelude().from;
    Requirement_target target =
        requirement_target( ast_.members( from )[0], type, types_.table().interface( from, std::array { text }, "from" ) );

    assert( target.method.is_valid() && "an adopted literal's type implements `from<str>`" );

    const Operand operand[] = { copy( builder_.place( local ), text ) };

    return emit_call( id, target.method, operand, target.at.bound, std::move( target.at.type_arguments ) );
}

Operand Lowering::lower_binary( Node_id id )
{
    const Token_kind op = ast_.op( id );

    // Not operations over two operands - the right side may not run at all - so they take a
    // different path entirely. Lowering them here would evaluate both sides unconditionally.
    if( op == Token_kind::Amp_amp || op == Token_kind::Pipe_pipe )
    {
        return lower_short_circuit( id );
    }

    // `a == b` on an aggregate, where the checker chose the left operand's operator.
    if( const Node_id method = types_.callee_of( id ); method.is_valid() )
    {
        return lower_operator_call( id, method, bindings_for_call( id ), type_arguments_for_call( id ) );
    }

    // Under a bound, at a type whose own conformance answers the operator.
    if( Requirement_target target = bound_operator( op, type_of( ast_.lhs( id ) ) ); target.method.is_valid() )
    {
        return lower_operator_call( id, target.method, target.at.bound, std::move( target.at.type_arguments ) );
    }

    const Span    span      = ast_.span( id );
    const Type_id operation = operation_type( id );

    // Both operands are lowered before either is converted: lowering is what can have effects,
    // so its order is §7.1's order, and the conversions are pure and can follow.
    const Operand raw_left  = lower_expression( ast_.lhs( id ) );
    const Operand raw_right = lower_expression( ast_.rhs( id ) );

    // A shift's count keeps its own type - it is a width, not a value meeting the left operand.
    const bool is_shift = op == Token_kind::Less_less || op == Token_kind::Greater_greater;

    // D41's other case: no common type, so each operand widens within its own kind and the backend
    // compares them as they are. The only binary whose operands reach KIR with different types.
    const bool by_cases = is_comparison( op ) && !operation.is_valid();

    const bool offsets = types_.table().is_many_pointer( raw_left.type );

    const Operand left  = converted( raw_left, by_cases ? comparison_width( raw_left.type ) : operation, span );
    const Operand right = is_shift || offsets
                              ? raw_right
                              : converted( raw_right, by_cases ? comparison_width( raw_right.type ) : operation, span );

    // The type the operation *produces*, which for a comparison is bool and for `<=>` is ordering.
    const Type_id type = type_of( id );

    if( op == Token_kind::Less_equal_greater )
    {
        return three_way( left, right, span );
    }
    return temporary( checked_arithmetic( op, left, right, type, span, wrapping_.contains( id.v ) ), type, span );
}

Lowering::Requirement_target Lowering::bound_operator( Token_kind op, Type_id held ) const
{
    const Type_table& table     = types_.table();
    const Type_id     interface = Conformance_model::interface_for( table, op );

    if( ( !table.is_struct( held ) && !table.is_enum( held ) ) || !interface.is_valid() )
    {
        return {};
    }

    // Each prelude bound declares exactly one method.
    const std::span<const Node_id> members = ast_.members( table.get( interface ).declaration );

    assert( members.size() == 1 );

    return requirement_target( members[0], held, interface );
}

Operand Lowering::call_operator(
    Node_id                  id,
    Node_id                  method,
    Place                    object,
    std::span<const Node_id> arguments,
    const Bindings&          bindings,
    std::vector<Type_id>     type_arguments
)
{
    // A bound's operator: through an interface's vtable, or the instance's implementer called directly.
    if( ast_.is_requirement( method ) )
    {
        const Node_id      operand = ast_.kind( id ) == Node_kind::Binary_expr  ? ast_.lhs( id )
                                     : ast_.kind( id ) == Node_kind::Unary_expr ? ast_.operand( id )
                                                                                : ast_.target( id );
        Type_table&        table   = types_.table();
        const Type_id      held    = type_of( operand );
        Requirement_target target  = requirement_target( method, held, table.substitute( types_.through_of( id ), bindings_ ) );

        if( target.through_vtable )
        {
            const bool    reads    = ast_.is_const_method( method );
            const Operand receiver = address_operand(
                object,
                table.pointer_to( held, reads ),
                ast_.span( id ),
                reads ? Address_purpose::Read : Address_purpose::Borrow
            );

            return lower_method_call_on(
                id, method, receiver, arguments, type_under( method, target.at.bound ), target.at.bound, {}
            );
        }

        return call_operator( id, target.method, object, arguments, target.at.bound, std::move( target.at.type_arguments ) );
    }

    const Operand receiver = address_operand(
        object,
        binding_type_under( ast_.params( method )[0], bindings ),
        ast_.span( id ),
        ast_.is_const_method( method ) ? Address_purpose::Read : Address_purpose::Borrow
    );

    return lower_method_call_on(
        id, method, receiver, arguments, type_under( method, bindings ), bindings, std::move( type_arguments )
    );
}

// `a + b` calls `a.operator+( b )`, as `a == b` calls `a.operator==( b )`; `a != b` negates it, and
// `a < b` compares what `a.operator<=>( b )` answers with a variant.
Operand
Lowering::lower_operator_call( Node_id id, Node_id method, const Bindings& bindings, std::vector<Type_id> type_arguments )
{
    const Span       span = ast_.span( id );
    const Node_id    rhs  = ast_.rhs( id );
    const Token_kind op   = ast_.op( id );

    const Operand result =
        call_operator( id, method, lower_place( ast_.lhs( id ) ), { &rhs, 1 }, bindings, std::move( type_arguments ) );

    if( !is_comparison( op ) || op == Token_kind::Equal_equal || op == Token_kind::Less_equal_greater )
    {
        return result;
    }

    if( op == Token_kind::Bang_equal )
    {
        return copy(
            builder_.place( builder_.into_temp( unary( Token_kind::Bang, result, result.type ), result.type, span ) ),
            result.type
        );
    }

    // less = 0, equal = 1, greater = 2.
    const auto [test, ordinal] = [&]() -> std::pair<Token_kind, u64>
    {
        switch( op )
        {
        case Token_kind::Less:
            return { Token_kind::Equal_equal, 0 };
        case Token_kind::Less_equal:
            return { Token_kind::Bang_equal, 2 };
        case Token_kind::Greater:
            return { Token_kind::Equal_equal, 2 };
        default:
            assert( op == Token_kind::Greater_equal && "the checker sends only comparisons to an operator" );
            return { Token_kind::Bang_equal, 0 };
        }
    }();

    const Type_id boolean = types_.table().builtin( Type_kind::Bool );
    const Operand variant = constant( literal_pool_.add_integer( ordinal ), result.type );

    return temporary( binary( test, result, variant, boolean ), boolean, span );
}

// `v[i]` calls `v.operator[]( i )`, and the place is what the returned pointer points at.
Operand Lowering::lower_operator_index( Node_id id, Node_id method )
{
    const Span     span     = ast_.span( id );
    const Bindings bindings = bindings_for_call( id );

    const Node_id index = ast_.index( id );

    const Operand receiver = address_operand(
        lower_place( ast_.object( id ) ), binding_type_under( ast_.params( method )[0], bindings ), span, Address_purpose::Read
    );

    return lower_method_call_on(
        id, method, receiver, { &index, 1 }, type_under( method, bindings ), bindings, type_arguments_for_call( id )
    );
}

Operand Lowering::lower_unary( Node_id id )
{
    const Token_kind op   = ast_.op( id );
    const Type_id    type = type_of( id );
    const Span       span = ast_.span( id );

    // `-a` on an aggregate, where the checker chose its operator.
    if( const Node_id method = types_.callee_of( id ); method.is_valid() && op != Token_kind::Amp )
    {
        return call_operator(
            id, method, lower_place( ast_.operand( id ) ), {}, bindings_for_call( id ), type_arguments_for_call( id )
        );
    }

    // Settled before the operand is lowered: `*p` names a place and `&x` wants where x lives, so
    // reading first would run `&*get()`'s call twice.
    if( op == Token_kind::Star )
    {
        return copy( lower_place( id ), type );
    }

    if( op == Token_kind::Amp )
    {
        // A function is not a place, so the callee travels on the rvalue: the callable the checker chose,
        // since an overload set's name binds only its first candidate.
        if( types_.table().is_function( type ) )
        {
            const Rvalue address = function_address( types_.callee_of( id ), type, type_arguments_for_call( id ) );

            return temporary( address, type, span );
        }

        // Nor is a field: `&C::x` is an offset, which names the field and no object.
        if( types_.table().is_field( type ) )
        {
            return temporary( field_offset( types_.callee_of( id ), type ), type, span );
        }

        // A place, not an operand - which is why verify's Address_of case looks at a.place and
        // ignores the operand's kind.
        const Rvalue address = address_of( lower_place( ast_.operand( id ) ), type, Address_purpose::Borrow );

        return temporary( address, type, span );
    }

    const Operand a = lower_expression( ast_.operand( id ) );

    if( op == Token_kind::Minus && !wrapping_.contains( id.v ) )
    {
        check_overflow( Token_kind::Minus, constant( literal_pool_.add_integer( 0 ), type ), a, type, span );
    }

    return temporary( unary( op, a, type ), type, span );
}

// D7. `case Shape::Circle( r ):` gives `r` a local. An owning field is borrowed in place, as D31
// borrows a bare owning parameter; anything else is copied. A consuming switch moves the whole value
// out first, so the switch's own drop is skipped, and each binding owns its field.
void Lowering::bind_variant_pattern( Place matched, Type_id matched_type, Node_id label, bool consumes )
{
    if( ast_.kind( label ) != Node_kind::Variant_pattern )
    {
        return;
    }

    const std::span<const Node_id> bindings = ast_.bindings( label );

    const Type_id enum_type = type_of( ast_.variant_path( label ) );
    const Node_id decl      = types_.table().get( enum_type ).declaration;

    const std::optional<Constant_value> ordinal = types_.constant_of( ast_.variant_path( label ) );

    assert( ordinal.has_value() && "the checker records an ordinal for every variant it accepts" );

    const Node_id variant = ast_.variants( decl )[static_cast<std::size_t>( ordinal->magnitude )];

    const std::vector<Node_id> payload = paired_payload( enum_type, variant, bindings.size() );

    // Moved into storage nothing drops: its fields are the bindings' now.
    if( consumes && !bindings.empty() )
    {
        const Type_id whole = builder_.type_of( matched.local );

        matched = builder_.place( builder_.into_temp( use( move( matched, whole ) ), whole, ast_.span( label ) ) );
    }

    // §6.7: the payload is the live member's.
    if( types_.table().is_union( matched_type ) )
    {
        matched = builder_.member( matched, enum_type );
    }

    for( std::size_t i = 0; i < bindings.size() && i < payload.size(); ++i )
    {
        const Type_id field_type = keel::field_type( ast_, types_.table(), enum_type, payload[i], types_.recorded() );
        const Span    span       = ast_.span( bindings[i] );
        const Place   field      = builder_.field( matched, payload[i] );

        const bool     borrowed   = !consumes && owns( field_type );
        const Type_id  local_type = borrowed ? types_.table().pointer_to( field_type ) : field_type;
        const Local_id local      = builder_.add_local( local_type, span, ast_.name( bindings[i] ) );

        locals_.emplace( bindings[i].v, local );

        builder_.storage_live( local, span );

        if( consumes )
        {
            drops_.add_to_scope( local );
        }

        if( borrowed )
        {
            borrowed_bindings_.insert( bindings[i].v );
            builder_.assign( builder_.place( local ), address_of( field, local_type, Address_purpose::Payload ), span );
        }
        else
        {
            builder_.assign( builder_.place( local ), use( copy( field, field_type ) ), span );
        }
    }
}

// A variant with no payload, in an enum that has them elsewhere: a local with only its tag written.
Operand Lowering::lower_empty_variant( Node_id id )
{
    const Type_id type = type_of( id );
    const Span    span = ast_.span( id );

    const Local_id local = builder_.add_local( type, span );
    const Place    place = builder_.place( local );

    const std::optional<Constant_value> ordinal = types_.constant_of( id );

    assert( ordinal.has_value() && "the checker records an ordinal for every variant it accepts" );

    const Type_id tag_type = types_.table().get( type ).element;

    builder_.assign(
        builder_.tag( place ), use( constant( literal_pool_.add_integer( ordinal->magnitude ), tag_type ) ), span
    );

    return copy( place, type );
}

// D7. `Shape::Circle( 1.0 )` is assembled in place, as a struct literal is: the tag, then each field.
Operand Lowering::lower_variant_construction( Node_id id )
{
    const Node_id                  path      = ast_.callee( id );
    const std::span<const Node_id> arguments = ast_.arguments( id );
    const Type_id                  type      = type_of( id );
    const Span                     span      = ast_.span( id );

    const Local_id local = builder_.add_local( type, span );
    const Place    place = builder_.place( local );

    // The ordinal the checker recorded on the path, which is the same number a payload-free variant
    // lowers to on its own.
    const std::optional<Constant_value> ordinal = types_.constant_of( path );

    assert( ordinal.has_value() && "the checker records an ordinal for every variant it accepts" );

    // The payload fields, in declaration order - which is the order the arguments were checked in.
    const Node_id decl    = types_.table().get( type ).declaration;
    const Node_id variant = ast_.variants( decl )[static_cast<std::size_t>( ordinal->magnitude )];

    const std::vector<Node_id> payload = paired_payload( type, variant, arguments.size() );

    // Each field's type through this instance, since the declaration records `T`. Every value is
    // lowered before the tag is written, as a struct literal's are.
    std::vector<Operand> values;

    for( std::size_t i = 0; i < arguments.size() && i < payload.size(); ++i )
    {
        values.push_back( converted(
            lower_expression( arguments[i] ),
            keel::field_type( ast_, types_.table(), type, payload[i], types_.recorded() ),
            span
        ) );
    }

    const Type_id tag_type = types_.table().get( type ).element;

    builder_.assign(
        builder_.tag( place ), use( constant( literal_pool_.add_integer( ordinal->magnitude ), tag_type ) ), span
    );

    // The variant owns what it was given, so a temporary is moved in rather than dropped twice.
    for( std::size_t i = 0; i < values.size(); ++i )
    {
        builder_.assign( builder_.field( place, payload[i] ), use( moved_if_owning( values[i] ) ), span );
    }

    drops_.own_temporary( local );

    return copy( place, type );
}

// D6. The operand moves into storage nothing drops. `ok` yields its payload; `err` is written into
// the return slot and returned through every drop the function owes.
Operand Lowering::lower_try( Node_id id )
{
    const Node_id operand    = ast_.operand( id );
    const Span    span       = ast_.span( id );
    const Operand expression = moved_if_owning( lower_expression( operand ) );
    const Type_id type       = expression.type;
    const Type_id yielded    = type_of( id );

    const Local_id local = builder_.into_temp( use( expression ), type, span );
    const Place    taken = builder_.place( local );

    const std::span<const Node_id> variants = ast_.variants( types_.table().get( type ).declaration );

    // The prelude's order.
    const Node_id ok  = variants[0];
    const Node_id err = variants[1];

    const Type_id boolean  = types_.table().builtin( Type_kind::Bool );
    const Type_id tag_type = types_.table().get( type ).element;

    const Local_id test = builder_.into_temp(
        binary(
            Token_kind::Equal_equal,
            copy( builder_.tag( taken ), tag_type ),
            constant( literal_pool_.add_integer( 0 ), tag_type ),
            boolean
        ),
        boolean,
        span
    );

    const Block_id ok_block  = builder_.add_block();
    const Block_id err_block = builder_.add_block();

    builder_.terminate_branch( copy( builder_.place( test ), boolean ), ok_block, err_block, span );

    builder_.switch_to( err_block );
    const Place   slot      = builder_.place( k_return_slot );
    const Type_id slot_type = builder_.type_of( k_return_slot );
    builder_.assign(
        builder_.tag( slot ), use( constant( literal_pool_.add_integer( 1 ), types_.table().get( slot_type ).element ) ), span
    );

    const std::vector<Node_id> err_fields = carried_payload( ast_, types_.table(), type, err, types_.recorded() );
    for( const Node_id field : err_fields )
    {
        write_error(
            builder_.field( slot, field ),
            field_type( ast_, types_.table(), slot_type, field, types_.recorded() ),
            builder_.field( taken, field ),
            field_type( ast_, types_.table(), type, field, types_.recorded() ),
            span
        );
    }

    drops_.drop_built( span );
    drops_.unwind_to( 0, span );
    builder_.terminate_return( span );

    builder_.switch_to( ok_block );
    const std::vector<Node_id> ok_fields = carried_payload( ast_, types_.table(), type, ok, types_.recorded() );

    if( ok_fields.empty() )
    {
        return Operand { .type = types_.table().builtin( Type_kind::Void ) };
    }

    const Local_id value = builder_.into_temp( use( copy( builder_.field( taken, ok_fields[0] ), yielded ) ), yielded, span );

    drops_.own_temporary( value );

    return copy( builder_.place( value ), yielded );
}

// D29/D32. A method call is an ordinary call with the receiver's address prepended as argument 0.
Operand Lowering::lower_method_call( Node_id id )
{
    const Node_id callee = ast_.callee( id );
    const Node_id object = ast_.object( callee );

    const Node_id method = types_.callee_of( id );

    assert( method.is_valid() && "the checker records the method for every call it accepts" );

    if( ast_.is_requirement( method ) )
    {
        return lower_interface_call( id, method );
    }

    const std::span<const Node_id> parameters = ast_.params( method );

    // D22: on a `Point*` the pointer's value is already the receiver's address; taking its address
    // would pass a `Point**`.
    const Type_id object_type = type_of( object );

    const Operand receiver = types_.table().is_pointer( object_type )
                                 ? lower_expression( object )
                                 : address_operand(
                                       lower_place( object ),
                                       binding_type_under( parameters[0], bindings_for_call( id ) ),
                                       ast_.span( object ),
                                       ast_.is_const_method( method ) ? Address_purpose::Read : Address_purpose::Borrow
                                   );

    // The receiver's own arguments, not the enclosing function's: `p.first()` on a `Pair<i32>`
    // reads `T` out of the declaration, and only this instance says what it is.
    return lower_method_call_on(
        id, method, receiver, ast_.arguments( id ), type_of( id ), bindings_for_call( id ), type_arguments_for_call( id )
    );
}

// A method an interface declares. On an interface the call stays a call of that method, and C
// reaches the implementer's through the vtable the receiver carries. On a parameter bound by the
// interface, the instance knows the implementer, so the call is a direct one to its method.
Operand Lowering::lower_interface_call( Node_id id, Node_id requirement )
{
    const Node_id object      = ast_.object( ast_.callee( id ) );
    const Type_id object_type = type_of( object );
    // As written: a `T` bound to `i32*` holds the pointer, where a `T*` points at what it holds.
    const bool    pointer = types_.table().is_pointer( types_.type_of( object ) );
    const Type_id held    = pointer ? types_.table().get( object_type ).element : object_type;
    const bool    reads   = ast_.is_const_method( requirement );

    const auto receiver = [&]( Type_id address ) -> Operand
    {
        return pointer ? lower_expression( object )
                       : address_operand(
                             lower_place( object ),
                             address,
                             ast_.span( object ),
                             reads ? Address_purpose::Read : Address_purpose::Borrow
                         );
    };

    // The instance selection chose, recorded on the callee.
    Requirement_target target = requirement_target( requirement, held, type_of( ast_.callee( id ) ) );

    // No method answers where the compiler's conformance does, and only `hashable`'s `hash` has one:
    // it is a call of the prelude hasher's `write` with the value's word.
    if( !target.method.is_valid() )
    {
        assert(
            ast_.enclosing_aggregate( requirement ) == types_.table().prelude().hashable &&
            "a bound is met only by a declared `implements` or the compiler's hash"
        );

        const Operand value = pointer ? copy( builder_.deref( lower_place( object ) ), held ) : lower_expression( object );

        const Operand word   = supplied_hash( value, ast_.span( id ) );
        const Operand hasher = lower_argument( ast_.arguments( id )[0], ast_.params( requirement )[1], {} );
        const u32     first  = builder_.add_operands( std::vector<Operand> { hasher, word } );

        const Node_id write = types_.table().prelude().hasher_write;
        assert( write.is_valid() && "the prelude's `hasher` has a `write`" );

        const Local_id result = call_result( id, call( write, first, 2, type_of( id ) ), type_of( id ), ast_.span( id ) );
        return copy( builder_.place( result ), type_of( id ) );
    }

    return lower_method_call_on(
        id,
        target.method,
        receiver( types_.table().pointer_to( held, reads ) ),
        ast_.arguments( id ),
        type_of( id ),
        target.at.bound,
        std::move( target.at.type_arguments )
    );
}

// Every method call: the lowered receiver, then the written arguments against parameters from 1.
// Arguments, result type and bindings are given, so an operator's operand, `v[i]`'s pointer result
// and an implementer reached through a bound can use it.
Operand Lowering::lower_method_call_on(
    Node_id                  id,
    Node_id                  method,
    Operand                  receiver,
    std::span<const Node_id> arguments,
    Type_id                  type,
    const Bindings&          callee_bound,
    std::vector<Type_id>     type_arguments
)
{
    std::vector<Operand> operands { receiver };

    for( const Operand& argument : lower_call_arguments( arguments, ast_.params( method ).subspan( 1 ), callee_bound ) )
    {
        operands.push_back( argument );
    }

    return emit_call( id, method, operands, callee_bound, std::move( type_arguments ), type );
}

Operand Lowering::lower_call( Node_id id )
{
    // D7: a call whose callee is a path may construct a variant. Checked first: the checker recorded
    // an ordinal for a variant and a callable for `P::make( 7 )`.
    if( ast_.kind( ast_.callee( id ) ) == Node_kind::Path_expr && !types_.callee_of( id ).is_valid() )
    {
        return lower_variant_construction( id );
    }

    // In value position there is no destination, so it constructs into a temporary.
    if( is_construction( id ) )
    {
        const Type_id  type  = type_of( id );
        const Local_id local = builder_.add_local( type, ast_.span( id ) );

        builder_.storage_live( local, ast_.span( id ) );

        lower_construction( builder_.place( local ), id );

        drops_.own_temporary( local );

        return copy( builder_.place( local ), type );
    }

    // `a.size()` on a `T[N]` is N. The object is still lowered, so a call there still runs.
    if( ast_.kind( ast_.callee( id ) ) == Node_kind::Field_expr )
    {
        const Node_id object = ast_.object( ast_.callee( id ) );
        const Type_id held   = types_.table().is_pointer( type_of( object ) ) ? types_.table().get( type_of( object ) ).element
                                                                              : type_of( object );

        if( types_.table().is_array( held ) )
        {
            lower_expression( object );
            return constant( literal_pool_.add_integer( types_.table().get( held ).count ), type_of( id ) );
        }
    }

    // `p.area()` - the callee is a Field_expr rather than a name, and the receiver is its object.
    // Handled before the lookup below, which reaches for a declaration a Field_expr does not have.
    if( ast_.kind( ast_.callee( id ) ) == Node_kind::Field_expr && types_.callee_of( id ).is_valid() )
    {
        return lower_method_call( id );
    }

    // The checker recorded which callable this call chose, and choosing is its rule to apply: the
    // name alone names only the first candidate of an overload set.
    const Node_id callee = types_.callee_of( id );

    // `T::zero()` through a bound: the requirement has no receiver, so the branch below misses it.
    if( callee.is_valid() && ast_.kind( callee ) == Node_kind::Method_decl &&
        ast_.kind( ast_.enclosing_aggregate( callee ) ) == Node_kind::Interface_decl )
    {
        return lower_static_interface_call( id, callee );
    }

    // A bare `add( by )` inside a method borrows this function's receiver again, as `this.add( by )`
    // does. A static method has none, and takes the ordinary path.
    if( callee.is_valid() && ast_.kind( callee ) == Node_kind::Method_decl && ast_.has_receiver( callee ) )
    {
        return lower_method_call_on(
            id,
            callee,
            address_operand(
                builder_.deref( builder_.place( receiver_ ) ),
                binding_type_under( ast_.params( callee )[0], bindings_for_call( id ) ),
                ast_.span( id ),
                ast_.is_const_method( callee ) ? Address_purpose::Read : Address_purpose::Borrow
            ),
            ast_.arguments( id ),
            type_of( id ),
            bindings_for_call( id ),
            type_arguments_for_call( id )
        );
    }

    if( !callee.is_valid() && type_of( ast_.callee( id ) ).is_valid() &&
        types_.table().is_function( type_of( ast_.callee( id ) ) ) )
    {
        return lower_indirect_call( id );
    }

    if( !callee.is_valid() && type_of( ast_.callee( id ) ).is_valid() &&
        types_.table().is_field( type_of( ast_.callee( id ) ) ) )
    {
        return lower_field_application( id );
    }

    assert(
        callee.is_valid() && ( ast_.kind( callee ) == Node_kind::Function_decl || ast_.is_static_method( callee ) ) &&
        "checker should have rejected an unresolved call"
    );

    const Bindings callee_bound = bindings_for_call( id );

    return emit_call(
        id,
        callee,
        lower_call_arguments( ast_.arguments( id ), ast_.params( callee ), callee_bound ),
        callee_bound,
        type_arguments_for_call( id )
    );
}

// The implementer's own static method, under the bindings of the type `T` stands for.
Operand Lowering::lower_static_interface_call( Node_id id, Node_id requirement )
{
    const Node_id      path   = ast_.callee( id );
    Requirement_target target = requirement_target( requirement, type_of( ast_.qualifier( path ) ), type_of( path ) );

    assert( target.method.is_valid() && "a static requirement is answered by a declared method" );

    return emit_call(
        id,
        target.method,
        lower_call_arguments( ast_.arguments( id ), ast_.params( target.method ), target.at.bound ),
        target.at.bound,
        std::move( target.at.type_arguments )
    );
}

// The written arguments, lowered in order and converted to `callee`'s parameters, a method's
// starting after the receiver. Converted against the parameter, not the argument's own type.
std::vector<Operand> Lowering::lower_call_arguments(
    std::span<const Node_id> arguments, std::span<const Node_id> parameters, const Bindings& callee_bound
)
{
    // All lowered before any is converted, so statements come out in written order (§7.1).
    std::vector<Operand> operands;

    operands.reserve( arguments.size() );

    // The callee's bindings, not this function's: its signature is written in its own parameters.
    for( std::size_t i = 0; i < arguments.size(); ++i )
    {
        operands.push_back( lower_argument( arguments[i], parameters[i], callee_bound ) );
    }

    for( std::size_t i = 0; i < operands.size(); ++i )
    {
        operands[i] = converted( operands[i], binding_type_under( parameters[i], callee_bound ), ast_.span( arguments[i] ) );

        // The parameter's mode rather than what the call wrote, so the caller's end-of-statement drop
        // never fires on what it handed away.
        if( is_move_parameter( parameters[i] ) )
        {
            operands[i] = moved_if_owning( operands[i] );
        }
    }

    return operands;
}

Operand Lowering::emit_call(
    Node_id                  id,
    Node_id                  callee,
    std::span<const Operand> operands,
    const Bindings&          callee_bound,
    std::vector<Type_id>     type_arguments,
    Type_id                  given
)
{
    const u32 first = builder_.add_operands( operands );

    // A void call still assigns, to a void local nothing reads, so Assign always has a destination.
    const Type_id type = given.is_valid() ? given : type_of( id );

    // A callee returning a binding hands back an address, so the call and its temporary are typed as
    // that pointer. Dereferenced once: a copy reads `(*_t)`, and a binding takes `&(*_t)`.
    const bool    binding     = is_borrowed_binding( ast_, types_, callee );
    const Type_id result_type = binding ? binding_type_under( callee, callee_bound ) : type;

    const Local_id result = call_result(
        id,
        call( callee, first, narrow_cast<u32>( operands.size() ), result_type, std::move( type_arguments ) ),
        result_type,
        ast_.span( id )
    );

    return copy( binding ? builder_.deref( builder_.place( result ) ) : builder_.place( result ), type );
}

Operand Lowering::lower_indirect_call( Node_id id )
{
    const Span span = ast_.span( id );

    const Type_id signature = type_of( ast_.callee( id ) );

    std::span<const Type_id> args = types_.table().get( signature ).arguments;
    std::vector<Type_id>     arguments( args.begin(), args.end() );

    Operand callee = lower_expression( ast_.callee( id ) );

    const std::span<const Param_mode> modes   = types_.table().get( signature ).modes;
    const std::span<const Node_id>    written = ast_.arguments( id );

    std::vector<Operand> operands;

    for( std::size_t i = 0; i < written.size(); ++i )
    {
        // `move` travels by value as a bare argument does, so only the three borrows take an address.
        const bool by_address = i < modes.size() && ( ( modes[i] != Param_mode::Value && modes[i] != Param_mode::Move ) ||
                                                      ( modes[i] == Param_mode::Value && owns( arguments[i] ) ) );

        operands.push_back(
            by_address ? borrowed_argument( written[i], types_.table().pointer_to( arguments[i] ) )
                       : lower_expression( written[i] )
        );
    }

    // Only a by-value argument converts: a borrow is the caller's variable itself, so there is
    // nothing for a conversion to happen to.
    for( std::size_t i = 0; i < operands.size() && i < arguments.size(); ++i )
    {
        if( ( modes[i] == Param_mode::Value && !owns( arguments[i] ) ) || modes[i] == Param_mode::Move )
        {
            operands[i] = converted( operands[i], arguments[i], ast_.span( ast_.arg_list( id ) ) );
        }

        // As lower_call_arguments: by the mode, so a bare temporary is not also dropped here.
        if( modes[i] == Param_mode::Move )
        {
            operands[i] = moved_if_owning( operands[i] );
        }
    }

    const u32 first = builder_.add_operands( operands );

    const bool    binding     = types_.table().get( signature ).return_mode == Param_mode::Const_ref;
    const Type_id result_type = binding ? types_.table().pointer_to( type_of( id ) ) : type_of( id );

    Local_id result =
        call_result( id, indirect_call( callee, first, narrow_cast<u32>( operands.size() ), result_type ), result_type, span );

    return copy( binding ? builder_.deref( builder_.place( result ) ) : builder_.place( result ), type_of( id ) );
}

Operand Lowering::lower_field_application( Node_id id )
{
    const Type_id offset    = type_of( ast_.callee( id ) );
    const Type_id aggregate = types_.table().get( offset ).arguments[0];

    const Operand at     = lower_expression( ast_.callee( id ) );
    const Operand object = borrowed_argument( ast_.arguments( id )[0], types_.table().pointer_to( aggregate ) );

    return copy(
        builder_.place( builder_.into_temp( field_read( object, at, type_of( id ) ), type_of( id ), ast_.span( id ) ) ),
        type_of( id )
    );
}

Operand Lowering::lower_expression( Node_id id )
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Name_expr:
    {
        const Node_id decl = resolution_.declaration_of( id );
        return copy( place_for( decl, static_owner( id, decl ) ), type_of( id ) );
    }
    case Node_kind::Int_literal:
    case Node_kind::Char_literal:
    {
        // aux is the Literal_id, and the checker recorded the type context gave it.
        const Type_id type = type_of( id );

        // §6.4: an integer literal in float context becomes a float constant here, since the emitter
        // reads a float type's value from the float pool.
        if( types_.table().is_float( type ) )
        {
            const u64 magnitude = literal_pool_.integer( ast_.literal( id ) );

            return constant( literal_pool_.add_float( static_cast<f64>( magnitude ) ), type );
        }

        return constant( ast_.literal( id ), type );
    }
    case Node_kind::Float_literal:
        return constant( ast_.literal( id ), type_of( id ) );
    case Node_kind::Null_literal:
        // `nullptr` carries no Literal_id, so it is the integer zero, as a bool is 0 or 1.
        return constant( literal_pool_.add_integer( 0 ), type_of( id ) );
    case Node_kind::Bool_literal:
    {
        Literal_id literal = literal_pool_.add_integer( ast_.is_true( id ) ? 1 : 0 );
        return constant( literal, type_of( id ) );
    }
    case Node_kind::Struct_literal:
        return lower_struct_literal( id );
    case Node_kind::Array_literal:
        return lower_array_literal( id );
    case Node_kind::String_literal:
        return lower_string_literal( id );
    case Node_kind::Binary_expr:
        return lower_binary( id );
    case Node_kind::Conditional_expr:
        return lower_conditional( id );
    case Node_kind::Unary_expr:
        return lower_unary( id );
    case Node_kind::Index_expr:
        return copy( lower_place( id ), type_of( id ) );
    case Node_kind::Call_expr:
        return lower_call( id );
    case Node_kind::Cast_expr:
        if( ast_.keyword( id ) == Keyword::Cast )
        {
            return checked_cast( lower_expression( ast_.operand( id ) ), type_of( id ), ast_.span( id ) );
        }
        mark_wrapping( ast_.operand( id ) );
        return converted( lower_expression( ast_.operand( id ) ), type_of( id ), ast_.span( id ) );
    case Node_kind::Alloc_expr:
    {
        const Type_id type = type_of( id );
        Rvalue        allocation {};
        if( ast_.count( id ).is_valid() )
        {
            const Operand count = lower_expression( ast_.count( id ) );
            allocation          = allocate( type, count );
        }
        else
        {
            allocation = allocate( type );
        }
        return temporary( allocation, type, ast_.span( id ) );
    }
    case Node_kind::Assert_expr:
    {
        const Node_id condition = ast_.condition( id );

        const Block_id check = builder_.add_block();
        const Block_id pass  = builder_.add_block();
        const Block_id fail  = builder_.add_block();
        const Span     span  = ast_.span( id );

        builder_.terminate_branch(
            Operand { .kind = Operand_kind::Debug, .type = types_.table().builtin( Type_kind::Bool ) }, check, pass, span
        );
        builder_.switch_to( check );
        const std::size_t first = drops_.mark();

        const Operand op = lower_expression( condition );

        builder_.terminate_branch( op, pass, fail, span );
        drops_.one_path_since( first );

        builder_.switch_to( fail );
        builder_.terminate_panic( ast_.span( condition ), Failure::Assert );

        builder_.switch_to( pass );

        return Operand { .type = types_.table().builtin( Type_kind::Void ) };
    }
    case Node_kind::Panic_expr:
    {
        Operand op = lower_expression( ast_.operand( id ) );
        builder_.terminate_panic( ast_.span( id ), Failure::Message, op );
        return Operand { .type = types_.table().builtin( Type_kind::Void ) };
    }
    case Node_kind::Free_expr:
    {
        const Operand target = lower_expression( ast_.pointer( id ) );
        const Type_id type   = types_.table().builtin( Type_kind::Void );

        // Into a void temp, exactly as a void call already lowers: KIR keeps Assign total and the
        // emitter drops a destination it cannot declare.
        return temporary( release( target ), type, ast_.span( id ) );
    }
    case Node_kind::Destroy_expr:
        lower_destroy( id );
        return Operand { .type = types_.table().builtin( Type_kind::Void ) };
    // Reading a place is a copy of it - no temporary, because a place is already readable. That is
    // the whole of what lower_place buys in value position.
    case Node_kind::Field_expr:
        return copy( lower_place( id ), type_of( id ) );
    case Node_kind::Marker_expr:
    {
        const Node_id operand = ast_.operand( id );
        const Type_id type    = type_of( id );

        // A named variable is a place, and moving empties it. A temporary is built into a statement
        // temporary first, so the caller still drops it unless the move clears its flag.
        if( ast_.kind( operand ) == Node_kind::Name_expr )
        {
            return move( lower_place( operand ), type );
        }

        const Operand built = lower_expression( operand );

        assert( built.kind != Operand_kind::Constant && "the checker rejects moving anything without a place" );

        return move( built.place, type );
    }
    case Node_kind::Try_expr:
        return lower_try( id );
    case Node_kind::Path_expr:
    {
        // `kl::count`: a global named through its package or a static field, the only paths the resolver binds.
        if( const Node_id decl = resolution_.declaration_of( id ); decl.is_valid() )
        {
            return copy( place_for( decl, static_owner( id, decl ) ), type_of( id ) );
        }

        // D7: a bare path to a variant of a payload enum builds the struct. The checker refused one that
        // carries a payload.
        if( types_.table().is_enum( type_of( id ) ) &&
            ast_.enum_has_payload( types_.table().get( type_of( id ) ).declaration ) )
        {
            return lower_empty_variant( id );
        }

        // The variant's ordinal, recorded by the checker. aux holds its name.
        const std::optional<Constant_value> value = types_.constant_of( id );

        assert( value.has_value() && "the checker records an ordinal for every variant it accepts" );

        return constant( literal_pool_.add_integer( value->magnitude ), type_of( id ) );
    }
    default:
        // Names the construct rather than the category: while the lowerer is incomplete this is
        // the message that says what to write next, and it costs nothing once it is complete.
        fmt::print( stderr, "keelc: cannot lower {} as an expression yet\n", node_kind_name( ast_.kind( id ) ) );
        assert( false && "expression kind not lowered yet" );
        return Operand {};
    }
}

Place Lowering::lower_place( Node_id id )
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Name_expr:
    case Node_kind::Path_expr: // `kl::count`, a global through its package
    {
        // Same lookup as lower_expression's, but producing where the value lives rather than a
        // read of it.
        const Node_id decl = resolution_.declaration_of( id );

        // A variant, as `-sign::minus`'s receiver: a value, held in a temporary.
        if( !decl.is_valid() )
        {
            const Operand value = lower_expression( id );
            return builder_.place( builder_.into_temp( use( value ), value.type, ast_.span( id ) ) );
        }

        return place_for( decl, static_owner( id, decl ) );
    }
    case Node_kind::Field_expr:
    {
        // The field's declaration, found on the object's struct type - same lookup as the emitter does.
        const Node_id   object      = ast_.object( id );
        const Type_id   object_type = type_of( object );
        const Symbol_id name        = ast_.name( id );

        // `size` and `data` are read off the run pointer; the temporary is a place for a binding.
        if( types_.table().is_run( object_type ) )
        {
            const Operand run   = run_pointer( object );
            const Type_id type  = type_of( id );
            const Rvalue  value = types_.table().is_many_pointer( type ) ? run_data( run, type ) : run_size( run, type );

            return builder_.place( builder_.into_temp( value, type, ast_.span( id ) ) );
        }

        // D22: `p.x` on a pointer is `( *p ).x`, two projections off the pointer's value.
        if( types_.table().is_pointer( object_type ) )
        {
            const Operand pointer = lower_expression( object );
            const Span    span    = ast_.span( id );

            const Place base = pointer.kind == Operand_kind::Constant
                                   ? builder_.place( builder_.into_temp( use( pointer ), pointer.type, span ) )
                                   : pointer.place;

            // The pointee, not the pointer: the field lives on what is pointed at.
            return builder_.field( builder_.deref( base ), field_of( types_.table().get( object_type ).element, name ) );
        }

        return builder_.field( lower_place( object ), field_of( object_type, name ) );
    }
    case Node_kind::Unary_expr: // Star only; Amp is not a place
    {
        // `-a` on an aggregate is a call, and its result a temporary.
        if( ast_.op( id ) != Token_kind::Star && ast_.op( id ) != Token_kind::Amp )
        {
            return lower_expression( id ).place;
        }

        if( ast_.op( id ) == Token_kind::Amp )
        {
            assert( false && "unary & not a place" );
            return Place {};
        }
        const Operand pointer = lower_expression( ast_.operand( id ) );
        const Span    span    = ast_.span( id );

        // A constant has no place to project from, so it needs a local first. Anything else already
        // names one - including a call result, which lower_expression put in a temporary.
        const Place base = pointer.kind == Operand_kind::Constant
                               ? builder_.place( builder_.into_temp( use( pointer ), pointer.type, span ) )
                               : pointer.place;

        return builder_.deref( base );
    }
    case Node_kind::Index_expr:
    {
        if( const Node_id method = types_.callee_of( id ); method.is_valid() )
        {
            return builder_.deref( lower_operator_index( id, method ).place );
        }

        if( types_.table().is_array( type_of( ast_.object( id ) ) ) )
        {
            return lower_array_index( id );
        }

        if( ast_.kind( ast_.index( id ) ) == Node_kind::Range_expr || types_.table().is_run( type_of( ast_.object( id ) ) ) )
        {
            return lower_slice_or_run_index( id );
        }

        const Operand pointer   = lower_expression( ast_.object( id ) );
        const Operand index     = lower_expression( ast_.index( id ) );
        const Type_id base_type = type_of( ast_.object( id ) );

        const Place place = builder_.place(
            builder_.into_temp( binary( Token_kind::Plus, pointer, index, base_type ), base_type, ast_.span( id ) )
        );

        return builder_.deref( place );
    }
    // A value, not a place - but `( c ? a : b ).t` projects from one, and lower_conditional has
    // already put the result in a local. The same answer a call in this position gets.
    case Node_kind::Conditional_expr:
    case Node_kind::String_literal:
    case Node_kind::Binary_expr: // `a + b` in `a + b + c`
    case Node_kind::Struct_literal:
    case Node_kind::Array_literal:
    {
        return lower_expression( id ).place;
    }
    case Node_kind::Call_expr:
    {
        // Only a binding-returning call reaches here; the checker rejects any other call in place
        // position, so an operand without a place would be an internal error.
        const Operand result = lower_expression( id );

        assert( result.kind != Operand_kind::Constant && "a call in place position returns a binding" );

        return result.place;
    }

    default:
        fmt::print( stderr, "keelc: cannot lower {} as a place yet\n", node_kind_name( ast_.kind( id ) ) );
        assert( false && "place kind not lowered yet" );
        return Place {};
    }
}

void Lowering::lower_block( Node_id id )
{
    drops_.push_scope();
    for( const Node_id child : ast_.children( id ) )
    {
        // Everything after a return in the same block is unreachable. Dropping it here is what
        // keeps statements from landing in a terminated block.
        if( builder_.is_terminated() )
        {
            break;
        }

        lower_statement( child );
    }
    drops_.pop_scope( ast_.span( id ) );
    return;
}

void Lowering::lower_return( Node_id id )
{
    const Node_id value = ast_.value( id ); // invalid for a bare `return;`
    const Span    span  = ast_.span( id );
    if( value.is_valid() )
    {
        // Returning moves the value out, or the callee's scope exit drops what the caller now holds.
        if( is_borrowed_binding( ast_, types_, declaration_ ) )
        {
            // The address, not a read of it: a read would return a copy of the referent and the
            // form would buy nothing.
            const Type_id address = binding_type_of( declaration_ );

            builder_.assign(
                builder_.place( k_return_slot ),
                borrow_of( lower_place( value ), type_of( value ), address, Address_purpose::Read, span ),
                span
            );
        }
        else
        {
            builder_.assign(
                builder_.place( k_return_slot ),
                use( moved_if_owning( united( lower_expression( value ), builder_.type_of( k_return_slot ), span ) ) ),
                span
            );
        }
    }
    drops_.end_statement( span );
    drops_.unwind_to( 0, span ); // everything in the function is dead after a return

    builder_.terminate_return( span );
    return;
}

void Lowering::lower_var( Node_id id )
{
    const Span      span     = ast_.span( id );
    const Symbol_id name     = ast_.name( id );
    const bool      borrowed = is_borrowed_binding( ast_, types_, id );
    const Type_id   type     = borrowed ? binding_type_of( id ) : type_of( id );
    const Local_id  local    = builder_.add_local( type, span, name );

    locals_.emplace( id.v, local );
    builder_.storage_live( local, span );
    if( borrowed )
    {
        borrowed_bindings_.insert( id.v );
    }

    const Node_id init = ast_.initialiser( id );
    if( init.is_valid() )
    {
        // A binding stores the address, so the initialiser is lowered as a place. A pointer owns nothing,
        // so no drop is elaborated.
        if( borrowed )
        {
            builder_.assign(
                builder_.place( local ),
                borrow_of(
                    lower_place( init ),
                    type_of( init ),
                    type,
                    ast_.is_const_binding( id ) ? Address_purpose::Read : Address_purpose::Borrow,
                    span
                ),
                span
            );
        }
        // Not when it converts: `box<Shape> s = box<Sq>( … );` builds a `box<Sq>`, which `s` is not.
        else if( is_construction( init ) && type_of( init ) == type )
        {
            // Constructed in place. Going via a temporary and copying would make two of an
            // owning type where the program said one, and only one of them would be dropped.
            lower_construction( builder_.place( local ), init );
        }
        else
        {
            // An owning value is never copied, or both copies would be dropped.
            builder_.assign(
                builder_.place( local ), use( moved_if_owning( united( lower_expression( init ), type, span ) ) ), span
            );
        }
    }
    // In scope only once it holds a value: a `try` in the initialiser leaves before then.
    drops_.add_to_scope( local );
    drops_.end_statement( span );
}

void Lowering::lower_assign( Node_id id )
{
    const Span       span   = ast_.span( id );
    const Place      target = lower_place( ast_.target( id ) );
    const Token_kind op     = ast_.op( id );
    // The *target's* type, not type_of( id ): the checker records nothing on a statement, so that
    // would be an invalid Type_id. It is also the type the checker measured the value against.
    const Type_id type   = type_of( ast_.target( id ) );
    const Node_id method = types_.callee_of( id );

    // `a op= b` on a user type: `operator op=` in place, or `a = a op b` with the place read once.
    if( method.is_valid() )
    {
        const Node_id value = ast_.value( id );
        const Operand result =
            call_operator( id, method, target, { &value, 1 }, bindings_for_call( id ), type_arguments_for_call( id ) );

        if( ast_.is_const_method( method ) )
        {
            store( target, result, type, ast_.target( id ), span );
        }

        drops_.end_statement( span );
        return;
    }

    const Operand value = lower_expression( ast_.value( id ) );

    if( op == Token_kind::Equal )
    {
        store( target, value, type, ast_.target( id ), span );
    }
    else
    {
        // `x += 3` is `x = x + 3`: read the target, combine, store back, at the target's type.
        const bool offsets = types_.table().is_many_pointer( type );

        const Operand left  = copy( target, type );
        const Operand right = offsets ? value : converted( value, type, span );

        builder_.assign( target, checked_arithmetic( base_operator( op ), left, right, type, span, false ), span );
    }
    drops_.end_statement( span );
}

// Same reason as an initialiser: an owning value is never copied, or both copies would be dropped.
// A compound assignment reaches here only as a user type's `a = a op b`.
void Lowering::store( Place target, Operand value, Type_id type, Node_id target_node, Span span )
{
    Operand source = moved_if_owning( united( value, type, span ) );

    // The old value is destroyed first, after the new one is read: `o = move o` goes through a
    // temporary so the drop cannot reach it.
    if( owns( type ) && !initialises_field( target ) && !writes_a_slot( target_node ) )
    {
        if( source.kind != Operand_kind::Constant && !source.place.is_global() && source.place.local == target.local )
        {
            source = move( builder_.place( builder_.into_temp( use( source ), type, span ) ), type );
        }

        drop_place( target, type, span, true );
    }

    builder_.assign( target, use( source ), span );
}

void Lowering::lower_increment( Node_id id )
{
    const Place      target      = lower_place( ast_.operand( id ) );
    const Type_id    target_type = type_of( ast_.operand( id ) );
    const Token_kind op          = ast_.op( id );
    const Token_kind base_op     = op == Token_kind::Plus_plus ? Token_kind::Plus : Token_kind::Minus;
    const Operand    left        = copy( target, target_type );
    // The literal pool matching the type: an integer 1 typed f64 would name an unrelated float.
    const Literal_id one =
        types_.table().is_float( target_type ) ? literal_pool_.add_float( 1.0 ) : literal_pool_.add_integer( 1 );

    // A pointer steps by an integer; a 1 typed as the pointer is a null constant.
    const Type_id step  = types_.table().is_many_pointer( target_type ) ? types_.table().integer( 64, false ) : target_type;
    const Operand right = constant( one, step );

    // The target's type again, for the same reason: a statement carries none of its own.
    builder_.assign( target, checked_arithmetic( base_op, left, right, target_type, ast_.span( id ), false ), ast_.span( id ) );
    drops_.end_statement( ast_.span( id ) );
}

// One label's test. Leaves the builder in the block a failed test falls into: the next label, the
// next arm, or the fallback. D34: a range is two chained comparisons, so KIR needs no conjunction.
void Lowering::lower_case_test( Operand scrutinee, Place matched, Type_id matched_type, Node_id label, Block_id body )
{
    const Span    span    = ast_.span( label );
    const Type_id boolean = types_.table().builtin( Type_kind::Bool );

    // What a label compares against: a variant's recorded ordinal, since the scrutinee is already the
    // tag.
    const auto value_of = [&]( Node_id bound ) -> Operand
    {
        const Type_id bound_type = type_of( bound );

        if( ast_.kind( bound ) == Node_kind::Path_expr && types_.table().is_enum( bound_type ) &&
            ast_.enum_has_payload( types_.table().get( bound_type ).declaration ) )
        {
            const std::optional<Constant_value> ordinal = types_.constant_of( bound );

            assert( ordinal.has_value() && "the checker records an ordinal for every variant it accepts" );

            return constant( literal_pool_.add_integer( ordinal->magnitude ), types_.table().get( bound_type ).element );
        }

        return lower_expression( bound );
    };

    const auto branch_on = [&]( Token_kind op, Node_id bound, Block_id on_true )
    {
        const Operand  value = value_of( bound );
        const Local_id test  = builder_.into_temp( binary( op, scrutinee, value, boolean ), boolean, span );
        const Block_id next  = builder_.add_block();

        builder_.terminate_branch( copy( builder_.place( test ), boolean ), on_true, next, span );
        builder_.switch_to( next );
    };

    const Node_id path = ast_.kind( label ) == Node_kind::Variant_pattern ? ast_.variant_path( label ) : label;

    // §6.7: over a union, the member's tag and then its variant, chained as a range's two tests are.
    if( types_.table().is_union( matched_type ) )
    {
        const Type_id  member   = type_of( path );
        const Type_id  tag_type = types_.table().get( matched_type ).element;
        const Block_id inner    = builder_.add_block();

        const Local_id held = builder_.into_temp(
            binary(
                Token_kind::Equal_equal,
                scrutinee,
                constant( literal_pool_.add_integer( types_.table().error_tag( member ) ), tag_type ),
                boolean
            ),
            boolean,
            span
        );
        const Block_id missed = builder_.add_block();

        builder_.terminate_branch( copy( builder_.place( held ), boolean ), inner, missed, span );
        builder_.switch_to( inner );

        const Place   value   = builder_.member( matched, member );
        const bool    tagged  = ast_.enum_has_payload( types_.table().get( member ).declaration );
        const Type_id ordinal = tagged ? types_.table().get( member ).element : member;
        const Operand read    = copy( tagged ? builder_.tag( value ) : value, ordinal );

        const Local_id test =
            builder_.into_temp( binary( Token_kind::Equal_equal, read, value_of( path ), boolean ), boolean, span );

        builder_.terminate_branch( copy( builder_.place( test ), boolean ), body, missed, span );
        builder_.switch_to( missed );
        return;
    }

    // A pattern's test is its path: the bindings are written inside the arm, once the test has
    // already said which variant is live.
    if( ast_.kind( label ) == Node_kind::Variant_pattern )
    {
        branch_on( Token_kind::Equal_equal, ast_.variant_path( label ), body );
        return;
    }

    if( ast_.kind( label ) != Node_kind::Range_expr )
    {
        branch_on( Token_kind::Equal_equal, label, body );
        return;
    }

    // D34: half-open, so the upper bound is `<` and not `<=`. The lower test's true edge is the
    // upper test rather than the body, which is what makes the pair a conjunction.
    const Block_id in_range = builder_.add_block();

    branch_on( Token_kind::Greater_equal, ast_.lower( label ), in_range );

    const Block_id resume = builder_.current();

    builder_.switch_to( in_range );
    branch_on( Token_kind::Less, ast_.upper( label ), body );

    // The upper test failing must land where the lower test's failure lands, so the arm has one
    // exit and the chain stays a chain.
    builder_.terminate_goto( resume, span );
    builder_.switch_to( resume );
}

// The fallback arm goes through here too, so it binds its pattern like any other - easy to miss,
// since the last arm of an exhaustive switch is the fallback and may well destructure.
void Lowering::lower_arm_body( Node_id arm, Place matched, Type_id matched_type, Block_id fallthrough_to, bool consumes )
{
    // A consuming switch's bindings own their payloads, so they drop when the arm ends.
    if( consumes )
    {
        drops_.push_scope();
    }

    // D7: a pattern's bindings are written at the top of the arm's own block, before anything in it
    // runs - so the names are live exactly where the resolver put them in scope, and nowhere else.
    for( const Node_id label : ast_.labels( arm ) )
    {
        bind_variant_pattern( matched, matched_type, label, consumes );
    }

    const Block_id enclosing_fallthrough = fallthrough_target_;
    fallthrough_target_                  = fallthrough_to;

    lower_statement( ast_.body( arm ) );

    fallthrough_target_ = enclosing_fallthrough;

    if( consumes )
    {
        drops_.pop_scope( ast_.span( arm ) );
    }
}

// D30. A chain of equality tests over a scrutinee read once. Stacked labels are an OR, each false
// edge going to the next label; the checker proved the set exhaustive or given a `default`.
void Lowering::lower_switch( Node_id id )
{
    const std::span<const Node_id> arms = ast_.arms( id );
    const Span                     span = ast_.span( id );

    Operand       scrutinee      = lower_expression( ast_.scrutinee( id ) );
    const Type_id scrutinee_type = scrutinee.type;

    // D7: a payload enum is compared by its tag, read once here, so the arms compare one operand
    // against one constant.
    const bool payloads = types_.table().is_union( scrutinee.type ) ||
                          ( types_.table().is_enum( scrutinee.type ) &&
                            ast_.enum_has_payload( types_.table().get( scrutinee.type ).declaration ) );

    const bool consumes = ast_.consumes( id ) && owns( scrutinee.type );

    Place matched = scrutinee.place;

    // The switch's own scope, which keeps an owning temporary alive across the arms: a binding
    // borrows it in place.
    drops_.push_scope();

    // D7: a consuming switch moves the value into a local of its own, dropped at the end unless an
    // arm takes its payload.
    if( consumes )
    {
        const Span     taken_span = ast_.span( ast_.scrutinee( id ) );
        const Local_id taken      = builder_.add_local( scrutinee.type, taken_span );

        builder_.storage_live( taken, taken_span );
        builder_.assign( builder_.place( taken ), use( scrutinee ), taken_span );
        drops_.add_to_scope( taken );

        matched = builder_.place( taken );
    }

    if( !matched.is_global() && matched.num_projections == 0 )
    {
        drops_.adopt_into_scope( matched.local );
    }

    if( payloads )
    {
        const Type_id tag_type = types_.table().get( scrutinee.type ).element;

        scrutinee = copy( builder_.tag( matched ), tag_type );
    }

    drops_.end_statement( span );

    // A switch is a break target, so the block after it is the block `break` jumps to, allocated on
    // demand by break_target().
    loops_.push_back( Loop_targets { Block_id {}, Block_id {}, drops_.depth(), true } );

    const auto leave = [&]()
    {
        if( builder_.is_terminated() )
        {
            return; // the arm ended in a return, a break, or a fallthrough of its own
        }

        builder_.terminate_goto( break_target(), span );
    };

    // The fallback, where the last false edge goes: the `default`, held aside so the arms keep their
    // spans, or else the last arm. Testing that arm anyway would leave C a path returning nothing.
    Node_id     fallback;
    std::size_t fallback_index = 0; // into `bodies` below

    for( std::size_t index = 0; index < arms.size(); ++index )
    {
        if( ast_.is_default_arm( arms[index] ) )
        {
            fallback       = arms[index];
            fallback_index = index;
        }
    }

    if( !fallback.is_valid() && !arms.empty() )
    {
        fallback       = arms.back();
        fallback_index = arms.size() - 1;
    }

    // One block per arm, allocated first so `fallthrough` can jump to an arm not yet lowered.
    std::vector<Block_id> bodies;

    bodies.reserve( arms.size() );

    for( std::size_t i = 0; i < arms.size(); ++i )
    {
        bodies.push_back( builder_.add_block() );
    }

    const Block_id fallback_block = fallback.is_valid() ? bodies[fallback_index] : Block_id {};

    for( std::size_t index = 0; index < arms.size(); ++index )
    {
        const Node_id arm = arms[index];

        if( arm == fallback )
        {
            continue;
        }

        const std::span<const Node_id> labels = ast_.labels( arm );

        const Block_id body = bodies[index];

        for( const Node_id label : labels )
        {
            lower_case_test( scrutinee, matched, scrutinee_type, label, body );
        }

        // The block the last false edge left us in, so the arms chain without any block needing to
        // know its successor before that successor exists. Captured before switching away from it.
        const Block_id resume = builder_.current();

        builder_.switch_to( body );

        // The checker has already refused a `fallthrough` in the last arm, so an invalid target
        // here is never reached.
        lower_arm_body( arm, matched, scrutinee_type, index + 1 < bodies.size() ? bodies[index + 1] : Block_id {}, consumes );
        leave();

        builder_.switch_to( resume );
    }

    // Every test has failed by the time control reaches here.
    if( fallback.is_valid() )
    {
        builder_.terminate_goto( fallback_block, span );
        builder_.switch_to( fallback_block );

        // The fallback is lowered last, so its `fallthrough` may jump back to a block lowered earlier.
        lower_arm_body(
            fallback,
            matched,
            scrutinee_type,
            fallback_index + 1 < bodies.size() ? bodies[fallback_index + 1] : Block_id {},
            consumes
        );
    }

    leave();

    const Block_id after = loops_.back().break_target;

    loops_.pop_back();

    if( after.is_valid() )
    {
        builder_.switch_to( after );
    }

    drops_.pop_scope( span );
}

void Lowering::lower_if( Node_id id )
{
    const Node_id condition   = ast_.condition( id );
    const Node_id then_branch = ast_.then_branch( id );
    const Node_id else_branch = ast_.else_branch( id );
    const Span    span        = ast_.span( id );

    // Before the blocks: whatever the condition emits belongs to the block being left.
    const Operand test = lower_expression( condition );

    const Block_id then_block = builder_.add_block();
    const Block_id else_block = builder_.add_block();

    drops_.end_statement( span );

    builder_.terminate_branch( test, then_block, else_block, span );

    // Allocated only when an arm falls out of the `if`; an unreachable block fails verify.
    Block_id join;

    const auto leave = [&]()
    {
        if( builder_.is_terminated() )
        {
            return; // the arm ended in a return of its own
        }

        if( !join.is_valid() )
        {
            join = builder_.add_block();
        }

        builder_.terminate_goto( join, span );
    };

    builder_.switch_to( then_block );
    lower_statement( then_branch );
    leave();

    // An absent else is still a block: it is where the branch's false edge goes, and with no
    // statements in it, it is what forces the join to exist.
    builder_.switch_to( else_block );

    if( else_branch.is_valid() )
    {
        lower_statement( else_branch );
    }

    leave();

    // No join means both arms terminated, so the `if` did too. The enclosing Block loop reads
    // is_terminated() and stops, which is what drops the statements after it.
    if( join.is_valid() )
    {
        builder_.switch_to( join );
    }

    return;
}

void Lowering::lower_while( Node_id id )
{
    const Node_id condition = ast_.condition( id );
    const Node_id body      = ast_.body( id );
    const Span    span      = ast_.span( id );

    // A fresh block, not the one being left: the condition is re-evaluated every iteration, so
    // the back edge targets it - and everything before the loop would re-run if it did not.
    const Block_id header     = builder_.add_block();
    const Block_id body_block = builder_.add_block();

    builder_.terminate_goto( header, span );

    // A while has no update, so continue re-tests immediately.
    loops_.push_back( Loop_targets { Block_id {}, header, drops_.depth() } );

    builder_.switch_to( header );

    // terminate_branch acts on the *current* block, which a `&&` in the condition may have
    // moved on to. The back edge still targets the header, so the whole condition re-runs.
    const Operand test = lower_expression( condition );

    drops_.end_statement( span );

    builder_.terminate_branch( test, body_block, break_target(), span );

    builder_.switch_to( body_block );
    lower_statement( body );

    // A body ending in break, continue or return has terminated itself already.
    if( !builder_.is_terminated() )
    {
        builder_.terminate_goto( header, span );
    }

    const Block_id exit = loops_.back().break_target;

    loops_.pop_back();

    builder_.switch_to( exit ); // a condition always has a false edge, so this always exists
    return;
}

void Lowering::lower_for( Node_id id )
{
    const Node_id init      = ast_.init( id );
    const Node_id condition = ast_.condition( id );
    const Node_id update    = ast_.update( id );
    const Node_id body      = ast_.body( id );
    const Span    span      = ast_.span( id );

    drops_.push_scope();

    // D59: a counter and its end, each bound evaluated once, lower first. Invalid otherwise.
    const bool    counts     = ast_.is_counter( init );
    const Type_id count_type = counts ? type_of( init ) : Type_id {};
    Local_id      count {};
    Local_id      end {};

    if( counts )
    {
        const Node_id range = ast_.initialiser( init );
        count               = builder_.add_local( count_type, span, ast_.name( init ) );
        end                 = builder_.add_local( count_type, span );
        locals_.emplace( init.v, count );

        drops_.begin_statement();
        for( const auto& [local, bound] : { std::pair { count, ast_.lower( range ) }, std::pair { end, ast_.upper( range ) } } )
        {
            builder_.storage_live( local, span );
            builder_.assign( builder_.place( local ), use( united( lower_expression( bound ), count_type, span ) ), span );
            drops_.add_to_scope( local );
        }
        drops_.end_statement( span );
    }
    // The init runs once, in the block being left.
    else if( init.is_valid() )
    {
        lower_statement( init );
    }

    const Block_id header     = builder_.add_block();
    const Block_id body_block = builder_.add_block();

    builder_.terminate_goto( header, span );

    // The latch is left invalid: it holds the update and is what continue targets, and a body
    // that always returns reaches neither.
    loops_.push_back( Loop_targets { Block_id {}, Block_id {}, drops_.depth() } );

    builder_.switch_to( header );

    drops_.end_statement( span );

    if( counts )
    {
        const Type_id boolean = types_.table().builtin( Type_kind::Bool );
        const Operand below   = temporary(
            binary(
                Token_kind::Less,
                copy( builder_.place( count ), count_type ),
                copy( builder_.place( end ), count_type ),
                boolean
            ),
            boolean,
            span
        );
        builder_.terminate_branch( below, body_block, break_target(), span );
    }
    else if( condition.is_valid() )
    {
        builder_.terminate_branch( lower_expression( condition ), body_block, break_target(), span );
    }
    else
    {
        // `for( ; ; )` has no false edge, so it has no exit unless a break asks for one.
        builder_.terminate_goto( body_block, span );
    }

    builder_.switch_to( body_block );
    lower_statement( body );

    if( !builder_.is_terminated() )
    {
        builder_.terminate_goto( continue_target(), span );
    }

    const Block_id latch = loops_.back().continue_target;
    const Block_id exit  = loops_.back().break_target;

    loops_.pop_back();

    // Only if something reaches it. Nothing can run the update of a loop whose body always
    // returns and which nothing continues.
    if( latch.is_valid() )
    {
        builder_.switch_to( latch );

        // Unchecked: the header held `count < end`, so `count + 1` is at most `end`.
        if( counts )
        {
            const Operand one = constant( literal_pool_.add_integer( 1 ), count_type );
            builder_.assign(
                builder_.place( count ),
                binary( Token_kind::Plus, copy( builder_.place( count ), count_type ), one, count_type ),
                span
            );
        }
        else if( update.is_valid() )
        {
            lower_statement( update );
        }

        builder_.terminate_goto( header, span );
    }

    // No exit means the loop never finishes. The enclosing Block loop reads is_terminated()
    // and drops what follows, which is correct - nothing after it can run.
    if( exit.is_valid() )
    {
        builder_.switch_to( exit );
    }

    drops_.pop_scope( span );

    return;
}

Operand Lowering::temporary( Rvalue value, Type_id type, Span span )
{
    return copy( builder_.place( builder_.into_temp( value, type, span ) ), type );
}

void Lowering::lower_statement( Node_id id )
{
    drops_.begin_statement();

    // What a statement can be, and nothing about how each lowers.
    switch( ast_.kind( id ) )
    {
    case Node_kind::Block:
        return lower_block( id );

    case Node_kind::Return_stmt:
        return lower_return( id );

    case Node_kind::Var_decl:
        return lower_var( id );

    case Node_kind::Assign_stmt:
        return lower_assign( id );

    case Node_kind::Increment_stmt:
        return lower_increment( id );

    case Node_kind::Switch_stmt:
        return lower_switch( id );

    case Node_kind::If_stmt:
        return lower_if( id );

    case Node_kind::While_stmt:
        return lower_while( id );

    case Node_kind::For_stmt:
        return lower_for( id );

    // Small enough to read here. Extracting them would cost a name and buy nothing.
    case Node_kind::Expr_stmt:
    case Node_kind::Discard_stmt:
    {
        // Discard the operand: a call (D15), or anything after `_ =`.
        Node_id old_discarded = discarded_;
        discarded_            = ast_.expression( id );
        const Type_id type    = type_of( discarded_ );
        Operand       operand = lower_expression( ast_.expression( id ) );
        const Span    span    = ast_.span( id );
        if( ast_.kind( id ) == Node_kind::Discard_stmt && ast_.kind( discarded_ ) == Node_kind::Marker_expr && owns( type ) )
        {
            const Local_id temp = builder_.into_temp( use( operand ), type, span );
            drops_.own_temporary( temp );
        }
        discarded_ = old_discarded;
        drops_.end_statement( span );
        return;
    }

    // One edge each. The checker already rejected either outside a loop, so the asserts in the
    // target helpers document that rather than handle it.
    case Node_kind::Break_stmt:
        drops_.unwind_to( loops_.back().depth, ast_.span( id ) );
        builder_.terminate_goto( break_target(), ast_.span( id ) );
        return;

    case Node_kind::Continue_stmt:
        // The *loop's* depth. loops_.back() may be a switch between here and it, and unwinding to
        // that would leave the loop's own locals undropped.
        drops_.unwind_to( enclosing_loop().depth, ast_.span( id ) );
        builder_.terminate_goto( continue_target(), ast_.span( id ) );
        return;

    case Node_kind::Fallthrough_stmt:
        drops_.unwind_to( loops_.back().depth, ast_.span( id ) );
        builder_.terminate_goto( fallthrough_target_, ast_.span( id ) );
        return;

    default:
        fmt::print( stderr, "keelc: cannot lower {} as a statement yet\n", node_kind_name( ast_.kind( id ) ) );
        assert( false && "statement kind not lowered yet" );
    }
}

Operand Lowering::lower_argument( Node_id argument, Node_id parameter, const Bindings& bindings )
{
    if( !is_borrowed_binding( ast_, types_, parameter ) )
    {
        return lower_expression( argument );
    }

    return borrowed_argument( argument, binding_type_under( parameter, bindings ) );
}

// What a borrow lowers to, given its pointer type. Its own member because a call through a
// variable reads the mode off a function type and has no Param_decl.
Operand Lowering::borrowed_argument( Node_id argument, Type_id address )
{
    const Span span = ast_.span( argument );

    // `ref x` is stepped through. A bare borrow and a temporary are lowered first and their address
    // taken where they landed, which also leaves the temporary to be dropped.
    if( ast_.kind( argument ) == Node_kind::Marker_expr )
    {
        const Keyword marker  = ast_.keyword( argument );
        const Node_id operand = ast_.operand( argument );
        return temporary(
            borrow_of(
                lower_place( operand ),
                type_of( operand ),
                address,
                marker == Keyword::Out ? Address_purpose::Initialise : Address_purpose::Borrow,
                span
            ),
            address,
            span
        );
    }

    const Operand value = lower_expression( argument );

    // A constant has no address; reachable only through a generic, as `f<i32>( 1 )` against a bare
    // `T a`. Materialised into a temporary that lives to the end of the statement.
    const Place place = value.kind == Operand_kind::Constant
                            ? builder_.place( builder_.into_temp( use( value ), value.type, span ) )
                            : value.place;

    return temporary( borrow_of( place, value.type, address, Address_purpose::Read, span ), address, span );
}

Operand Lowering::address_operand( Place place, Type_id type, Span span, Address_purpose purpose )
{
    return temporary( address_of( place, type, purpose ), type, span );
}

Rvalue Lowering::borrow_of( Place place, Type_id held, Type_id address, Address_purpose purpose, Span span )
{
    Type_table& table = types_.table();

    if( !table.is_run_pointer( address ) || !held.is_valid() || !table.is_array( held ) )
    {
        return address_of( place, address, purpose );
    }

    const Type_id  u64   = table.integer( 64, false );
    const Type_id  data  = table.many_pointer_to( table.get( held ).element );
    const Local_id zero  = builder_.into_temp( use( constant( literal_pool_.add_integer( 0 ), u64 ) ), u64, span );
    const Operand  first = temporary( address_of( builder_.index( place, zero ), data, purpose ), data, span );
    const Operand  count = constant( literal_pool_.add_integer( table.get( held ).count ), u64 );

    return run_of( first, count, address );
}

// D30: `~E( E* self )`, testing the tag against each variant whose payload owns and dropping that
// payload. A variant that owns nothing gets no test.
Function enum_destructor( const Ast& ast, Types& types, Literal_pool& literals, Type_id enumeration )
{
    const Node_id decl     = types.table().get( enumeration ).declaration;
    const Span    span     = ast.span( decl );
    const Type_id tag_type = types.table().get( enumeration ).element;
    const Type_id boolean  = types.table().builtin( Type_kind::Bool );

    Builder builder( decl, types.table().builtin( Type_kind::Void ), span );

    const Local_id self  = builder.add_parameter( types.table().pointer_to( enumeration ), span, Symbol_id {} );
    const Place    value = builder.deref( builder.place( self ) );
    const Block_id done  = builder.add_block();

    const std::span<const Node_id> variants = ast.variants( decl );

    for( std::size_t ordinal = 0; ordinal < variants.size(); ++ordinal )
    {
        const std::span<const Node_id> payload = ast.payload( variants[ordinal] );

        std::vector<Type_id> field_types;
        bool                 owning = false;

        for( const Node_id field : payload )
        {
            field_types.push_back( field_type( ast, types.table(), enumeration, field, types.recorded() ) );
            owning = owning || instance_owns( ast, types.table(), field_types.back(), types.recorded() );
        }

        if( !owning )
        {
            continue;
        }

        const Block_id body = builder.add_block();
        const Block_id next = builder.add_block();

        const Local_id active = builder.into_temp(
            binary(
                Token_kind::Equal_equal,
                copy( builder.tag( value ), tag_type ),
                constant( literals.add_integer( ordinal ), tag_type ),
                boolean
            ),
            boolean,
            span
        );

        builder.terminate_branch( copy( builder.place( active ), boolean ), body, next, span );
        builder.switch_to( body );

        for( std::size_t i = payload.size(); i > 0; --i )
        {
            drop_place( builder, ast, types, builder.field( value, payload[i - 1] ), field_types[i - 1], span, false );
        }

        builder.terminate_goto( done, span );
        builder.switch_to( next );
    }

    builder.terminate_goto( done, span );
    builder.switch_to( done );
    builder.terminate_return( span );

    Function function = builder.finish();

    const std::span<const Type_id> arguments = types.table().get( enumeration ).arguments;
    function.type_arguments.assign( arguments.begin(), arguments.end() );

    return function;
}

// §6.7: `~( A | B )( self )`, testing the tag against each member that owns. Declared by nothing;
// its type argument is the union.
Function union_destructor( const Ast& ast, Types& types, Literal_pool& literals, Type_id united )
{
    const std::span<const Type_id> members  = types.table().get( united ).arguments;
    const Span                     span     = ast.span( types.table().get( members[0] ).declaration );
    const Type_id                  tag_type = types.table().get( united ).element;
    const Type_id                  boolean  = types.table().builtin( Type_kind::Bool );

    Builder builder( Node_id {}, types.table().builtin( Type_kind::Void ), span );

    const Local_id self  = builder.add_parameter( types.table().pointer_to( united ), span, Symbol_id {} );
    const Place    value = builder.deref( builder.place( self ) );
    const Block_id done  = builder.add_block();

    for( const Type_id member : members )
    {
        if( !instance_owns( ast, types.table(), member, types.recorded() ) )
        {
            continue;
        }

        const Block_id body = builder.add_block();
        const Block_id next = builder.add_block();

        const Local_id live = builder.into_temp(
            binary(
                Token_kind::Equal_equal,
                copy( builder.tag( value ), tag_type ),
                constant( literals.add_integer( types.table().error_tag( member ) ), tag_type ),
                boolean
            ),
            boolean,
            span
        );

        builder.terminate_branch( copy( builder.place( live ), boolean ), body, next, span );
        builder.switch_to( body );
        drop_place( builder, ast, types, builder.member( value, member ), member, span, false );
        builder.terminate_goto( done, span );
        builder.switch_to( next );
    }

    builder.terminate_goto( done, span );
    builder.switch_to( done );
    builder.terminate_return( span );

    Function function = builder.finish();
    function.type_arguments.push_back( united );

    return function;
}

// `~T[N]( T[N]* self )`, dropping the elements last to first. Declared by nothing; its type
// argument is the array.
Function array_destructor( const Ast& ast, Types& types, Literal_pool& literals, Type_id array )
{
    Type_table&   table   = types.table();
    const Type_id element = table.get( array ).element;
    const Type_id u64     = table.integer( 64, false );
    const Type_id boolean = table.builtin( Type_kind::Bool );

    Type_id named = element;
    while( table.is_array( named ) )
    {
        named = table.get( named ).element;
    }
    const Span span = table.get( named ).declaration.is_valid() ? ast.span( table.get( named ).declaration ) : Span {};

    Builder builder( Node_id {}, table.builtin( Type_kind::Void ), span );

    const Local_id self = builder.add_parameter( table.pointer_to( array ), span, Symbol_id {} );
    const Local_id left =
        builder.into_temp( use( constant( literals.add_integer( table.get( array ).count ), u64 ) ), u64, span );

    const Block_id header = builder.add_block();
    const Block_id body   = builder.add_block();
    const Block_id done   = builder.add_block();

    builder.terminate_goto( header, span );
    builder.switch_to( header );

    const Local_id more = builder.into_temp(
        binary( Token_kind::Less, constant( literals.add_integer( 0 ), u64 ), copy( builder.place( left ), u64 ), boolean ),
        boolean,
        span
    );

    builder.terminate_branch( copy( builder.place( more ), boolean ), body, done, span );
    builder.switch_to( body );
    builder.assign(
        builder.place( left ),
        binary( Token_kind::Minus, copy( builder.place( left ), u64 ), constant( literals.add_integer( 1 ), u64 ), u64 ),
        span
    );
    drop_place( builder, ast, types, builder.index( builder.deref( builder.place( self ) ), left ), element, span, false );
    builder.terminate_goto( header, span );

    builder.switch_to( done );
    builder.terminate_return( span );

    Function function = builder.finish();
    function.type_arguments.push_back( array );

    return function;
}

// A vtable's drop slot: the implementer's whole drop, its `~` and then its fields, which no
// function of its own does. Declared by nothing; its type argument is the implementer.
Function drop_function( const Ast& ast, Types& types, Type_id implementer )
{
    const Span span = ast.span( types.table().get( implementer ).declaration );

    Builder builder( Node_id {}, types.table().builtin( Type_kind::Void ), span );

    const Local_id self = builder.add_parameter( types.table().pointer_to( implementer ), span, Symbol_id {} );

    drop_place( builder, ast, types, builder.deref( builder.place( self ) ), implementer, span, false );
    builder.terminate_return( span );

    Function function = builder.finish();
    function.type_arguments.push_back( implementer );

    return function;
}

bool implements_anything( const Types& types, Type_id type )
{
    return std::ranges::any_of(
        types.conformances().all(),
        [&]( const Conformance& conformance )
        { return types.conformances().of( types.table(), type, conformance.interface ) != nullptr; }
    );
}

// Every closed owning enum, owning union and owning implementer's drop function. Repeated until
// nothing new appears, since one destructor can intern another: `Option<Option<Buf>>`.
void append_synthesised_drops( const Ast& ast, Types& types, Literal_pool& literals, std::vector<Function>& functions )
{
    std::vector<Type_id> done;

    for( bool grew = true; grew; )
    {
        grew = false;

        for( const Type_id type : types.table().composite_types() )
        {
            if( !types.table().is_enum( type ) || types.table().mentions_parameter( type ) ||
                std::find( done.begin(), done.end(), type ) != done.end() ||
                !instance_owns( ast, types.table(), type, types.recorded() ) )
            {
                continue;
            }

            done.push_back( type );
            functions.push_back( enum_destructor( ast, types, literals, type ) );
            grew = true;
        }
    }

    for( const Type_id type : types.table().union_types() )
    {
        if( instance_owns( ast, types.table(), type, types.recorded() ) )
        {
            functions.push_back( union_destructor( ast, types, literals, type ) );
        }
    }

    // Last, since an enum's destructor may intern an array but not the reverse.
    for( const Type_id type : types.table().array_types() )
    {
        if( !types.table().mentions_parameter( type ) && instance_owns( ast, types.table(), type, types.recorded() ) )
        {
            functions.push_back( array_destructor( ast, types, literals, type ) );
        }
    }

    for( const Type_id type : types.table().composite_types() )
    {
        if( ( types.table().is_struct( type ) || types.table().is_enum( type ) ) && !types.table().mentions_parameter( type ) &&
            implements_anything( types, type ) && instance_owns( ast, types.table(), type, types.recorded() ) )
        {
            functions.push_back( drop_function( ast, types, type ) );
        }
    }
}

} // namespace

std::vector<Function>
lower( const Ast& ast, const Resolution& resolution, Types& types, Literal_pool& literals, bool overflow_checks )
{
    std::vector<Function> functions;

    for( Node_id id { 0 }; id.v < ast.node_count(); ++id.v )
    {
        // A generic has no code of its own - it is the template the instantiations below are
        // emitted from, and emitting it directly would try to give `T` a C spelling.
        if( is_function_like( ast.kind( id ) ) && ast.body( id ).is_valid() && !ast.is_generic( id ) )
        {
            Lowering lowering( id, ast, resolution, types, literals, overflow_checks );
            functions.push_back( lowering.run() );
        }
    }

    Instance_queue instances( ast, types );

    while( const std::optional<Instantiation> instance = instances.next() )
    {
        Lowering lowering(
            instance->declaration, ast, resolution, types, literals, overflow_checks, bindings_for( ast, types, *instance )
        );

        Function emitted = lowering.run();

        emitted.type_arguments = instance->arguments;

        functions.push_back( std::move( emitted ) );
    }

    append_synthesised_drops( ast, types, literals, functions );

    return functions;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/print.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

// The whole front end, so lowering runs on a genuinely typed AST.
struct Lowered : Compiled
{
    explicit Lowered( std::string_view source, Build build = { .asserts = true } )
        : Compiled( source, Stage::Lowered, build )
    {
    }

    std::string text( std::size_t index = 0 )
    {
        return print( functions[index], ast, types.table(), literals, interner );
    }

    // By name rather than by index: a class contributes its constructor and destructor to the same
    // list, and which order those land in is not what any case asking for a function is about.
    std::string named( std::string_view name )
    {
        const std::string prefix = fmt::format( "fn {} ", name );

        for( std::size_t i = 0; i < functions.size(); ++i )
        {
            if( text( i ).starts_with( prefix ) )
            {
                return text( i );
            }
        }

        return {};
    }
};
// A class contributes functions of its own, so verification is asked of the whole list.
bool every_function_verifies( Lowered& p )
{
    for( const Function& function : p.functions )
    {
        if( !verify( function ).empty() )
        {
            return false;
        }
    }

    return true;
}

} // namespace

TEST_CASE( "lower_produces_a_function_per_declaration", "[ir][lower]" )
{
    Lowered p( "i32 first() { return 1; }\ni32 second() { return 2; }\ni32 main() { return 0; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.functions.size() == 3 );

    // In declaration order, and each carrying the declaration it came from.
    REQUIRE( p.text( 0 ).find( "fn first" ) != std::string::npos );
    REQUIRE( p.text( 1 ).find( "fn second" ) != std::string::npos );
    REQUIRE( p.text( 2 ).find( "fn main" ) != std::string::npos );
}

// Everything the builder produces has to satisfy verify.
TEST_CASE( "lower_produces_functions_that_verify", "[ir][lower]" )
{
    for( const char* source :
         { "i32 main() { return 0; }",
           "void nothing() { return; }\ni32 main() { return 0; }",
           "i32 take( i32 a, i32 b ) { return 1; }\ni32 main() { return 0; }" } )
    {
        Lowered p( source );

        INFO( source << "\n" << p.rendered() );
        REQUIRE( p.clean() );

        for( const Function& function : p.functions )
        {
            const std::vector<std::string> errors = verify( function );

            INFO( fmt::format( "{}", fmt::join( errors, "\n" ) ) );
            REQUIRE( errors.empty() );
        }
    }
}

TEST_CASE( "lower_returns_through_the_return_slot", "[ir][lower]" )
{
    Lowered p( "i32 main() { return 7; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();

    INFO( text );

    // The value goes into local 0 and the terminator carries nothing - that is the convention the
    // whole IR is built on.
    REQUIRE( text.find( "_0 = const 7" ) != std::string::npos );
    REQUIRE( text.find( "return" ) != std::string::npos );
}

TEST_CASE( "lower_maps_parameters_onto_locals", "[ir][lower]" )
{
    Lowered p( "i32 take( i32 a, u8 b ) { return 0; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( 0 );

    INFO( text );

    // Locals 1..parameter_count, in declaration order, after the return slot - and each keeps its
    // source name, which is what makes a dump readable.
    REQUIRE( p.functions[0].parameter_count == 2 );
    REQUIRE( text.find( "let _1: i32; // parameter a" ) != std::string::npos );
    REQUIRE( text.find( "let _2: u8; // parameter b" ) != std::string::npos );
}

// A void function's body that falls off the end still gets a terminator.
TEST_CASE( "lower_terminates_a_body_that_falls_off_the_end", "[ir][lower]" )
{
    Lowered p( "void nothing() { }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.functions[0] ).empty() );
    REQUIRE( p.text( 0 ).find( "return" ) != std::string::npos );
}

TEST_CASE( "lower_returns_nothing_from_a_bare_return", "[ir][lower]" )
{
    Lowered p( "void nothing() { return; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( 0 );

    INFO( text );

    // No assignment to the return slot: there is no value to put there.
    REQUIRE( text.find( "_0 =" ) == std::string::npos );
    REQUIRE( text.find( "return" ) != std::string::npos );
}

// Statements after a return cannot run, and emitting them would put statements into a block that
// already has a terminator.
TEST_CASE( "lower_drops_statements_after_a_return", "[ir][lower]" )
{
    Lowered p( "i32 main() { return 1; return 2; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();

    INFO( text );
    REQUIRE( text.find( "const 1" ) != std::string::npos );
    REQUIRE( text.find( "const 2" ) == std::string::npos );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_takes_a_literals_type_from_the_checker", "[ir][lower]" )
{
    Lowered p( "u8 small() { return 200; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    // The literal adopted u8 from the return type, and the return slot is typed to match.
    REQUIRE( p.text( 0 ).find( "let _0: u8;" ) != std::string::npos );
}

TEST_CASE( "lower_gives_a_local_to_each_declaration", "[ir][lower]" )
{
    Lowered p( "i32 main() { i32 x = 1; i32 y = 2; return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();

    INFO( text );

    // Named, in declaration order, after the return slot - and the read comes back to the same
    // local, which is what locals_ exists to make possible.
    REQUIRE( text.find( "let _1: i32; // x" ) != std::string::npos );
    REQUIRE( text.find( "let _2: i32; // y" ) != std::string::npos );
    REQUIRE( text.find( "_1 = const 1" ) != std::string::npos );
    REQUIRE( text.find( "_2 = const 2" ) != std::string::npos );
    REQUIRE( text.find( "_0 = copy _1" ) != std::string::npos );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_declares_without_initialising", "[ir][lower]" )
{
    Lowered p( "i32 main() { i32 x; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();

    INFO( text );

    // The local exists and nothing is assigned to it.
    REQUIRE( text.find( "let _1: i32; // x" ) != std::string::npos );
    REQUIRE( text.find( "_1 =" ) == std::string::npos );
}

TEST_CASE( "lower_takes_an_auto_type_from_the_checker", "[ir][lower]" )
{
    Lowered p( "i32 main() { auto x = 200; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.text().find( "let _1: i32; // x" ) != std::string::npos );
}

TEST_CASE( "lower_reads_a_parameter_by_name", "[ir][lower]" )
{
    Lowered p( "i32 take( i32 a ) { return a; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.text( 0 ).find( "_0 = copy _1" ) != std::string::npos );
}

// Each literal kind reaches KIR as a constant, including a bool and `nullptr`, which carry no
// Literal_id.
TEST_CASE( "lower_turns_every_literal_into_a_constant", "[ir][lower]" )
{
    struct Case
    {
        const char* source;
        const char* expected;
    };

    for( const Case& c :
         { Case { "i32 f() { return 7; }", "_0 = const 7" },
           Case { "f64 f() { return 1.5; }", "_0 = const 1.5" },
           Case { "bool f() { return true; }", "_0 = const 1" },
           Case { "bool f() { return false; }", "_0 = const 0" },
           Case { "u8 f() { return 'a'; }", "_0 = const 97" },
           Case { "i32* f() { return nullptr; }", "_0 = const 0" } } )
    {
        Lowered p( std::string( c.source ) + "\ni32 main() { return 0; }" );

        INFO( c.source << "\n" << p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( c.expected ) != std::string::npos );

        // The one that matters: a constant with no literal behind it does not verify.
        REQUIRE( verify( p.functions[0] ).empty() );
    }
}

TEST_CASE( "lower_keeps_shadowed_names_apart", "[ir][lower]" )
{
    Lowered p( "i32 main() { i32 x = 1; { i32 y = 2; } return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();

    INFO( text );

    // Two declarations, two locals - a nested scope does not reuse storage, and the read after the
    // inner block still finds the outer one.
    REQUIRE( p.functions[0].locals.size() == 3 );
    REQUIRE( text.find( "_0 = copy _1" ) != std::string::npos );
}

// Three-address form: every intermediate becomes a local, in evaluation order (§7.1).
TEST_CASE( "lower_puts_every_operation_in_a_temporary", "[ir][lower]" )
{
    Lowered p( "i32 f( i32 a, i32 b, i32 c ) { return a + b * c; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( 0 );

    INFO( text );

    // The multiply is evaluated first because it is the deeper operand, and its temporary is what
    // the addition then reads.
    REQUIRE( text.find( "_4 = copy _2 * copy _3" ) != std::string::npos );
    REQUIRE( text.find( "_5 = copy _1 + copy _4" ) != std::string::npos );
    REQUIRE( verify( p.functions[0] ).empty() );
}

// §6.4: an operation happens at its operands' common type, and KIR makes that explicit.
TEST_CASE( "lower_makes_conversions_explicit", "[ir][lower]" )
{
    SECTION( "operands already at one type need none" )
    {
        Lowered p( "i32 f( i32 a, i32 b ) { return a + b; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( " as " ) == std::string::npos );
        REQUIRE( text.find( "_3 = copy _1 + copy _2" ) != std::string::npos );
    }

    SECTION( "a narrower operand widens to meet the other" )
    {
        Lowered p( "i64 f( u8 a, i64 b ) { return a + b; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_3 = copy _1 as i64" ) != std::string::npos );
        REQUIRE( text.find( "_4 = copy _3 + copy _2" ) != std::string::npos );
    }

    // The case the AST cannot answer on its own: the node's recorded type is bool, while the
    // operands still meet at their common type.
    SECTION( "a comparison meets at the common type and produces bool" )
    {
        Lowered p( "bool f( u8 a, i32 b ) { return a < b; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "let _3: i32;" ) != std::string::npos );  // the widened operand
        REQUIRE( text.find( "let _4: bool;" ) != std::string::npos ); // the result
        REQUIRE( text.find( "_3 = copy _1 as i32" ) != std::string::npos );
        REQUIRE( text.find( "_4 = copy _3 < copy _2" ) != std::string::npos );
    }

    // A shift's operands do not meet: the result is the left one's type and the count is a width.
    SECTION( "a shift count keeps its own type" )
    {
        Lowered p( "u32 f( u32 a, u8 n ) { return a << n; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( " as " ) == std::string::npos );
        REQUIRE( text.find( "_3 = copy _2 & const 31" ) != std::string::npos );
        REQUIRE( text.find( "_4 = copy _1 << copy _3" ) != std::string::npos );
    }
}

TEST_CASE( "lower_expands_assignment", "[ir][lower]" )
{
    SECTION( "a plain assignment writes the value" )
    {
        Lowered p( "i32 main() { i32 x = 1; x = 2; return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text();

        INFO( text );
        REQUIRE( text.find( "_1 = const 1" ) != std::string::npos );
        REQUIRE( text.find( "_1 = const 2" ) != std::string::npos );
    }

    // `x += 3` is `x = x + 3`, at the *target's* type. The checker records nothing on a statement,
    // so reading a type off the Assign_stmt node would give an invalid one.
    SECTION( "a compound assignment reads, combines and stores back" )
    {
        Lowered p( "i32 main() { u8 x = 1; x += 3; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text();

        INFO( text );
        REQUIRE( text.find( "_1 = copy _1 + const 3" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "and an increment is the same shape" )
    {
        Lowered p( "i32 main() { u8 x = 1; x++; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text().find( "_1 = copy _1 + const 1" ) != std::string::npos );
    }

    SECTION( "a decrement subtracts" )
    {
        Lowered p( "i32 main() { u8 x = 1; x--; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text().find( "_1 = copy _1 - const 1" ) != std::string::npos );
    }
}

TEST_CASE( "lower_handles_the_unary_operators", "[ir][lower]" )
{
    SECTION( "negation" )
    {
        Lowered p( "i32 f( i32 a ) { return -a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "_2 = -copy _1" ) != std::string::npos );
    }

    SECTION( "logical not" )
    {
        Lowered p( "bool f( bool a ) { return !a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "_2 = !copy _1" ) != std::string::npos );
    }

    SECTION( "bitwise not" )
    {
        Lowered p( "u32 f( u32 a ) { return ~a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "_2 = ~copy _1" ) != std::string::npos );
    }
}

// The first real control flow, and the reason add_block() does not switch to what it makes: a
// branch needs the ids of blocks that do not exist yet.
TEST_CASE( "lower_builds_an_if", "[ir][lower][cfg]" )
{
    SECTION( "with both arms and a join" )
    {
        Lowered p( "i32 f( bool c ) { i32 x = 0; if ( c ) { x = 1; } else { x = 2; } return x; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( p.functions[0].blocks.size() == 4 );
        REQUIRE( text.find( "branch copy _1 -> bb1, bb2" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // An absent else is still a block - it is where the branch's false edge goes - and with
    // nothing in it, it is what forces the join to exist.
    SECTION( "with no else" )
    {
        Lowered p( "i32 f( bool c ) { i32 x = 0; if ( c ) { x = 1; } return x; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( p.functions[0].blocks.size() == 4 );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // The case the lazy join exists for: nothing falls out of the `if`, so a join block would be
    // unreachable - which verify rejects, correctly.
    SECTION( "with both arms returning, and so no join at all" )
    {
        Lowered p( "i32 f( bool c ) { if ( c ) { return 1; } else { return 2; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( p.functions[0].blocks.size() == 3 );
        REQUIRE( text.find( "goto" ) == std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // One arm terminating is what the is_terminated() guard on each arm is for: terminating it a
    // second time would trip the builder's assert.
    SECTION( "with one arm returning" )
    {
        Lowered p( "i32 f( bool c ) { i32 x = 0; if ( c ) { return 1; } else { x = 2; } return x; }\ni32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.functions[0].blocks.size() == 4 );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // An `else if` is another If_stmt in the else arm, so it nests rather than needing a case.
    SECTION( "chained as else if" )
    {
        Lowered p( "i32 f( i32 n ) { if ( n == 0 ) { return 1; } else if ( n == 1 ) { return 2; } else { return 3; } }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "goto" ) == std::string::npos ); // every path returns
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "and statements after an if that always returns are dropped" )
    {
        Lowered p( "i32 f( bool c ) { if ( c ) { return 1; } else { return 2; } return 3; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "const 3" ) == std::string::npos );
    }
}

// The right side runs only if the left demands it: `p != nullptr && (*p).x > 0`.
TEST_CASE( "lower_short_circuits_and_and_or", "[ir][lower][cfg]" )
{
    SECTION( "and evaluates the right side when the left is true" )
    {
        Lowered p( "bool f( bool a, bool b ) { return a && b; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "&&" ) == std::string::npos ); // it is control flow, not an operation
        REQUIRE( text.find( "branch copy _3 -> bb1, bb2" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // The only difference between them: the branch targets swap.
    SECTION( "or evaluates it when the left is false" )
    {
        Lowered p( "bool f( bool a, bool b ) { return a || b; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "||" ) == std::string::npos );
        REQUIRE( text.find( "branch copy _3 -> bb2, bb1" ) != std::string::npos );
    }

    // The result is written on both paths, which is why it needs a local rather than an operand.
    SECTION( "the result is one local written by both paths" )
    {
        Lowered p( "bool f( bool a, bool b ) { return a && b; }\ni32 main() { return 0; }" );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_3 = copy _1" ) != std::string::npos );
        REQUIRE( text.find( "_3 = copy _2" ) != std::string::npos );
        REQUIRE( text.find( "_0 = copy _3" ) != std::string::npos );
    }

    // A nested one leaves the cursor in its own join, so the outer goto has to come from wherever
    // lowering ended up rather than from the block it started in.
    SECTION( "nested on the right" )
    {
        Lowered p( "bool f( bool a, bool b, bool c ) { return a && ( b && c ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "and inside an if condition" )
    {
        Lowered p( "i32 f( bool a, bool b ) { if ( a && b ) { return 1; } return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }
}

// A conditional: one result local, two arms, one join. The arms may need widening, and may own.
TEST_CASE( "lower_builds_a_conditional", "[ir][lower][cfg]" )
{
    SECTION( "three blocks, and the result is one local written by both paths" )
    {
        Lowered p( "i32 f( bool c, i32 a, i32 b ) { return c ? a : b; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "branch copy _1 -> bb1, bb2" ) != std::string::npos );
        REQUIRE( text.find( "_4 = copy _2" ) != std::string::npos );
        REQUIRE( text.find( "_4 = copy _3" ) != std::string::npos );
        REQUIRE( text.find( "_0 = copy _4" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // An expectation reaches an arm without retyping it, so both arms are still i32 while the
    // conditional is i64. Without the conversion the assignment writes an i32 into an i64 local.
    SECTION( "an arm is widened to reach the result" )
    {
        Lowered p( "i64 f( bool c, i32 a, i32 b ) { i64 r = c ? a : b; return r; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "copy _2 as i64" ) != std::string::npos );
        REQUIRE( text.find( "copy _3 as i64" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // Moved, not copied: a copy would leave the arm and the result holding one resource between
    // them, and drop elaboration would then drop it twice.
    SECTION( "an owning arm is moved into the result" )
    {
        Lowered p( "class Owner { public i32 t; Owner( i32 v ) { t = v; } ~Owner() { } };\n"
                   "i32 f( bool c ) { Owner a = Owner( 1 ); Owner b = Owner( 2 ); "
                   "Owner r = c ? move a : move b; return r.t; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "_9 = move _2" ) != std::string::npos );
        REQUIRE( text.find( "_9 = move _5" ) != std::string::npos );
        REQUIRE( text.find( "_8 = move _9" ) != std::string::npos );
        REQUIRE( every_function_verifies( p ) );
    }

    // Nothing else drops the result when it owns and nothing moves out of it. Reading a field is
    // the case that reaches it: the temporary survives the statement and has to be dropped once.
    SECTION( "an owning result nothing moves out of is dropped once" )
    {
        Lowered p( "class Owner { public i32 t; Owner( i32 v ) { t = v; } ~Owner() { } };\n"
                   "i32 f( bool c ) { Owner a = Owner( 1 ); Owner b = Owner( 2 ); "
                   "return ( c ? move a : move b ).t; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "f" );

        INFO( text );
        // Lowering emits the drop; the flag that makes the arms' own drops conditional is added
        // later, by drop elaboration, so what is asserted here is that the result has one at all.
        REQUIRE( text.find( "drop _8" ) != std::string::npos );
        REQUIRE( every_function_verifies( p ) );
    }

    // `move a` already arrives as a move; an arm that builds an owning value is a copy of its own
    // temporary, which moved_if_owning converts.
    SECTION( "an owning arm built in place is moved, not copied" )
    {
        Lowered p( "class Owner { public i32 t; Owner( i32 v ) { t = v; } ~Owner() { } };\n"
                   "i32 f( bool c ) { Owner r = c ? Owner( 1 ) : Owner( 2 ); return r.t; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "_3 = move _4" ) != std::string::npos );
        REQUIRE( text.find( "_3 = move _7" ) != std::string::npos );
        REQUIRE( every_function_verifies( p ) );
    }

    // A conditional is a value, but `( c ? a : b ).t` projects from one - and lower_conditional
    // has already put the result in a local, which is the place to project from.
    SECTION( "a field reads through the result local" )
    {
        Lowered p( "struct P { i32 t; };\n"
                   "i32 f( bool c, P a, P b ) { return ( c ? a : b ).t; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_0 = copy _4.t" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // A nested conditional leaves the cursor in its own join, so neither the assignment nor the
    // goto may name the block the outer one created.
    SECTION( "nested in an arm" )
    {
        Lowered p( "i32 f( bool a, bool b, i32 x, i32 y, i32 z ) { return a ? x : b ? y : z; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "nested in the condition" )
    {
        Lowered p( "i32 f( bool a, bool b, bool c ) { return ( a ? b : c ) ? 1 : 2; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "inside an if condition" )
    {
        Lowered p( "i32 f( bool a, bool b, bool c ) { if ( a ? b : c ) { return 1; } return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }
}

TEST_CASE( "lower_builds_calls", "[ir][lower][calls]" )
{
    SECTION( "the callee is the declaration, not a local" )
    {
        Lowered p( "i32 helper() { return 1; }\ni32 main() { return helper(); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( text.find( "_1 = call helper()" ) != std::string::npos );
        REQUIRE( verify( p.functions[1] ).empty() );
    }

    // The order the statements come out in is the order the arguments were written. That is what
    // §7.1 guarantees and C leaves unspecified.
    SECTION( "arguments are evaluated in source order" )
    {
        Lowered p( "i32 two( i32 a, i32 b ) { return a; }\n"
                   "i32 f( i32 x ) { return two( x + 1, x + 2 ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 1 );

        INFO( text );

        const std::size_t first  = text.find( "+ const 1" );
        const std::size_t second = text.find( "+ const 2" );
        const std::size_t call   = text.find( "call two" );

        REQUIRE( first < second );
        REQUIRE( second < call );
    }

    // The conversion is to the *parameter's* type. Reading it off the argument instead makes every
    // conversion a no-op, which is silent - the call still looks right.
    SECTION( "an argument widens to reach its parameter" )
    {
        Lowered p( "i64 wide( i64 v ) { return v; }\n"
                   "i64 f( u8 small ) { return wide( small ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( text.find( "_2 = copy _1 as i64" ) != std::string::npos );
        REQUIRE( text.find( "call wide(copy _2)" ) != std::string::npos );
    }

    SECTION( "and needs no conversion when it already matches" )
    {
        Lowered p( "i32 take( i32 v ) { return v; }\n"
                   "i32 f( i32 v ) { return take( v ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 1 ).find( " as " ) == std::string::npos );
    }

    // Nesting is what three-address form removes: the inner call is a statement of its own, and a
    // call never sits inside another's argument list.
    SECTION( "a call as an argument becomes its own statement" )
    {
        Lowered p( "i32 take( i32 v ) { return v; }\n"
                   "i32 f( i32 v ) { return take( take( v ) ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( text.find( "_2 = call take(copy _1)" ) != std::string::npos );
        REQUIRE( text.find( "_3 = call take(copy _2)" ) != std::string::npos );
    }

    SECTION( "recursion needs nothing special" )
    {
        Lowered p( "i32 fib( i32 n ) { if ( n < 2 ) { return n; } return fib( n - 1 ) + fib( n - 2 ); }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // Mutual recursion works because the checker resolved both signatures before either body, so
    // the callee is just a Node_id by the time lowering runs.
    SECTION( "so does mutual recursion" )
    {
        Lowered p( "bool even( i32 n ) { if ( n == 0 ) { return true; } return odd( n - 1 ); }\n"
                   "bool odd( i32 n ) { if ( n == 0 ) { return false; } return even( n - 1 ); }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        for( const Function& function : p.functions )
        {
            REQUIRE( verify( function ).empty() );
        }
    }

    // A void call still assigns, to a local nothing reads, which a backend emits as a bare call.
    SECTION( "a void call assigns to a void local" )
    {
        Lowered p( "void nothing() { return; }\ni32 main() { nothing(); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( text.find( "let _1: void;" ) != std::string::npos );
        REQUIRE( text.find( "_1 = call nothing()" ) != std::string::npos );
        REQUIRE( verify( p.functions[1] ).empty() );
    }
}

// Through a function-typed variable the callee is an operand: the local holding the address.
TEST_CASE( "lower_calls_through_a_function_typed_variable", "[ir][lower][calls][m7]" )
{
    SECTION( "the callee is a local, not a symbol" )
    {
        Lowered p( "i32 twice( i32 a ) { return a * 2; }\n"
                   "i32 main() { fn( i32 ) -> i32 p = &twice; return p( 21 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( text.find( "= call copy _" ) != std::string::npos );
        REQUIRE( text.find( "call twice" ) == std::string::npos );
        REQUIRE( verify( p.functions[1] ).empty() );
    }

    // The parameter types come from the signature rather than from a Param_list, so this is where
    // reading them off the argument instead would make every conversion a no-op.
    SECTION( "an argument widens to reach the signature's parameter" )
    {
        Lowered p( "i32 twice( i32 a ) { return a * 2; }\n"
                   "i32 main() { u8 b = 1; fn( i32 ) -> i32 p = &twice; return p( b ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( text.find( "as i32" ) != std::string::npos );
        REQUIRE( verify( p.functions[1] ).empty() );
    }

    SECTION( "one taking nothing and returning nothing" )
    {
        Lowered p( "void nothing() { }\ni32 main() { fn() -> void q = &nothing; q(); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( text.find( "= call copy _" ) != std::string::npos );
        REQUIRE( verify( p.functions[1] ).empty() );
    }

    // The order the operands come out in is the order the arguments were written. A signature gives
    // lowering no Param_list to read that order off, so this is the only thing pinning it.
    SECTION( "the arguments keep their written order" )
    {
        Lowered p( "i32 sub( i32 a, i32 b ) { return a - b; }\n"
                   "i32 main() { fn( i32, i32 ) -> i32 p = &sub; return p( 5, 3 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( text.find( "(const 5, const 3)" ) != std::string::npos );
        REQUIRE( verify( p.functions[1] ).empty() );
    }

    SECTION( "and through a parameter, where no address was taken at all" )
    {
        Lowered p( "i32 apply( fn( i32 ) -> i32 f, i32 v ) { return f( v ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "= call copy _1(copy _2)" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // M9: a field called by its name is read where it is called, through the receiver in a method.
    SECTION( "and through a field, read at the call" )
    {
        Lowered p( "i32 twice( i32 a ) { return a * 2; }\n"
                   "struct H { fn( i32 ) -> i32 cb; i32 bare() const { return cb( 1 ); } };\n"
                   "i32 main() { H h = H { &twice }; return h.cb( 21 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string method = p.text( 1 );

        INFO( method );
        REQUIRE( method.find( "= call copy (*_1).cb(const 1)" ) != std::string::npos );
        REQUIRE( verify( p.functions[1] ).empty() );

        const std::string text = p.text( 2 );

        INFO( text );
        REQUIRE( text.find( ".cb(const 21)" ) != std::string::npos );
        REQUIRE( text.find( "call bare" ) == std::string::npos );
        REQUIRE( verify( p.functions[2] ).empty() );
    }
}

// Overloads share a name, so the declaration on the rvalue is what tells them apart.
TEST_CASE( "lower_takes_the_address_of_the_chosen_overload", "[ir][lower][m7]" )
{
    constexpr std::string_view pair = "i32 f( i32 a ) { return a; }\nf64 f( f64 a ) { return a; }\n";

    const auto address_callee = []( Lowered& p )
    {
        std::vector<Node_id> callees;

        for( const Function& func : p.functions )
        {
            for( const Statement& s : func.statements )
            {
                if( s.value.kind == Rvalue_kind::Function_address )
                {
                    callees.push_back( s.value.callee );
                }
            }
        }

        REQUIRE( callees.size() == 1 );

        return std::string( p.types.table().name( p.types.type_of( callees[0] ) ) );
    };

    SECTION( "the f64 one when that is the annotation" )
    {
        Lowered p( std::string( pair ) + "i32 main() { fn( f64 ) -> f64 p = &f; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( address_callee( p ) == "f64" );
    }

    SECTION( "and the i32 one when that is" )
    {
        Lowered p( std::string( pair ) + "i32 main() { fn( i32 ) -> i32 p = &f; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( address_callee( p ) == "i32" );
    }
}

// Two instances share a declaration, so the type arguments beside the Node_id tell them apart, as
// a call's do.
TEST_CASE( "lower_takes_the_address_of_a_generic_instance", "[ir][lower][m7][generic]" )
{
    constexpr std::string_view id = "T id<T>( T a ) where T : copyable { return a; }\n";

    const auto addresses = []( Lowered& p )
    {
        std::vector<std::string> spelled;

        for( const Function& func : p.functions )
        {
            for( const Statement& s : func.statements )
            {
                if( s.value.kind != Rvalue_kind::Function_address )
                {
                    continue;
                }

                std::string text;

                for( const Type_id argument : s.value.type_arguments )
                {
                    text += p.types.table().name( argument );
                }

                spelled.push_back( text );
            }
        }

        return spelled;
    };

    SECTION( "the arguments travel on the rvalue" )
    {
        Lowered p( std::string( id ) + "i32 main() { fn( i32 ) -> i32 p = &id<i32>; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( addresses( p ) == std::vector<std::string> { "i32" } );
    }

    SECTION( "and tell two instances apart" )
    {
        Lowered p(
            std::string( id ) + "i32 main() { fn( i32 ) -> i32 p = &id<i32>; fn( f64 ) -> f64 q = &id<f64>; return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( addresses( p ) == std::vector<std::string> { "i32", "f64" } );
    }

    // An ordinary function has none, and the slot stays empty rather than picking up the enclosing
    // instantiation's - a non-generic callee is one function however it was reached.
    SECTION( "an ordinary function carries none" )
    {
        Lowered p( "i32 f( i32 a ) { return a; }\ni32 main() { fn( i32 ) -> i32 p = &f; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( addresses( p ) == std::vector<std::string> { "" } );
    }

    // The seed, which is the whole reason the checker records an instantiation here: nothing else
    // in the program mentions `id<i32>`, so without it there is no body to point at.
    SECTION( "the instance is lowered even though nobody calls it" )
    {
        Lowered p( std::string( id ) + "i32 main() { fn( i32 ) -> i32 p = &id<i32>; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.functions.size() == 2 );
    }
}

TEST_CASE( "lower_builds_a_while", "[ir][lower][loops]" )
{
    Lowered p( "i32 f( i32 n ) { i32 s = 0; while ( s < n ) { s = s + 1; } return s; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( 0 );

    INFO( text );

    // The header is a fresh block, not the one being left: the condition re-runs every iteration,
    // so the back edge targets it, and anything before the loop would re-run if it did not.
    REQUIRE( text.find( "bb0:\n        storage_live _2\n        _2 = const 0\n        goto -> bb1" ) != std::string::npos );
    REQUIRE( text.find( "branch copy _3 -> bb2, bb3" ) != std::string::npos );
    REQUIRE( text.find( "goto -> bb1" ) != std::string::npos ); // the back edge

    // verify's reachability walk needs its seen set for exactly this - a naive recursion hangs.
    REQUIRE( verify( p.functions[0] ).empty() );
}

// `continue` in a for runs the update before re-testing: an edge to the latch block.
TEST_CASE( "lower_builds_a_for", "[ir][lower][loops]" )
{
    SECTION( "the update lives in a latch the body falls into" )
    {
        Lowered p( "i32 f( i32 n ) { for ( i32 i = 0; i < n; i++ ) { } return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "+ const 1" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // The case the C emitter got wrong first time: a continue that skips the update never advances.
    SECTION( "continue targets the latch, not the header" )
    {
        Lowered p( "i32 f( i32 n ) { for ( i32 i = 0; i < n; i++ ) { continue; } return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );

        // The body jumps to the latch, and the latch is where the increment is.
        const std::size_t body   = text.find( "bb2:" );
        const std::size_t latch  = text.find( "bb4:" );
        const std::size_t update = text.find( "+ const 1" );

        REQUIRE( body != std::string::npos );
        REQUIRE( latch != std::string::npos );
        REQUIRE( latch < update ); // the increment is inside the latch block
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "the init runs once, before the loop" )
    {
        Lowered p( "i32 f( i32 n ) { for ( i32 i = 0; i < n; i++ ) { } return 0; }\ni32 main() { return 0; }" );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "bb0:\n        storage_live _2\n        _2 = const 0" ) != std::string::npos );
    }
}

// A block nothing jumps to is unreachable, and verify rejects one - so both loop targets are
// allocated only when something actually needs them.
TEST_CASE( "lower_allocates_loop_targets_only_when_reached", "[ir][lower][loops]" )
{
    // No condition means no false edge, so nothing exits unless a break asks to.
    SECTION( "for( ; ; ) with no break has no exit block" )
    {
        Lowered p( "void f() { for ( ; ; ) { i32 x = 1; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( p.functions[0].blocks.size() == 4 ); // entry, header, body, latch
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "and one with a break does" )
    {
        Lowered p( "void f() { for ( ; ; ) { break; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.functions[0].blocks.size() == 4 ); // entry, header, body, exit - and no latch
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // Nothing can reach the update of a loop whose body always returns.
    SECTION( "a body that always returns needs no latch" )
    {
        Lowered p( "i32 f( i32 n ) { for ( i32 i = 0; i < n; i++ ) { return i; } return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "+ const 1" ) == std::string::npos ); // the increment was never emitted
        REQUIRE( p.functions[0].blocks.size() == 4 );
        REQUIRE( verify( p.functions[0] ).empty() );
    }
}

TEST_CASE( "lower_binds_break_and_continue_to_the_innermost_loop", "[ir][lower][loops]" )
{
    SECTION( "break leaves a while" )
    {
        Lowered p( "i32 f() { while ( true ) { break; } return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "an inner break leaves only the inner loop" )
    {
        Lowered p( "i32 f( i32 n ) { while ( n > 0 ) { for ( i32 i = 0; i < n; i++ ) { break; } n = n - 1; } return 0; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "an inner continue runs the inner update" )
    {
        Lowered p( "i32 f( i32 n ) { while ( n > 0 ) { for ( i32 i = 0; i < n; i++ ) { continue; } n = n - 1; } return 0; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // The arm terminates itself, so the if does not try to terminate it a second time.
    SECTION( "break inside an if inside a loop" )
    {
        Lowered p( "i32 f( i32 n ) { while ( true ) { if ( n > 0 ) { break; } n = n + 1; } return 0; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "statements after a break are unreachable and dropped" )
    {
        Lowered p( "i32 f( i32 n ) { while ( true ) { break; n = 99; } return n; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "const 99" ) == std::string::npos );
    }
}

// A place is a local plus a path, and reading one is a copy of it. Nothing is copied into a
// temporary on the way, which is what makes `(*p).next` one place rather than three statements.
TEST_CASE( "lower_builds_places", "[ir][lower][places]" )
{
    SECTION( "a field read is a copy of the place" )
    {
        Lowered p( "struct P { i32 x; };\ni32 f( P p ) { return p.x; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_0 = copy _1.x" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "nested fields keep nesting" )
    {
        Lowered p( "struct A { i32 v; };\nstruct B { A a; };\ni32 f( B b ) { return b.a.v; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "_0 = copy _1.a.v" ) != std::string::npos );
    }

    SECTION( "a deref read" )
    {
        Lowered p( "i32 f( i32* q ) { return *q; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "_0 = copy (*_1)" ) != std::string::npos );
    }

    // The one that justifies places being a path rather than a local: no intermediate copy, so a
    // later move check can ask whether `(*p).next` specifically was moved out of.
    SECTION( "a field through a pointer is one place, not two steps" )
    {
        Lowered p( "struct N { i32 v; N* next; };\ni32 f( N* p ) { return (*p).v; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_0 = copy (*_1).v" ) != std::string::npos );
        REQUIRE( p.functions[0].locals.size() == 2 ); // the return slot and the parameter, nothing else
    }

    SECTION( "and writes go straight into the place" )
    {
        Lowered p( "struct N { i32 v; N* next; };\n"
                   "void f( i32* q, N* p ) { *q = 1; (*p).v = 2; (*p).next = p; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "(*_1) = const 1" ) != std::string::npos );
        REQUIRE( text.find( "(*_2).v = const 2" ) != std::string::npos );
        REQUIRE( text.find( "(*_2).next = copy _2" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }
}

// Taking an address is not reading what is there, so its operand goes through lower_place. Lowering
// it as an expression first would emit a read - and for `&*get()` would run the call twice.
TEST_CASE( "lower_takes_addresses_of_places", "[ir][lower][places]" )
{
    SECTION( "of a local" )
    {
        Lowered p( "i32 f() { i32 x = 1; i32* p = &x; return *p; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_3 = &borrow _1" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "of a field" )
    {
        Lowered p( "struct P { i32 x; };\ni32 f( P p ) { i32* q = &p.x; return *q; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "_3 = &borrow _1.x" ) != std::string::npos );
    }
}

TEST_CASE( "lower_builds_casts", "[ir][lower][places]" )
{
    SECTION( "a widening cast converts" )
    {
        Lowered p( "i64 f( i32 v ) { return cast<i64>( v ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "_2 = copy _1 as i64" ) != std::string::npos );
    }

    SECTION( "a wrap narrows" )
    {
        Lowered p( "u8 f( i32 v ) { return wrap<u8>( v ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "_2 = copy _1 as u8" ) != std::string::npos );
    }

    // Nothing to convert, so nothing is emitted - converted() returns the operand unchanged.
    SECTION( "a cast to the type it already has emits nothing" )
    {
        Lowered p( "i32 f( i32 v ) { return cast<i32>( v ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( " as " ) == std::string::npos );
    }
}

// A struct literal is a temporary plus one assignment per field, so fields run in source order.
TEST_CASE( "lower_builds_struct_literals", "[ir][lower][structs]" )
{
    SECTION( "designated form" )
    {
        Lowered p( "struct P { i32 x; i32 y; };\nP f() { return P { .x = 1, .y = 2 }; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_1.x = const 1" ) != std::string::npos );
        REQUIRE( text.find( "_1.y = const 2" ) != std::string::npos );
        REQUIRE( text.find( "_0 = copy _1" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // Positional form names no field, so the i-th initialiser fills the i-th field. Reading the
    // name off aux and looking it up would find nothing at all.
    SECTION( "positional form fills fields in order" )
    {
        Lowered p( "struct P { i32 x; i32 y; };\nP f() { return P { 1, 2 }; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_1.x = const 1" ) != std::string::npos );
        REQUIRE( text.find( "_1.y = const 2" ) != std::string::npos );
    }

    // §7.1: the order the fields are *written* is the order they run, even when that is not the
    // order they were declared in.
    SECTION( "designated form keeps written order, not declaration order" )
    {
        Lowered p( "struct P { i32 x; i32 y; };\nP f() { return P { .y = 2, .x = 1 }; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_1.y = const 2" ) < text.find( "_1.x = const 1" ) );
    }

    // The same widening an argument gets on its way to a parameter.
    SECTION( "a value widens to reach its field" )
    {
        Lowered p( "struct P { i64 v; };\nP f( u8 small ) { return P { .v = small }; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( " as i64" ) != std::string::npos );
    }

    SECTION( "a nested literal builds its own temporary first" )
    {
        Lowered p( "struct A { i32 v; };\nstruct B { A a; };\nB f() { return B { A { 1 } }; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_2.v = const 1" ) != std::string::npos );
        REQUIRE( text.find( "_1.a = copy _2" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }
}

// The whole corpus, through the lowerer and the verifier.
TEST_CASE( "lower_handles_every_construct_in_the_corpus", "[ir][lower]" )
{
    for( const char* source : {
             "i32 gcd( i32 a, i32 b ) { while ( b != 0 ) { i32 r = a % b; a = b; b = r; } return a; }",
             "i32 popcount( u32 v ) { i32 n = 0; while ( v != 0 ) { n = n + wrap<i32>( v & 1 ); v = v >> 1; } return n; }",
             "struct P { f64 x; f64 y; };\nf64 d( P a, P b ) { f64 dx = a.x - b.x; return dx * dx; }",
             "i32 f( i32* q ) { if ( q == nullptr ) { return 0; } return *q; }",
             "i32 f( bool a, bool b ) { if ( a && b ) { return 1; } return 0; }",
         } )
    {
        Lowered p( std::string( source ) + "\ni32 main() { return 0; }" );

        INFO( source << "\n" << p.rendered() );
        REQUIRE( p.clean() );

        for( const Function& function : p.functions )
        {
            const std::vector<std::string> errors = verify( function );

            INFO( fmt::format( "{}", fmt::join( errors, "\n" ) ) );
            REQUIRE( errors.empty() );
        }
    }
}

// A file-scope variable has no frame slot, so a place is rooted in a local or a global; the
// projections above the root are unchanged.
TEST_CASE( "lower_roots_places_in_globals", "[ir][lower][globals]" )
{
    SECTION( "reading one" )
    {
        Lowered p( "i32 counter = 1;\ni32 f() { return counter; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );

        // Spelled by name, where a local is _N - so the two are distinguishable with no extra
        // syntax, and no local was invented to hold it.
        REQUIRE( text.find( "_0 = copy counter" ) != std::string::npos );
        REQUIRE( p.functions[0].locals.size() == 1 ); // the return slot, nothing else
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "writing one" )
    {
        Lowered p( "i32 counter = 1;\nvoid f() { counter = 2; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "counter = const 2" ) != std::string::npos );
    }

    SECTION( "a compound assignment reads and writes it" )
    {
        Lowered p( "i32 counter = 1;\nvoid f() { counter += 3; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "counter = copy counter + const 3" ) != std::string::npos );
    }

    // field() and deref() keep the base's root, so a place that started global stays global.
    SECTION( "taking its address" )
    {
        Lowered p( "i32 counter = 1;\ni32* f() { return &counter; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "&borrow counter" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "a local of the same name shadows it" )
    {
        Lowered p( "i32 counter = 1;\ni32 f() { i32 counter = 2; return counter; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_0 = copy _1" ) != std::string::npos ); // the local, not the global
    }
}

namespace
{

// These helpers let marker tests assert order rather than local numbers, which shift whenever a
// temporary is added earlier.

// Where a line appears, so one can be asserted to precede another.
std::size_t at( const std::string& text, std::string_view needle )
{
    const std::size_t index = text.find( needle );
    REQUIRE( index != std::string::npos );
    return index;
}

// How many times a marker appears - the only way to say "this local dies exactly once".
std::size_t count( const std::string& text, std::string_view needle )
{
    std::size_t total = 0;
    for( std::size_t i = text.find( needle ); i != std::string::npos; i = text.find( needle, i + 1 ) )
    {
        ++total;
    }
    return total;
}

// The dump names every local in a trailing comment, so a test can ask for "x" and get "_3".
std::string local_of( const std::string& text, std::string_view name )
{
    const std::string needle = fmt::format( "; // {}\n", name );
    const std::size_t end    = text.find( needle );
    REQUIRE( end != std::string::npos );

    const std::size_t line = text.rfind( "let ", end );
    REQUIRE( line != std::string::npos );

    const std::size_t start = line + 4;
    return text.substr( start, text.find( ':', start ) - start );
}

constexpr std::string_view k_return = "\n        return";

} // namespace

TEST_CASE( "lower_brackets_a_local_with_storage_markers", "[ir][lower][storage]" )
{
    Lowered p( "i32 main() { i32 x = 1; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    const std::string x = local_of( text, "x" );

    // Live before the initialiser can assign into it, dead before the block ends.
    REQUIRE( at( text, "storage_live " + x ) < at( text, x + " = const 1" ) );
    REQUIRE( at( text, x + " = const 1" ) < at( text, "storage_dead " + x ) );
    REQUIRE( at( text, "storage_dead " + x ) < at( text, k_return ) );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_ends_a_scope_in_reverse_declaration_order", "[ir][lower][storage]" )
{
    Lowered p( "i32 main() { i32 a = 1; i32 b = 2; i32 c = 3; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    // Reverse declaration order, which is the order destructors have to run in - so getting it
    // right here is what drop elaboration inherits.
    REQUIRE( at( text, "storage_dead " + local_of( text, "c" ) ) < at( text, "storage_dead " + local_of( text, "b" ) ) );
    REQUIRE( at( text, "storage_dead " + local_of( text, "b" ) ) < at( text, "storage_dead " + local_of( text, "a" ) ) );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_gives_no_storage_markers_to_parameters", "[ir][lower][storage]" )
{
    Lowered p( "i32 add( i32 a, i32 b ) { return a + b; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    // A parameter is live on entry and dead on exit by construction, so it carries no marker -
    // and neither does the return slot.
    REQUIRE( text.find( "storage_live " + local_of( text, "parameter a" ) ) == std::string::npos );
    REQUIRE( text.find( "storage_dead " + local_of( text, "parameter a" ) ) == std::string::npos );
    REQUIRE( text.find( "storage_live " + local_of( text, "parameter b" ) ) == std::string::npos );
    REQUIRE( text.find( "storage_live _0" ) == std::string::npos );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_gives_no_storage_markers_to_temporaries", "[ir][lower][storage]" )
{
    Lowered p( "i32 main() { i32 x = 1 + 2; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    // Markers bracket declared locals only. A temporary has no name, no scope and no destructor to
    // schedule; whether an owning value can live in one is an M3 question, not this one.
    REQUIRE( count( text, "storage_live" ) == 1 );
    REQUIRE( count( text, "storage_dead" ) == 1 );
    REQUIRE( at( text, "storage_live " + local_of( text, "x" ) ) != std::string::npos );
}

TEST_CASE( "lower_ends_an_inner_scope_at_its_closing_brace", "[ir][lower][storage]" )
{
    Lowered p( "i32 main() { i32 x = 1; { i32 y = 2; } x = 3; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    const std::string x = local_of( text, "x" );
    const std::string y = local_of( text, "y" );

    // The inner local dies at the brace, not at the end of the function: the statement after the
    // block is already outside its scope.
    REQUIRE( at( text, "storage_dead " + y ) < at( text, x + " = const 3" ) );
    REQUIRE( at( text, x + " = const 3" ) < at( text, "storage_dead " + x ) );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_emits_no_markers_for_a_scope_with_no_declarations", "[ir][lower][storage]" )
{
    Lowered p( "i32 main() { { } return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    // An empty scope is bookkeeping, not output.
    REQUIRE( count( text, "storage_" ) == 0 );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_computes_a_return_value_before_ending_storage", "[ir][lower][storage]" )
{
    Lowered p( "i32 main() { i32 x = 7; return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    const std::string x = local_of( text, "x" );

    // The ordering trap: unwind before the value is read and the return reads storage that has
    // already ended. Once these are drops rather than markers, that is a use-after-free.
    REQUIRE( at( text, "_0 = copy " + x ) < at( text, "storage_dead " + x ) );
    REQUIRE( at( text, "storage_dead " + x ) < at( text, k_return ) );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_ends_every_scope_on_an_early_return", "[ir][lower][storage]" )
{
    Lowered p( "i32 f() { i32 x = 1; { i32 y = 2; return 0; } }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    // A return leaves every enclosing scope at once, innermost first - not just the one it stands in.
    REQUIRE( at( text, "storage_dead " + local_of( text, "y" ) ) < at( text, "storage_dead " + local_of( text, "x" ) ) );
    REQUIRE( at( text, "storage_dead " + local_of( text, "x" ) ) < at( text, k_return ) );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_ends_the_loop_body_scope_once_per_iteration", "[ir][lower][storage]" )
{
    Lowered p( "i32 f( i32 n ) { i32 total = 0; while ( n > 0 ) { i32 step = n; n = n - 1; } return total; }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    const std::string step  = local_of( text, "step" );
    const std::string total = local_of( text, "total" );

    // The body's local is bracketed inside the loop, so its storage ends on the back edge and
    // begins again on the next iteration. The enclosing local is untouched by that.
    REQUIRE( at( text, "storage_live " + step ) < at( text, "storage_dead " + step ) );
    REQUIRE( count( text, "storage_dead " + step ) == 1 );
    REQUIRE( count( text, "storage_dead " + total ) == 1 );
    REQUIRE( at( text, "storage_dead " + step ) < at( text, "storage_dead " + total ) );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_ends_inner_scopes_on_break", "[ir][lower][storage]" )
{
    Lowered p( "i32 f() { while ( true ) { i32 x = 1; { i32 y = 2; break; } } return 0; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    // A break is an exit path like any other: everything between it and the loop ends, innermost
    // first. Nothing else reaches the end of this body, so each dies exactly once.
    REQUIRE( at( text, "storage_dead " + local_of( text, "y" ) ) < at( text, "storage_dead " + local_of( text, "x" ) ) );
    REQUIRE( count( text, "storage_dead " + local_of( text, "y" ) ) == 1 );
    REQUIRE( count( text, "storage_dead " + local_of( text, "x" ) ) == 1 );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_keeps_a_for_init_alive_across_continue", "[ir][lower][storage]" )
{
    Lowered p( "i32 f() { for ( i32 i = 0; true; ) { i32 x = 1; continue; } return 0; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    const std::string i = local_of( text, "i" );
    const std::string x = local_of( text, "x" );

    // The case most likely to be got wrong. `continue` unwinds the body but stops short of the
    // scope holding the init - `i` has to survive into the next iteration. `x` does not.
    REQUIRE( count( text, "storage_dead " + x ) == 1 );
    REQUIRE( count( text, "storage_dead " + i ) == 1 );
    REQUIRE( at( text, "storage_dead " + x ) < at( text, "storage_dead " + i ) );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_ends_a_for_init_on_break", "[ir][lower][storage]" )
{
    Lowered p( "i32 f() { for ( i32 i = 0; true; ) { break; } return 0; }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    const std::string i = local_of( text, "i" );

    // Where `continue` stops short of the init's scope, `break` leaves it - so the init dies on
    // the break edge, and the loop having no other exit means it dies exactly once.
    REQUIRE( count( text, "storage_dead " + i ) == 1 );
    REQUIRE( at( text, "storage_live " + i ) < at( text, "storage_dead " + i ) );
    REQUIRE( verify( p.functions[0] ).empty() );
}

TEST_CASE( "lower_keeps_every_function_verifiable_with_markers", "[ir][lower][storage]" )
{
    Lowered p( "i32 f( i32 n ) {\n"
               "    i32 total = 0;\n"
               "    for ( i32 i = 0; i < n; i++ )\n"
               "    {\n"
               "        i32 step = i;\n"
               "        if ( step == 3 ) { i32 inner = 1; break; }\n"
               "        if ( step == 4 ) { return total; }\n"
               "        total = total + step;\n"
               "    }\n"
               "    return total;\n"
               "}\n"
               "i32 main() { return 0; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text();
    INFO( text );

    // Every exit path in one function: fall through, break out of a nested scope, and return from
    // one. verify() checks that no marker names a projection and that no block is unreachable.
    REQUIRE( verify( p.functions[0] ).empty() );
    REQUIRE( count( text, "storage_live " + local_of( text, "step" ) ) == 1 );
}

// A destructor is an ordinary Function whose declaration is a Destructor_decl.
TEST_CASE( "lower_lowers_a_destructor", "[ir][lower][aggregates]" )
{
    SECTION( "it becomes a function, with `this` as its only parameter" )
    {
        Lowered p( "class Buffer { u8* ptr; ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.functions.size() == 2 ); // the destructor and main
        REQUIRE( p.functions[0].parameter_count == 1 );
        REQUIRE( verify( p.functions[0] ).empty() );

        // Printed with the tilde: aux holds the type's name, so `fn Buffer` would read as a free
        // function of that name rather than as the destructor.
        REQUIRE( p.text( 0 ).find( "fn ~Buffer {" ) != std::string::npos );
    }

    SECTION( "a class without one produces no function" )
    {
        Lowered p( "class Handle { u64 value; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.functions.size() == 1 );
    }

    // A bare field is `this.field`, so it lowers to the same two projections - a deref of the
    // receiver, then the field. The implicit and explicit spellings must not diverge.
    SECTION( "a bare field and `this.field` lower identically" )
    {
        Lowered bare( "class Buffer { u64 len; ~Buffer() { len = 1; } };\ni32 main() { return 0; }" );
        Lowered qualified( "class Buffer { u64 len; ~Buffer() { this.len = 1; } };\ni32 main() { return 0; }" );

        INFO( bare.rendered() << qualified.rendered() );
        REQUIRE( bare.clean() );
        REQUIRE( qualified.clean() );
        REQUIRE( bare.text( 0 ) == qualified.text( 0 ) );
    }

    SECTION( "the field is reached through the receiver, not from a local" )
    {
        Lowered p( "class Buffer { u64 len; ~Buffer() { len = 1; } };\ni32 main() { return 0; }" );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( p.clean() );
        REQUIRE( text.find( "(*_1).len = const 1" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "reading a field works too" )
    {
        Lowered p( "void release( u8* p ) { }\n"
                   "class Buffer { u8* ptr; ~Buffer() { release( ptr ); } };\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        for( const Function& function : p.functions )
        {
            REQUIRE( verify( function ).empty() );
        }
    }

    // The storage markers do not bracket a field: it is not a local, and its lifetime is the
    // object's. Only what the body itself declares gets one.
    SECTION( "a local inside a destructor is bracketed, a field is not" )
    {
        Lowered p( "class Buffer { u64 len; ~Buffer() { u64 seen = 0; } };\ni32 main() { return 0; }" );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( p.clean() );
        REQUIRE( text.find( "storage_live" ) != std::string::npos );
        REQUIRE( text.find( "storage_live _1" ) == std::string::npos ); // never the receiver
    }
}

// A literal is `str`'s constructor called on its bytes and their count, which leaves out the `0`
// C is given after them.
TEST_CASE( "lower_lowers_a_string_literal_to_str's_constructor", "[ir][lower][prelude]" )
{
    SECTION( "as a value" )
    {
        Lowered p( "i32 main() { str s = \"a\\0b\"; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "main" );

        INFO( text );
        REQUIRE( text.find( "_3 = &init _2" ) != std::string::npos );
        REQUIRE( text.find( "_4 = bytes \"a\\x00b\"" ) != std::string::npos );
        REQUIRE( text.find( "call str(copy _3, copy _4, const 3)" ) != std::string::npos );
        REQUIRE( text.find( "_1 = copy _2" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "and as a place, for a field read off it" )
    {
        Lowered p( "i32 main() { u64 n = \"hello\".size; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "main" );

        INFO( text );
        REQUIRE( text.find( "bytes \"hello\"" ) != std::string::npos );
        REQUIRE( text.find( "const 5)" ) != std::string::npos );
        REQUIRE( text.find( ".size" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }
}

// A branch on the build, then one whose failing side ends the program. Nothing is dropped on that
// side: the program aborts, and runs no destructor on the way out.
TEST_CASE( "lower_lowers_assert_to_a_branch_and_a_failed_terminator", "[ir][lower][assert]" )
{
    Lowered p( "class Owner { public Owner() { unsafe { q = alloc<i32>(); } } ~Owner() { unsafe { free( q ); } } i32* q; };\n"
               "i32 check( i32 x ) { Owner o = Owner(); assert( x > 0 ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "check" );

    INFO( text );
    REQUIRE( text.find( "_5 = copy _1 > const 0" ) != std::string::npos );
    REQUIRE( text.find( "branch debug -> bb1, bb2" ) != std::string::npos ); // the check may not run
    REQUIRE( text.find( "branch copy _5 -> bb2, bb3" ) != std::string::npos );
    REQUIRE( text.find( "bb3:\n        assert_failed\n" ) != std::string::npos );
    REQUIRE( text.find( "drop _2" ) < text.find( "bb3:" ) ); // only on the passing side

    for( const Function& function : p.functions )
    {
        REQUIRE( verify( function ).empty() );
    }
}

TEST_CASE( "lower_checks_overflow_only_outside_wrap", "[ir][lower][overflow]" )
{
    const Build checked { .overflow_checks = true };

    SECTION( "a bare sum is checked" )
    {
        Lowered p( "i32 f( i32 a, i32 b ) { return a + b; }", checked );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "overflows copy _1 + copy _2" ) != std::string::npos );
        REQUIRE( text.find( "        overflow\n" ) != std::string::npos );
    }

    SECTION( "a sum inside wrap is not" )
    {
        Lowered p( "i32 f( i32 a, i32 b ) { return wrap<i32>( -( a + b ) * 2 ); }", checked );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "overflows" ) == std::string::npos );
        REQUIRE( text.find( "copy _1 + copy _2" ) != std::string::npos );
    }

    SECTION( "nor without the flag" )
    {
        Lowered p( "i32 f( i32 a, i32 b ) { return a + b; }" );

        REQUIRE( p.named( "f" ).find( "overflows" ) == std::string::npos );
    }
}

// The value must survive the round trip, and keep its sign when the signedness changes.
TEST_CASE( "lower_checks_a_narrowing_cast", "[ir][lower][cast]" )
{
    SECTION( "narrower" )
    {
        Lowered p( "u8 f( i32 x ) { return cast<u8>( x ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "copy _1 as u8" ) != std::string::npos );
        REQUIRE( text.find( "as i32" ) != std::string::npos );
        REQUIRE( text.find( "== copy _1" ) != std::string::npos );
        REQUIRE( text.find( "copy _1 >= const 0" ) != std::string::npos );
        REQUIRE( text.find( "cast_failed" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    // Unsigned to signed: the converted value is the one whose sign is tested.
    SECTION( "same width, signedness changed" )
    {
        Lowered p( "i32 f( u32 x ) { return cast<i32>( x ); }" );

        INFO( p.rendered() );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "cast_failed" ) != std::string::npos );
        REQUIRE( text.find( "copy _1 >= const 0" ) == std::string::npos );
        REQUIRE( text.find( ">= const 0" ) != std::string::npos );
    }

    SECTION( "but not one that widens, nor a wrap" )
    {
        for( const char* body :
             { "i64 f( i32 x ) { return cast<i64>( x ); }",
               "i64 f( u32 x ) { return cast<i64>( x ); }",
               "u8 f( i32 x ) { return wrap<u8>( x ); }" } )
        {
            Lowered p( body );

            INFO( body << "\n" << p.rendered() );
            REQUIRE( p.clean() );
            REQUIRE( p.named( "f" ).find( "cast_failed" ) == std::string::npos );
        }
    }
}

// A constructor is an ordinary Function with the receiver first; its call returns nothing and
// takes the local's address.
TEST_CASE( "lower_lowers_a_constructor", "[ir][lower][aggregates]" )
{
    SECTION( "it becomes a function, receiver first" )
    {
        Lowered p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.functions.size() == 2 );
        REQUIRE( p.functions[0].parameter_count == 2 ); // `this` and `n`
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "the body writes through the receiver" )
    {
        Lowered p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "(*_1).len = copy _2" ) != std::string::npos );
    }

    // Not an assignment: there is no value to assign, because the constructor writes through the
    // pointer it is given.
    SECTION( "an initialisation is a call taking the local's address" )
    {
        Lowered p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\n"
                   "i32 main() { Buffer b = Buffer( 16 ); return 0; }" );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( p.clean() );
        REQUIRE( text.find( "storage_live _1" ) != std::string::npos );
        REQUIRE( text.find( "&init _1" ) != std::string::npos );
        REQUIRE( text.find( "call" ) != std::string::npos );
        REQUIRE( verify( p.functions[1] ).empty() );
    }

    SECTION( "a constructed local is still dropped at scope exit" )
    {
        Lowered p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } ~Buffer() { } };\n"
                   "i32 main() { { Buffer b = Buffer( 16 ); } return 0; }" );

        const std::string text = p.text( 2 );

        INFO( text );
        REQUIRE( p.clean() );
        REQUIRE( text.find( "drop _1" ) != std::string::npos );

        for( const Function& function : p.functions )
        {
            REQUIRE( verify( function ).empty() );
        }
    }
}

// `move` reads the same place with Move instead of Copy.
TEST_CASE( "lower_reads_a_move_as_a_move", "[ir][lower][move]" )
{
    SECTION( "an argument" )
    {
        Lowered p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( move a ); return 0; }" );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( p.clean() );
        REQUIRE( text.find( "call f(move _1)" ) != std::string::npos );
    }

    SECTION( "and without the marker it is still a copy" )
    {
        Lowered p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( a ); return 0; }" );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( text.find( "call f(copy _1)" ) != std::string::npos );
    }

    SECTION( "an initialiser" )
    {
        Lowered p( "i32 main() { i32 a = 1; i32 b = move a; return b; }" );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( p.clean() );
        REQUIRE( text.find( "_2 = move _1" ) != std::string::npos );
    }

    // A whole object, which is the only thing that can be moved: a field on its own would leave
    // the object partly moved, and the checker refuses it.
    SECTION( "a whole struct" )
    {
        Lowered p( "struct Point { i32 x; i32 y; };\n"
                   "void f( Point p ) { }\n"
                   "i32 main() { Point q = Point { 1, 2 }; f( move q ); return 0; }" );

        const std::string text = p.text( 1 );

        INFO( text );
        REQUIRE( p.clean() );
        REQUIRE( text.find( "call f(move _1)" ) != std::string::npos );
    }

    SECTION( "every function still verifies" )
    {
        Lowered p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( move a ); return 0; }" );

        for( const Function& function : p.functions )
        {
            REQUIRE( verify( function ).empty() );
        }
    }
}

// D32. A ref binding is a pointer local and every use a deref, so passes below sema see an
// ordinary pointer.
TEST_CASE( "lower_binds_a_ref_parameter_through_a_pointer", "[ir][lower][ref]" )
{
    Lowered p( "void bump( ref i32 n ) { n = n + 1; }\ni32 main() { i32 x = 1; bump( ref x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( 0 );

    INFO( text );
    REQUIRE( text.find( "let _1: i32*; // parameter n" ) != std::string::npos );

    // Reading and writing the name are the same place, through the binding.
    REQUIRE( text.find( "_2 = copy (*_1) + const 1" ) != std::string::npos );
    REQUIRE( text.find( "(*_1) = copy _2" ) != std::string::npos );
}

TEST_CASE( "lower_passes_a_ref_argument_as_an_address", "[ir][lower][ref]" )
{
    Lowered p( "void bump( ref i32 n ) { n = n + 1; }\ni32 main() { i32 x = 1; bump( ref x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( 1 );

    INFO( text );

    // The address goes into a temporary first, because an operand carries a place and not an
    // rvalue - the same step the synthesised destructor call already takes.
    REQUIRE( text.find( "_2 = &borrow _1" ) != std::string::npos );
    REQUIRE( text.find( "call bump(copy _2)" ) != std::string::npos );

    // And the caller's own local is untouched: a borrow is not a transfer, so nothing here is a
    // move and nothing is dropped.
    REQUIRE( text.find( "move" ) == std::string::npos );
}

// Passing a binding on is `&(*_1)` - the address it already holds. Worth a case of its own because
// it is the one place lower_place runs on a name that is itself a ref.
TEST_CASE( "lower_forwards_a_ref_binding", "[ir][lower][ref]" )
{
    Lowered p( "void bump( ref i32 n ) { n = n + 1; }\n"
               "void twice( ref i32 n ) { bump( ref n ); bump( ref n ); }\n"
               "i32 main() { i32 x = 1; twice( ref x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( 1 );

    INFO( text );
    REQUIRE( text.find( "= &borrow (*_1)" ) != std::string::npos );
}

// A class travels the same way, and the field reached through the binding is the caller's field.
// Nothing about the parameter is owning - its local is a pointer - so no drop is elaborated for it.
TEST_CASE( "lower_lends_a_class_by_ref", "[ir][lower][ref]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "void grow( ref B b ) { b.n = b.n + 1; }\n"
               "i32 main() { B a = B( 1 ); grow( ref a ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "grow" );

    INFO( text );
    REQUIRE_FALSE( text.empty() );
    REQUIRE( text.find( "let _1: B*; // parameter b" ) != std::string::npos );
    REQUIRE( text.find( "(*_1).n =" ) != std::string::npos );
    REQUIRE( text.find( "drop" ) == std::string::npos );
}

// D31. A bare owning parameter travels by address as `ref` does; only the checker tells them apart.
TEST_CASE( "lower_passes_a_bare_owning_parameter_by_address", "[ir][lower][borrow]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "u64 peek( B b ) { return b.n; }\n"
               "i32 main() { B a = B( 1 ); return wrap<i32>( peek( a ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "peek" );

    INFO( text );
    REQUIRE_FALSE( text.empty() );
    REQUIRE( text.find( "let _1: B*; // parameter b" ) != std::string::npos );
    REQUIRE( text.find( "copy (*_1).n" ) != std::string::npos );
}

TEST_CASE( "lower_borrows_at_the_call_without_moving", "[ir][lower][borrow]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "u64 peek( B b ) { return b.n; }\n"
               "i32 main() { B a = B( 1 ); return wrap<i32>( peek( a ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "= &read _1" ) != std::string::npos );

    // A borrow: nothing is moved and the caller still drops. A copy here would free the resource twice.
    REQUIRE( text.find( "move" ) == std::string::npos );
    REQUIRE( text.find( "drop _1" ) != std::string::npos );
}

// A temporary has no place until it is built, so it takes the other path through lower_argument -
// and the address being taken of where it landed is also what leaves it registered for the drop.
TEST_CASE( "lower_drops_a_borrowed_temporary", "[ir][lower][borrow]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "u64 peek( B b ) { return b.n; }\n"
               "i32 main() { return wrap<i32>( peek( B( 1 ) ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "= &read _1" ) != std::string::npos );
    REQUIRE( text.find( "drop _1" ) != std::string::npos );
}

// A call's owning result is a temporary like a struct literal's, so the statement drops it.
TEST_CASE( "lower_drops_an_owning_call_result", "[ir][lower][borrow]" )
{
    const std::string counter = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } B twin() const { return B( n ); } };\n"
                                "B make() { return B( 1 ); }\n"
                                "u64 peek( B b ) { return b.n; }\n"
                                "const ref B keep( const ref B b ) { return b; }\n";

    const auto drops_in_main = [&]( std::string_view body )
    {
        Lowered p( counter + "i32 main() { " + std::string( body ) + " return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "main" );

        INFO( text );
        return count( text, "drop _" );
    };

    SECTION( "discarded" )
    {
        REQUIRE( drops_in_main( "make();" ) == 1 );
    }

    SECTION( "through a function pointer" )
    {
        REQUIRE( drops_in_main( "fn() -> B p = &make; p();" ) == 1 );
    }

    SECTION( "from a method" )
    {
        REQUIRE( drops_in_main( "{ B h = B( 2 ); h.twin(); }" ) == 2 );
    }

    SECTION( "read through a field" )
    {
        REQUIRE( drops_in_main( "u64 n = make().n;" ) == 1 );
    }

    SECTION( "lent to another call" )
    {
        REQUIRE( drops_in_main( "u64 n = peek( make() );" ) == 1 );
    }

    // The address of the caller's own object, so dropping it would destroy `h` early.
    SECTION( "but not a const ref result, which is borrowed" )
    {
        REQUIRE( drops_in_main( "{ B h = B( 2 ); keep( h ); }" ) == 1 );
    }
}

// `f();` wants the call and not its result, so a result nothing drops lands in a void local, which
// the backend emits as a bare call.
TEST_CASE( "lower_calls_for_effect_into_a_void_local", "[ir][lower][calls]" )
{
    const auto main_of = []( const std::string& source )
    {
        Lowered p( source );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        for( const Function& function : p.functions )
        {
            REQUIRE( verify( function ).empty() );
        }

        return p.named( "main" );
    };

    SECTION( "a plain call" )
    {
        const std::string text = main_of( "i32 seven() { return 7; }\ni32 main() { seven(); return 0; }" );

        INFO( text );
        REQUIRE( text.find( "let _1: void;" ) != std::string::npos );
        REQUIRE( text.find( "_1 = call seven()" ) != std::string::npos );
    }

    SECTION( "through a function pointer" )
    {
        const std::string text = main_of( "i32 seven() { return 7; }\ni32 main() { fn() -> i32 p = &seven; p(); return 0; }" );

        INFO( text );
        REQUIRE( text.find( ": void;" ) != std::string::npos );
    }

    SECTION( "a method" )
    {
        const std::string text =
            main_of( "struct P { i32 x; i32 get() const { return x; } };\ni32 main() { P v = P { 1 }; v.get(); return 0; }" );

        INFO( text );
        REQUIRE( text.find( ": void;" ) != std::string::npos );
    }

    SECTION( "a const ref result, which is only an address" )
    {
        const std::string text =
            main_of( "const ref i32 pick( const ref i32 a ) { return a; }\ni32 main() { i32 x = 1; pick( x ); return 0; }" );

        INFO( text );
        REQUIRE( text.find( ": void;" ) != std::string::npos );
    }

    // Dropping needs the value, so an owning result keeps its type.
    SECTION( "but not an owning result" )
    {
        const std::string text =
            main_of( "class B { u64 n; B() { n = 0; } ~B() { } };\nB make() { return B(); }\ni32 main() { make(); return 0; }"
            );

        INFO( text );
        REQUIRE( text.find( ": void;" ) == std::string::npos );
        REQUIRE( text.find( "drop _" ) != std::string::npos );
    }
}

// Forwarding is `&(*_1)`: the address the binding already holds, taken back out of the deref that
// every use of the name goes through. No copy at either hop.
TEST_CASE( "lower_forwards_a_bare_borrow", "[ir][lower][borrow]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "u64 peek( B b ) { return b.n; }\n"
               "u64 forward( B b ) { return peek( b ); }\n"
               "i32 main() { B a = B( 1 ); return wrap<i32>( forward( a ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "forward" );

    INFO( text );
    REQUIRE( text.find( "let _1: B*; // parameter b" ) != std::string::npos );
    REQUIRE( text.find( "= &read (*_1)" ) != std::string::npos );
}

// A `move` parameter is owned, so it stays a value and the callee drops it.
TEST_CASE( "lower_keeps_a_move_parameter_by_value", "[ir][lower][borrow]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "void own( move B b ) { }\n"
               "i32 main() { own( move B( 1 ) ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string callee = p.named( "own" );

    INFO( callee );
    REQUIRE( callee.find( "let _1: B; // parameter b" ) != std::string::npos );
    REQUIRE( callee.find( "drop _1" ) != std::string::npos );

    // And the caller hands it over rather than lending it, so nothing is dropped twice.
    const std::string caller = p.named( "main" );

    INFO( caller );
    REQUIRE( caller.find( "move" ) != std::string::npos );
}

// D32. A `ref` local is a pointer holding an address, its initialiser lowered as a place.
TEST_CASE( "lower_binds_a_ref_local_to_an_address", "[ir][lower][binding]" )
{
    Lowered p( "i32 main() { i32 x = 1; ref i32 r = x; r = 5; return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "let _2: i32*; // r" ) != std::string::npos );
    REQUIRE( text.find( "_2 = &borrow _1" ) != std::string::npos );

    // Writing through the binding reaches the referent, and reading `x` afterwards reads the same
    // place - which is what makes the exit code of the codegen fixture mean anything.
    REQUIRE( text.find( "(*_2) = const 5" ) != std::string::npos );
    REQUIRE( text.find( "_0 = copy _1" ) != std::string::npos );
}

TEST_CASE( "lower_binds_a_ref_local_to_a_field", "[ir][lower][binding]" )
{
    Lowered p( "struct P { i32 x; };\ni32 main() { P p = P { 1 }; ref i32 r = p.x; r = 5; return p.x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "= &borrow _1.x" ) != std::string::npos );
}

// A binding owns nothing, so no drop is elaborated for it - the local it points at is dropped
// exactly once, by the scope that declared it.
TEST_CASE( "lower_never_drops_a_ref_binding", "[ir][lower][binding]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "i32 main() { B a = B( 1 ); ref B r = a; r.n = 5; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "drop _1" ) != std::string::npos );

    // Exactly one drop. Counting the word rather than naming the local is what would catch a
    // second one appearing on the binding.
    std::size_t drops = 0;
    for( std::size_t at = text.find( "drop " ); at != std::string::npos; at = text.find( "drop ", at + 1 ) )
    {
        ++drops;
    }

    REQUIRE( drops == 1 );

    // Nothing is moved either: binding is not a transfer.
    REQUIRE( text.find( "move" ) == std::string::npos );
}

// D32. `const ref` travels by address exactly as `ref` does, marked as only read through, which
// is what lets a borrow check tell a call that could change its argument from one that cannot.
TEST_CASE( "lower_passes_a_const_ref_parameter_by_address", "[ir][lower][constref]" )
{
    Lowered p( "struct P { i32 x; };\n"
               "i32 peek( const ref P p ) { return p.x; }\n"
               "i32 main() { P v = P { 1 }; return peek( v ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string callee = p.named( "peek" );

    INFO( callee );
    REQUIRE( callee.find( "let _1: P*; // parameter p" ) != std::string::npos );
    REQUIRE( callee.find( "copy (*_1).x" ) != std::string::npos );

    // A struct that would have been copied is borrowed. Asserts what travels, not which temporary
    // holds it.
    const std::string caller = p.named( "main" );

    INFO( caller );
    REQUIRE( caller.find( "= &read _1" ) != std::string::npos );
    REQUIRE( caller.find( "call peek(copy _1)" ) == std::string::npos );
}

TEST_CASE( "lower_binds_a_const_ref_local_to_an_address", "[ir][lower][constref]" )
{
    Lowered p( "i32 main() { i32 x = 1; const ref i32 r = x; return r + 1; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "let _2: i32*; // r" ) != std::string::npos );
    REQUIRE( text.find( "_2 = &read _1" ) != std::string::npos );
    REQUIRE( text.find( "copy (*_2)" ) != std::string::npos );
}

// A class passed by `const ref` is borrowed, not copied - which for an owning type is the
// difference between one destructor run and two. Nothing is moved and the caller still drops.
TEST_CASE( "lower_borrows_a_class_by_const_ref", "[ir][lower][constref]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "u64 peek( const ref B b ) { return b.n; }\n"
               "i32 main() { B a = B( 1 ); return wrap<i32>( peek( a ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "= &read _1" ) != std::string::npos );
    REQUIRE( text.find( "move" ) == std::string::npos );
    REQUIRE( text.find( "drop _1" ) != std::string::npos );
}

// §8. A `const ref` return travels as an address: the return slot holds a pointer.
TEST_CASE( "lower_returns_a_const_ref_as_an_address", "[ir][lower][escape]" )
{
    Lowered p( "const ref i32 pick( const ref i32 a ) { return a; }\n"
               "i32 main() { i32 x = 1; return pick( x ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string callee = p.named( "pick" );

    INFO( callee );
    REQUIRE( callee.find( "let _0: i32*; // return slot" ) != std::string::npos );
    REQUIRE( callee.find( "let _1: i32*; // parameter a" ) != std::string::npos );

    // The address, not a read: `&(*_1)`, since the parameter is itself a binding.
    REQUIRE( callee.find( "_0 = &read (*_1)" ) != std::string::npos );
}

// A field of a parameter is where the form earns its keep, and the address is of the projection.
TEST_CASE( "lower_returns_a_const_ref_to_a_field", "[ir][lower][escape]" )
{
    Lowered p( "struct P { i32 x; };\n"
               "const ref i32 get( const ref P p ) { return p.x; }\n"
               "i32 main() { P v = P { 1 }; return get( v ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string callee = p.named( "get" );

    INFO( callee );
    REQUIRE( callee.find( "= &read (*_1).x" ) != std::string::npos );
}

// At the caller the result is already an address, so binding takes it directly and copying derefs
// it. Those are the two things a caller can do with one.
TEST_CASE( "lower_uses_the_result_of_a_const_ref_return", "[ir][lower][escape]" )
{
    SECTION( "binding takes the pointer as it comes" )
    {
        Lowered p( "const ref i32 pick( const ref i32 a ) { return a; }\n"
                   "i32 main() { i32 x = 1; const ref i32 r = pick( x ); return r; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "main" );

        INFO( text );
        REQUIRE( text.find( "; // r" ) != std::string::npos );

        // Reading through it derefs, which is what says the binding holds an address rather than a
        // copy of the value.
        REQUIRE( text.find( "copy (*_" ) != std::string::npos );
    }

    SECTION( "and copying derefs it" )
    {
        Lowered p( "const ref i32 pick( const ref i32 a ) { return a; }\n"
                   "i32 main() { i32 x = 1; i32 v = pick( x ); return v; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "main" );

        INFO( text );
        REQUIRE( text.find( "copy (*_" ) != std::string::npos );
    }
}

// D31. An `out` parameter travels by address as a borrow does, and KIR records which locals arrive
// empty, since the pointer is valid but its referent unwritten.
TEST_CASE( "lower_passes_an_out_parameter_by_address", "[ir][lower][out]" )
{
    Lowered p( "void init( out i32 n ) { n = 1; }\ni32 main() { i32 x = 0; init( out x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string callee = p.named( "init" );

    INFO( callee );
    REQUIRE( callee.find( "let _1: i32*; // parameter n" ) != std::string::npos );
    REQUIRE( callee.find( "(*_1) = const 1" ) != std::string::npos );

    const std::string caller = p.named( "main" );

    INFO( caller );
    REQUIRE( caller.find( "= &init _1" ) != std::string::npos );
}

TEST_CASE( "lower_records_which_parameters_are_out", "[ir][lower][out]" )
{
    Lowered p( "void init( i32 a, out i32 b, out i32 c ) { b = a; c = a; }\n"
               "i32 main() { i32 x = 0; i32 y = 0; init( 1, out x, out y ); return x + y; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    // Locals 1..parameter_count in declaration order, so `b` is _2 and `c` is _3. Recorded rather
    // than inferred: assign_check has no other way to know which locals start empty.
    const Function& init = p.functions[0];

    REQUIRE( init.out_parameters.size() == 2 );
    REQUIRE( init.out_parameters[0] == Local_id { 2 } );
    REQUIRE( init.out_parameters[1] == Local_id { 3 } );
}

// A function with none records none, which is what lets assign_check leave every existing function
// alone without walking it.
TEST_CASE( "lower_records_no_out_parameters_where_there_are_none", "[ir][lower][out]" )
{
    Lowered p( "i32 add( i32 a, i32 b ) { return a + b; }\ni32 main() { return add( 1, 2 ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.functions[0].out_parameters.empty() );
}

// D30. A variant lowers to its ordinal, not the name in aux. Checked by value, since a golden
// would record wrong numbers as correct.
TEST_CASE( "lower_gives_a_variant_its_ordinal", "[ir][lower][enum]" )
{
    Lowered p( "enum Colour : u8 { Red, Green, Blue };\n"
               "i32 main() { Colour a = Colour::Red; Colour b = Colour::Green; Colour c = Colour::Blue; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "_1 = const 0" ) != std::string::npos );
    REQUIRE( text.find( "_2 = const 1" ) != std::string::npos );
    REQUIRE( text.find( "_3 = const 2" ) != std::string::npos );

    // And the local keeps the enum's own type, not the underlying integer: KIR is what the C
    // emitter reads, and the width comes from the enum rather than from the literal.
    REQUIRE( text.find( "let _1: Colour;" ) != std::string::npos );
}

TEST_CASE( "lower_compares_enums_as_integers", "[ir][lower][enum]" )
{
    Lowered p( "enum Colour { Red, Green };\n"
               "bool is_green( Colour c ) { return c == Colour::Green; }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "is_green" );

    INFO( text );
    REQUIRE( text.find( "copy _1 == const 1" ) != std::string::npos );
}

// D30. A switch is a chain of equality tests, so KIR and the passes over it gain no case.
TEST_CASE( "lower_turns_a_switch_into_a_branch_chain", "[ir][lower][switch]" )
{
    Lowered p( "enum Colour { Red, Green, Blue };\n"
               "i32 f( Colour c ) { switch( c ) { case Colour::Red: return 1; case Colour::Green: return 2;"
               " default: return 3; } }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "f" );

    INFO( text );
    REQUIRE( text.find( "== const 0" ) != std::string::npos );
    REQUIRE( text.find( "== const 1" ) != std::string::npos );

    // Nothing new in the IR: a switch is branches and gotos, which every pass already handles.
    REQUIRE( text.find( "switch" ) == std::string::npos );
}

// `fallthrough` jumps into the next arm's body, so every arm's block exists before any is filled.
TEST_CASE( "lower_sends_a_fallthrough_into_the_next_arm", "[ir][lower][switch]" )
{
    Lowered p( "enum Colour { Red, Green, Blue };\n"
               "i32 f( Colour c ) { i32 t = 0; switch( c ) { case Colour::Red: t = 1; fallthrough;"
               " case Colour::Green: t = t + 10; break; default: t = 100; } return t; }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "f" );

    INFO( text );

    // The arm that falls through must end in a goto rather than in the jump to the block after the
    // switch, and the block it names must be the one holding the next arm's body.
    REQUIRE( text.find( "const 1" ) != std::string::npos );
    REQUIRE( text.find( "const 10" ) != std::string::npos );
}

// A switch is a break target like a loop is, so the block after it and the block `break` jumps to
// are one block rather than two that happen to agree.
TEST_CASE( "lower_treats_a_switch_as_a_break_target", "[ir][lower][switch]" )
{
    SECTION( "break leaves the switch, not the enclosing loop" )
    {
        Lowered p( "enum Colour { Red, Green };\n"
                   "i32 f( Colour c ) { i32 n = 0; while( n < 3 ) { n = n + 1; switch( c ) { case Colour::Red: break;"
                   " default: n = n + 10; } } return n; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "continue looks past it to the loop" )
    {
        Lowered p( "enum Colour { Red, Green };\n"
                   "i32 f( Colour c ) { i32 n = 0; while( n < 3 ) { n = n + 1; switch( c ) { case Colour::Red: continue;"
                   " default: continue; } } return n; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "break with no loop at all" )
    {
        Lowered p( "enum Colour { Red, Green };\n"
                   "i32 f( Colour c ) { i32 n = 0; switch( c ) { case Colour::Red: break; default: n = 1; } return n; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Leaving an arm has to drop what the arm owns, whichever way it leaves - and `continue` is the
// one that would go wrong by unwinding to the switch's scope depth rather than the loop's.
TEST_CASE( "lower_drops_an_arms_locals_however_it_leaves", "[ir][lower][switch]" )
{
    const std::string_view owner = "enum Colour { Red, Green };\n"
                                   "class Owned { i32 x; Owned( i32 v ) { x = v; } ~Owned() { } };\n";

    for( const char* leaving : { "break;", "continue;", "return 1;" } )
    {
        const std::string source = std::string( owner ) +
                                   "i32 f( Colour c ) { i32 n = 0; while( n < 3 ) { n = n + 1; switch( c ) {"
                                   " case Colour::Red: { Owned o = Owned( 1 ); " +
                                   leaving + " } default: n = n + 1; } } return n; }\ni32 main() { return 0; }";

        Lowered p( source );

        INFO( source << "\n" << p.rendered() );
        REQUIRE( p.clean() );

        // Exactly one drop of the arm's local, on the path that leaves through it.
        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "drop" ) != std::string::npos );
    }
}

// Stacked labels are an OR, and an OR of equalities is two tests reaching one body. Checking that
// both branch to the *same* block is what distinguishes a shared arm from two copies of it.
TEST_CASE( "lower_points_stacked_labels_at_one_body", "[ir][lower][switch]" )
{
    Lowered p( "enum Colour { Red, Green, Blue };\n"
               "i32 f( Colour c ) { switch( c ) { case Colour::Red: case Colour::Green: return 1;"
               " default: return 3; } }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "f" );

    INFO( text );

    // Both tests name the same true-target. Read out of the dump rather than assumed, so a change
    // that duplicated the body would fail here rather than merely emitting more code.
    const std::size_t first  = text.find( "branch copy " );
    const std::size_t second = text.find( "branch copy ", first + 1 );

    REQUIRE( first != std::string::npos );
    REQUIRE( second != std::string::npos );

    const std::string first_target  = text.substr( text.find( "-> ", first ) + 3, 3 );
    const std::string second_target = text.substr( text.find( "-> ", second ) + 3, 3 );

    REQUIRE( first_target == second_target );
}

// The scrutinee is read once, before any block exists. A call scrutinee re-read per test would run
// the call once per arm - the kind of bug that produces right answers until the call has an effect.
TEST_CASE( "lower_reads_the_scrutinee_once", "[ir][lower][switch]" )
{
    Lowered p( "enum Colour { Red, Green };\n"
               "Colour pick() { return Colour::Red; }\n"
               "i32 f() { switch( pick() ) { case Colour::Red: return 1; case Colour::Green: return 2; } }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "f" );

    INFO( text );

    std::size_t calls = 0;

    for( std::size_t at = text.find( "call pick" ); at != std::string::npos; at = text.find( "call pick", at + 1 ) )
    {
        ++calls;
    }

    REQUIRE( calls == 1 );
}

// §6.4: `f64 x = 1;` stores an integer literal, which lowering turns into a float constant.
TEST_CASE( "lower_converts_an_integer_literal_in_float_context", "[ir][lower]" )
{
    Lowered p( "i32 main() { f64 x = 1; f32 y = 2; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "let _1: f64; // x" ) != std::string::npos );
    REQUIRE( text.find( "let _2: f32; // y" ) != std::string::npos );
}

// D34. A range label is two comparisons, one more link in the label chain.
TEST_CASE( "lower_turns_a_range_label_into_two_comparisons", "[ir][lower][range]" )
{
    Lowered p( "i32 f( i32 n ) { switch( n ) { case 1..5: return 1; default: return 0; } }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "f" );

    INFO( text );

    // Half-open: `>=` on the lower bound and `<` on the upper. A `<=` here would be an off-by-one
    // that every checker test above would still pass.
    REQUIRE( text.find( ">= const 1" ) != std::string::npos );
    REQUIRE( text.find( "< const 5" ) != std::string::npos );
    REQUIRE( text.find( "<= const 5" ) == std::string::npos );
}

// Mixing a range and a value in one stacked arm needs no special handling: a label list is already
// an OR, and the range is one more link in it.
TEST_CASE( "lower_stacks_a_range_with_a_value", "[ir][lower][range]" )
{
    Lowered p( "i32 f( i32 n ) { switch( n ) { case 1..5: case 20: return 1; default: return 0; } }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "f" );

    INFO( text );
    REQUIRE( text.find( ">= const 1" ) != std::string::npos );
    REQUIRE( text.find( "== const 20" ) != std::string::npos );
}

// Over a float the comparisons are against floats, because the bound is checked against the
// scrutinee's type rather than carried as an integer into codegen.
TEST_CASE( "lower_compares_a_float_range_as_floats", "[ir][lower][range]" )
{
    Lowered p( "i32 f( f64 x ) { switch( x ) { case 1..5: return 1; default: return 0; } }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "f" );

    INFO( text );
    REQUIRE( text.find( "let _1: f64; // parameter x" ) != std::string::npos );
    REQUIRE( text.find( ">= const 1" ) != std::string::npos );
}

// D7. An enum with a payload anywhere is a struct of tag and payload fields; one without stays its
// underlying integer.
TEST_CASE( "lower_gives_a_payload_enum_a_tag", "[ir][lower][payload]" )
{
    Lowered p( "enum Shape { Circle( f64 radius ), Dot };\n"
               "i32 main() { Shape s = Shape::Circle( 2.0 ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );

    // The tag, then the payload - assembled in place rather than copied in from somewhere, the same
    // shape a struct literal has.
    REQUIRE( text.find( ".tag = const 0" ) != std::string::npos );
    REQUIRE( text.find( ".radius = const 2" ) != std::string::npos );
}

// A variant with no payload, in an enum that has them elsewhere, is still a struct: its tag is
// written and nothing else.
TEST_CASE( "lower_builds_a_payload_free_variant_of_a_payload_enum", "[ir][lower][payload]" )
{
    Lowered p( "enum Shape { Circle( f64 radius ), Dot };\n"
               "i32 main() { Shape s = Shape::Dot; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( ".tag = const 1" ) != std::string::npos );
}

// An enum with no payloads keeps the plain ordinal: no tag, no struct.
TEST_CASE( "lower_leaves_a_payload_free_enum_as_an_integer", "[ir][lower][payload]" )
{
    Lowered p( "enum Colour { Red, Green };\ni32 main() { Colour c = Colour::Green; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "_1 = const 1" ) != std::string::npos );
    REQUIRE( text.find( ".tag" ) == std::string::npos );
}

// With no `default` the last arm is the fallback, and still binds its pattern.
TEST_CASE( "lower_binds_a_pattern_in_the_fallback_arm", "[ir][lower][payload]" )
{
    Lowered p( "enum Shape { Circle( f64 radius ), Rect( f64 w, f64 h ) };\n"
               "f64 area( Shape s ) { switch( s ) {"
               " case Shape::Circle( r ): return r;"
               " case Shape::Rect( w, h ): return w * h; } }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "area" );

    INFO( text );
    REQUIRE( text.find( "// w" ) != std::string::npos );
    REQUIRE( text.find( "// h" ) != std::string::npos );
}

TEST_CASE( "lower_switches_on_the_tag_and_binds_the_payload", "[ir][lower][payload]" )
{
    Lowered p( "enum Shape { Circle( f64 radius ), Rect( f64 w, f64 h ), Dot };\n"
               "f64 area( Shape s ) { switch( s ) {"
               " case Shape::Circle( r ): return r;"
               " case Shape::Rect( w, h ): return w * h;"
               " case Shape::Dot: return 0.0; } }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "area" );

    INFO( text );

    // The tag is what a label compares against, and it is read once - the arms never learn that
    // payloads exist, because what they are handed is one operand and one constant as before.
    REQUIRE( text.find( ".tag == const 0" ) != std::string::npos );
    REQUIRE( text.find( ".tag == const 1" ) != std::string::npos );

    // The bindings are locals holding a copy of the field - nothing here owns - written at the top
    // of the arm's own block so the names are live exactly where the resolver put them in scope.
    REQUIRE( text.find( "// r" ) != std::string::npos );
    REQUIRE( text.find( "// w" ) != std::string::npos );
    REQUIRE( text.find( "// h" ) != std::string::npos );
}

// D30. An owning enum is dropped by one synthesised destructor that tests the tag and drops
// only the live variant's payload; a variant that owns nothing gets no test.
TEST_CASE( "lower_synthesises_an_owning_enum's_destructor", "[ir][lower][payload][drop]" )
{
    constexpr std::string_view owning = "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n";

    SECTION( "it drops the active variant's payload, right to left" )
    {
        Lowered p(
            std::string( owning ) + "enum H { Full( B b ), Empty, Pair( u64 k, B l, B r ) };\n"
                                    "i32 main() { H h = H::Empty; return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "~H" );

        INFO( text );
        REQUIRE( text.find( "let _1: H*; // parameter" ) != std::string::npos );
        REQUIRE( text.find( "(*_1).tag == const 0" ) != std::string::npos );
        REQUIRE( text.find( "(*_1).tag == const 2" ) != std::string::npos );
        REQUIRE( text.find( "(*_1).tag == const 1" ) == std::string::npos );
        REQUIRE( text.find( "drop (*_1).b" ) != std::string::npos );
        REQUIRE( text.find( "drop (*_1).r" ) < text.find( "drop (*_1).l" ) );
    }

    SECTION( "and a drop site calls it rather than expanding the payload" )
    {
        Lowered p(
            std::string( owning ) + "enum H { Full( B b ), Empty };\n"
                                    "i32 main() { H h = H::Full( B( 1 ) ); return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "main" );

        INFO( text );
        REQUIRE( text.find( "drop _" ) != std::string::npos );

        std::istringstream lines( text );

        for( std::string line; std::getline( lines, line ); )
        {
            REQUIRE_FALSE( ( line.find( "drop " ) != std::string::npos && line.find( ".b" ) != std::string::npos ) );
        }
    }

    SECTION( "one per instance, including one only a payload names" )
    {
        Lowered p(
            std::string( owning ) + "enum Option<T> { Some( T value ), None };\n"
                                    "enum H { Full( Option<B> o ), Empty };\n"
                                    "i32 main() { H h = H::Empty; return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );
        REQUIRE_FALSE( p.named( "~H" ).empty() );
        REQUIRE_FALSE( p.named( "~Option" ).empty() );
    }

    SECTION( "never for an enum that owns nothing" )
    {
        Lowered p( "enum Shape { Circle( f64 r ), Dot };\ni32 main() { Shape s = Shape::Dot; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.named( "~Shape" ).empty() );
    }
}

// D7. A binding of an owning payload is the payload's address, and the variant takes what it is
// built from.
TEST_CASE( "lower_borrows_an_owning_payload_and_moves_one_in", "[ir][lower][payload][borrow]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "enum H { Full( B b ), Empty };\n"
               "u64 size( H h ) { switch( h ) { case H::Full( b ): return b.n; case H::Empty: return 0; } }\n"
               "i32 main() { B x = B( 1 ); H h = H::Full( move x ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string size = p.named( "size" );

    INFO( size );
    REQUIRE( size.find( "B*; // b" ) != std::string::npos );
    REQUIRE( size.find( "= &payload (*_1).b" ) != std::string::npos );

    const std::string main = p.named( "main" );

    INFO( main );
    REQUIRE( main.find( ".b = move _1" ) != std::string::npos );
}

// A binding borrows the scrutinee in place, so a temporary one is dropped when the switch ends
// rather than when the statement that built it does - which would be before any arm ran.
TEST_CASE( "lower_keeps_an_owning_scrutinee_alive_across_the_arms", "[ir][lower][payload][drop]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "enum H { Full( B b ), Empty };\n"
               "H make() { return H::Full( B( 1 ) ); }\n"
               "u64 f() { switch( make() ) { case H::Full( b ): return b.n; case H::Empty: return 0; } }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string text = p.named( "f" );

    INFO( text );
    REQUIRE( text.find( "drop _1" ) > text.find( "= copy (*_3).n" ) );
}

// D7. A switch over `move h` moves `h` whole, and a binding holds its payload by value rather than
// borrowing it.
TEST_CASE( "lower_moves_a_consumed_scrutinee_into_its_bindings", "[ir][lower][payload][move]" )
{
    Lowered p( "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "enum H { Full( B b ), Empty };\n"
               "B f( move H h ) { switch( move h ) { case H::Full( b ): return b; case H::Empty: return B( 0 ); } }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string text = p.named( "f" );

    INFO( text );
    REQUIRE( text.find( "= move _1" ) != std::string::npos );
    REQUIRE( text.find( ": B; // b" ) != std::string::npos );
    REQUIRE( text.find( "&borrow" ) == std::string::npos );
}

// D6. `try` tests the tag: `ok` yields the payload, and `err` is written into the return slot and
// returned through every drop the function owes.
TEST_CASE( "lower_try_returns_the_error_through_the_drops", "[ir][lower][try]" )
{
    Lowered p( "enum Bad { nope };\n"
               "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "result<B, Bad> make() { return result::ok( B( 1 ) ); }\n"
               "result<u64, Bad> f() { B keep = B( 2 ); B v = try make(); return result::ok( v.n ); }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string text = p.named( "f" );

    INFO( text );
    REQUIRE( text.find( ".tag == const 0" ) != std::string::npos );
    REQUIRE( text.find( "_0.tag = const 1" ) != std::string::npos );
    REQUIRE( text.find( "_0.error = copy" ) != std::string::npos );
    REQUIRE( count( text, "return\n" ) == 2 );
    REQUIRE( count( text, "drop _1\n" ) == 2 ); // `keep`, once on each path

    // `v` holds nothing yet when the `err` leaves, so only the `ok` path drops it.
    const std::size_t named = text.find( ": B; // v\n" );

    REQUIRE( named != std::string::npos );

    const std::size_t start = text.rfind( "let ", named ) + 4;

    REQUIRE( count( text, "drop " + text.substr( start, named - start ) + "\n" ) == 1 );
}

// D51. An `ok` of `void` carries nothing, so nothing is read out of it.
TEST_CASE( "lower_try_yields_nothing_from_a_void_result", "[ir][lower][try]" )
{
    Lowered p( "enum Bad { nope };\n"
               "result<void, Bad> step() { return result::ok(); }\n"
               "result<void, Bad> f() { try step(); return result::ok(); }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string text = p.named( "f" );

    INFO( text );
    REQUIRE( text.find( "_0.tag = const 1" ) != std::string::npos );
    REQUIRE( text.find( ".value" ) == std::string::npos );
}

// What the statement built before the `try` is the statement's to drop, on the way out as well.
TEST_CASE( "lower_try_drops_the_statement's_earlier_temporaries", "[ir][lower][try]" )
{
    Lowered p( "enum Bad { nope };\n"
               "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "result<u64, Bad> size() { return result::ok( 1 ); }\n"
               "u64 sum( move B b, u64 n ) { return n; }\n"
               "result<u64, Bad> f() { u64 r = sum( move B( 7 ), try size() ); return result::ok( r ); }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string text = p.named( "f" );

    INFO( text );

    const std::size_t declared = text.find( ": B;" );

    REQUIRE( declared != std::string::npos );

    const std::size_t start     = text.rfind( "let ", declared ) + 4;
    const std::string temporary = text.substr( start, declared - start );

    REQUIRE( count( text, "drop " + temporary + "\n" ) == 2 );
}

// An aggregate is written once every value in it exists, so an `err` leaves none half-built for the
// statement's drops to reach.
TEST_CASE( "lower_try_runs_before_an_aggregate_is_written", "[ir][lower][try]" )
{
    constexpr std::string_view held = "enum Bad { nope };\n"
                                      "class B { public u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                                      "class P { public B a; public B b; };\n"
                                      "result<B, Bad> make() { return result::ok( B( 1 ) ); }\n";

    SECTION( "a variant" )
    {
        Lowered p(
            std::string( held ) + "result<B, Bad> f() { return result::ok( try make() ); }\n"
                                  "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( ".tag = const 0" ) > text.find( ".tag == const 0" ) );
    }

    SECTION( "a struct literal" )
    {
        Lowered p(
            std::string( held ) + "result<u64, Bad> f() { P p = P { B( 1 ), try make() }; return result::ok( 0 ); }\n"
                                  "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( ".a = move" ) > text.find( ".tag == const 0" ) );
    }
}

// D32. A method call's first argument is the receiver's address.
TEST_CASE( "lower_passes_the_receiver_by_address", "[ir][lower][method]" )
{
    Lowered p( "struct P { i32 x; i32 get() const { return x; } };\n"
               "i32 main() { P p = P { 1 }; return p.get(); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string callee = p.named( "get" );

    INFO( callee );
    REQUIRE( callee.find( "let _1: P*; // parameter this" ) != std::string::npos );

    // A bare field name is `this.field`, and the receiver is a borrowed binding - so place_for
    // derefs it and the projection is off the referent, not off the pointer.
    REQUIRE( callee.find( "copy (*_1).x" ) != std::string::npos );

    const std::string caller = p.named( "main" );

    // A `const` method's receiver is only read through.
    INFO( caller );
    REQUIRE( caller.find( "= &read _1" ) != std::string::npos );
    REQUIRE( caller.find( "call get(copy _" ) != std::string::npos );
}

// The receiver is parameter 0, so the written arguments start at 1 - and lining them up wrongly
// would convert each against the parameter beside it rather than its own.
TEST_CASE( "lower_passes_arguments_after_the_receiver", "[ir][lower][method]" )
{
    Lowered p( "struct P { i32 x; void set( i32 a, i32 b ) { x = a + b; } };\n"
               "i32 main() { P p = P { 0 }; p.set( 7, 9 ); return p.x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string caller = p.named( "main" );

    INFO( caller );
    REQUIRE( caller.find( "const 7, const 9" ) != std::string::npos );
}

// A free function's borrow and a method's travel alike. The method's was cast from the value, which
// C refuses for a struct and, for an `i32`, runs and reads address 1.
TEST_CASE( "lower_passes_a_method's_borrowed_argument_by_address", "[ir][lower][method]" )
{
    SECTION( "`const ref`" )
    {
        Lowered p( "struct B { i32 n; bool same( const ref i32 o ) const { return n == o; } };\n"
                   "i32 main() { B a = B { 1 }; i32 c = 1; bool r = a.same( c ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string caller = p.named( "main" );

        INFO( caller );
        REQUIRE( caller.find( "_6 = &read _3" ) != std::string::npos );
        REQUIRE( caller.find( "as i32*" ) == std::string::npos );
    }

    SECTION( "`ref`, marked at the call" )
    {
        Lowered p( "struct B { i32 n; void give( ref i32 o ) const { o = n; } };\n"
                   "i32 main() { B a = B { 1 }; i32 c = 0; a.give( ref c ); return c; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string caller = p.named( "main" );

        INFO( caller );
        REQUIRE( caller.find( "= &borrow _3" ) != std::string::npos );
        REQUIRE( caller.find( "as i32*" ) == std::string::npos );
    }
}

// D22 reaches through a pointer for a field, and a method is reached the same way - so the receiver
// address is the pointer's value rather than the pointer's own address.
TEST_CASE( "lower_calls_a_method_through_a_pointer", "[ir][lower][method]" )
{
    Lowered p( "struct P { i32 x; i32 get() const { return x; } };\n"
               "i32 main() { P p = P { 5 }; P* q = &p; return q.get(); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string caller = p.named( "main" );

    INFO( caller );

    // The pointer is handed over as it stands. `&q` would be a `P**` - which is what this did
    // until the D22 branch was written, and it compiled to C that dereferenced the wrong thing.
    REQUIRE( caller.find( "call get(copy _" ) != std::string::npos );
    REQUIRE( caller.find( "= &_2" ) == std::string::npos );
}

// §8. A method returning a binding hands back an address, so the call is typed as that pointer.
TEST_CASE( "lower_returns_a_reference_from_a_method", "[ir][lower][method]" )
{
    Lowered p( "struct P { i32 x; const ref i32 get() const { return x; } };\n"
               "i32 main() { P p = P { 7 }; const ref i32 r = p.get(); return r; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string callee = p.named( "get" );

    INFO( callee );
    REQUIRE( callee.find( "let _0: i32*; // return slot" ) != std::string::npos );
    REQUIRE( callee.find( "_0 = &read (*_1).x" ) != std::string::npos );

    // And the caller binds the pointer it was handed rather than the address of a copy of it.
    const std::string caller = p.named( "main" );

    INFO( caller );
    REQUIRE( caller.find( "let _3: i32*; // r" ) != std::string::npos );
    REQUIRE( caller.find( "copy (*_3)" ) != std::string::npos );
}

// §6.7. A static method has no receiver, so the parameter walk must not take its first parameter
// as `this`.
TEST_CASE( "lower_gives_a_static_method_no_receiver", "[ir][lower][static]" )
{
    Lowered p( "struct P { i32 x; static P make( i32 v ) { return P { v }; } };\n"
               "i32 main() { P p = P::make( 7 ); return p.x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string callee = p.named( "make" );

    INFO( callee );
    REQUIRE( callee.find( "parameter this" ) == std::string::npos );
    REQUIRE( callee.find( "// parameter v" ) != std::string::npos );
}

// And the call site prepends nothing. A method call takes the receiver's address as argument 0, so
// a static one emitting that would pass an address the callee has no parameter for.
TEST_CASE( "lower_prepends_no_receiver_to_a_static_call", "[ir][lower][static]" )
{
    Lowered p( "struct P { i32 x; static P make( i32 v ) { return P { v }; } };\n"
               "i32 main() { P p = P::make( 7 ); return p.x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string caller = p.named( "main" );

    INFO( caller );
    REQUIRE( caller.find( "call make(const 7)" ) != std::string::npos );
}

// A plain method beside a static one still gets its receiver.
TEST_CASE( "lower_still_passes_a_plain_method_its_receiver", "[ir][lower][static]" )
{
    Lowered p( "struct P { i32 x; static P make( i32 v ) { return P { v }; } i32 get() const { return x; } };\n"
               "i32 main() { P p = P::make( 7 ); return p.get(); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string callee = p.named( "get" );

    INFO( callee );
    REQUIRE( callee.find( "let _1: P*; // parameter this" ) != std::string::npos );
    REQUIRE( callee.find( "copy (*_1).x" ) != std::string::npos );
}

// `p[ i ]` is `*( p + i )` in KIR: an offset into a temporary, then a Deref projection of it. No new
// projection kind, so every pass that already reads a place through a pointer reads this one.
TEST_CASE( "lower_indexes_through_an_offset", "[ir][lower][many]" )
{
    SECTION( "a read" )
    {
        Lowered p( "i32 get( i32[*] p, u64 i ) { i32 r = 0; unsafe { r = p[ i ]; } return r; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "get" );

        INFO( text );
        REQUIRE( text.find( "= copy _1 + copy _2" ) != std::string::npos ); // p is _1, i is _2
        REQUIRE( text.find( "let _4: i32[*];" ) != std::string::npos );     // the offset is a pointer, not an element
        REQUIRE( text.find( "copy (*_4)" ) != std::string::npos );
        REQUIRE( every_function_verifies( p ) );
    }

    // Only the pointer skips the operand conversion; an element is an ordinary value and widens.
    SECTION( "an element widens like any other value" )
    {
        Lowered p( "u64 add( u64[*] p, u32 k ) { u64 r = 0; unsafe { r = p[ 0 ] + k; } return r; }\n"
                   "void grow( u64[*] p, u32 k ) { unsafe { p[ 1 ] += k; } }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.named( "add" ).find( "copy _2 as u64" ) != std::string::npos );
        REQUIRE( p.named( "grow" ).find( "copy _2 as u64" ) != std::string::npos );
        REQUIRE( every_function_verifies( p ) );
    }

    SECTION( "a write" )
    {
        Lowered p( "void set( i32[*] p, u64 i, i32 v ) { unsafe { p[ i ] = v; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "set" );

        INFO( text );
        REQUIRE( text.find( "= copy _1 + copy _2" ) != std::string::npos );
        REQUIRE( text.find( ") = copy _3" ) != std::string::npos ); // `(*_n) = copy _3`
        REQUIRE( every_function_verifies( p ) );
    }

    SECTION( "a constant index" )
    {
        Lowered p( "i32 first( i32[*] p ) { i32 r = 0; unsafe { r = p[ 0 ]; } return r; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.named( "first" ).find( "= copy _1 + const 0" ) != std::string::npos );
    }

    SECTION( "the address of an element" )
    {
        Lowered p( "i32* at( i32[*] p, u64 i ) { i32* r = nullptr; unsafe { r = &p[ i ]; } return r; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "at" );

        INFO( text );
        REQUIRE( text.find( "= &borrow (*_" ) != std::string::npos );
        REQUIRE( every_function_verifies( p ) );
    }

    SECTION( "a field of an element" )
    {
        Lowered p( "struct P { i32 x; };\n"
                   "i32 get( P[*] p, u64 i ) { i32 r = 0; unsafe { r = p[ i ].x; } return r; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "get" );

        INFO( text );
        REQUIRE( text.find( ").x" ) != std::string::npos ); // `(*_n).x`
        REQUIRE( every_function_verifies( p ) );
    }

    SECTION( "an element of an element" )
    {
        Lowered p( "u8 get( u8[*][*] rows ) { u8 r = 0; unsafe { r = rows[ 1 ][ 2 ]; } return r; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "get" );

        INFO( text );
        REQUIRE( text.find( "= copy _1 + const 1" ) != std::string::npos );
        REQUIRE( text.find( "+ const 2" ) != std::string::npos );
        REQUIRE( every_function_verifies( p ) );
    }

    // The index is evaluated once, even where the place is both read and written.
    SECTION( "a compound write evaluates the index once" )
    {
        Lowered p( "u64 next() { return 1; }\n"
                   "void bump( i32[*] p ) { unsafe { p[ next() ] += 1; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "bump" );

        INFO( text );

        std::size_t calls = 0;

        for( std::size_t at = text.find( "call next(" ); at != std::string::npos; at = text.find( "call next(", at + 1 ) )
        {
            calls += 1;
        }

        REQUIRE( calls == 1 );
    }
}

TEST_CASE( "lower_offsets_a_many_item_pointer", "[ir][lower][many]" )
{
    SECTION( "`+` is one binary operation of the pointer's type" )
    {
        Lowered p( "i32[*] skip( i32[*] p, u64 n ) { i32[*] r = nullptr; unsafe { r = p + n; } return r; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "skip" );

        INFO( text );
        REQUIRE( text.find( "= copy _1 + copy _2" ) != std::string::npos );
        REQUIRE( text.find( " as " ) == std::string::npos ); // no conversion of either side
        REQUIRE( every_function_verifies( p ) );
    }

    SECTION( "`++` steps by one element" )
    {
        Lowered p( "void step( i32[*] p ) { unsafe { p++; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.named( "step" ).find( "copy _1 + const 1" ) != std::string::npos );
    }

    SECTION( "`+=` offsets without converting the step" )
    {
        Lowered p( "i32[*] skip( i32[*] p, u64 n ) { unsafe { p += n; } return p; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "skip" );

        INFO( text );
        REQUIRE( text.find( "_1 = copy _1 + copy _2" ) != std::string::npos );
        REQUIRE( text.find( " as " ) == std::string::npos );
        REQUIRE( every_function_verifies( p ) );
    }
}

TEST_CASE( "lower_allocates_a_count", "[ir][lower][alloc][many]" )
{
    SECTION( "the count is an operand" )
    {
        Lowered p( "i32[*] make( u64 n ) { i32[*] r = nullptr; unsafe { r = alloc<i32>( n ); } return r; }\n"
                   "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "make" );

        INFO( text );
        REQUIRE( text.find( "allocate i32[*], copy _1" ) != std::string::npos );
        REQUIRE( every_function_verifies( p ) );
    }

    SECTION( "no count is unchanged" )
    {
        Lowered p( "i32* make() { i32* r = nullptr; unsafe { r = alloc<i32>(); } return r; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "make" );

        INFO( text );
        REQUIRE( text.find( "allocate i32*" ) != std::string::npos );
        REQUIRE( text.find( "allocate i32*," ) == std::string::npos );
    }
}

// D37: an element write through a pointer never drops what was there, because nothing may have been.
TEST_CASE( "lower_writes_an_owning_element_without_dropping_it", "[ir][lower][many]" )
{
    Lowered p( "class C { i32 n; C( i32 v ) { n = v; } ~C() { } };\n"
               "void put( C[*] cs ) { unsafe { cs[ 0 ] = C( 1 ); } }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "put" );

    INFO( text );
    REQUIRE( text.find( "drop (*" ) == std::string::npos );
    REQUIRE( every_function_verifies( p ) );
}

namespace
{

const std::string shape_source = "interface Shape { f64 area() const; void grow( f64 by ); };\n"
                                 "struct Sq implements Shape { f64 side; f64 area() const { return side * side; } "
                                 "void grow( f64 by ) { side = side + by; } };\n";

} // namespace

// The call stays a call of the interface's method, on an address typed at the interface.
TEST_CASE( "lower_calls_an_interface's_method_on_its_address", "[ir][lower][interfaces]" )
{
    SECTION( "`const ref` reads through the borrow" )
    {
        Lowered p( shape_source + "f64 f( const ref Shape s ) { return s.area(); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "let _2: const Shape*;" ) != std::string::npos );
        REQUIRE( text.find( "_2 = &read (*_1)" ) != std::string::npos );
        REQUIRE( text.find( "call area(copy _2)" ) != std::string::npos );
    }

    SECTION( "`ref` borrows for a method that writes" )
    {
        Lowered p( shape_source + "void f( ref Shape s ) { s.grow( 1.0 ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "_2 = &borrow (*_1)" ) != std::string::npos );
        REQUIRE( text.find( "call grow(copy _2, const 1)" ) != std::string::npos );
    }

    SECTION( "a pointer is passed as it stands" )
    {
        Lowered p( shape_source + "f64 f( Shape* s ) { return s.area(); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "call area(copy _1)" ) != std::string::npos );
        REQUIRE( text.find( "&" ) == std::string::npos );
    }
}

// Through a bound the instance knows the implementer, so the receiver is typed at it and no
// `Shape*` is made.
TEST_CASE( "lower_calls_through_a_bound_directly", "[ir][lower][interfaces][generic]" )
{
    SECTION( "a plain implementer" )
    {
        Lowered p(
            shape_source + "f64 f<T>( const ref T t ) where T : Shape { return t.area(); }\n"
                           "i32 main() { Sq q = Sq { 2.0 }; f64 a = f( q ); return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "let _2: const Sq*;" ) != std::string::npos );
        REQUIRE( text.find( "call area(copy _2)" ) != std::string::npos );
        REQUIRE( text.find( "Shape" ) == std::string::npos );
    }

    SECTION( "a generic implementer brings its own arguments" )
    {
        Lowered p( "interface Shape { f64 area() const; };\n"
                   "struct Pair<T> implements Shape where T : copyable { T a; f64 area() const { return 2.0; } };\n"
                   "f64 f<T>( const ref T t ) where T : Shape { return t.area(); }\n"
                   "i32 main() { Pair<i32> q = Pair { 1 }; f64 a = f( q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "let _2: const Pair<i32>*;" ) != std::string::npos );
        REQUIRE( text.find( "call area(copy _2)" ) != std::string::npos );
        REQUIRE( p.named( "area" ).find( "let _1: Pair<i32>*; // parameter this" ) != std::string::npos );
    }
}

// Every initialiser, assignment and return casts an implementer's address into the `Shape*`; an
// argument already did.
TEST_CASE( "lower_casts_an_implementer's_address_to_its_interface", "[ir][lower][interfaces]" )
{
    for( const char* body :
         { "Shape* f( Sq* q ) { Shape* s = q; return s; }",
           "Shape* f( Sq* q, Shape* s ) { s = q; return s; }",
           "Shape* f( Sq* q ) { return q; }",
           "f64 g( Shape* s ) { return s.area(); }\nf64 f( Sq* q ) { return g( q ); }" } )
    {
        Lowered p( shape_source + body + "\ni32 main() { return 0; }" );

        INFO( body << "\n" << p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "f" );

        INFO( text );
        REQUIRE( text.find( "copy _1 as Shape*" ) != std::string::npos );
    }
}

// A vtable's drop slot needs the whole drop as one function, which an implementer that owns has only
// if lowering makes it: `Framed` has no `~`, and its `Buf` is dropped by nothing else.
TEST_CASE( "lower_synthesises_a_drop_function_for_an_owning_implementer", "[ir][lower][interfaces][drop]" )
{
    Lowered p( "interface Shape { f64 area() const; };\n"
               "class Buf { u8[*] b; public Buf() { unsafe { b = alloc<u8>( 1 ); } } ~Buf() { unsafe { free( b ); } } };\n"
               "class Framed implements Shape { Buf frame; public Framed() { frame = Buf(); } "
               "public f64 area() const { return 1.0; } };\n"
               "struct Sq implements Shape { f64 side; f64 area() const { return side; } };\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string text = p.named( "drop Framed" );

    INFO( text );
    REQUIRE( text.find( "let _1: Framed*; // parameter" ) != std::string::npos );
    REQUIRE( text.find( "drop (*_1).frame" ) != std::string::npos );

    // Owning nothing, its vtable's slot calls nothing.
    REQUIRE( p.named( "drop Sq" ).empty() );
}

// `box<Circle>` to `box<Shape>` is a `Cast` of the moved box, so the source is not dropped again.
TEST_CASE( "lower_casts_a_box_to_a_box_of_its_interface", "[ir][lower][interfaces]" )
{
    Lowered p(
        shape_source + "box<Shape> f( move box<Sq> b ) { box<Shape> s = move b; return s; }\n"
                       "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string text = p.named( "f" );

    INFO( text );
    REQUIRE( text.find( "move _1 as box<Shape>" ) != std::string::npos );
}

// `Box<i64>` is first interned by lowering `make<i64>`, after every instance before it was queued.
TEST_CASE( "lower_reaches_what_lowering_an_instance_interns", "[ir][lower][generic]" )
{
    SECTION( "a destructor" )
    {
        Lowered p( "class Box<T> where T : copyable { T v; public Box( T x ) { v = x; } ~Box() { } };\n"
                   "i32 make<T>( T x ) where T : copyable { Box<T> b = Box<T>( x ); return 0; }\n"
                   "i32 main() { return make<i64>( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "~Box" );

        INFO( text );
        REQUIRE( text.find( "let _1: Box<i64>*; // parameter this" ) != std::string::npos );
    }

    SECTION( "an implementer's method" )
    {
        Lowered p( "interface Shape { f64 area() const; };\n"
                   "struct Pair<T> implements Shape where T : copyable { T a; f64 area() const { return 2.0; } };\n"
                   "f64 measure( const ref Shape s ) { return s.area(); }\n"
                   "f64 make<T>( T x ) where T : copyable { Pair<T> p = Pair { x }; return measure( p ); }\n"
                   "i32 main() { f64 a = make<i64>( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "area" );

        INFO( text );
        REQUIRE( text.find( "let _1: Pair<i64>*; // parameter this" ) != std::string::npos );
    }
}

// D60: an element is an `Index` projection off the array's own place, so the checks see `a[i]` as
// part of `a`. A run-time index is checked against N first; a literal one sema has already checked.
TEST_CASE( "lower_indexes_a_fixed_array", "[ir][lower][fixed_array]" )
{
    SECTION( "a run-time index is checked" )
    {
        Lowered p( "i32 at( i32[4] a, u64 i ) { return a[ i ]; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "at" );

        INFO( text );
        REQUIRE( text.find( "let _1: i32[4]; // parameter a" ) != std::string::npos );
        REQUIRE( text.find( "out_of_bounds" ) != std::string::npos );
        REQUIRE( text.find( "copy _1[_" ) != std::string::npos );
    }

    SECTION( "a literal index is not" )
    {
        Lowered p( "i32 third( i32[4] a ) { return a[ 2 ]; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "third" );

        INFO( text );
        REQUIRE( text.find( "out_of_bounds" ) == std::string::npos );
        REQUIRE( text.find( "copy _1[_" ) != std::string::npos );
    }

    SECTION( "through a borrow, and written" )
    {
        Lowered p( "void set( ref i32[4] a ) { a[ 1 ] = 7; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( every_function_verifies( p ) );

        const std::string text = p.named( "set" );

        INFO( text );
        REQUIRE( text.find( "(*_1)[_" ) != std::string::npos );
        REQUIRE( text.find( "= const 7" ) != std::string::npos );
    }

    SECTION( "the size is the constant N" )
    {
        Lowered p( "u64 n( i32[4] a ) { return a.size(); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.named( "n" ).find( "_0 = const 4" ) != std::string::npos );
    }
}

TEST_CASE( "lower_fills_an_array_from_a_literal", "[ir][lower][fixed_array]" )
{
    Lowered p( "i32 main() { i32[3] a = [ 1, 2, 3 ]; return a[ 0 ]; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "] = const 1" ) != std::string::npos );
    REQUIRE( text.find( "] = const 3" ) != std::string::npos );
    REQUIRE( text.find( "out_of_bounds" ) == std::string::npos );
}

// One synthesised destructor per owning array type, looping over the elements, as a union's is
// one function rather than an expansion at every drop.
TEST_CASE( "lower_drops_every_element_of_an_owning_array", "[ir][lower][fixed_array][drop]" )
{
    Lowered p( "class Tok { u8 b; public Tok() { b = 0; } ~Tok() { } };\n"
               "void keep( move Tok[3] a ) { }\n"
               "void reset( ref Tok[3] a ) { a[ 0 ] = Tok(); }\n"
               "void plain( i32[3] a ) { }\n"
               "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    REQUIRE( p.named( "keep" ).find( "drop _1" ) != std::string::npos );

    // Assigning an element drops the one it replaces.
    REQUIRE( p.named( "reset" ).find( "drop (*_1)[_" ) != std::string::npos );

    const std::string destructor = p.named( "~Tok[3]" );

    INFO( destructor );
    REQUIRE( destructor.find( "let _1: Tok[3]*; // parameter" ) != std::string::npos );
    REQUIRE( destructor.find( "drop (*_1)[_" ) != std::string::npos );

    REQUIRE( p.named( "~i32[3]" ).empty() );
}

TEST_CASE( "lower_lends_an_array_as_a_run", "[ir][lower][fixed_array][run]" )
{
    Lowered p( "i32 sum( const ref i32[] v ) { return v[ 0 ]; }\n"
               "i32 main() { i32[2] a = [ 1, 2 ]; const ref i32[] r = a; return sum( a ) + r[ 1 ]; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( every_function_verifies( p ) );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( count( text, "run( " ) == 2 );
    REQUIRE( text.find( ", const 2 )" ) != std::string::npos );
}

} // namespace keel
#endif
