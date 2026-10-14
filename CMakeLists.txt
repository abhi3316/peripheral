cmake_minimum_required(VERSION 3.25)
project(peripheral LANGUAGES CXX)

# Language standard: Target C++26, falling back to C++23 where CMake/compiler dialect flags require
if (NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++26" HAS_STD_CPP26)
if (HAS_STD_CPP26)
    add_compile_options(-std=c++26)
endif()

# Compiler warnings and performance flags
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -O2
    )
endif()

find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# peripheral_socket Library (Modular socket support for camera streams)
# -----------------------------------------------------------------------------
add_library(peripheral_socket
    src/transport/socket.cc
    src/transport/stream_capture.cc
    src/memory/stream_buffer.cc
    src/memory/frame_pool.cc
    src/protocol/video_framer.cc
    src/protocol/control_framer.cc
    src/camera/camera_session.cc
    src/web/web_server.cc
)

target_include_directories(peripheral_socket PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(peripheral_socket PUBLIC
    Threads::Threads
)

# -----------------------------------------------------------------------------
# Main Application Server Executable
# -----------------------------------------------------------------------------
add_executable(peripheral_server
    src/main.cc
)

target_link_libraries(peripheral_server PRIVATE
    peripheral_socket
)

# -----------------------------------------------------------------------------
# Unit / Integration Tests
# -----------------------------------------------------------------------------
enable_testing()

add_executable(test_socket
    tests/test_socket.cc
)

target_link_libraries(test_socket PRIVATE
    peripheral_socket
)

add_test(NAME SocketTest COMMAND test_socket)

add_executable(test_stream_capture
    tests/test_stream_capture.cc
)

target_link_libraries(test_stream_capture PRIVATE
    peripheral_socket
)

add_test(NAME StreamCaptureTest COMMAND test_stream_capture)

add_executable(test_stream_buffer
    tests/test_stream_buffer.cc
)

target_link_libraries(test_stream_buffer PRIVATE
    peripheral_socket
)

add_test(NAME StreamBufferTest COMMAND test_stream_buffer)

add_executable(test_camera_session
    tests/test_camera_session.cc
)

target_link_libraries(test_camera_session PRIVATE
    peripheral_socket
)

add_test(NAME CameraSessionTest COMMAND test_camera_session)

add_executable(test_framers
    tests/test_framers.cc
)

target_link_libraries(test_framers PRIVATE
    peripheral_socket
)

add_test(NAME FramersTest COMMAND test_framers)

add_executable(test_frame_pool
    tests/test_frame_pool.cc
)

target_link_libraries(test_frame_pool PRIVATE
    peripheral_socket
)

add_test(NAME FramePoolTest COMMAND test_frame_pool)

# Python Functional Test with Virtual Camera Simulator
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    add_test(NAME FunctionalCameraTest COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_functional_camera.py)
endif()
