# CMake generated Testfile for 
# Source directory: /Users/achu/Jobs/peripheral
# Build directory: /Users/achu/Jobs/peripheral/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[SocketTest]=] "/Users/achu/Jobs/peripheral/build/test_socket")
set_tests_properties([=[SocketTest]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/achu/Jobs/peripheral/CMakeLists.txt;87;add_test;/Users/achu/Jobs/peripheral/CMakeLists.txt;0;")
add_test([=[StreamCaptureTest]=] "/Users/achu/Jobs/peripheral/build/test_stream_capture")
set_tests_properties([=[StreamCaptureTest]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/achu/Jobs/peripheral/CMakeLists.txt;97;add_test;/Users/achu/Jobs/peripheral/CMakeLists.txt;0;")
add_test([=[StreamBufferTest]=] "/Users/achu/Jobs/peripheral/build/test_stream_buffer")
set_tests_properties([=[StreamBufferTest]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/achu/Jobs/peripheral/CMakeLists.txt;107;add_test;/Users/achu/Jobs/peripheral/CMakeLists.txt;0;")
add_test([=[CameraSessionTest]=] "/Users/achu/Jobs/peripheral/build/test_camera_session")
set_tests_properties([=[CameraSessionTest]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/achu/Jobs/peripheral/CMakeLists.txt;117;add_test;/Users/achu/Jobs/peripheral/CMakeLists.txt;0;")
add_test([=[FramersTest]=] "/Users/achu/Jobs/peripheral/build/test_framers")
set_tests_properties([=[FramersTest]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/achu/Jobs/peripheral/CMakeLists.txt;127;add_test;/Users/achu/Jobs/peripheral/CMakeLists.txt;0;")
add_test([=[FunctionalCameraTest]=] "/opt/homebrew/Frameworks/Python.framework/Versions/3.13/bin/python3.13" "/Users/achu/Jobs/peripheral/tests/test_functional_camera.py")
set_tests_properties([=[FunctionalCameraTest]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/achu/Jobs/peripheral/CMakeLists.txt;132;add_test;/Users/achu/Jobs/peripheral/CMakeLists.txt;0;")
