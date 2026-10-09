# Sources are globbed, as keelc's are, so adding one needs no edit here.
file(GLOB_RECURSE keeldoc_sources CONFIGURE_DEPENDS "*.cpp")

# The primitives' docs are compiled in as a string, as keelc's prelude is, so keeldoc reads no file to
# find them. Editing primitives.md re-runs CMake, which regenerates the string.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS primitives.md)
file(READ primitives.md KEELDOC_PRIMITIVES)
configure_file(primitives.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/primitives_source.cpp @ONLY)
list(APPEND keeldoc_sources ${CMAKE_CURRENT_BINARY_DIR}/primitives_source.cpp)

find_package(fmt CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)

add_executable(keeldoc ${keeldoc_sources})
target_include_directories(keeldoc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(keeldoc PRIVATE keel_options fmt::fmt cxxopts::cxxopts)

# Unit tests sit at the bottom of the file they test, under ENABLE_UNIT_TESTS, as in keelc.
if(KEEL_TESTS)
    find_package(Catch2 CONFIG REQUIRED)

    add_executable(keeldoc_tests ${keeldoc_sources})
    target_compile_definitions(keeldoc_tests PRIVATE ENABLE_UNIT_TESTS)
    target_include_directories(keeldoc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(keeldoc_tests PRIVATE keel_options fmt::fmt cxxopts::cxxopts Catch2::Catch2WithMain)

    add_test(NAME keeldoc_unit COMMAND keeldoc_tests)
endif()
