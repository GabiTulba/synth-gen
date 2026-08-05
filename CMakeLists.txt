cmake_minimum_required(VERSION 3.20)
project(synthgraph CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

add_library(synthgraph_core
  src/diagnostics.cpp
  src/lexer.cpp
  src/parser.cpp
  src/types.cpp
  src/primitives.cpp
  src/checker.cpp
  src/signal.cpp
  src/eval.cpp
  src/wav.cpp
  src/build.cpp
)
target_include_directories(synthgraph_core PUBLIC src)

add_executable(synthc src/main.cpp)
target_link_libraries(synthc PRIVATE synthgraph_core)

enable_testing()
add_executable(synth_tests
  tests/test_main.cpp
  tests/test_lexer.cpp
  tests/test_parser.cpp
  tests/test_checker.cpp
  tests/test_engine.cpp
  tests/test_build.cpp
)
target_link_libraries(synth_tests PRIVATE synthgraph_core)
target_include_directories(synth_tests PRIVATE tests)
add_test(NAME synth_tests COMMAND synth_tests)
