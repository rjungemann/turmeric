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
# Spice-aware per-file resolver: SC0/SC1 (later SC phases extend the script).
add_test(
  NAME tur_spice_resolver_tests
  COMMAND bash tests/spice-resolver-tests.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_flags_tests
  COMMAND bash tests/run-flags.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP0: `tur build --shared <dir>` produces a dlopen-able shared library.
# The script builds tests/fixtures/build-shared-smoke as a .so and runs a
# hand-written C harness that dlsyms the exported defn and calls it.
add_test(
  NAME tur_build_shared
  COMMAND bash tests/run-build-shared.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP2: unit test for the generated FFI dispatcher trampolines in
# src/runtime/ffi_dispatch.{h,c}. The executable is defined in
# src/CMakeLists.txt as tur_ffi_dispatch_unit.
add_test(
  NAME tur_ffi_dispatch_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_ffi_dispatch_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP3: integration test for the REPL's auto spice-image loader. Runs
# `tur repl` inside fixture projects and asserts the discover/build/
# dlopen/manifest pipeline behaves correctly (initial load, freshness
# skip, rebuild-on-change, opt-out, .gitignore handshake).
add_test(
  NAME tur_repl_spice_load
  COMMAND bash tests/turi/repl-spice-load.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP4: integration test for the FFI binding layer. Builds a fixture
# spice with several signature shapes and calls each export from the
# REPL, asserting correct results, qualified-name routing, and clear
# typed-error feedback.
add_test(
  NAME tur_repl_spice_call
  COMMAND bash tests/turi/repl-spice-call.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP5: integration test for the `(reload)` REPL form. Exercises the
# no-spice / no-changes / rebuild-after-edit / arity-check paths.
add_test(
  NAME tur_repl_spice_reload
  COMMAND bash tests/turi/repl-spice-reload.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP6: integration test for `tur repl --watch`. Mutates source mid-
# session and asserts the next call sees the new code (with --watch)
# vs the old one (without). Also covers the no-changes silent path
# and CLI argument parsing.
add_test(
  NAME tur_repl_spice_watch
  COMMAND bash tests/turi/repl-spice-watch.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RUN_SERIAL: this test drives a live REPL through a FIFO and calls cc to
# AOT-compile a spice; under a parallel ctest run with ASAN it can time out
# waiting for the first eval result.  Serial execution avoids that flake.
set_tests_properties(tur_repl_spice_watch PROPERTIES RUN_SERIAL TRUE)
# RP7: regression guards for the polished error surfaces -- stale
# manifest, build failure (with self-heal via reload), no-dispatcher
# hint format, and the "no spice project here" reload outcome.
add_test(
  NAME tur_repl_spice_errors
  COMMAND bash tests/turi/repl-spice-errors.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Type-checks a curated allowlist of non-auto-loaded stdlib files so
# regressions in their loadability are caught even when no fixture
# imports them. See docs/stdlib-check-allowlist-plan.md.
add_test(
  NAME tur_stdlib_checks
  COMMAND bash tests/run-stdlib-checks.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# GS-M2: global spice install / uninstall / subcommand fallthrough.
add_test(
  NAME tur_install_tests
  COMMAND bash tests/run-install.sh
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
add_test(
  NAME tur_codegen_reinterpret
  COMMAND "${CMAKE_BINARY_DIR}/tur_codegen_reinterpret"
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
                     tur_install_tests
                     turi_fixture_tests
                     tur_repl_tests tur_repl_smoke tur_eval_import tur_eval_basic
                     tur_eval_sandbox tur_codegen_reinterpret tur_ffi_dispatch_unit
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
                     tur_install_tests
                     turi_fixture_tests
                     tur_repl_tests tur_repl_smoke tur_eval_basic
                     tur_eval_sandbox tur_codegen_reinterpret tur_ffi_dispatch_unit
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
