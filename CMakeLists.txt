cmake_minimum_required(VERSION 3.20)
project(turmeric LANGUAGES C ASM)

# ---------------------------------------------------------------------------
# CPM: package manager for fetching Raylib (and any future C dependencies).
# ---------------------------------------------------------------------------
include(cmake/CPM.cmake)

# ---------------------------------------------------------------------------
# Turmeric compiler (native CMake build)
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# Tests (CTest)
# ---------------------------------------------------------------------------
enable_testing()

add_test(
  NAME tur_tests
  COMMAND bash tests/run.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_cli_tests
  COMMAND bash tests/run-cli.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_span_tests
  COMMAND bash tests/check-span-unknown.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_flags_tests
  COMMAND bash tests/run-flags.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Phase S0: tur repl / libturi smoke tests
add_test(
  NAME tur_repl_tests
  COMMAND bash tests/turi/eval-test.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Phase S1: REPL smoke tests (multi-line, :type, :doc, :reload, history)
add_test(
  NAME tur_repl_smoke
  COMMAND bash tests/turi/repl-smoke.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Phase S2: (import turi/eval) from Turmeric source
add_test(
  NAME tur_eval_import
  COMMAND bash tests/turi/eval-import.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Set TUR_CC_FLAGS so the generated C can find turi/eval.h and libturi.a.
# In Debug builds libturi.a is ASAN-instrumented so we must also pass
# -fsanitize=address,undefined to avoid undefined ASAN symbol errors.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_eval_import_asan " -fsanitize=address,undefined")
else()
  set(_eval_import_asan "")
endif()
set_tests_properties(tur_eval_import PROPERTIES
  ENVIRONMENT "TUR_CC_FLAGS=-O2 -std=c99 -Wall${_eval_import_asan} -I${CMAKE_CURRENT_SOURCE_DIR}/src -L${CMAKE_CURRENT_BINARY_DIR}/src"
)
add_test(
  NAME tur_eval_basic
  COMMAND "${CMAKE_BINARY_DIR}/tur_eval_basic"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Make sure tur is built before any test is run
set_tests_properties(tur_tests tur_cli_tests tur_span_tests tur_flags_tests
                     tur_repl_tests tur_repl_smoke tur_eval_import
  PROPERTIES DEPENDS tur
)
set_tests_properties(tur_eval_basic
  PROPERTIES DEPENDS tur_eval_basic
)

# ---------------------------------------------------------------------------
# Examples (opt-in — requires Raylib)
# OFF by default so that `cmake --build` / `ctest` never trigger a Raylib
# compile.  Enable with:  cmake -DTUR_EXAMPLES=ON ...
# or via:  just configure-examples
# ---------------------------------------------------------------------------
option(TUR_EXAMPLES "Build example programs (requires Raylib)" OFF)
if (TUR_EXAMPLES)
    add_subdirectory(examples)
endif()
