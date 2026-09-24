#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"

namespace keel
{
void simplify( Function& func, const Literal_pool& literals );
}
