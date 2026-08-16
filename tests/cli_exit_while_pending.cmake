file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}")

set(input "${TEST_WORK_DIR}/input.txt")
file(WRITE "${input}" "/t echo\nrequest before eof\n")

execute_process(
    COMMAND "${POSTLINE_BIN}"
        --ui cli
        --journal "${TEST_WORK_DIR}/pending.journal"
    WORKING_DIRECTORY "${TEST_WORK_DIR}"
    INPUT_FILE "${input}"
    RESULT_VARIABLE runtime_result
    OUTPUT_VARIABLE runtime_output
    ERROR_VARIABLE runtime_error
    TIMEOUT 10
)

if(NOT runtime_result EQUAL 0)
    message(FATAL_ERROR
        "CLI did not exit cleanly while a request was pending: ${runtime_result}\n"
        "stdout:\n${runtime_output}\n"
        "stderr:\n${runtime_error}"
    )
endif()

if(runtime_error MATCHES "CHECK failed")
    message(FATAL_ERROR "CLI triggered a CHECK failure: ${runtime_error}")
endif()
