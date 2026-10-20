# The kl package's tests: its kl::test suite, its programs, and its goldens.
if(KEEL_TESTS)
    add_test(
        NAME stl_tests
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" $<TARGET_FILE:keelc>
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
