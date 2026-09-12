# keel_rt: the C floor every Keel program links against (docs/PLAN.md §7).
#
# Built here so that a change to it is caught by the same build as a change to keelc - it is data
# the compiler ships rather than part of the compiler, and one repository is what makes that cheap.
#
# The golden runner deliberately does NOT link this target. It compiles kl_rt.c directly beside the
# emitted .c, with the same $CC and flags a real Keel program would use, so the suite needs to know
# nothing about build directories, presets, or configurations. This library exists to keep the
# runtime under the project's warning settings and to give keel_rt's own tests something to link.
file(GLOB keel_rt_sources CONFIGURE_DEPENDS "*.c")

# Still a shell. Returning rather than failing is what lets keelc build before the runtime exists,
# and is the same conditional shape keel_rt/test and keel_stl already use.
if(NOT keel_rt_sources)
    return()
endif()

add_library(keel_rt STATIC ${keel_rt_sources})

# C11 with extensions off, matching both the standard keelc's output is compiled with (§7.8) and
# the golden runner's default flags. Pinned rather than inherited: §2.2 forbids the *output* from
# depending on one compiler's extensions, and the runtime it links against is held to the same rule.
set_target_properties(keel_rt PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

# PUBLIC: kl_rt.h is this project's interface, and keel_rt/test includes it the same way a consumer
# would. Includes are path-qualified from src/, as in keelc - `#include "kl_rt.h"` from within the
# project, and nothing names the directory it lives in.
target_include_directories(keel_rt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(keel_rt PRIVATE keel_options)
