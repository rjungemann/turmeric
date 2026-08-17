cmake_minimum_required(VERSION 3.20)
project(turmeric LANGUAGES C ASM)

# Export compile_commands.json for clangd.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Debug-build sanitizers (AddressSanitizer + UBSan)
# ---------------------------------------------------------------------------
# The Debug build instruments `tur` (and, in the eval_import test, the C it
# generates) with -fsanitize=address,undefined. This is ON by default on every
# platform -- the sanitizers are load-bearing coverage and we never want to
# silently carve a platform out of testing them.
#
# Known failure mode: on some newer macOS/dyld the ASan runtime *baked into the
# binary at link time by an outdated clang* deadlocks in InitializeShadowMemory
# while walking the dyld shared cache -- it spins in a startup spinlock *before*
# main(), so every `tur` invocation (even `tur --version`) hangs. That is a
# toolchain/OS runtime bug, not a bug in `tur`, and it reproduces with a bare
# `int main(){}` compiled `-fsanitize=address`.
#
#   * Real fix (keeps coverage): build with a current LLVM whose ASan runtime
#     knows the new dyld cache layout, e.g. Homebrew LLVM:
#         brew install llvm
#         cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
#               -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang"
#   * Escape hatch (drops leak/UB detection locally): explicitly opt out --
#         cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUR_DEBUG_SANITIZE=OFF
#
# We deliberately do NOT auto-disable on any platform: a silent, permanent loss
# of sanitizer coverage in CI is worse than a loud hang the `tur --version`
# smoke check catches.
option(TUR_DEBUG_SANITIZE
  "Instrument the Debug build with AddressSanitizer + UBSan"
  ON)

# MinGW GCC ships no ASan/UBSan runtime at all -- there is no libasan.a or
# libubsan.a in the toolchain -- so -fsanitize=address,undefined fails at LINK
# time with "cannot find -lasan" and no Debug build can succeed.
#
# This is not the macOS situation described above and does not contradict the
# "never auto-disable" rule.  There the runtime exists and merely misbehaves, so
# staying loud is the right call.  Here the feature does not exist on the
# toolchain, so the only choices are "disable it" or "no Windows Debug build";
# the loss of coverage is announced rather than silent.  MSYS2's clang DOES ship
# compiler-rt, so the opt-out is scoped to GNU rather than to Windows at large.
if(TUR_DEBUG_SANITIZE AND WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
  set(TUR_DEBUG_SANITIZE OFF)
  message(STATUS
    "tur: Debug sanitizers disabled -- MinGW GCC has no ASan/UBSan runtime. "
    "Build with the MSYS2 clang toolchain to get sanitizer coverage on Windows.")
endif()

if(TUR_DEBUG_SANITIZE)
  set(TUR_ASAN_FLAGS "-fsanitize=address,undefined")
else()
  set(TUR_ASAN_FLAGS "")
  message(STATUS "tur: Debug sanitizers disabled (TUR_DEBUG_SANITIZE=OFF)")
endif()

# ---------------------------------------------------------------------------
# RT3: the Z3 refinement scaffold was RETIRED in 0.32.5.
#
# `TUR_REFINE_Z3_ORACLE`, the `find_package(Z3)` block, `refine_libz3.c` and
# every `#ifdef TUR_REFINE_Z3_ORACLE` are gone, per the retirement criteria in
# docs/archive/refinement-types-plan.md ("Z3 retirement criteria").  Both
# criteria were met: the in-house chain discharges the fixtures the bootstrap
# carried, and oracle trust was established over a labelled corpus, a 3600-
# benchmark generated soak, a 7000-VC differential run, and a real-program
# dogfood (tur-ecs) -- all clean.
#
# Nothing replaces it, because nothing shipped ever used it: the in-house
# staged decision procedure (S0..S3, src/compiler/refine_solver_*.c) is and was
# the only solver a user runs.  What survives as the standing regression is the
# labelled SMT-LIB corpus under tests/corpus/smtlib/, replayed by the
# `tur_refine_corpus` ctest target with NO solver linked -- see below.
#
# ---------------------------------------------------------------------------
# CPM: package manager for fetching Raylib (and any future C dependencies).
# ---------------------------------------------------------------------------
include(cmake/CPM.cmake)

# ---------------------------------------------------------------------------
# Turmeric compiler (native CMake build)
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# J0 JIT spike (docs/archive/jit-engine-plan.md) -- OFF by default.
# ---------------------------------------------------------------------------
# The spike harness vendors MIR (c2mir + MIR-gen) via FetchContent, which means
# a network fetch at configure time.  Same posture as the Z3 oracle block
# above: a default `cmake -S . -B build` -- Debug, Release, or WASM -- must not
# reach the network and must not grow a dependency, so this is opt-in and the
# spike binary is never part of `tur`.
option(TUR_JIT_SPIKE
  "build the Phase J0 JIT spike harness (fetches MIR; not part of tur)"
  OFF)

# J1: build `tur` itself with the in-process MIR JIT engine (`tur jit`).
# Same network posture as the spike: OFF by default, opt-in fetch.  Since the
# `jit` experiment graduated (2026-08-17) this is the ONLY gate on `tur jit`:
# the run-time `--enable=jit` requirement is gone, so a binary built with this
# ON runs `tur jit` unprompted, and one built without it says so.
option(TUR_JIT
  "build tur with the in-process MIR JIT engine (tur jit; fetches MIR)"
  OFF)

if(TUR_JIT_SPIKE OR TUR_JIT)
  include(cmake/mir.cmake)   # defines tur_mir (guarded; spike re-include is a no-op)
endif()
if(TUR_JIT_SPIKE)
  add_subdirectory(tools/jit-spike)
endif()

# ---------------------------------------------------------------------------
# Tests (CTest)
# ---------------------------------------------------------------------------
enable_testing()

add_test(
  NAME tur_tests
  COMMAND bash tests/run.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# tests/run.sh and tests/run-turi.sh each fan out across every core on their
# own (JOBS defaults to nproc). Under `ctest -jN` they are therefore not N
# tests sharing a machine but N x nproc processes on it, and both harnesses
# impose per-fixture timeouts (10s compiled, 15s interpreted) that a
# 4x-oversubscribed box blows straight through. The result is a batch of
# timeout FAILs that look like product bugs and are not reproducible in
# isolation.
#
# RUN_SERIAL tells ctest to give these two the machine to themselves, which is
# what they already assume.
set_tests_properties(tur_tests PROPERTIES RUN_SERIAL TRUE)
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
# `:tur-version` in build.tur (no-compiler-version-constraint-in-manifest):
# a satisfied range is silent, a missed floor is a hard error AND a non-zero
# exit, an exceeded ceiling only warns, and a malformed range is an error.
add_test(
  NAME tur_version_range
  COMMAND bash tests/run-tur-version.sh
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
# concrete-codegen-layout-kind-enumerations-drift: append_type_mangle and
# type_has_concrete_codegen_layout must stay exhaustive over TypeKind, and the
# mangle must stay injective against type_eq, or two distinct instantiations
# silently share one monomorph C name.  Source-text check, no build needed.
add_test(
  NAME tur_typekind_mangle_tests
  COMMAND bash tests/check-typekind-mangle-exhaustive.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Same report, behavioural half: compile programs that instantiate one ADT at
# two type arguments and assert the two monomorphs do not land on one C name
# (and that no #ifndef-guarded name is defined twice with differing bodies).
add_test(
  NAME tur_monomorph_name_collision
  COMMAND bash tests/check-monomorph-name-collision.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Increment 4 stage 4 (repr-decision-function-plan): representation-DECISION
# predicates must not gain new inline call sites -- new code consults
# repr_of / the shared predicates at chokepoints, or consciously updates the
# baseline.  Source-text check, no build needed.
add_test(
  NAME tur_repr_decision_ratchet
  COMMAND bash tests/check-repr-decision-ratchet.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# F2.6 (compiled-shallow-handlers-plan): acceptance probes for `handle-shallow`.
# The distinguishing shallow behavior (a resume that re-performs) is
# interpreter-only and hard-errors on the compiled fiber fallback, so it lives
# in a probe rather than the compile-everything fixture harness.
add_test(
  NAME tur_shallow_handler_probes
  COMMAND bash tests/shallow-handler-probes.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 1: breakpoint-span coverage gate. Every fixture's elaborated
# AST must carry a usable source span on each breakpoint-eligible node (defn,
# let form, call site, top-level form). See docs/archive/history/debugger-plan.md.
add_test(
  NAME tur_span_coverage
  COMMAND bash tests/check-span-coverage.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 2: the interactive interpreter debugger (`tur debug`). Drives a
# scripted session (break / step / next / finish / backtrace / locals / print /
# (break) builtin / continue) over a fixture and asserts the output. The fixture
# carries requires.dedicated-runner so tests/run.sh skips it (debug is an
# interpreter-only path). See docs/archive/history/debugger-plan.md.
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
# path). See docs/archive/history/debugger-plan.md.
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
# it to this target. See docs/archive/history/debugger-plan.md.
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
# docs/archive/history/debugger-plan.md and docs/archive/history/debugger-native-types-plan.md.
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
# docs/archive/history/let-bound-sf-loses-outer-arg-type-when-inner-captures.md).
add_test(
  NAME tur_sf_let_bind_inner_call
  COMMAND bash tests/check-sf-let-bind-inner-call.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# WR2 stdlib ownership guard: no stored rc<T> field may land in stdlib without an
# explicit `rc-cycle-ok` review marker, since a live strong rc<T> cycle is not
# reclaimable today. See docs/archive/history/stdlib-weak-ref-audit-plan.md and the
# "Ownership across the stdlib" section of docs/guides/gc-guide.md.
add_test(
  NAME tur_stdlib_no_rc_cycles
  COMMAND bash tests/check-stdlib-no-rc-cycles.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Numeric-tower guard: emitted C must never contain `_Complex`, `<complex.h>`,
# or the `__mul*c3` / `__div*c3` compiler-runtime helpers -- c2mir's C11 subset
# excludes the first and the JIT's symbol boundary should not grow the rest.
# See docs/upcoming/numeric-tower-rational-complex-plan.md section 1.
add_test(
  NAME tur_no_c_complex
  COMMAND bash tests/check-no-c-complex.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Self-test for run.sh's emitted-C pointer/integer ratchet: builds a canary whose
# emitted C provably mixes a pointer and an integer and asserts the warning
# pattern still fires.  A grep that matches nothing is indistinguishable from a
# clean corpus, which is exactly how two sweeps behind
# docs/archive/emitted-c-pointer-integer-warnings-unwatched.md reported a false
# zero.  run.sh runs this at startup too; the target makes it visible to ctest.
add_test(
  NAME tur_cc_warn_ratchet
  COMMAND bash tests/check-cc-warn-ratchet.sh
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
# engine-selection-plan E1-E3: `tur run` engine resolution ladder
# (--engine > TUR_ENGINE > build.tur :engine > cc), the unsatisfiable-engine
# hard errors, and the P0 manifest-experiment gate.  Runs on every build --
# the jit rows assert whichever outcome (run vs hard error) this binary has.
add_test(
  NAME tur_engine_select
  COMMAND bash tests/run-engine-select.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Off-tree (load "stdlib/<file>") resolution: a freestanding script run from
# outside the repo resolves the stdlib via the exe-relative root, not just
# cwd-relative. See docs/archive/history/one-off-script-print-and-annotation-ergonomics.md
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
# dedicated target the graduation plan (docs/archive/history/catch-unwind-graduation-plan.md,
# Part A) calls for.  Invoke explicitly:
#   ctest --test-dir build -R stackless_signoff_probes --output-on-failure
add_test(
  NAME stackless_signoff_probes
  COMMAND bash tests/stackless-signoff-probes.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Defense-in-depth backstop: the script already caps each probe with a portable
# per-probe timeout, but a ctest-level TIMEOUT guarantees the whole target can
# never wedge the job even if that logic is bypassed. A clean run finishes in a
# few seconds; 300s is comfortably above the sum of every per-probe cap.
set_tests_properties(stackless_signoff_probes PROPERTIES TIMEOUT 300)
# Fat-closure-env scoped-free gate. Compiles tests/fixtures/closure-env-no-leak
# (a capturing closure built in a loop and in a repeatedly-invoked effect
# handler) with ASan/UBSan + LeakSanitizer ON and asserts the emitted program is
# leak-clean: the scoped-free pass must release each non-escaping closure env at
# scope exit. The normal suite compiles spawned programs without ASan, so this
# leak would otherwise slip past. See docs/archive/history/fat-closure-env-leak.md.
add_test(
  NAME tur_closure_env_leak
  COMMAND bash tests/run-closure-env-leak.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Sibling gate for the fat-sink shim box: a bare fn passed to a `^fat` parameter
# used to malloc a { shim, orig } box per CALL that nothing ever freed (109 MiB
# over 4e6 iterations).  Same reason for a dedicated runner -- the normal suite
# compiles the spawned program without ASan.  See
# docs/archive/fat-sink-shim-box-leaks-per-call.md.
add_test(
  NAME tur_fat_shim_leak
  COMMAND bash tests/run-fat-shim-leak.sh
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
# HAMT delete-path sibling refcount regression
# (docs/archive/history/hamt-delete-sibling-refcount.md).  Compiles
# tests/test_hamt_del_lineage.c with ASan/UBSan (+LSan on Linux) and asserts
# that freeing a whole persistent-map lineage containing a tur_hamt_del-derived
# map is double-free / use-after-free clean.  Pre-fix this tripped a
# heap-use-after-free in tur_hamt_node_release.
add_test(
  NAME tur_hamt_del_lineage
  COMMAND bash tests/run-hamt-del-lineage.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# collections-cannot-hold-rc-values, map side, step (b): caller-supplied VALUE
# ownership ops.  Value ownership used to be a bool selecting the hardcoded
# boxed-value refcount, which an rc<T> value cannot use.  Compiles
# tests/test_hamt_val_ops.c with ASan/UBSan (+LSan on Linux) and asserts the
# caller's ops are the ones that run -- across structural sharing, and for
# maps derived by a later set/del -- and that they balance exactly.
add_test(
  NAME tur_hamt_val_ops
  COMMAND bash tests/run-hamt-val-ops.sh
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
# tur init --sweet.  See docs/archive/history/build-tur-sweet-manifest-plan.md.
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
# RT2/S0--S3: refinement VC + decision-procedure unit test
# (docs/archive/refinement-types-plan.md).
add_test(
  NAME tur_refine_solver
  COMMAND "${CMAKE_BINARY_DIR}/tur_refine_solver"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Z3 retirement: the labelled SMT-LIB corpus, replayed with no Z3 present.
# Labels come from the corpus files, not from a live solver, which is the whole
# point -- this is what stays behind when the scaffold is deleted.
# POSIX-only: the harness needs fork() to sandbox each case (src/CMakeLists.txt).
if(NOT WIN32)
add_test(
  NAME tur_refine_corpus
  COMMAND "${CMAKE_BINARY_DIR}/tur_refine_corpus" tests/corpus/smtlib
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
endif()
# Source-level differential fuzz: generated programs compiled and run with the
# `refined` gate off and on, asserting the gate never turns a program the
# runtime contract caught into one that exits 0.  tur_refine_solver fuzzes
# BELOW the encoder and is blind to translation bugs; this one starts at the
# top.  Smoke size here -- run tests/refine-fuzz-src.py directly for a session.
add_test(
  NAME tur_refine_fuzz_src
  COMMAND bash tests/run-refine-fuzz-src.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_refine_fuzz_src PROPERTIES TIMEOUT 600)
# Typed-boundary fuzz: correct-by-construction programs routing known values
# through random wrappers (structs, ADTs, Option/Result, Vec, closures) and
# boundary crossings, asserting check-accepted => compiles, links, runs, and
# prints the predicted output.  Shapes reproducing open docs/reported entries
# are excluded via known_bug_slug (pinned by --known-probes instead), so a
# finding here is a NEW checker/codegen disagreement.  Smoke size -- run
# tests/type-fuzz-src.py directly for a session.
add_test(
  NAME tur_type_fuzz_src
  COMMAND bash tests/run-type-fuzz-src.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_type_fuzz_src PROPERTIES TIMEOUT 600)
# Representation trace smoke (representation-consolidation-meta-plan,
# increment 0): --emit-abi-trace's repr-trace lines classify every fn-typed
# parameter's representation and every emit-side representation bridge, so
# consolidation increments can diff traces.  This pins each classification.
add_test(
  NAME tur_repr_trace
  COMMAND bash tests/run-repr-trace.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_repr_trace PROPERTIES TIMEOUT 120)
# RT5a: the refinement solver on wasm32 -- compiles under Emscripten AND gives
# the same answers at 32-bit pointers.  Skips cleanly when emcc is absent.
add_test(
  NAME tur_refine_wasm
  COMMAND bash tests/run-refine-wasm.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_refine_wasm PROPERTIES TIMEOUT 600)
# UC-2: user-level experiments file reader unit test
# (docs/archive/history/user-config-experiments-plan.md). Standalone executable
# defined in src/CMakeLists.txt as tur_experiments_user_config.
add_test(
  NAME tur_experiments_user_config
  COMMAND "${CMAKE_BINARY_DIR}/tur_experiments_user_config"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# UC-3: end-to-end precedence/suppression matrix for the user-level
# experiments file, driven through the built `tur` against synthetic
# $XDG_CONFIG_HOME homes (docs/archive/history/user-config-experiments-plan.md).
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
# J2 (jit-engine-plan 3.3): the IN-PROCESS spice build behind
# `repl --engine jit` -- load with no .so artifact, calls (incl. the
# __ffi high-arity shim and a fractional float), the compile-error
# surface, and (reload) self-heal.  The script self-skips on a binary
# without the engine; the registration is gated the same way.
if(TUR_JIT)
  add_test(
    NAME tur_repl_spice_jit
    COMMAND ${CMAKE_COMMAND} -E env TUR=$<TARGET_FILE:tur>
            bash tests/turi/repl-spice-jit.sh
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  set_tests_properties(tur_repl_spice_jit PROPERTIES RUN_SERIAL TRUE)
  # J3 (jit-engine-plan section 5): the full fixture corpus through
  # `tur jit`.  RUN_SERIAL like the other whole-corpus
  # harnesses -- it fans out across nproc internally.  TUR is pinned to
  # THIS build's binary (the script's default would probe a sibling
  # build dir).  Note a fully green run needs a Debug binary (refine-*
  # discharge is Debug-only), same as tests/run.sh.
  add_test(
    NAME tur_jit_fixture_tests
    COMMAND ${CMAKE_COMMAND} -E env TUR=$<TARGET_FILE:tur>
            bash tests/run-jit.sh
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  set_tests_properties(tur_jit_fixture_tests PROPERTIES RUN_SERIAL TRUE)
endif()
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
# Self-parallelizing across nproc, like tur_tests above -- see the note there.
# This is the target a consumer reported failing under `ctest -j4` and passing
# in isolation.
set_tests_properties(turi_fixture_tests PROPERTIES RUN_SERIAL TRUE)
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
# The REPL surface a GUI host drives: OSC 133 busy/idle markers, the logical
# working directory, :load-string, and TUR_STDLIB_DIR validation.
add_test(
  NAME tur_repl_host_integration
  COMMAND bash tests/turi/repl-host-integration.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The tree-walking interpreter never frees its closures/registered natives
# (process-lifetime by design), so leak detection is off here as it is for the
# other REPL-driving harnesses.
set_tests_properties(tur_repl_host_integration PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
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
# -fsanitize=address,undefined to avoid undefined ASAN symbol errors. When the
# Debug sanitizers are disabled (TUR_DEBUG_SANITIZE=OFF) libturi.a is clean, so
# the fixture compile must stay clean too -- gate on the same flag.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND TUR_ASAN_FLAGS)
  set(_eval_import_asan " ${TUR_ASAN_FLAGS}")
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
# I4: a libturi embedder reaches the JIT engine (and falls back to the
# tree-walking interpreter when the build has no engine).  The interpreter
# half registers process-lifetime closures it never frees by design, so this
# takes the same detect_leaks=0 the other turi harnesses do.
add_test(
  NAME tur_jit_embed
  COMMAND "${CMAKE_BINARY_DIR}/tur_jit_embed"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_jit_embed PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# web-repl-lang-switch-drops-stdlib: a reader-type change truncates src_acc back
# to the pinned stdlib preload rather than emptying it, so the first collection
# literal after a `#lang` switch still resolves hamt-of.
add_test(
  NAME tur_lang_switch_prelude
  COMMAND "${CMAKE_BINARY_DIR}/tur_lang_switch_prelude"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_lang_switch_prelude PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# map-show-keyword-key-raw-int (root cause B): the auto-show display path keeps
# the full result type beside the head-only tag, so a collection's elements
# resolve their own Show instance instead of the int-carrier representative.
add_test(
  NAME tur_show_collection_elems
  COMMAND "${CMAKE_BINARY_DIR}/tur_show_collection_elems"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_show_collection_elems PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# TR2.0: reader macros defined in one eval must stay usable across later evals
# (pins the contract the incremental-elaboration change must preserve).
add_test(
  NAME tur_reader_macro_cross_eval
  COMMAND "${CMAKE_BINARY_DIR}/tur_reader_macro_cross_eval"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_reader_macro_cross_eval PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# TR2: the incremental parse path must be result-identical to the default
# whole-blob re-parse path (A/B differential over scripted eval sessions).
add_test(
  NAME tur_incremental_elab_diff
  COMMAND "${CMAKE_BINARY_DIR}/tur_incremental_elab_diff"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_incremental_elab_diff PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# DEDUP-4: the runtime copy of the GC must behave like the emitted copy.
add_test(
  NAME tur_gc_runtime_copy_parity
  COMMAND "${CMAKE_BINARY_DIR}/tur_gc_runtime_copy_parity"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_gc_runtime_copy_parity PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# SB0: sandboxed environment test harness
add_test(
  NAME tur_eval_sandbox
  COMMAND "${CMAKE_BINARY_DIR}/tur_eval_sandbox"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# turi-interp-collections-libturi: Vec/Set/HAMT parity through the libturi embed
# API. Leak detection OFF -- the tree-walker's collection buffers are
# process-lifetime (matching the other turi harnesses' policy).
add_test(
  NAME tur_collections_embed
  COMMAND "${CMAKE_BINARY_DIR}/tur_collections_embed"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_collections_embed PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# turi-interp-stdlib-natives-libturi: option/result/str/math embed-API parity.
# Leak detection OFF -- the tree-walker's carrier allocations are
# process-lifetime (matching the other turi harnesses' policy).
add_test(
  NAME tur_stdlib_natives_embed
  COMMAND "${CMAKE_BINARY_DIR}/tur_stdlib_natives_embed"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_stdlib_natives_embed PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# LeakSanitizer ships with ASan on Linux but is ABSENT on macOS, where
# detect_leaks=1 aborts at process startup ("detect_leaks is not supported on
# this platform") before the harness runs at all. The two leak harnesses below
# force detection ON, so gate it by platform: keep the leak assertion as a
# Linux-CI guarantee, and on macOS fall back to detect_leaks=0 (the
# create/eval/free cycle still runs under ASan's use-after-free / OOB checks,
# just without the leak count). See CLAUDE.md "Leak detection (ASan/LSan)".
if(APPLE)
  set(TUR_LEAK_HARNESS_ASAN "ASAN_OPTIONS=detect_leaks=0")
else()
  set(TUR_LEAK_HARNESS_ASAN "ASAN_OPTIONS=detect_leaks=1")
endif()
# libturi-per-embed-env-and-peripherals: Gaps 1-4 embed-API harness. Runs with
# leak detection FORCED ON (Linux) -- the create/reset/free cycles plus the
# owned module_base_dir copy must leave zero turi-attributable live allocations.
add_test(
  NAME tur_embed_peripherals
  COMMAND "${CMAKE_BINARY_DIR}/tur_embed_peripherals"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_embed_peripherals PROPERTIES
  ENVIRONMENT "${TUR_LEAK_HARNESS_ASAN}"
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
  ENVIRONMENT "${TUR_LEAK_HARNESS_ASAN}"
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
# Semver + :tur-version range unit test: pins the two bugs pkg_semver_* carried
# while dead (pre-release ignored, trailing garbage accepted) plus the range
# grammar, including the 0.x caret rule.
add_test(
  NAME tur_semver_range_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_semver_range_unit"
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
# Transport-free LSP session tests (try-turmeric-lsp-plan, §4).  Drives
# scripted JSON-RPC through lsp_session_handle with a stubbed analysis
# backend -- no pipe, no subprocess, and no dependence on what the stdlib
# happens to export today.  This is the browser path, tested without a browser.
add_test(
  NAME tur_lsp_session_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_lsp_session_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The playground's analysis backend (src/web/wasm_lsp.c), run natively against
# the in-tree stdlib.  It normally only executes inside a wasm module, where a
# mistake surfaces months later as "completion in the browser is empty";
# nothing in it is actually wasm-specific, so it runs here instead.
add_test(
  NAME tur_lsp_wasm_backend_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_lsp_wasm_backend_unit" "${CMAKE_SOURCE_DIR}/stdlib"
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
                     tur_collections_embed tur_stdlib_natives_embed
                     tur_reader_macro_cross_eval tur_incremental_elab_diff
                     tur_lang_switch_prelude tur_show_collection_elems
                     tur_gc_runtime_copy_parity tur_cc_warn_ratchet
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
