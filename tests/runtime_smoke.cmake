file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}")

set(journal "${TEST_WORK_DIR}/runtime.journal")

execute_process(
    COMMAND "${POSTLINE_BIN}" --ui null --journal "${journal}"
    WORKING_DIRECTORY "${TEST_WORK_DIR}"
    RESULT_VARIABLE runtime_result
    OUTPUT_VARIABLE runtime_output
    ERROR_VARIABLE runtime_error
    TIMEOUT 10
)

if(NOT runtime_result EQUAL 0)
    message(FATAL_ERROR
        "Runtime smoke test failed with ${runtime_result}\n"
        "stdout:\n${runtime_output}\n"
        "stderr:\n${runtime_error}"
    )
endif()

if(NOT EXISTS "${journal}")
    message(FATAL_ERROR "Runtime did not create ${journal}")
endif()

file(SIZE "${journal}" journal_size)
if(journal_size EQUAL 0)
    message(FATAL_ERROR "Runtime created an empty journal")
endif()

execute_process(
    COMMAND "${DUMP_JOURNAL_BIN}" "${journal}"
    RESULT_VARIABLE dump_result
    OUTPUT_VARIABLE journal_dump
    ERROR_VARIABLE dump_error
)

if(NOT dump_result EQUAL 0)
    message(FATAL_ERROR "dump_journal failed: ${dump_error}")
endif()

if(NOT journal_dump MATCHES "Subject: create_agents")
    message(FATAL_ERROR "Journal does not contain arena initialization")
endif()

if(NOT journal_dump MATCHES "begin_shutdown")
    message(FATAL_ERROR "Journal does not contain clean shutdown markers")
endif()
