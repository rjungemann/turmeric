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
# NW6 acceptance gate: a freshly scaffolded spice (`tur new`) passes its own
# `tur run ci` (clean + check + test) end-to-end. Hermetic: no git, no
# network, no ../turmeric-spices, no `just`. See docs/tur-new-ci-bootstrap-plan.md.
add_test(
  NAME tur_new_bootstrap
  COMMAND bash tests/run-tur-new.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Regression for tur-run-alias-breaks-snapshot-ci-guard: `alias NAME :=
# TARGET` lines used to abort the whole Justfile parse, making recipes
# after them unreachable.
add_test(
  NAME tur_run_alias
  COMMAND bash tests/run-tur-run-alias.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Regression for tur-run-just-assignment-eval-plan: `name := VALUE`
# assignments used to be raw byte fragments; verify the balanced scanner
# + RHS expression evaluator handle function calls, `/` and `+` concat,
# conditionals, and forward-reference errors.
add_test(
  NAME tur_run_rhs_eval
  COMMAND bash tests/run-tur-run-rhs-eval.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_fmt_tests
  COMMAND bash tests/run-fmt.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# parse-check subcommand: AST-equality of turmeric + sweet-exp file pairs,
# the backing check for tools/check-guide-pairs.py.
add_test(
  NAME tur_parse_check_tests
  COMMAND bash tests/run-parse-check.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_span_tests
  COMMAND bash tests/check-span-unknown.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 1: breakpoint-span coverage gate. Every fixture's elaborated
# AST must carry a usable source span on each breakpoint-eligible node (defn,
# let form, call site, top-level form). See docs/upcoming/debugger-plan.md.
add_test(
  NAME tur_span_coverage
  COMMAND bash tests/check-span-coverage.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 2: the interactive interpreter debugger (`tur debug`). Drives a
# scripted session (break / step / next / finish / backtrace / locals / print /
# (break) builtin / continue) over a fixture and asserts the output. The fixture
# carries requires.dedicated-runner so tests/run.sh skips it (debug is an
# interpreter-only path). See docs/upcoming/debugger-plan.md.
add_test(
  NAME tur_debugger
  COMMAND bash tests/run-debugger.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 3: the Debug Adapter Protocol server (`tur dap`). Drives a
# scripted JSON-RPC session (tests/dap-driver.py) over a fixture and asserts the
# handshake, breakpoints (plain + conditional), step/next/stepOut, the call
# stack, locals/evaluate, output events, and the exit code. The fixture carries
# requires.dedicated-runner so tests/run.sh skips it (DAP is an interpreter-only
# path). See docs/upcoming/debugger-plan.md.
add_test(
  NAME tur_dap
  COMMAND bash tests/run-dap.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 4: native source maps for emit-C. `tur --debug` threads
# `#line N "file.tur"` directives into the generated C (gated off by default so
# emit-c snapshots are unchanged) and compiles single-file builds with -g -Og,
# so gdb/lldb step through `.tur` source and backtraces name turmeric frames.
# The harness asserts both the gated #line emission and a turmeric-source gdb
# backtrace of a deliberate crash; it skips the gdb half cleanly when gdb is
# absent. The fixture carries requires.dedicated-runner so tests/run.sh leaves
# it to this target. See docs/upcoming/debugger-plan.md.
add_test(
  NAME tur_phase4_gdb
  COMMAND bash tests/run-phase4-gdb.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 5 (rich type display in native debuggers): N1 confirms the
# deterministic by-value carrier type names emit-c produces (Option__int,
# Result, ...) round-trip into the binary's DWARF; N2 confirms the gdb
# pretty-printers in tools/debug/turmeric_gdb.py render a Result parameter as
# `(ok 14)` and a returned Option as `(some 42)` rather than the raw C carrier.
# Skips the gdb half cleanly when gdb is absent. The fixture carries
# requires.dedicated-runner so tests/run.sh leaves it to this target. See
# docs/upcoming/debugger-plan.md and docs/upcoming/debugger-native-types-plan.md.
add_test(
  NAME tur_phase5_gdb
  COMMAND bash tests/run-phase5-gdb.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# HttpdConn single-definition invariant: no inline-C block may reintroduce a
# local redeclaration of the consolidated httpd connection structs.
add_test(
  NAME tur_httpd_conn_single_def
  COMMAND bash tests/check-httpd-conn-single-def.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# SF let-bind regression: a let-bound two-level closure (signal-function) whose
# inner closure captures and calls the outer ^fat param must keep its declared
# outer arg type (tur check guard; see
# docs/reported/let-bound-sf-loses-outer-arg-type-when-inner-captures.md).
add_test(
  NAME tur_sf_let_bind_inner_call
  COMMAND bash tests/check-sf-let-bind-inner-call.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Spice-aware per-file resolver: SC0/SC1 (later SC phases extend the script).
add_test(
  NAME tur_spice_resolver_tests
  COMMAND bash tests/spice-resolver-tests.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Spice-level auxiliary C sources: :c-sources / :c-includes (spices-c-sources-plan).
add_test(
  NAME tur_spice_c_sources_tests
  COMMAND bash tests/spice-c-sources-tests.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_flags_tests
  COMMAND bash tests/run-flags.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Off-tree (load "stdlib/<file>") resolution: a freestanding script run from
# outside the repo resolves the stdlib via the exe-relative root, not just
# cwd-relative. See docs/reported/one-off-script-print-and-annotation-ergonomics.md
# (Finding 3).
add_test(
  NAME tur_offtree_load
  COMMAND bash tests/run-offtree-load.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# End-to-end MCP + LSP server tests: drives `tur mcp` and `tur lsp` over
# stdio JSON-RPC and checks tools, hover, definition, documentSymbol,
# completion (incl. dynamic stdlib discovery), and the TUR_NO_MCP kill
# switch.  Skips cleanly if python3 is unavailable.
add_test(
  NAME tur_mcp_lsp_tests
  COMMAND bash tests/lsp/run-mcp-lsp.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# PH3.2: LeakSanitizer regression gate for composite-type diagnostics. Runs the
# sanitized `tur` directly on handler-mismatch error fixtures with
# detect_leaks=1 and asserts no leak (the normal suite compiles generated
# programs without ASan, so a leak in tur's own error-reporting path -- e.g. the
# type_name composite-string leak fixed in PH2 -- would otherwise slip past).
# See docs/handler-typecheck-and-typename-followups-plan.md.
add_test(
  NAME tur_leak_gate
  COMMAND bash tests/run-leak-gate.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# D4 sign-off probes for the (graduated) stackless catch-unwind lowering: the
# five cu-rec / cu-catch-deep / atom-rec / mutual-rec / fiber-rec shapes run at
# 1,000,000 depth under a reduced stack (ulimit -s 256).  Slow / memory-heavy,
# so deliberately kept out of the default `tests/run.sh` fast suite; this is the
# dedicated target the graduation plan (docs/archive/catch-unwind-graduation-plan.md,
# Part A) calls for.  Invoke explicitly:
#   ctest --test-dir build -R stackless_signoff_probes --output-on-failure
add_test(
  NAME stackless_signoff_probes
  COMMAND bash tests/stackless-signoff-probes.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Fat-closure-env scoped-free gate. Compiles tests/fixtures/closure-env-no-leak
# (a capturing closure built in a loop and in a repeatedly-invoked effect
# handler) with ASan/UBSan + LeakSanitizer ON and asserts the emitted program is
# leak-clean: the scoped-free pass must release each non-escaping closure env at
# scope exit. The normal suite compiles spawned programs without ASan, so this
# leak would otherwise slip past. See docs/reported/fat-closure-env-leak.md.
add_test(
  NAME tur_closure_env_leak
  COMMAND bash tests/run-closure-env-leak.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# WKC2: boxed-key ownership gate. Compiles the HAMT runtime + a C unit test
# with ASan/UBSan (+LSan on Linux) and asserts that a wide-keyed map
# build+drop is leak-clean and double-free/use-after-free clean: a boxed key
# shared across structurally-shared persistent versions is freed exactly once.
# See docs/float32-map-key-carrier-plan.md (WKC2).
add_test(
  NAME tur_hamt_owned_keys
  COMMAND bash tests/run-hamt-owned-keys.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Prereq 2a (turi-open-reports-prereqs.md / turi-map-set-hamt-interpreter-gap.md):
# the context-carrying key comparator family (tur_hamt_*_eq_ctx) that lets a
# stateful/turi-closure comparator ride along via a `void *ctx` word.  Compiles
# tests/test_hamt_eq_ctx.c with ASan/UBSan (+LSan on Linux) and asserts ctx is
# threaded to every compare, content-keyed ops work, and plain/ctx ops nest.
add_test(
  NAME tur_hamt_eq_ctx
  COMMAND bash tests/run-hamt-eq-ctx.sh
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
# Manifest-driven `tur build <dir>` descent: builds tests/fixtures/
# build-project-smoke (a build.tur project with modules under src/<pkg>/),
# resolving cross-module imports, and asserts the manifest is not compiled
# as source. See docs/manifest-driven-build-descent-plan.md.
add_test(
  NAME tur_build_project
  COMMAND bash tests/run-build-project.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# result-typedef-duplicated-across-modules regression: two modules export
# functions returning the same (Result cstr cstr) instantiation; a third
# module imports both.  Without a per-instantiation include guard around
# the `typedef struct Result__cstr__cstr` in each module's .h, the third
# TU fails to compile with `redefinition`.  See
# docs/archive/result-typedef-duplicated-across-modules.md.
add_test(
  NAME tur_result_typedef_multi_module
  COMMAND bash tests/run-result-typedef-multi-module.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Sweet-exp manifest support: verifies build.tur.sweet is treated as a
# fully equivalent manifest by walk-up discovery, pkg_manifest_read, and
# tur init --sweet.  See docs/upcoming/build-tur-sweet-manifest-plan.md.
add_test(
  NAME tur_sweet_manifest
  COMMAND bash tests/run-sweet-manifest.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# scscm-compile: build the scscm-compile fixture project (imports the scscm
# spice via :path) and diff its stdout against expected.stdout.  Skips with
# PASS when ../turmeric-spices is absent.
add_test(
  NAME tur_scscm_compile
  COMMAND bash tests/run-scscm-compile.sh
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
# UC-2: user-level experiments file reader unit test
# (docs/upcoming/user-config-experiments-plan.md). Standalone executable
# defined in src/CMakeLists.txt as tur_experiments_user_config.
add_test(
  NAME tur_experiments_user_config
  COMMAND "${CMAKE_BINARY_DIR}/tur_experiments_user_config"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# UC-3: end-to-end precedence/suppression matrix for the user-level
# experiments file, driven through the built `tur` against synthetic
# $XDG_CONFIG_HOME homes (docs/upcoming/user-config-experiments-plan.md).
add_test(
  NAME tur_experiments_user_config_e2e
  COMMAND bash tests/run-experiments-user-config.sh
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
set_tests_properties(tur_repl_spice_reload PROPERTIES RUN_SERIAL TRUE)
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
# Eval-mode unknown-call diagnostic: an unbound call head under --interpret must
# warn (TUR-W0040) at elaboration time rather than silently deferring to runtime.
add_test(
  NAME tur_eval_unknown_call_warn
  COMMAND bash tests/turi/eval-unknown-call-warn.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_eval_unknown_call_warn PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
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
# libturi-per-embed-env-and-peripherals: Gaps 1-4 embed-API harness. Runs with
# leak detection FORCED ON -- the create/reset/free cycles plus the owned
# module_base_dir copy must leave zero turi-attributable live allocations.
add_test(
  NAME tur_embed_peripherals
  COMMAND "${CMAKE_BINARY_DIR}/tur_embed_peripherals"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_embed_peripherals PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1"
)
# turi-env-owned-value-arena-pool-plan: env-teardown leak harness. Unlike the
# other turi targets (which default detect_leaks=0 because the interpreter
# never frees its process-lifetime closures), this one runs with leak
# detection FORCED ON -- a create/eval/free cycle must leave zero
# turi-attributable live allocations once the value pool is reclaimed.
add_test(
  NAME tur_env_teardown
  COMMAND "${CMAKE_BINARY_DIR}/tur_env_teardown"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_env_teardown PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1"
)
# turi-value-pool-scratch-promotion-plan: long-lived-env steady-state + cross-
# reset correctness harness. Exercises the scratch/permanent split with escape
# promotion enabled. Leak detection OFF: the control env deliberately runs with
# promotion disabled (the process-lifetime interpreter pattern) and never frees
# its transient value pool until turi_env_free, so LSan would flag that choice;
# ASan's use-after-poison checking (the point of this test) stays on.
add_test(
  NAME tur_env_longlived
  COMMAND "${CMAKE_BINARY_DIR}/tur_env_longlived"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_env_longlived PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
add_test(
  NAME tur_codegen_reinterpret
  COMMAND "${CMAKE_BINARY_DIR}/tur_codegen_reinterpret"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Theme E (TP3): arbitrary-arity Kind helper round-trip unit test
add_test(
  NAME tur_kind_arity_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_kind_arity_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# structdef-retirement DS-D: tur_heap_abi_unit retired with StructDef (the :heap
# typed-pointer lowering is now on the record-ADT path, fixture-covered).
# Variadic HKT rows (Direction 1): kind-level List Type (KIND_TYPEROW) unit test
add_test(
  NAME tur_kind_row_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_kind_row_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Variadic HKT rows (Layer 2): TY_TYPEROW type-variant unit test
add_test(
  NAME tur_typerow_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_typerow_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# TY4: lifetime elision + outlives-cycle unit test
add_test(
  NAME tur_lifetime_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_lifetime_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_lifetime_unit PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# CPS4 (cps-transform-plan): heap-reified continuation + trampoline runtime unit
add_test(
  NAME tur_cps_rt_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_cps_rt_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# CPS5 (cps-transform-plan): multi-prompt delimited-control machine unit
add_test(
  NAME tur_cps_prompt_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_cps_prompt_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# AI1 (application-image-dumps-plan): image header codec unit test
add_test(
  NAME tur_image_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_image_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# LSP docstring scanner unit test
add_test(
  NAME tur_docscanner_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_docscanner_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Injective name-mangler round-trip + injectivity unit test
add_test(
  NAME tur_mangle_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_mangle_unit"
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
# Phase TI4: Software Transactional Memory (tvar/atomically/stm/or-else) under
# the interpreter.  Runs via `tur --interpret` (not `tur run`, which compiles).
add_test(
  NAME tur_eval_stm
  COMMAND bash tests/turi/eval-stm.sh
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
                     tur_eval_stm tur_env_teardown tur_env_longlived tur_embed_peripherals
                     tur_experiments_user_config_e2e
  PROPERTIES DEPENDS tur
)

# LeakSanitizer ships enabled with ASan on Linux but is absent on macOS, so the
# Debug/ASan CI run on Ubuntu would otherwise fail on a deliberate design
# choice: the tree-walking turi/eval interpreter intentionally does not free
# its closures or registered-native environment entries (they live for the
# process lifetime -- see turi_env_register_native / eval_expr_impl). Disable
# leak detection for the targets that exercise the interpreter so the test run
# still catches real memory errors (use-after-free, out-of-bounds) without
# flagging that choice.
#
# The compiler/codegen path used to leak too: emit_resolve_type and
# emit_abi_instantiate_type cloned transient Type nodes for ABI specialization
# that were never freed. Those are now arena-managed and bulk-freed at the end
# of each emit pass (see EmitCtx.type_arena), so the pure compiler-path targets
# below run WITH leak detection ON as a regression guard. tur_eval_import sets
# ASAN_OPTIONS via its own ENVIRONMENT above.
set_tests_properties(tur_flags_tests
                     tur_install_tests
                     turi_fixture_tests
                     tur_repl_tests tur_repl_smoke tur_eval_basic
                     tur_eval_sandbox tur_codegen_reinterpret tur_ffi_dispatch_unit
                     tur_eval_effects tur_eval_s4 tur_eval_tco
                     tur_eval_async_basic tur_eval_async_composition
                     tur_eval_async_effects tur_eval_async_timeout
                     tur_eval_async_cancel tur_eval_async_error tur_eval_async_io
                     tur_eval_stm
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
