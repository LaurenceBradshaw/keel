#pragma once
#include <string>
#include <vector>
#include "ir/kir.h"

namespace keel
{

std::vector<std::string> verify( const Function& func );

} // namespace keel