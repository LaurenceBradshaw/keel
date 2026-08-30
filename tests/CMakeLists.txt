# This directory holds the golden-file corpus (lex/ parse/ typecheck/ errors/ run/) described in
# docs/PLAN.md §10. Unit tests are NOT here - they live inline in the sources under src/.
#
# The shell runner does not exist yet, so the suite registers itself conditionally and appears the
# moment run_tests.sh is written.
if(KEEL_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh")
    add_test(
        NAME golden
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" $<TARGET_FILE:keelc>
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
