cmake_minimum_required(VERSION 3.16)

project(embedded_flight_recorder VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FLIGHT_RECORDER_BUILD_TESTS "Build unit tests" ON)

add_library(flight_recorder_lib
    src/binary_log_writer.cpp
    src/flight_recorder.cpp
    src/recovery_manager.cpp
    src/sensor_simulator.cpp
)

target_include_directories(flight_recorder_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(flight_recorder_lib
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

find_package(Threads REQUIRED)
target_link_libraries(flight_recorder_lib PUBLIC Threads::Threads)

add_executable(flight_recorder src/main.cpp)
target_link_libraries(flight_recorder PRIVATE flight_recorder_lib)

add_executable(replay_tool tools/replay_tool.cpp)
target_link_libraries(replay_tool PRIVATE flight_recorder_lib)

add_executable(recovery_tool tools/recovery_tool.cpp)
target_link_libraries(recovery_tool PRIVATE flight_recorder_lib)

if(FLIGHT_RECORDER_BUILD_TESTS)
    enable_testing()

    add_executable(flight_recorder_tests
        tests/flight_recorder_tests.cpp
    )
    target_link_libraries(flight_recorder_tests PRIVATE flight_recorder_lib)

    add_test(NAME flight_recorder_tests COMMAND flight_recorder_tests)
endif()
