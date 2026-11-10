# keelb, the build tool, written in Keel. It is built by one keelc command from source, so the tool
# never builds itself, and it lands beside keelc, where it looks for keelc first.

file(GLOB keelb_inputs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.kl"
    "${CMAKE_SOURCE_DIR}/keel_stl/src/*.kl"
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/bin/keelb${CMAKE_EXECUTABLE_SUFFIX}
    COMMAND $<TARGET_FILE:keelc> ${CMAKE_CURRENT_SOURCE_DIR}/src/main.kl -o ${CMAKE_BINARY_DIR}/bin/keelb${CMAKE_EXECUTABLE_SUFFIX}
    DEPENDS keelc ${keelb_inputs}
    COMMENT "Building keelb"
)
add_custom_target(keelb ALL DEPENDS ${CMAKE_BINARY_DIR}/bin/keelb${CMAKE_EXECUTABLE_SUFFIX})

if(KEEL_TESTS)
    add_test(
        NAME keel_build_tests
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test/run_tests.sh" ${CMAKE_BINARY_DIR}/bin/keelb${CMAKE_EXECUTABLE_SUFFIX}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
