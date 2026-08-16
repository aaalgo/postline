file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}")

set(journal "${TEST_WORK_DIR}/echo.journal")
set(input "${TEST_WORK_DIR}/input.txt")
file(WRITE "${input}" "/t echo\nhello runtime\n")

execute_process(
    COMMAND "${POSTLINE_BIN}" --ui cli --journal "${journal}"
    WORKING_DIRECTORY "${TEST_WORK_DIR}"
    INPUT_FILE "${input}"
    RESULT_VARIABLE runtime_result
    OUTPUT_VARIABLE runtime_output
    ERROR_VARIABLE runtime_error
    TIMEOUT 10
)

if(NOT runtime_result EQUAL 0)
    message(FATAL_ERROR
        "Echo runtime failed with ${runtime_result}\n"
        "stdout:\n${runtime_output}\n"
        "stderr:\n${runtime_error}"
    )
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

if(NOT journal_dump MATCHES "From: user\nTo: echo")
    message(FATAL_ERROR "Journal does not contain the call to echo")
endif()

if(NOT journal_dump MATCHES "From: echo\nTo: user")
    message(FATAL_ERROR "Journal does not contain the response from echo")
endif()

if(NOT journal_dump MATCHES "hello runtime")
    message(FATAL_ERROR "Echo response body is missing")
endif()
