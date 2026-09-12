# keelc's golden-file corpus (§10). Unit tests are NOT here - they live inline in the sources under
# src/, beside the code they exercise.
#
# Registered conditionally so the suite appears the moment run_tests.sh exists, which is also what
# lets keel_rt and keel_stl carry the same file before they have anything to run.
if(KEEL_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh")
    add_test(
        NAME golden
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" $<TARGET_FILE:keelc>
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
