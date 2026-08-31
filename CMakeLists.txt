cmake_minimum_required(VERSION 3.16)

project(embedded_flight_recorder VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FLIGHT_RECORDER_BUILD_TESTS "Build unit tests" ON)
option(FLIGHT_RECORDER_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(FLIGHT_RECORDER_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(FLIGHT_RECORDER_ENABLE_ASAN OR FLIGHT_RECORDER_ENABLE_UBSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        set(FLIGHT_RECORDER_SANITIZERS "")
        if(FLIGHT_RECORDER_ENABLE_ASAN)
            list(APPEND FLIGHT_RECORDER_SANITIZERS address)
        endif()
        if(FLIGHT_RECORDER_ENABLE_UBSAN)
            list(APPEND FLIGHT_RECORDER_SANITIZERS undefined)
        endif()
        list(JOIN FLIGHT_RECORDER_SANITIZERS "," FLIGHT_RECORDER_SANITIZER_FLAGS)
        add_compile_options(-fsanitize=${FLIGHT_RECORDER_SANITIZER_FLAGS} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${FLIGHT_RECORDER_SANITIZER_FLAGS} -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR "Sanitizer builds require Clang or GCC")
    endif()
endif()

add_library(flight_recorder_lib
    src/binary_log_writer.cpp
    src/fault_injection.cpp
    src/flight_recorder.cpp
    src/platform_info.cpp
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

target_compile_definitions(flight_recorder_lib
    PRIVATE
        FLIGHT_RECORDER_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        FLIGHT_RECORDER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

add_executable(flight_recorder src/main.cpp)
target_link_libraries(flight_recorder PRIVATE flight_recorder_lib)

add_executable(replay_tool tools/replay_tool.cpp)
target_link_libraries(replay_tool PRIVATE flight_recorder_lib)

add_executable(recovery_tool tools/recovery_tool.cpp)
target_link_libraries(recovery_tool PRIVATE flight_recorder_lib)

add_executable(fault_injector tools/fault_injector.cpp)
target_link_libraries(fault_injector PRIVATE flight_recorder_lib)

add_executable(platform_info tools/platform_info.cpp)
target_link_libraries(platform_info PRIVATE flight_recorder_lib)

add_executable(queue_benchmark tools/queue_benchmark.cpp)
target_link_libraries(queue_benchmark PRIVATE flight_recorder_lib)

add_executable(crash_matrix tools/crash_matrix.cpp)
target_link_libraries(crash_matrix PRIVATE flight_recorder_lib)

add_executable(recorder_benchmark tools/recorder_benchmark.cpp)
target_link_libraries(recorder_benchmark PRIVATE flight_recorder_lib)

if(FLIGHT_RECORDER_BUILD_TESTS)
    enable_testing()

    add_executable(flight_recorder_tests
        tests/flight_recorder_tests.cpp
    )
    target_link_libraries(flight_recorder_tests PRIVATE flight_recorder_lib)

    add_test(NAME flight_recorder_tests COMMAND flight_recorder_tests)
endif()
