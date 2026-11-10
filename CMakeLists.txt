cmake_minimum_required(VERSION 3.20)
project(micro_exchange VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MICRO_EXCHANGE_BUILD_TESTS "Build tests" ON)
option(MICRO_EXCHANGE_BUILD_BENCH "Build local benchmark" ON)

add_library(micro_exchange STATIC src/matching_engine.cpp src/journal.cpp src/ledger.cpp src/sha256.cpp)
target_include_directories(micro_exchange PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(micro_exchange PRIVATE /W4)
else()
    target_compile_options(micro_exchange PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(exchange_server src/tcp_server.cpp)
target_link_libraries(exchange_server PRIVATE micro_exchange)
if(MSVC)
    target_compile_options(exchange_server PRIVATE /W4)
else()
    target_compile_options(exchange_server PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    target_link_libraries(exchange_server PRIVATE ws2_32)
endif()

add_executable(exchange_cli src/main.cpp)
target_link_libraries(exchange_cli PRIVATE micro_exchange)

if(MICRO_EXCHANGE_BUILD_TESTS)
    enable_testing()
    add_executable(exchange_tests tests/test_engine.cpp)
    target_link_libraries(exchange_tests PRIVATE micro_exchange)
    add_test(NAME exchange_unit COMMAND exchange_tests)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME exchange_tcp_integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tcp.py
                    $<TARGET_FILE:exchange_server>)
        set_tests_properties(exchange_tcp_integration PROPERTIES TIMEOUT 25)
        add_test(NAME exchange_wal_risk_integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wal_risk.py
                    $<TARGET_FILE:exchange_server>)
        set_tests_properties(exchange_wal_risk_integration PROPERTIES TIMEOUT 25)
        add_test(NAME exchange_identity_stp_integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_identity_stp.py
                    $<TARGET_FILE:exchange_server>)
        set_tests_properties(exchange_identity_stp_integration PROPERTIES TIMEOUT 35)
        add_test(NAME exchange_ledger_integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ledger.py
                    $<TARGET_FILE:exchange_server>)
        set_tests_properties(exchange_ledger_integration PROPERTIES TIMEOUT 30)
    endif()
    if(Python3_Interpreter_FOUND)
        # Capture CLI stdout/stderr with diagnostics on Windows and Linux.
        add_test(NAME exchange_replay_golden
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_replay_golden.py
                    $<TARGET_FILE:exchange_cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/demo_orders.txt
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/demo_expected.txt)
    else()
        add_test(NAME exchange_replay_golden
            COMMAND ${CMAKE_COMMAND}
                -DCLI=$<TARGET_FILE:exchange_cli>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/demo_orders.txt
                -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/examples/demo_expected.txt
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_replay.cmake)
    endif()
endif()

if(MICRO_EXCHANGE_BUILD_BENCH)
    add_executable(exchange_bench bench/benchmark.cpp)
    target_link_libraries(exchange_bench PRIVATE micro_exchange)
endif()

# Synthetic, repeatable depth/mixed-operation engine benchmark (v0.6 retained).
if(MICRO_EXCHANGE_BUILD_BENCH)
    add_executable(exchange_depth_bench bench/depth_benchmark.cpp)
    target_link_libraries(exchange_depth_bench PRIVATE micro_exchange)
endif()

if(MICRO_EXCHANGE_BUILD_TESTS)
    if(MICRO_EXCHANGE_BUILD_BENCH)
        add_test(NAME exchange_depth_workload_smoke
                 COMMAND exchange_depth_bench --levels 10,100,1000 --cycles 25 --repetitions 1)
        set_tests_properties(exchange_depth_workload_smoke PROPERTIES TIMEOUT 25)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME exchange_concurrent_replay_stress
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/client/bench_scaling.py
                    --server $<TARGET_FILE:exchange_server> --pairs 1,3 --rounds 10 --mode both --tcp-nodelay)
        set_tests_properties(exchange_concurrent_replay_stress PROPERTIES TIMEOUT 140)
    endif()
endif()

if(MICRO_EXCHANGE_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME exchange_group_commit_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_group_commit.py
                $<TARGET_FILE:exchange_server>)
    set_tests_properties(exchange_group_commit_integration PROPERTIES TIMEOUT 100)
endif()

if(MICRO_EXCHANGE_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME exchange_fault_idempotency_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fault_idempotency.py
                $<TARGET_FILE:exchange_server>)
    set_tests_properties(exchange_fault_idempotency_integration PROPERTIES TIMEOUT 90)
endif()

# v0.9: in-memory, fully synthetic order-event trace generation and CSV replay.
# Separate executable: deliberately does not open sockets, journals, or broker accounts.
add_executable(exchange_flow_sim sim/flow_replay.cpp)
target_link_libraries(exchange_flow_sim PRIVATE micro_exchange)
if(MSVC)
    target_compile_options(exchange_flow_sim PRIVATE /W4)
else()
    target_compile_options(exchange_flow_sim PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MICRO_EXCHANGE_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME exchange_flow_replay_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_flow_replay.py
                $<TARGET_FILE:exchange_flow_sim>)
    set_tests_properties(exchange_flow_replay_integration PROPERTIES TIMEOUT 90)
endif()

if(MICRO_EXCHANGE_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME exchange_flow_tools_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_flow_tools.py)
    set_tests_properties(exchange_flow_tools_integration PROPERTIES TIMEOUT 25)
endif()

# v1.0 synthetic multi-seed and scenario-sensitivity validation.
if(MICRO_EXCHANGE_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME exchange_research_v100_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_research_v100.py
                $<TARGET_FILE:exchange_flow_sim>)
    set_tests_properties(exchange_research_v100_integration PROPERTIES TIMEOUT 120)
endif()
