cmake_minimum_required(VERSION 3.22)

project(
  outer_memory_graph
  VERSION 0.1.0
  DESCRIPTION "Destination-sharded external-memory PageRank"
  LANGUAGES CXX
)

option(OMG_BUILD_TESTS "Build the test suite" ON)
option(OMG_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(omg_core
  src/input.cpp
  src/manifest.cpp
  src/pagerank.cpp
  src/preprocess.cpp
)

target_include_directories(omg_core PUBLIC include)
target_link_libraries(omg_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json)

target_compile_options(omg_core PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wshadow
)

if(OMG_ENABLE_SANITIZERS)
  target_compile_options(omg_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(omg_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(omg src/main.cpp)
target_link_libraries(omg PRIVATE omg_core)
target_compile_options(omg PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

include(CTest)
if(BUILD_TESTING AND OMG_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)

  add_executable(omg_tests
    tests/input_test.cpp
    tests/manifest_test.cpp
    tests/pagerank_test.cpp
    tests/preprocess_test.cpp
  )
  target_link_libraries(omg_tests PRIVATE omg_core Catch2::Catch2WithMain)
  target_compile_definitions(omg_tests PRIVATE
    OMG_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
  )
  target_compile_options(omg_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

  include(Catch)
  catch_discover_tests(omg_tests)
endif()
