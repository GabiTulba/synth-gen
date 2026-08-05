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
  src/incremental.cpp
  src/signal.cpp
  src/eval.cpp
  src/wav.cpp
  src/build.cpp
)
target_include_directories(synthgraph_core PUBLIC src)

add_executable(synthc src/main.cpp)
target_link_libraries(synthc PRIVATE synthgraph_core)

# --- Dev app (Epic 7): SDL2 + Dear ImGui artifact browser/player ---------
option(SYNTHGRAPH_BUILD_DEVAPP "Build the synth-dev app (needs SDL2)" ON)
if(SYNTHGRAPH_BUILD_DEVAPP)
  find_package(SDL2 QUIET)
  if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found - skipping synth-dev "
                   "(run scripts/install-deps.sh)")
  else()
    add_library(imgui STATIC
      third_party/imgui/imgui.cpp
      third_party/imgui/imgui_draw.cpp
      third_party/imgui/imgui_tables.cpp
      third_party/imgui/imgui_widgets.cpp
      third_party/imgui/backends/imgui_impl_sdl2.cpp
      third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(imgui PUBLIC
      third_party/imgui third_party/imgui/backends)
    target_link_libraries(imgui PUBLIC SDL2::SDL2)

    add_library(synthdev_core
      src/devapp/json.cpp
      src/devapp/metadata.cpp
      src/devapp/player.cpp
    )
    target_include_directories(synthdev_core PUBLIC src/devapp)
    target_link_libraries(synthdev_core PUBLIC synthgraph_core SDL2::SDL2)

    add_executable(synth-dev src/devapp/app.cpp)
    target_link_libraries(synth-dev PRIVATE synthdev_core imgui)

    add_executable(devapp_tests
      tests/test_main.cpp
      tests/devapp/test_devapp.cpp
    )
    target_link_libraries(devapp_tests PRIVATE synthdev_core)
    target_include_directories(devapp_tests PRIVATE tests)
    add_test(NAME devapp_tests COMMAND devapp_tests)
    add_test(NAME devapp_selftest
             COMMAND synth-dev --self-test
                     ${CMAKE_CURRENT_SOURCE_DIR}/examples/pluck)
  endif()
endif()

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
