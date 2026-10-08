cmake_minimum_required(VERSION 3.16)
project(CloudGamingEngine VERSION 1.0.0 LANGUAGES CXX)

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
# The engine as a static library, shared by the server, the bot client and
# the unit tests.
# ---------------------------------------------------------------------------
add_library(engine_core STATIC
  src/core/Config.cpp
  src/core/Json.cpp
  src/core/Logger.cpp
  src/core/Process.cpp
  src/core/Sha1.cpp
  src/core/StringUtils.cpp
  src/core/ThreadPool.cpp
  src/net/Socket.cpp
  src/http/HttpMessage.cpp
  src/http/HttpServer.cpp
  src/http/Router.cpp
  src/ws/WebSocket.cpp
  src/codec/FrameCodec.cpp
  src/protocol/Protocol.cpp
  src/engine/GameLibrary.cpp
  src/engine/GameRegistry.cpp
  src/engine/GameSession.cpp
  src/engine/SessionManager.cpp
  src/engine/Viewer.cpp
  src/api/ApiController.cpp
)
target_include_directories(engine_core PUBLIC src sdk/include)
target_link_libraries(engine_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
  target_link_libraries(engine_core PUBLIC stdc++fs)
endif()

# The server.
add_executable(cloud-gaming-engine src/main.cpp)
target_link_libraries(cloud-gaming-engine PRIVATE engine_core)

# A command-line client that joins a session over the same WebSocket protocol
# (handy for load tests and for learning how the protocol works).
add_executable(cge-bot tools/bot_client.cpp)
target_link_libraries(cge-bot PRIVATE engine_core)

# Unit tests:  ./build/unit_tests   (or: ctest --test-dir build)
option(CGE_BUILD_TESTS "Build the unit tests" ON)
if(CGE_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE engine_core)
  add_test(NAME unit_tests COMMAND unit_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
