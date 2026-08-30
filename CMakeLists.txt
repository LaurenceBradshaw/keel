cmake_minimum_required(VERSION 3.24)

# C is enabled alongside C++ because keelc's own output is C: runtime/kl_rt.c will be built here,
# and the golden "run" tests compile generated .c files. It also stops CMake warning that the
# CMAKE_C_COMPILER set by the presets is unused.
project(keel VERSION 0.1.0 LANGUAGES CXX C)

option(KEEL_TESTS     "Build the unit test binary"            ON)
option(KEEL_WERROR    "Treat compiler warnings as errors"     OFF)
option(KEEL_SANITIZE  "Build with AddressSanitizer + UBSan"   OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug builds stay at the CMake default (-O0 -g). A compiler spends most of its debugging life
# being single-stepped through the parser and type checker, where -Og's variable elision and
# statement reordering actively get in the way. keelc is not throughput-bound at -O0.

# Binaries go under the build directory, not a shared top-level bin/. Each configure preset has its
# own build/<preset>/bin, so a sanitized or Release build can never overwrite the Debug binaries.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Shared compile/link settings. Linked PUBLIC by every target so that sanitizer flags, which must
# be identical at compile and link time, propagate automatically.
add_library(keel_options INTERFACE)

target_compile_options(keel_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow                # shadowed locals are a real hazard in deeply nested tree walks
    -Wconversion            # silent narrowing corrupts u32 node handles and span offsets
    -Wsign-conversion
    -Wno-unused-parameter   # relaxed: stub visitors and interface methods routinely ignore args
    $<$<BOOL:${KEEL_WERROR}>:-Werror>
)

if(KEEL_SANITIZE)
    target_compile_options(keel_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(keel_options INTERFACE -fsanitize=address,undefined)
endif()

if(KEEL_TESTS)
    include(CTest)
endif()

add_subdirectory(src)
add_subdirectory(tests)
