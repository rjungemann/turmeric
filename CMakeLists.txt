cmake_minimum_required(VERSION 3.20)
project(turmeric LANGUAGES C ASM)

# Export compile_commands.json for clangd.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
add_test(
  NAME turi_fixture_tests
  COMMAND bash tests/run-turi.sh
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
  ENVIRONMENT "TUR_CC_FLAGS=-O2 -std=c99 -Wall${_eval_import_asan} -I${CMAKE_CURRENT_SOURCE_DIR}/src -I${CMAKE_CURRENT_SOURCE_DIR}/src/compiler -I${CMAKE_CURRENT_SOURCE_DIR}/src/passes -I${CMAKE_CURRENT_SOURCE_DIR}/src/runtime -I${CMAKE_CURRENT_SOURCE_DIR}/src/async -I${CMAKE_CURRENT_SOURCE_DIR}/src/web -L${CMAKE_CURRENT_BINARY_DIR}/src;ASAN_OPTIONS=detect_leaks=0"
)
add_test(
  NAME tur_eval_basic
  COMMAND "${CMAKE_BINARY_DIR}/tur_eval_basic"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# SB0: sandboxed environment test harness
add_test(
  NAME tur_eval_sandbox
  COMMAND "${CMAKE_BINARY_DIR}/tur_eval_sandbox"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Phase S3: algebraic effects via eval core
add_test(
  NAME tur_eval_effects
  COMMAND bash tests/turi/eval-effects.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Phase S4: structs, exceptions, defer
add_test(
  NAME tur_eval_s4
  COMMAND bash tests/turi/eval-s4.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# TCO: tail call optimization in eval
add_test(
  NAME tur_eval_tco
  COMMAND bash tests/turi/eval-tco.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Phase S7: async/await, fiber scheduler
add_test(
  NAME tur_eval_async_basic
  COMMAND bash tests/turi/eval-async-basic.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_composition
  COMMAND bash tests/turi/eval-async-composition.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_effects
  COMMAND bash tests/turi/eval-async-effects.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_timeout
  COMMAND bash tests/turi/eval-async-timeout.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_cancel
  COMMAND bash tests/turi/eval-async-cancel.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_error
  COMMAND bash tests/turi/eval-async-error.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_io
  COMMAND bash tests/turi/eval-async-io.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Make sure tur is built before any test is run
set_tests_properties(tur_tests tur_cli_tests tur_span_tests tur_flags_tests
                     turi_fixture_tests
                     tur_repl_tests tur_repl_smoke tur_eval_import tur_eval_basic
                     tur_eval_sandbox
                     tur_eval_effects tur_eval_s4 tur_eval_tco
                     tur_eval_async_basic tur_eval_async_composition
                     tur_eval_async_effects tur_eval_async_timeout
                     tur_eval_async_cancel tur_eval_async_error tur_eval_async_io
  PROPERTIES DEPENDS tur
)

# LeakSanitizer ships enabled with ASan on Linux but is absent on macOS, so the
# Debug/ASan CI run on Ubuntu would otherwise fail: the tree-walking turi/eval
# interpreter intentionally does not free closures (they live for the process
# lifetime). Disable leak detection so the test run still catches real memory
# errors (use-after-free, out-of-bounds) without flagging that design choice.
# tur_eval_import sets ASAN_OPTIONS via its own ENVIRONMENT above.
set_tests_properties(tur_tests tur_cli_tests tur_span_tests tur_flags_tests
                     turi_fixture_tests
                     tur_repl_tests tur_repl_smoke tur_eval_basic
                     tur_eval_sandbox
                     tur_eval_effects tur_eval_s4 tur_eval_tco
                     tur_eval_async_basic tur_eval_async_composition
                     tur_eval_async_effects tur_eval_async_timeout
                     tur_eval_async_cancel tur_eval_async_error tur_eval_async_io
  PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
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
