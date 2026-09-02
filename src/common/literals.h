#pragma once
#include <vector>
#include "common/types.h"

namespace keel
{

// Handle into Literals, the same pattern as Symbol_id and Type_id. Slot 0 is reserved so a
// default-constructed handle means "no value".
struct Literal_id
{
    u32 v = 0;

    bool is_valid() const
    {
        return v != 0;
    }

    friend bool operator==( Literal_id lhs, Literal_id rhs ) = default;
};

// The values the lexer scanned, kept beside the token stream rather than inside Token so that a
// token stays small. Threaded like Interner: made once in the driver, filled by lex(), read by
// sema and codegen.
//
// Integers are stored as an unsigned magnitude. Sign is a separate Unary_expr node, so the pool
// never sees a negative number - which is why Type_table::fits() takes `negative` separately.
class Literals
{
public:
    Literals();

    Literal_id add_integer( u64 magnitude );
    Literal_id add_float( f64 value );

    // The node kind chooses: Int_literal reads integer(), Float_literal reads floating().
    u64 integer( Literal_id id ) const;
    f64 floating( Literal_id id ) const;

private:
    std::vector<u64> integers_; // [0] reserved so Literal_id {} is invalid
    std::vector<f64> floats_;   // likewise
};

} // namespace keel
