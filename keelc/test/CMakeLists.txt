# keelc's golden-file corpus (§10). Unit tests are NOT here - they live inline in the sources under
# src/, beside the code they exercise.
#
if(KEEL_TESTS)
    add_test(
        NAME golden
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" $<TARGET_FILE:keelc>
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
