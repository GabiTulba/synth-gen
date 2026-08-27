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
  src/checker.cpp
  src/incremental.cpp
  src/signal.cpp
  src/vis.cpp
  src/eval.cpp
  src/external.cpp
  src/wav.cpp
  src/library.cpp
  src/build.cpp
  src/json.cpp
  src/lsp.cpp
)
# src/ext holds the public API headers (<synth/external.hpp>,
# <synth/engine.hpp>) that external implementations - the bundled
# stdlib's and user code's alike - compile against at run time; the host
# includes the same files, so there is exactly one definition of the
# boundary types.
target_include_directories(synthgraph_core PUBLIC src src/ext)
# The bundled standard library (stdlib/core, .synth interface plus its
# external .cpp implementations) and the API headers are read at run
# time; the source-tree locations are the defaults, overridable via
# $SYNTH_STDLIB_DIR and $SYNTH_EXT_INCLUDE_DIR.
target_compile_definitions(synthgraph_core
  PUBLIC SYNTH_STDLIB_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/stdlib"
         SYNTH_EXT_INCLUDE_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/src/ext")
# User `external "file.cpp"` implementations are dlopen()ed.
target_link_libraries(synthgraph_core PUBLIC ${CMAKE_DL_LIBS})

add_executable(synthc src/main.cpp)
target_link_libraries(synthc PRIVATE synthgraph_core)
# External implementations resolve the engine's constructors against the
# host executable's dynamic symbol table at dlopen time; every binary
# that evaluates synth programs must export them.
set_target_properties(synthc PROPERTIES ENABLE_EXPORTS ON)

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
      src/devapp/metadata.cpp
      src/devapp/player.cpp
      src/devapp/projectstate.cpp
      src/devapp/waveform.cpp
    )
    target_include_directories(synthdev_core PUBLIC src/devapp)
    target_link_libraries(synthdev_core PUBLIC synthgraph_core SDL2::SDL2)

    add_executable(synth-dev src/devapp/app.cpp)
    target_link_libraries(synth-dev PRIVATE synthdev_core imgui)
    set_target_properties(synth-dev PROPERTIES ENABLE_EXPORTS ON)

    add_executable(devapp_tests
      tests/test_main.cpp
      tests/devapp/test_devapp.cpp
    )
    target_link_libraries(devapp_tests PRIVATE synthdev_core)
    target_include_directories(devapp_tests PRIVATE tests)
    set_target_properties(devapp_tests PROPERTIES ENABLE_EXPORTS ON)
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
  tests/test_lsp.cpp
)
target_link_libraries(synth_tests PRIVATE synthgraph_core)
target_include_directories(synth_tests PRIVATE tests)
set_target_properties(synth_tests PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME synth_tests COMMAND synth_tests)
