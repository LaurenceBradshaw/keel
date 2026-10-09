# The kl package's golden tests: programs using the package, run by keelc's golden runner.
if(KEEL_TESTS)
    add_test(
        NAME stl_golden
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" $<TARGET_FILE:keelc>
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
