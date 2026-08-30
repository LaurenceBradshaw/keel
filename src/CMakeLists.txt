# Sources are globbed recursively from the subdirectory layout described in docs/PLAN.md §11
# (common/ lex/ parse/ ast/ sema/ ir/ check/ codegen_c/). CONFIGURE_DEPENDS makes Ninja re-run
# CMake when a file appears or disappears, so adding a source needs no edit here.
#
# Includes are path-qualified from src/ - `#include "lex/lexer.h"`, not `#include "lexer.h"`.
# Only src/ goes on the include path. This matters more here than in most projects: a compiler
# naturally grows several same-named files (ast/node.h, ir/node.h; sema/scope.h, ir/scope.h),
# and a flat include path would make those ambiguous.
file(GLOB_RECURSE keel_sources CONFIGURE_DEPENDS "*.cpp")

find_package(fmt CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)

add_executable(keelc ${keel_sources})
target_include_directories(keelc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(keelc PRIVATE keel_options fmt::fmt cxxopts::cxxopts)

# Unit tests live at the bottom of the .cpp file they exercise, inside `#ifdef ENABLE_UNIT_TESTS`.
# The test binary is therefore the same source set compiled a second time with that define, rather
# than a shared library both binaries link: the two compilations genuinely differ.
#
# The cost is that every source is compiled twice. The benefit is that a test sits next to the code
# it tests and can reach file-local statics and anonymous-namespace helpers directly - which for a
# compiler means the lexer's internal classification tables, the parser's recovery predicates, and
# the dataflow lattice joins are all directly testable without widening any public header.
#
# main() is excluded from this binary by the same define; Catch2WithMain supplies its own.
if(KEEL_TESTS)
    find_package(Catch2 CONFIG REQUIRED)

    add_executable(keel_tests ${keel_sources})
    target_compile_definitions(keel_tests PRIVATE ENABLE_UNIT_TESTS)
    target_include_directories(keel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(keel_tests PRIVATE keel_options fmt::fmt cxxopts::cxxopts Catch2::Catch2WithMain)

    add_test(NAME unit COMMAND keel_tests)
endif()
