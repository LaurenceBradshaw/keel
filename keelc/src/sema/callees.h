#pragma once
#include <unordered_map>
#include "ast/node.h"

namespace keel
{
namespace sema
{

// Which callable each call resolved to. A collaborator rather than a field because Places reads it
// and Overloads writes it, and neither may name the other.
class Callees
{
public:
    Callees() = default;

    void record( Node_id call, Node_id callable )
    {
        callees_.emplace( call.v, callable );
    }

    Node_id callee_of( Node_id call ) const
    {
        const auto found = callees_.find( call.v );

        if( found == callees_.end() )
        {
            return Node_id {};
        }

        return found->second;
    }

    std::unordered_map<u32, Node_id> take()
    {
        return std::move( callees_ );
    }

private:
    std::unordered_map<u32, Node_id> callees_;
};

} // namespace sema
} // namespace keel
