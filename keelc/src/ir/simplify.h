#pragma once
#include "common/literals.h"
#include "ir/kir.h"

namespace keel
{
void simplify( Function& func, const Literals& literals );
}