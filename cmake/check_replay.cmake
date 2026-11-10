if(NOT DEFINED CLI OR NOT DEFINED INPUT OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "CLI, INPUT and EXPECTED required")
endif()
execute_process(
    COMMAND "${CLI}" --script "${INPUT}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE observed
    ERROR_VARIABLE errors
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "CLI exited with ${status}: ${errors}")
endif()
file(READ "${EXPECTED}" expected)
string(REPLACE "\r\n" "\n" observed "${observed}")
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT observed STREQUAL expected)
    message(FATAL_ERROR "Replay output differs from the committed golden transcript.\nACTUAL:\n${observed}\nEXPECTED:\n${expected}")
endif()
message(STATUS "Deterministic replay matches golden transcript")
