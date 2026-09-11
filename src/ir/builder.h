#pragma once
#include <span>
#include <vector>
#include "ast/node.h"
#include "common/span.h"
#include "common/types.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{

class Builder
{
public:
    // Create block 0 and local 0, so those conventions are established in
    // exactly one place instead of at every call site.
    Builder( Node_id declaration, Type_id return_type, Span span );

    Local_id add_parameter( Type_id type, Span span, Symbol_id name );

    // An `out` parameter's local holds a valid address; it is the *referent* that starts empty, and
    // nothing else in KIR says so. Recorded here because the lowerer is the only thing that knows.
    void     mark_out_parameter( Local_id local );
    Local_id add_local( Type_id type, Span span, Symbol_id name = {} );

    Block_id add_block();
    void     switch_to( Block_id block );
    Block_id current() const;

    // Places. field()/deref() build on a base rather than mutating it.
    Place place( Local_id id ) const;
    Place global( Node_id decl ) const;
    Place field( Place base, Node_id field_decl );
    Place deref( Place base );

    // D7: the discriminant. A payload enum is a struct in C, and this is the field that says which
    // variant is live - the only part of one a `switch` reads without a pattern.
    Place tag( Place base );

    Type_id type_of( Local_id id ) const;

private:
    // The shared half of field() and deref(): copy the base's projections to the end of the table,
    // append one, and return a place over the copy.
    Place projected( Place base, Projection projection );

public:
    void assign( Place target, Rvalue value, Span span );
    void drop( Place place, Span span );
    void storage_live( Local_id local, Span span );
    void storage_dead( Local_id local, Span span );

    // The workhorse of expression lowering: make a temporary, assign into it, hand it back.
    Local_id into_temp( Rvalue value, Type_id type, Span span );

    void terminate_goto( Block_id target, Span span );
    void terminate_branch( Operand condition, Block_id true_target, Block_id false_target, Span span );
    void terminate_return( Span span );

    bool is_terminated() const;

    u32 add_operands( std::span<const Operand> operands ); // call arguments, return first_argument

    Function finish();

private:
    // Every statement emitter funnels through here, so `assign`, `drop` and the two storage
    // markers are two lines each.
    void push_statement( Statement statement );

    // Asserts the current block is still Unset. A block terminated twice is a lowering bug, and
    // catching it at the call site beats reading it out of a verify message later.
    void set_terminator( Terminator terminator );

    // Everything except statements is append-only and needs no staging: locals, blocks,
    // projections and operands are all indexed absolutely.
    Function function_;

    // Statements, staged per block. The flat form's (first, count) ranges require a block's
    // statements to be contiguous, so they cannot be appended as they are emitted - switching
    // blocks would interleave them. finish() concatenates these in block order and fills the
    // ranges in. Indexed by Block_id, so add_block() pushes here too.
    std::vector<std::vector<Statement>> pending_;

    Block_id current_;

    bool finished_ = false;
};

} // namespace keel