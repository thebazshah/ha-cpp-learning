cmake_minimum_required(VERSION 3.16)
project(MediaStreamingService VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Compiler settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# All server code goes into one static library, so the server executable and
# the unit tests share it.
# ---------------------------------------------------------------------------
add_library(streaming_core STATIC
  src/core/Config.cpp
  src/core/Json.cpp
  src/core/Logger.cpp
  src/core/Process.cpp
  src/core/StringUtils.cpp
  src/core/ThreadPool.cpp
  src/net/Socket.cpp
  src/http/HttpMessage.cpp
  src/http/HttpServer.cpp
  src/http/Router.cpp
  src/cache/SegmentCache.cpp
  src/media/Aac.cpp
  src/media/BitrateLadder.cpp
  src/media/H264.cpp
  src/media/HlsPlaylist.cpp
  src/media/MediaLibrary.cpp
  src/media/MediaProbe.cpp
  src/media/TranscodeManager.cpp
  src/media/TsDemuxer.cpp
  src/streaming/HlsService.cpp
  src/rtsp/AbrController.cpp
  src/rtsp/Rtcp.cpp
  src/rtsp/RtpPacketizer.cpp
  src/rtsp/RtspMessage.cpp
  src/rtsp/RtspServer.cpp
  src/rtsp/RtspSession.cpp
  src/rtsp/Sdp.cpp
  src/api/ApiController.cpp
)
target_include_directories(streaming_core PUBLIC src)
target_link_libraries(streaming_core PUBLIC Threads::Threads)

# Old GCC versions (before 9.1) keep std::filesystem in a separate library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
  target_link_libraries(streaming_core PUBLIC stdc++fs)
endif()

# ---------------------------------------------------------------------------
# The server
# ---------------------------------------------------------------------------
add_executable(media-streaming-service src/main.cpp)
target_link_libraries(media-streaming-service PRIVATE streaming_core)

# ---------------------------------------------------------------------------
# Unit tests:  cmake --build build && ./build/unit_tests   (or: ctest --test-dir build)
# ---------------------------------------------------------------------------
option(MSS_BUILD_TESTS "Build the unit tests" ON)
if(MSS_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE streaming_core)
  add_test(NAME unit_tests COMMAND unit_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
