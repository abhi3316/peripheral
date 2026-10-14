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
    src/socket.cc
    src/stream_capture.cc
    src/stream_buffer.cc
    src/video_framer.cc
    src/control_framer.cc
    src/camera_session.cc
    src/web_server.cc
    include/peripheral/socket.hpp
    include/peripheral/socket_types.hpp
    include/peripheral/camera_endpoint.hpp
    include/peripheral/stream_capture.hpp
    include/peripheral/stream_buffer.hpp
    include/peripheral/crc32.hpp
    include/peripheral/protocol.hpp
    include/peripheral/video_frame.hpp
    include/peripheral/video_framer.hpp
    include/peripheral/control_framer.hpp
    include/peripheral/camera_session.hpp
    include/peripheral/web_server.hpp
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

# Python Functional Test with Virtual Camera Simulator
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    add_test(NAME FunctionalCameraTest COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_functional_camera.py)
endif()
