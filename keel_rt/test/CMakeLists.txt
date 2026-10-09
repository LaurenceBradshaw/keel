# The runtime's edge cases, as a C binary linking keel_rt as a program does.
if(NOT KEEL_TESTS OR NOT TARGET keel_rt)
    return()
endif()

add_executable(keel_rt_tests kl_rt_tests.c)
set_target_properties(keel_rt_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(keel_rt_tests PRIVATE keel_rt keel_options)

add_test(NAME rt_unit COMMAND keel_rt_tests)
