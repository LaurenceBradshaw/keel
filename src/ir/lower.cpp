#include "ir/lower.h"
#include <cassert>
#include <unordered_map>

#include <fmt/format.h>
#include <vector>
#include "ast/node.h"
#include "ir/builder.h"

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
        const Types&      types,
        Literals&         literals,
        const Interner&   interner
    );

    Function run();

private:
    Place   place_for( Node_id declaration );
    Node_id field_of( Type_id type, Symbol_id name ) const;

    Operand lower_expression( Node_id id ); // produces a value

    // One per construct, dispatched from lower_expression. The small cases stay inline there:
    // extracting a two-line literal would cost a name and buy nothing.
    Operand lower_struct_literal( Node_id id );
    Operand lower_binary( Node_id id );
    Operand lower_unary( Node_id id );
    Operand lower_call( Node_id id );
    Place   lower_place( Node_id id );     // names where a value lives
    void    lower_statement( Node_id id ); // emits, produces nothing
    Operand lower_argument( Node_id argument, Node_id parameter );

    Operand address_operand( Place place, Type_id type, Span span );

    // One per construct, dispatched from lower_statement - the shape visit_* uses next door.
    void lower_block( Node_id id );
    void lower_return( Node_id id );
    void lower_var( Node_id id );
    void lower_assign( Node_id id );
    void lower_increment( Node_id id );
    void lower_if( Node_id id );
    void lower_while( Node_id id );
    void lower_for( Node_id id );

    // Where `break` and `continue` jump to. In KIR these are plain edges - no labels, no
    // emit-the-label-only-if-used, and no -Wunused-label to design around. That is the whole of
    // what the C emitter needed a stack of names and a used-flag for.
    struct Loop_targets
    {
        Block_id break_target;    // the exit block
        Block_id continue_target; // a while re-tests at the header; a for runs its update first
        u32      depth;
    };

    // Allocated on demand, because a block nothing jumps to is unreachable and verify rejects one.
    // `for( ; ; )` with no break needs no exit, and a body that always returns needs no latch.
    Block_id break_target();
    Block_id continue_target();

    // `&&` and `||` are not operators over two operands: the right side must not run unless the
    // left demands it, so they lower to control flow.
    Operand lower_short_circuit( Node_id id );

    // The type an operation happens in, and the conversion that puts an operand there. See §6.4.
    Type_id operation_type( Node_id node ) const;
    Operand converted( Operand operand, Type_id to, Span span );
    Operand moved_if_owning( Operand operand ) const;

    bool is_move_parameter( Node_id param ) const;

    u32  scope_depth() const;
    void push_scope();
    void pop_scope( Span span );
    void unwind_to( u32 depth, Span span );
    void drop_place( Place place, Type_id type, Span span );
    // Drops what the statement built, in reverse order, and clears the list. Reverse for the same
    // reason locals unwind in reverse: it is the order destructors run in.
    void drop_statement_temporaries( Span span );
    // A call whose callee names a type rather than a function.
    bool is_construction( Node_id id ) const;

    // Runs a construct with `target`'s address as its receiver. Not an expression: a constructor
    // returns nothing and writes through the pointer it is handed.
    void lower_construction( Place target, Node_id call_expr );

    const Ast& ast_;

    // Held but not read yet: resolution_ is what turns a Name_expr into the declaration whose
    // local it is, which arrives with names. literals_ and interner_ may turn out unnecessary -
    // a Literal_id comes straight off aux, and a Symbol_id is passed through without a lookup.
    const Resolution&                resolution_;
    const Types&                     types_;
    Literals&                        literals_;
    [[maybe_unused]] const Interner& interner_;

    const Node_id declaration_;

    Builder                           builder_;
    std::unordered_map<u32, Local_id> locals_;
    Local_id                          receiver_ {}; // A destructor's synthesised `this`. Invalid in a plain function.
    // Parameters the callee owns, and therefore drops. Collected in the constructor and put in a
    // scope by run(), because scope_locals_ has no scope to go into until then.
    std::vector<Local_id>   owned_parameters_;
    std::unordered_set<u32> borrowed_bindings_; // Param_decl ids whose local holds an address

    // Owning temporaries built by the statement being lowered. They have an owner - the caller -
    // and therefore a drop, and the right moment for it is the end of the statement, which is after
    // every use of them within it.
    std::vector<Local_id> statement_temporaries_;

    std::vector<Loop_targets> loops_; // innermost last, so break and continue bind to it
    std::vector<Local_id>     scope_locals_;
    std::vector<u32>          scope_marks_;
};

Lowering::Lowering(
    Node_id           declaration,
    const Ast&        ast,
    const Resolution& resolution,
    const Types&      types,
    Literals&         literals,
    const Interner&   interner
)
    : ast_( ast ),
      resolution_( resolution ),
      types_( types ),
      literals_( literals ),
      interner_( interner ),
      declaration_( declaration ),
      builder_( declaration, binding_type( ast, types, declaration ), ast_.span( declaration ) ),
      locals_()
{
    // walk ast_.children( declaration )[1] - the Param_list - and for each Param_decl, add_parameter() and record the Local_id
    // in locals_.
    for( const Node_id param : ast_.children( ast_.children( declaration )[1] ) )
    {
        if( ast_.kind( param ) == Node_kind::Param_decl )
        {
            const Type_id   type  = binding_type( ast_, types_, param );
            const Span      span  = ast_.span( param );
            const Symbol_id name  = Symbol_id { ast_.aux( param ) };
            const Local_id  local = builder_.add_parameter( type, span, name );
            locals_.emplace( param.v, local );

            if( is_borrowed_binding( ast_, types_, param ) )
            {
                borrowed_bindings_.insert( param.v );
            }

            if( parameter_mode( ast_, param ) == Keyword::Out )
            {
                builder_.mark_out_parameter( local );
            }

            if( is_move_parameter( param ) )
            {
                owned_parameters_.push_back( local );
            }

            // The parser puts the receiver first, and it is what a bare field name is reached
            // through. Captured from the walk rather than assumed to be local 1, so it survives
            // anything that adds a local before this runs.
            if( !receiver_.is_valid() && ast_.kind( declaration ) != Node_kind::Function_decl )
            {
                receiver_ = local;
            }
        }
    }
}

Function Lowering::run()
{
    const Span span = ast_.span( declaration_ );

    // An owned parameter is dropped like a local, in a scope *enclosing* the body - which is what
    // makes `return`'s unwind_to( 0 ) reach it, since depth 0 is this scope's mark rather than the
    // body's. Pushed after the mark so the parameters sit above it.
    push_scope();

    for( const Local_id parameter : owned_parameters_ )
    {
        scope_locals_.push_back( parameter );
    }

    lower_statement( ast_.children( declaration_ )[2] ); // the body block

    // The fall-off-the-end path: a `return` has already unwound to 0, and unwind_to is a no-op on a
    // terminated block, so this only fires where nothing returned.
    pop_scope( span );

    if( !builder_.is_terminated() )
    {
        builder_.terminate_return( span );
    }

    return builder_.finish();
}

Place Lowering::place_for( Node_id declaration )
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
        return builder_.field( builder_.deref( builder_.place( receiver_ ) ), declaration );
    }

    // Anything else that resolves to a Var_decl is at file scope: a function-local one is in the
    // map by the time any name can refer to it, because use-before-declaration is a resolver error.
    assert( ast_.kind( declaration ) == Node_kind::Var_decl && "a name resolves to a local or a global" );
    return builder_.global( declaration );
}

Node_id Lowering::field_of( Type_id type, Symbol_id name ) const
{
    for( const Node_id field : ast_.children( types_.table().get( type ).declaration ) )
    {
        if( ast_.kind( field ) == Node_kind::Field_decl && ast_.aux( field ) == name.v )
        {
            return field;
        }
    }

    return Node_id {};
}

// Not always the type the operation *produces*: a comparison yields bool while its operands still
// meet at their common type, and §6.4's answer for that is recorded nowhere on the node.
Type_id Lowering::operation_type( Node_id node ) const
{
    const Type_id left  = types_.type_of( ast_.children( node )[0] );
    const Type_id right = types_.type_of( ast_.children( node )[1] );

    const Type_id common = types_.table().arithmetic_result( left, right );

    // A shift takes no common type: its result is the left operand's, which is why
    // arithmetic_result has no answer for one.
    return common.is_valid() ? common : left;
}

// Both operands of a binary end up sharing a type, so a backend can read KIR without re-deriving
// §6.4 - and so an LLVM backend gets what it already requires of an add. Most code produces no
// conversion at all: a literal has already adopted its type from the checker.
Operand Lowering::converted( Operand operand, Type_id to, Span span )
{
    if( operand.type == to )
    {
        return operand;
    }

    return copy( builder_.place( builder_.into_temp( cast_to( operand, to ), to, span ) ), to );
}

Operand Lowering::moved_if_owning( Operand operand ) const
{
    if( operand.kind != Operand_kind::Copy || !types_.is_owning( operand.type ) )
    {
        return operand;
    }

    return move( operand.place, operand.type );
}

bool Lowering::is_move_parameter( Node_id param ) const
{
    const Node_id annotation = ast_.children( param )[0];

    return ast_.kind( annotation ) == Node_kind::Mode_type && static_cast<Keyword>( ast_.aux( annotation ) ) == Keyword::Move;
}

u32 Lowering::scope_depth() const
{
    return static_cast<u32>( scope_marks_.size() );
}

void Lowering::push_scope()
{
    scope_marks_.push_back( static_cast<u32>( scope_locals_.size() ) );
}

void Lowering::pop_scope( Span span )
{
    unwind_to( scope_depth() - 1, span );
    scope_locals_.resize( scope_marks_.back() );
    scope_marks_.pop_back();
}

void Lowering::unwind_to( u32 depth, Span span )
{
    // A break or return already left this block; nothing after its terminator can run.
    if( builder_.is_terminated() )
    {
        return;
    }

    for( std::size_t i = scope_locals_.size(); i > scope_marks_[depth]; --i )
    {
        const Local_id local = scope_locals_[i - 1];

        drop_place( builder_.place( local ), builder_.type_of( local ), span );
        builder_.storage_dead( local, span );
    }
}

void Lowering::drop_place( Place place, Type_id type, Span span )
{
    if( !types_.is_owning( type ) )
    {
        return;
    }

    const Node_id decl = types_.table().get( type ).declaration;

    // Its own destructor first, then its members.
    for( const Node_id member : ast_.children( decl ) )
    {
        if( ast_.kind( member ) == Node_kind::Destructor_decl )
        {
            builder_.drop( place, span );
            break;
        }
    }

    // Reverse declaration order.
    const auto members = ast_.children( decl );
    for( std::size_t i = members.size(); i > 0; --i )
    {
        const Node_id member = members[i - 1];

        if( ast_.kind( member ) == Node_kind::Field_decl )
        {
            drop_place( builder_.field( place, member ), types_.type_of( member ), span );
        }
    }
}

void Lowering::drop_statement_temporaries( Span span )
{
    for( std::size_t i = statement_temporaries_.size(); i > 0; --i )
    {
        const Local_id temporary = statement_temporaries_[i - 1];

        drop_place( builder_.place( temporary ), builder_.type_of( temporary ), span );
    }

    statement_temporaries_.clear();
}

bool Lowering::is_construction( Node_id id ) const
{
    if( ast_.kind( id ) != Node_kind::Call_expr )
    {
        return false;
    }

    const Node_id decl = resolution_.declaration_of( ast_.children( id )[0] );

    return decl.is_valid() && is_aggregate( ast_.kind( decl ) );
}

void Lowering::lower_construction( Place target, Node_id call_expr )
{
    const Span    span      = ast_.span( call_expr );
    const Node_id aggregate = resolution_.declaration_of( ast_.children( call_expr )[0] );

    Node_id constructor {};

    for( const Node_id member : ast_.children( aggregate ) )
    {
        if( ast_.kind( member ) == Node_kind::Constructor_decl )
        {
            constructor = member;
            break;
        }
    }

    assert( constructor.is_valid() && "the checker rejects constructing a type that has none" );

    const std::span<const Node_id> parameters = ast_.children( ast_.children( constructor )[1] );
    const std::span<const Node_id> arguments  = ast_.children( ast_.children( call_expr )[1] );

    // Parameter 0 is the receiver, and the signature pass already recorded its type as `t*` - so
    // there is no pointer type to build here, only one to read.
    const Type_id receiver_type = types_.type_of( parameters[0] );

    std::vector<Operand> operands;

    operands.reserve( arguments.size() + 1 );
    operands.push_back(
        copy( builder_.place( builder_.into_temp( address_of( target, receiver_type ), receiver_type, span ) ), receiver_type )
    );

    // Two loops, as a plain call has, and for the same reason: every argument is lowered before any
    // is converted, so the statements come out in the order they were written (§7.1).
    for( const Node_id argument : arguments )
    {
        operands.push_back( lower_expression( argument ) );
    }

    for( std::size_t i = 0; i < arguments.size(); ++i )
    {
        const Type_id param_type = types_.type_of( parameters[i + 1] );
        operands[i + 1]          = converted( operands[i + 1], param_type, ast_.span( arguments[i] ) );
    }

    const u32     first = builder_.add_operands( operands );
    const Type_id type  = types_.type_of( constructor ); // void

    // The result is discarded, but Assign stays total: a void local is what the backend drops the
    // assignment from, leaving the bare call.
    builder_.into_temp( call( constructor, first, static_cast<u32>( operands.size() ), type ), type, span );
}

Block_id Lowering::break_target()
{
    assert( !loops_.empty() && "the checker rejects a break outside a loop" );

    Loop_targets& loop = loops_.back();

    if( !loop.break_target.is_valid() )
    {
        loop.break_target = builder_.add_block();
    }

    return loop.break_target;
}

Block_id Lowering::continue_target()
{
    assert( !loops_.empty() && "the checker rejects a continue outside a loop" );

    Loop_targets& loop = loops_.back();

    // A while sets this to its header up front, so only a for's latch is ever allocated here.
    if( !loop.continue_target.is_valid() )
    {
        loop.continue_target = builder_.add_block();
    }

    return loop.continue_target;
}

// The result is a local rather than an operand because it has to be readable after the join
// whichever path ran. This is the one place so far where a local exists purely to merge two paths -
// in an SSA IR it would be a phi node, which is what §3.1 chose not to have.
Operand Lowering::lower_short_circuit( Node_id id )
{
    const Token_kind op   = static_cast<Token_kind>( ast_.aux( id ) );
    const Span       span = ast_.span( id );
    const Type_id    type = types_.type_of( id );

    const Local_id result = builder_.add_local( type, span );

    builder_.assign( builder_.place( result ), use( lower_expression( ast_.children( id )[0] ) ), span );

    const Block_id right_block = builder_.add_block();
    const Block_id join        = builder_.add_block();

    // `&&` evaluates the right side when the left is true, `||` when it is false. That is the only
    // difference between them.
    const bool is_and = op == Token_kind::Amp_amp;

    builder_.terminate_branch(
        copy( builder_.place( result ), type ), is_and ? right_block : join, is_and ? join : right_block, span
    );

    builder_.switch_to( right_block );
    builder_.assign( builder_.place( result ), use( lower_expression( ast_.children( id )[1] ) ), span );

    // From wherever lowering ended up, not from right_block: a nested `&&` on the right leaves the
    // cursor in its own join.
    builder_.terminate_goto( join, span );

    builder_.switch_to( join );

    return copy( builder_.place( result ), type );
}

Operand Lowering::lower_struct_literal( Node_id id )
{
    // A temporary, then one assignment per field. Lowered and assigned in a single pass, so the
    // order the fields are *written* is the order they run - which is what §7.1 asks for, and
    // what the C emitter needed two phases to achieve because it was building one expression.
    const Type_id  type = types_.type_of( id );
    const Span     span = ast_.span( id );
    const Local_id temp = builder_.add_local( type, span );

    if( types_.is_owning( type ) )
    {
        statement_temporaries_.push_back( temp );
    }

    // Positional form names no field, so the i-th initialiser fills the i-th field. The two
    // forms cannot be mixed - the checker rejects that - so an index is enough here.
    const std::span<const Node_id> fields = ast_.children( types_.table().get( type ).declaration );

    std::size_t index = 0;

    for( const Node_id initialiser : ast_.children( id ) )
    {
        const Symbol_id name  = Symbol_id { ast_.aux( initialiser ) };
        const Node_id   field = name.is_valid() ? field_of( type, name ) : fields[index];

        assert( field.is_valid() && "checker should have rejected an unknown field" );

        // §6.4 may have widened the value to reach the field, the same as an argument reaching
        // a parameter. Saying so here is what keeps a backend from re-deriving it.
        const Operand value = lower_expression( ast_.children( initialiser )[0] );

        builder_.assign(
            builder_.field( builder_.place( temp ), field ),
            // Moved, not copied, when the field owns something: a copy would leave the temporary
            // and the field holding one resource between them, and both would be dropped.
            use( moved_if_owning( converted( value, types_.type_of( field ), ast_.span( initialiser ) ) ) ),
            ast_.span( initialiser )
        );

        index += 1;
    }

    return copy( builder_.place( temp ), type );
}

Operand Lowering::lower_binary( Node_id id )
{
    const Token_kind op = static_cast<Token_kind>( ast_.aux( id ) );

    // Not operations over two operands - the right side may not run at all - so they take a
    // different path entirely. Lowering them here would evaluate both sides unconditionally.
    if( op == Token_kind::Amp_amp || op == Token_kind::Pipe_pipe )
    {
        return lower_short_circuit( id );
    }

    const Span    span      = ast_.span( id );
    const Type_id operation = operation_type( id );

    // Both operands are lowered before either is converted: lowering is what can have effects,
    // so its order is §7.1's order, and the conversions are pure and can follow.
    const Operand raw_left  = lower_expression( ast_.children( id )[0] );
    const Operand raw_right = lower_expression( ast_.children( id )[1] );

    // A shift's count keeps its own type - it is a width, not a value meeting the left operand.
    const bool is_shift = op == Token_kind::Less_less || op == Token_kind::Greater_greater;

    const Operand left  = converted( raw_left, operation, span );
    const Operand right = is_shift ? raw_right : converted( raw_right, operation, span );

    // The type the operation *produces*, which for a comparison is bool.
    const Type_id type = types_.type_of( id );

    return copy( builder_.place( builder_.into_temp( binary( op, left, right, type ), type, span ) ), type );
}

Operand Lowering::lower_unary( Node_id id )
{
    const Token_kind op   = static_cast<Token_kind>( ast_.aux( id ) );
    const Type_id    type = types_.type_of( id );
    const Span       span = ast_.span( id );

    // Both of these are settled before the operand is lowered, because neither reads it the
    // way an arithmetic operator does: `*p` names a place, and `&x` wants where x lives rather
    // than what is in it. Lowering the operand as an expression first would read it, and for
    // `&*get()` would run the call twice.
    if( op == Token_kind::Star )
    {
        return copy( lower_place( id ), type );
    }

    if( op == Token_kind::Amp )
    {
        // A place, not an operand - which is why verify's Address_of case looks at a.place and
        // ignores the operand's kind.
        const Rvalue address = address_of( lower_place( ast_.children( id )[0] ), type );

        return copy( builder_.place( builder_.into_temp( address, type, span ) ), type );
    }

    const Operand a = lower_expression( ast_.children( id )[0] );

    return copy( builder_.place( builder_.into_temp( unary( op, a, type ), type, span ) ), type );
}

Operand Lowering::lower_call( Node_id id )
{
    // In value position there is no destination, so it constructs into a temporary. Nothing
    // drops that temporary - the same hole an owning struct literal already has, and no new one.
    if( is_construction( id ) )
    {
        const Type_id  type  = types_.type_of( id );
        const Local_id local = builder_.add_local( type, ast_.span( id ) );

        if( types_.is_owning( type ) )
        {
            statement_temporaries_.push_back( local );
        }

        lower_construction( builder_.place( local ), id );

        return copy( builder_.place( local ), type );
    }

    const Node_id callee = resolution_.declaration_of( ast_.children( id )[0] );
    assert(
        callee.is_valid() && ast_.kind( callee ) == Node_kind::Function_decl &&
        "checker should have rejected an unresolved call"
    );
    const std::span<const Node_id> arguments = ast_.children( ast_.children( id )[1] ); // the Arg_list's children

    // The *parameter* types, from the callee's Param_list. An argument's own type is not the
    // same thing: §6.4 may have widened it to reach the parameter, and reading the type off
    // the argument would make every conversion a no-op.
    const std::span<const Node_id> parameters = ast_.children( ast_.children( callee )[1] );

    // Every argument is lowered before any is converted, so the order the statements come out
    // in is the order the arguments were written - which is what §7.1 guarantees and C leaves
    // unspecified.
    std::vector<Operand> operands;

    operands.reserve( arguments.size() );

    for( std::size_t i = 0; i < arguments.size(); ++i )
    {
        operands.push_back( lower_argument( arguments[i], parameters[i] ) );
    }

    for( std::size_t i = 0; i < operands.size(); ++i )
    {
        operands[i] = converted( operands[i], binding_type( ast_, types_, parameters[i] ), ast_.span( arguments[i] ) );

        // The parameter's mode rather than what was written at the call: KIR records what
        // happens. Today they coincide, because the checker requires the marker - but the
        // invariant is what stops the caller's end-of-statement drop firing on something it
        // handed away.
        if( is_move_parameter( parameters[i] ) )
        {
            operands[i] = moved_if_owning( operands[i] );
        }
    }

    const u32 first = builder_.add_operands( operands );

    // A void call still assigns, to a local of type void that nothing reads. Keeping Assign
    // total is worth more than avoiding it: an optional destination would mean every consumer
    // handling a statement that writes nowhere. It is what MIR does with the unit type, and a
    // backend spells it by emitting the call and dropping the assignment.
    const Type_id type = types_.type_of( id );

    // A callee that returns a binding hands back an address, so the call's own type is that pointer
    // and so is the temporary holding it - `type` above is the language type, `T`, and the two
    // differ only here. Dereferencing once is what makes both uses at the call site fall out: a
    // copy reads `(*_t)`, and a binding takes `&(*_t)`, which is `_t` again.
    const bool    binding     = is_borrowed_binding( ast_, types_, callee );
    const Type_id result_type = binding ? binding_type( ast_, types_, callee ) : type;

    const Local_id result = builder_.into_temp(
        call( callee, first, static_cast<u32>( operands.size() ), result_type ), result_type, ast_.span( id )
    );

    return copy( binding ? builder_.deref( builder_.place( result ) ) : builder_.place( result ), type );
}

Operand Lowering::lower_expression( Node_id id )
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Name_expr:
    {
        const Node_id decl = resolution_.declaration_of( id );
        return copy( place_for( decl ), types_.type_of( id ) );
    }
    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Char_literal:
        // aux is the Literal_id, and the checker recorded the type context gave it.
        return constant( Literal_id { ast_.aux( id ) }, types_.type_of( id ) );
    case Node_kind::Null_literal:
        // Unlike the other literals, aux carries no Literal_id - the parser records nothing for
        // `nullptr`, and Literal_id { 0 } is the invalid sentinel. A null pointer is the integer
        // zero here, the same way a bool is 0 or 1.
        return constant( literals_.add_integer( 0 ), types_.type_of( id ) );
    case Node_kind::Bool_literal:
    {
        Literal_id literal = literals_.add_integer( ast_.aux( id ) != 0 ? 1 : 0 );
        return constant( literal, types_.type_of( id ) );
    }
    case Node_kind::Struct_literal:
        return lower_struct_literal( id );
    case Node_kind::Binary_expr:
        return lower_binary( id );
    case Node_kind::Unary_expr:
        return lower_unary( id );
    case Node_kind::Call_expr:
        return lower_call( id );
    case Node_kind::Cast_expr:
        // TODO: distinguish between cast and wrap.
        return converted( lower_expression( ast_.children( id )[1] ), types_.type_of( id ), ast_.span( id ) );
    // Reading a place is a copy of it - no temporary, because a place is already readable. That is
    // the whole of what lower_place buys in value position.
    case Node_kind::Field_expr:
        return copy( lower_place( id ), types_.type_of( id ) );
    case Node_kind::Marker_expr:
    {
        const Node_id operand = ast_.children( id )[0];
        const Type_id type    = types_.type_of( id );

        // A named variable is already a place, and moving it empties that place. A temporary is
        // not: it has to be built somewhere before it can be handed over. lower_expression puts it
        // in a local of its own and registers it as a statement temporary, so if the transfer never
        // happens the caller still drops it - and if it does, the move clears the flag and the
        // caller's drop is skipped.
        if( ast_.kind( operand ) == Node_kind::Name_expr )
        {
            return move( lower_place( operand ), type );
        }

        const Operand built = lower_expression( operand );

        assert( built.kind != Operand_kind::Constant && "the checker rejects moving anything without a place" );

        return move( built.place, type );
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
        // Same lookup as lower_expression's, but producing where the value lives rather than a
        // read of it.
        return place_for( resolution_.declaration_of( id ) );
    case Node_kind::Field_expr:
    {
        // The field's declaration, found on the object's struct type - same lookup as the emitter does.
        const Node_id   object      = ast_.children( id )[0];
        const Type_id   object_type = types_.type_of( object );
        const Symbol_id name        = Symbol_id { ast_.aux( id ) };

        // D22: `.` reaches through a pointer, so `p.x` is the implicit form of `( *p ).x` and
        // lowers to the same two projections. It takes the pointer's *value* rather than its
        // place - the Star case below is the same shape, written out.
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
        if( static_cast<Token_kind>( ast_.aux( id ) ) == Token_kind::Amp )
        {
            assert( false && "unary & not a place" );
            return Place {};
        }
        const Operand pointer = lower_expression( ast_.children( id )[0] );
        const Span    span    = ast_.span( id );

        // A constant has no place to project from, so it needs a local first. Anything else already
        // names one - including a call result, which lower_expression put in a temporary.
        const Place base = pointer.kind == Operand_kind::Constant
                               ? builder_.place( builder_.into_temp( use( pointer ), pointer.type, span ) )
                               : pointer.place;

        return builder_.deref( base );
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
    push_scope();
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
    pop_scope( ast_.span( id ) );
    return;
}

void Lowering::lower_return( Node_id id )
{
    const Node_id value = ast_.children( id )[0]; // invalid for a bare `return;`
    const Span    span  = ast_.span( id );
    if( value.is_valid() )
    {
        // Returning transfers ownership out of the function, so the value is read as a move: without
        // it the callee's scope exit drops what the caller now holds. §8 exempts `return` from
        // needing a written marker precisely because the transfer is unambiguous here - there is no
        // later use for one to warn about.
        if( is_borrowed_binding( ast_, types_, declaration_ ) )
        {
            // The address, not a read of it: a read would return a copy of the referent and the
            // form would buy nothing.
            const Type_id address = binding_type( ast_, types_, declaration_ );

            builder_.assign( builder_.place( k_return_slot ), address_of( lower_place( value ), address ), span );
        }
        else
        {
            builder_.assign( builder_.place( k_return_slot ), use( moved_if_owning( lower_expression( value ) ) ), span );
        }
    }
    drop_statement_temporaries( span );
    unwind_to( 0, span ); // everything in the function is dead after a return

    builder_.terminate_return( span );
    return;
}

void Lowering::lower_var( Node_id id )
{
    // Children are { type, init }. aux is the Symbol_id of the name, which is what the checker
    // recorded in the resolution for a Name_expr that refers to this declaration.
    const Span      span     = ast_.span( id );
    const Symbol_id name     = Symbol_id { ast_.aux( id ) };
    const bool      borrowed = is_borrowed_binding( ast_, types_, id );
    const Type_id   type     = borrowed ? binding_type( ast_, types_, id ) : types_.type_of( id );
    const Local_id  local    = builder_.add_local( type, span, name );

    locals_.emplace( id.v, local );
    builder_.storage_live( local, span );
    scope_locals_.push_back( local );
    if( borrowed )
    {
        borrowed_bindings_.insert( id.v );
    }

    const Node_id init = ast_.children( id )[1];
    if( init.is_valid() )
    {
        // A binding stores the address, so the initialiser is lowered as a place rather than read.
        // Nothing else about the local changes: it is storage_live like any other, and a pointer
        // owns nothing, so no drop is elaborated for it.
        if( borrowed )
        {
            builder_.assign( builder_.place( local ), address_of( lower_place( init ), type ), span );
        }
        else if( is_construction( init ) )
        {
            // Constructed in place. Going via a temporary and copying would make two of an
            // owning type where the program said one, and only one of them would be dropped.
            lower_construction( builder_.place( local ), init );
        }
        else
        {
            // An owning value is never copied: the copy would share the resource, and both would be
            // dropped. Where the source is a temporary that is exactly right - it has no other
            // owner.
            builder_.assign( builder_.place( local ), use( moved_if_owning( lower_expression( init ) ) ), span );
        }
    }
    drop_statement_temporaries( span );
}

void Lowering::lower_assign( Node_id id )
{
    const Span       span   = ast_.span( id );
    const Place      target = lower_place( ast_.children( id )[0] );
    const Operand    value  = lower_expression( ast_.children( id )[1] );
    const Token_kind op     = static_cast<Token_kind>( ast_.aux( id ) );
    if( op == Token_kind::Equal )
    {
        // Same reason as an initialiser: an owning value is never copied, or both copies would be
        // dropped. A compound assignment cannot reach here for one - arithmetic on an owning type
        // has no meaning.
        builder_.assign( target, use( moved_if_owning( value ) ), ast_.span( id ) );
    }
    else
    {
        // `x += 3` is `x = x + 3`: read the target, combine, store back.
        //
        // The operation happens at the *target's* type, not at types_.type_of( id ) - the
        // checker records nothing on a statement, so that would be an invalid Type_id. It is
        // also the type the checker measured the value against, so the two agree by
        // construction.
        const Type_id type = types_.type_of( ast_.children( id )[0] );

        const Operand left  = copy( target, type );
        const Operand right = converted( value, type, span );

        builder_.assign( target, binary( base_operator( op ), left, right, type ), span );
    }
    drop_statement_temporaries( span );
}

void Lowering::lower_increment( Node_id id )
{
    const Place      target      = lower_place( ast_.children( id )[0] );
    const Type_id    target_type = types_.type_of( ast_.children( id )[0] );
    const Token_kind op          = static_cast<Token_kind>( ast_.aux( id ) );
    const Token_kind base_op     = op == Token_kind::Plus_plus ? Token_kind::Plus : Token_kind::Minus;
    const Operand    left        = copy( target, target_type );
    // Into the table the *type* will be read from. Literals live in two tables and an operand
    // says which by its type, so an integer 1 typed f64 sends every reader to the float table
    // at that index - a valid Literal_id naming an unrelated value.
    const Literal_id one = types_.table().is_float( target_type ) ? literals_.add_float( 1.0 ) : literals_.add_integer( 1 );

    const Operand right = constant( one, target_type );

    // The target's type again, for the same reason: a statement carries none of its own.
    builder_.assign( target, binary( base_op, left, right, target_type ), ast_.span( id ) );
    drop_statement_temporaries( ast_.span( id ) );
}

void Lowering::lower_if( Node_id id )
{
    const Node_id condition   = ast_.children( id )[0];
    const Node_id then_branch = ast_.children( id )[1];
    const Node_id else_branch = ast_.children( id )[2]; // invalid when absent
    const Span    span        = ast_.span( id );

    // Before the blocks: whatever the condition emits belongs to the block being left.
    const Operand test = lower_expression( condition );

    const Block_id then_block = builder_.add_block();
    const Block_id else_block = builder_.add_block();

    drop_statement_temporaries( span );

    builder_.terminate_branch( test, then_block, else_block, span );

    // Allocated only when an arm actually falls out of the `if`. When both arms return there
    // is nothing to join, and a block created anyway would be unreachable - which verify
    // rejects, correctly. Blocks are indices, so one built by mistake cannot be taken back.
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
    const Node_id condition = ast_.children( id )[0];
    const Node_id body      = ast_.children( id )[1];
    const Span    span      = ast_.span( id );

    // A fresh block, not the one being left: the condition is re-evaluated every iteration, so
    // the back edge targets it - and everything before the loop would re-run if it did not.
    const Block_id header     = builder_.add_block();
    const Block_id body_block = builder_.add_block();

    builder_.terminate_goto( header, span );

    // A while has no update, so continue re-tests immediately.
    loops_.push_back( Loop_targets { Block_id {}, header, scope_depth() } );

    builder_.switch_to( header );

    // terminate_branch acts on the *current* block, which a `&&` in the condition may have
    // moved on to. The back edge still targets the header, so the whole condition re-runs.
    const Operand test = lower_expression( condition );

    drop_statement_temporaries( span );

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
    const Node_id init      = ast_.children( id )[0];
    const Node_id condition = ast_.children( id )[1];
    const Node_id update    = ast_.children( id )[2];
    const Node_id body      = ast_.children( id )[3];
    const Span    span      = ast_.span( id );

    push_scope();

    // The init runs once, in the block being left.
    if( init.is_valid() )
    {
        lower_statement( init );
    }

    const Block_id header     = builder_.add_block();
    const Block_id body_block = builder_.add_block();

    builder_.terminate_goto( header, span );

    // The latch is left invalid: it holds the update and is what continue targets, and a body
    // that always returns reaches neither.
    loops_.push_back( Loop_targets { Block_id {}, Block_id {}, scope_depth() } );

    builder_.switch_to( header );

    drop_statement_temporaries( span );

    if( condition.is_valid() )
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

        if( update.is_valid() )
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

    pop_scope( span );

    return;
}

void Lowering::lower_statement( Node_id id )
{
    // Every statement that builds a temporary drops it before finishing, so by the time the next
    // one starts the list is empty. Cheaper to assert than to find a leak in a golden later.
    assert( statement_temporaries_.empty() && "a statement left an owning temporary undropped" );

    // A table of contents: what a statement can be, and nothing about how any of them lowers. The
    // shape the checker's visit_* already uses, and the reason a construct can grow without the
    // switch growing with it.
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

    case Node_kind::If_stmt:
        return lower_if( id );

    case Node_kind::While_stmt:
        return lower_while( id );

    case Node_kind::For_stmt:
        return lower_for( id );

    // Small enough to read here. Extracting them would cost a name and buy nothing.
    case Node_kind::Expr_stmt:
        // Discard the operand. D15 means the only thing that reaches here is a call.
        lower_expression( ast_.children( id )[0] );
        drop_statement_temporaries( ast_.span( id ) );
        return;

    // One edge each. The checker already rejected either outside a loop, so the asserts in the
    // target helpers document that rather than handle it.
    case Node_kind::Break_stmt:
        unwind_to( loops_.back().depth, ast_.span( id ) );
        builder_.terminate_goto( break_target(), ast_.span( id ) );
        return;

    case Node_kind::Continue_stmt:
        unwind_to( loops_.back().depth, ast_.span( id ) );
        builder_.terminate_goto( continue_target(), ast_.span( id ) );
        return;

    default:
        fmt::print( stderr, "keelc: cannot lower {} as a statement yet\n", node_kind_name( ast_.kind( id ) ) );
        assert( false && "statement kind not lowered yet" );
    }
}

Operand Lowering::lower_argument( Node_id argument, Node_id parameter )
{
    if( !is_borrowed_binding( ast_, types_, parameter ) )
    {
        return lower_expression( argument );
    }

    const Type_id address = binding_type( ast_, types_, parameter );
    const Span    span    = ast_.span( argument );

    // `ref x` is stepped through: the marker says how the argument travels and has no value of its
    // own to produce. A bare borrow has no marker to step through, and a temporary has no place
    // until it is built - so both go through lower_expression and the address is taken of
    // wherever it landed, which is also what drops the temporary afterwards.
    if( ast_.kind( argument ) == Node_kind::Marker_expr )
    {
        return address_operand( lower_place( ast_.children( argument )[0] ), address, span );
    }

    const Operand value = lower_expression( argument );

    assert( value.kind != Operand_kind::Constant && "an owning value is never a constant" );

    return address_operand( value.place, address, span );
}

Operand Lowering::address_operand( Place place, Type_id type, Span span )
{
    return copy( builder_.place( builder_.into_temp( address_of( place, type ), type, span ) ), type );
}

} // namespace

std::vector<Function>
lower( const Ast& ast, const Resolution& resolution, const Types& types, Literals& literals, const Interner& interner )
{
    std::vector<Function> functions;

    for( Node_id id { 0 }; id.v < ast.node_count(); ++id.v )
    {
        if( is_function_like( ast.kind( id ) ) )
        {
            Lowering lowering( id, ast, resolution, types, literals, interner );
            functions.push_back( lowering.run() );
        }
    }

    return functions;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <catch2/catch_test_macros.hpp>

#include "common/source_manager.h"
#include "ir/print.h"
#include "ir/verify.h"
#include "lex/lexer.h"
#include "parse/parser.h"

namespace keel
{
namespace
{

// The whole front end, so what is lowered is a genuinely typed AST rather than one assembled by
// hand. A lowering bug that only appears on real input is the kind worth catching.
struct Lowered
{
    Source_manager sm;
    Interner       interner;
    Literals       literals;
    Diagnostics    diags;
    Ast            ast;
    Resolution     resolution;
    Types          types;

    std::vector<Function> functions;

    explicit Lowered( std::string_view source )
    {
        const File_id file = sm.add_file( "t.kl", std::string( source ) );

        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
        resolution = resolve( ast, sm, interner, diags );
        types      = type_check( ast, resolution, literals, sm, interner, diags );

        if( !diags.has_errors() )
        {
            functions = lower( ast, resolution, types, literals, interner );
        }
    }

    bool clean() const
    {
        return !diags.has_errors();
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags.render( sm, out );
        return out.str();
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

// Everything the builder produces has to satisfy verify. This is the assertion that matters most
// while the lowerer grows: a shape it cannot express shows up here rather than three passes later.
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

// A void function's body simply ends. Without a terminator the function would not verify, and
// nothing checks that a non-void function returns on every path yet - that needs the CFG being
// built here.
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

// Each literal kind reaches KIR as a constant. Two of them carry no Literal_id in aux and have to
// make one: a bool's value lives in aux directly, and `nullptr` records nothing at all - so
// Literal_id { aux } would be the invalid sentinel, which verify rejects.
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

// Three-address form: every intermediate result becomes a named local, in the order the operands
// were evaluated. That order is §7.1's guarantee, and here it is structural rather than a rule -
// the lowering calls happen left to right, so the statements do too.
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

// §6.4 says an operation happens at the common type of its operands. Making that explicit here is
// what lets a backend read KIR without re-deriving the table.
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
        REQUIRE( text.find( "_3 = copy _1 << copy _2" ) != std::string::npos );
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

// Not operators over two operands: the right side must not run unless the left demands it.
// Lowering them as ordinary binaries evaluates both, which is invisible for parameters and wrong
// for anything with an effect - `p != nullptr && (*p).x > 0` would dereference null.
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

    // A void call still assigns, to a local nothing reads. Keeping Assign total is worth more than
    // avoiding it - an optional destination would mean every consumer handling a statement that
    // writes nowhere. A backend emits the call and drops the assignment.
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

// The latch is why a for is not just a while: continue has to run the update before re-testing.
// In the C emitter this needed a generated label, a used-flag, and care about -Wunused-label. Here
// it is an edge to a block.
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
        Lowered p( "i32* f() { i32 x = 1; return &x; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::string text = p.text( 0 );

        INFO( text );
        REQUIRE( text.find( "_2 = &_1" ) != std::string::npos );
        REQUIRE( verify( p.functions[0] ).empty() );
    }

    SECTION( "of a field" )
    {
        Lowered p( "struct P { i32 x; };\ni32* f( P p ) { return &p.x; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "_2 = &_1.x" ) != std::string::npos );
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

// A struct literal is a temporary plus one assignment per field. There is no aggregate rvalue -
// dropping Struct_init from the enum is what makes source order fall out of statement order,
// where the C emitter needed two phases because it was building a single expression.
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

// The whole corpus, through the lowerer and the verifier. This is the assertion that says the IR
// can express the language rather than just the examples chosen to test it.
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

// A file-scope variable has no slot in the frame - it outlives every one - so a place is rooted in
// either a local or a global. Everything above the root is unchanged: the same projections, the
// same copies, the same assignments.
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

    // The root travels with the projections. field() and deref() build their result from the base,
    // so a place that started global stays global - dropping that is what makes `counter.x` a
    // place rooted in nothing.
    SECTION( "taking its address" )
    {
        Lowered p( "i32 counter = 1;\ni32* f() { return &counter; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.text( 0 ).find( "&counter" ) != std::string::npos );
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

// The three helpers below exist because a marker test is about *order*, not about which number a
// local happens to have. Asserting on `_3` breaks the moment a temporary appears earlier in the
// function, which is exactly the kind of edit these tests must survive.

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

// A destructor is a Function like any other: its declaration is a Destructor_decl rather than a
// Function_decl, and everything else - the return slot, the parameter walk, the body at children[2]
// - is the shape run() already handles. Nothing in KIR knows what a destructor is.
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

// A constructor lowers like a destructor - an ordinary Function whose first parameter is the
// receiver - but its *call* is a new shape: it returns nothing and writes through `this`, so an
// initialisation is a call taking the local's address rather than an assignment.
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
        REQUIRE( text.find( "&_1" ) != std::string::npos );
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

// The whole of what `move` costs in KIR: the same place, read with Move instead of Copy. Nothing
// downstream distinguishes them yet - the C backend emits identical text, because a move *is* a
// byte copy there - so this changes the dump and nothing else.
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

// PLAN D32. In KIR a ref binding is a pointer local and every use of the name is a deref - exactly
// the shape the receiver already has. That is what keeps the mode out of every pass below sema:
// verify, the move analysis and drop elaboration all see an ordinary pointer.
TEST_CASE( "lower_binds_a_ref_parameter_through_a_pointer", "[ir][lower][ref]" )
{
    Lowered p( "void bump( ref i32 n ) { n = n + 1; }\ni32 main() { i32 x = 1; bump( ref x ); return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.text( 0 );

    INFO( text );
    REQUIRE( text.find( "let _1: i32*; // parameter n" ) != std::string::npos );

    // Both ends of the assignment go through the binding: reading the name and writing to it are
    // the same place, which is the one thing a mode that is not a type has to get right. The
    // binary lands in a temporary on the way, as every other assignment of one already does.
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
    REQUIRE( text.find( "_2 = &_1" ) != std::string::npos );
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
    REQUIRE( text.find( "= &(*_1)" ) != std::string::npos );
}

// A class travels the same way, and the field reached through the binding is the caller's field.
// Nothing about the parameter is owning - its local is a pointer - so no drop is elaborated for it.
TEST_CASE( "lower_lends_a_class_by_ref", "[ir][lower][ref]" )
{
    Lowered p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
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

// PLAN D31. A bare parameter of an owning type travels by address, exactly as `ref` does: the local
// is a pointer and every use of the name is a deref. KIR shows the two borrows as one shape,
// because the difference between them is entirely what the checker permits through each.
TEST_CASE( "lower_passes_a_bare_owning_parameter_by_address", "[ir][lower][borrow]" )
{
    Lowered p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
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
    Lowered p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "u64 peek( B b ) { return b.n; }\n"
               "i32 main() { B a = B( 1 ); return wrap<i32>( peek( a ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "= &_1" ) != std::string::npos );

    // The two halves that make it a borrow: nothing is moved, and the caller still drops. Before
    // this, the argument was `copy _1` - a struct copy of an owning value, which is the one thing
    // "an owning value is never copied" forbids, and which freed the resource twice.
    REQUIRE( text.find( "move" ) == std::string::npos );
    REQUIRE( text.find( "drop _1" ) != std::string::npos );
}

// A temporary has no place until it is built, so it takes the other path through lower_argument -
// and the address being taken of where it landed is also what leaves it registered for the drop.
TEST_CASE( "lower_drops_a_borrowed_temporary", "[ir][lower][borrow]" )
{
    Lowered p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "u64 peek( B b ) { return b.n; }\n"
               "i32 main() { return wrap<i32>( peek( B( 1 ) ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "= &_1" ) != std::string::npos );
    REQUIRE( text.find( "drop _1" ) != std::string::npos );
}

// Forwarding is `&(*_1)`: the address the binding already holds, taken back out of the deref that
// every use of the name goes through. No copy at either hop.
TEST_CASE( "lower_forwards_a_bare_borrow", "[ir][lower][borrow]" )
{
    Lowered p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "u64 peek( B b ) { return b.n; }\n"
               "u64 forward( B b ) { return peek( b ); }\n"
               "i32 main() { B a = B( 1 ); return wrap<i32>( forward( a ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "forward" );

    INFO( text );
    REQUIRE( text.find( "let _1: B*; // parameter b" ) != std::string::npos );
    REQUIRE( text.find( "= &(*_1)" ) != std::string::npos );
}

// The regression the borrow rule broke once, and the reason it tests the *mode* rather than only
// the type: a `move` parameter is owned, so it stays a value and the callee is what drops it.
TEST_CASE( "lower_keeps_a_move_parameter_by_value", "[ir][lower][borrow]" )
{
    Lowered p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
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

// PLAN D32. A `ref` local is a pointer local holding an address, and every use of the name is a
// deref of it - the same shape a `ref` parameter and the receiver already have. The initialiser is
// lowered as a *place* rather than read, which is the only thing that distinguishes it.
TEST_CASE( "lower_binds_a_ref_local_to_an_address", "[ir][lower][binding]" )
{
    Lowered p( "i32 main() { i32 x = 1; ref i32 r = x; r = 5; return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "let _2: i32*; // r" ) != std::string::npos );
    REQUIRE( text.find( "_2 = &_1" ) != std::string::npos );

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
    REQUIRE( text.find( "= &_1.x" ) != std::string::npos );
}

// A binding owns nothing, so no drop is elaborated for it - the local it points at is dropped
// exactly once, by the scope that declared it.
TEST_CASE( "lower_never_drops_a_ref_binding", "[ir][lower][binding]" )
{
    Lowered p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
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

// PLAN D32. `const ref` travels by address exactly as `ref` does - the const half is a rule the
// checker enforces and nothing below sema knows about, so KIR shows the two as one shape.
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

    // The win this form exists for: a struct that would have been copied is borrowed instead. The
    // claim is about what travels, not which temporary holds it - the struct literal takes an index
    // of its own, so naming one here would pin the wrong thing.
    const std::string caller = p.named( "main" );

    INFO( caller );
    REQUIRE( caller.find( "= &_1" ) != std::string::npos );
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
    REQUIRE( text.find( "_2 = &_1" ) != std::string::npos );
    REQUIRE( text.find( "copy (*_2)" ) != std::string::npos );
}

// A class passed by `const ref` is borrowed, not copied - which for an owning type is the
// difference between one destructor run and two. Nothing is moved and the caller still drops.
TEST_CASE( "lower_borrows_a_class_by_const_ref", "[ir][lower][constref]" )
{
    Lowered p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
               "u64 peek( const ref B b ) { return b.n; }\n"
               "i32 main() { B a = B( 1 ); return wrap<i32>( peek( a ) ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::string text = p.named( "main" );

    INFO( text );
    REQUIRE( text.find( "= &_1" ) != std::string::npos );
    REQUIRE( text.find( "move" ) == std::string::npos );
    REQUIRE( text.find( "drop _1" ) != std::string::npos );
}

// PLAN §8. A `const ref` return travels as an address, like every other binding: the return slot
// holds a pointer, and `return a` hands back the address the parameter already holds rather than a
// read of what is there.
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

    // The address, not a read of it - a read would return a copy of the referent and the form
    // would buy nothing. It comes out as `&(*_1)` rather than `copy _1` because the parameter is
    // itself a binding, so lower_place derefs it and the address is taken straight back: the same
    // round trip forwarding a borrow already prints, and one a C compiler folds for free.
    REQUIRE( callee.find( "_0 = &(*_1)" ) != std::string::npos );
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
    REQUIRE( callee.find( "= &(*_1).x" ) != std::string::npos );
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

// PLAN D31. An `out` parameter travels by address exactly as a borrow does - the local is a pointer
// and every use of the name is a deref. What KIR records on top is which locals arrive empty,
// because nothing in the graph says so: the pointer itself is perfectly valid, and it is the
// referent that has not been written.
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
    REQUIRE( caller.find( "= &_1" ) != std::string::npos );
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

} // namespace keel
#endif
