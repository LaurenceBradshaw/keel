# keelc: the compiler itself, C++20.
#
# Includes are path-qualified from src/ - `#include "lex/lexer.h"`, not `#include "lexer.h"` - and
# only src/ goes on the include path. That is what lets this directory move without a single
# include changing: the project root for an include is this project's src/, not the repository's.
add_subdirectory(src)
add_subdirectory(test)
