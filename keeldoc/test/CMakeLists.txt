# keeldoc's golden pages: a small package documented, and the pages it must produce.
if(KEEL_TESTS)
    add_test(
        NAME keeldoc_golden
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" $<TARGET_FILE:keeldoc> $<TARGET_FILE:keelc>
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
