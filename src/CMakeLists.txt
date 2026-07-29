cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Feature-test macro: glibc hides POSIX/BSD declarations (strdup, isatty,
# clock_gettime, ...) under strict -std=c11/-pedantic unless a feature macro
# is defined. Apple libc exposes them regardless, so this is a no-op there.
# Must be defined before any system header is included, hence a -D flag.
# ---------------------------------------------------------------------------
add_compile_definitions(_DEFAULT_SOURCE)

# WIN3: the reactor's Windows I/O backend (async/io_iocp.c) links against
# Winsock. tur_core is an OBJECT library, so its Winsock dependency does not
# propagate to the many executables that embed its objects (tur, the tur_*
# test harnesses). A directory-scoped link_libraries applies to every target
# defined below, which is the simplest way to cover them all.
if(WIN32)
    link_libraries(ws2_32)
endif()

# RT3: the dev-only refinement oracle needs the same treatment for exactly the
# same reason. refine_libz3.c is compiled into tur_core, and EVERY executable
# that embeds $<TARGET_OBJECTS:tur_core> -- not just `tur` -- then carries an
# unresolved Z3_* reference. Per-target PRIVATE links on an OBJECT library do
# not propagate to objects-embedding consumers, so the first oracle build
# failed to link the whole test-harness fleet. Directory scope covers them all.
if(TUR_REFINE_Z3_ORACLE)
    link_libraries(z3::libz3)
endif()

# ---------------------------------------------------------------------------
# Feature-test macro: macOS gates the POSIX-XSI ucontext_t (used by
# TuriEnv::sched_ctx for the async scheduler) behind _XOPEN_SOURCE.
# Without it, <sys/_types/_ucontext.h> exposes a legacy 56-byte stub,
# and TUs that pull in <signal.h> / <sys/wait.h> BEFORE turi/env.h get
# the small stub baked into their view of `struct TuriEnv` -- producing
# a struct-layout mismatch (sizeof differs by ~832 bytes) and silent
# null-pointer reads of fields declared after sched_ctx. Forcing it
# project-wide guarantees every TU agrees on TuriEnv's layout.
# _DARWIN_C_SOURCE keeps the macOS-specific extensions visible on top
# of POSIX-XSI (mkstemps, NSIG, st_mtimespec, ...).
# ---------------------------------------------------------------------------
if(APPLE)
    add_compile_definitions(_XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
endif()

# ---------------------------------------------------------------------------
# Warnings-as-errors, except during the Windows bring-up.
#
# The Windows port is in progress (docs/upcoming/v1/windows-support-plan.md).
# A first-time portability warning on a platform nobody has compiled for yet is
# information, not a defect to halt on -- promoting it to a hard error just
# hides the next twenty behind it.  Restore -Werror here once the port is warning
# clean; every other platform keeps it today.
# ---------------------------------------------------------------------------
if(WIN32)
    set(TUR_WERROR "")
else()
    set(TUR_WERROR -Werror)
endif()

# ---------------------------------------------------------------------------
# C standard: gnu11 on Windows, c11 everywhere else.
#
# -std=c11 defines __STRICT_ANSI__, and MinGW uses that to hide every
# non-ISO CRT declaration.  That breaks more than the POSIX calls we reach for
# (mkdir, getcwd, ...): it breaks MinGW's OWN headers, because <dirent.h>
# declares a `struct _finddata_t` field whose definition lives behind the same
# gate in <io.h> -- so <dirent.h> fails to compile with "field 'dd_dta' has
# incomplete type" before any of our code is even reached.
#
# -std=gnu11 is the same language with the platform's declarations left visible.
# It is not a laxity knob; it is the only spelling under which MinGW's headers
# are self-consistent.
# ---------------------------------------------------------------------------
if(WIN32)
    set(TUR_CSTD -std=gnu11)
else()
    set(TUR_CSTD -std=c11)
endif()

# ---------------------------------------------------------------------------
# Phase S1: detect libedit for REPL line editing + history
# ---------------------------------------------------------------------------
find_library(LIBEDIT_LIB edit)
if(LIBEDIT_LIB)
    set(HAVE_EDITLINE 1)
    message(STATUS "libedit found: ${LIBEDIT_LIB} — enabling REPL line editing")
else()
    set(HAVE_EDITLINE 0)
    message(STATUS "libedit not found — REPL will use plain fgets")
endif()

# ---------------------------------------------------------------------------
# Phase S0: turi eval core sources (shared between tur and libturi)
# ---------------------------------------------------------------------------
set(TURI_EVAL_SOURCES
    turi/value.c
    turi/env.c
    turi/fiber.c
    turi/eval.c
    # Vec/Set/Map/HAMT interpreter native overrides, relocated from main.c so
    # they land in libturi for all interpreter consumers (turi-interp-collections).
    turi/collections_native.c
    # string_native.c -- interpreter native overrides for the owned String type
    # (owned-string-type-plan), all delegating to src/runtime/tur_string.c so
    # --interpret matches the compiled path.
    turi/string_native.c
    # preload.c -- shared stdlib preload sequence (macros + typed collections),
    # relocated from main.c so the native --interpret path and the WASM REPL
    # load the same list (web-repl-missing-stdlib-preload).
    turi/preload.c
    # interpreter_natives.c -- the wk_register_* / native_* inline-C override
    # block relocated from main.c so `tur repl` and the WASM REPL register the
    # same interpreter natives as --interpret (web-repl-repl-inline-c-native-gap).
    turi/interpreter_natives.c
    # dap.c -- Debug Adapter Protocol server (debugger Phase 3), a thin shell
    # over the Phase 2 interpreter debugger's control API in eval.c.
    turi/dap.c
    turi/repl.c
    # RP3: REPL spice-image loader. Walks up to build.tur, dlopens
    # .tur-repl-cache/lib.so, primes a symbol map from exports.manifest.
    turi/spice_loader.c
    # RP4: TuriValue<->scalar marshaling layer that installs each
    # spice export as a TuriNativeFn binding the eval loop can call.
    turi/ffi_thunk.c
)

# ---------------------------------------------------------------------------
# Include directories: src/ plus each subsystem subdirectory, so that the
# existing bare `#include "foo.h"` directives keep resolving after the
# source-layout split. turi/ is intentionally omitted -- turi sources resolve
# their own headers via the compiler's current-directory rule, which also
# avoids the async/fiber.h vs turi/fiber.h basename clash.
# ---------------------------------------------------------------------------
set(TUR_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/compiler
    ${CMAKE_CURRENT_SOURCE_DIR}/passes
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/async
    ${CMAKE_CURRENT_SOURCE_DIR}/web
    ${CMAKE_CURRENT_SOURCE_DIR}/lsp
)

# ---------------------------------------------------------------------------
# Compiler sources (all except main.c and turi eval core)
# ---------------------------------------------------------------------------
set(TUR_CORE_SOURCES
    # compiler/ -- frontend pipeline: text -> IR
    compiler/builtins.c
    compiler/diag.c
    compiler/elab_core.c
    compiler/elab_macros.c
    compiler/elab_unsafe.c
    compiler/elab_forms.c
    compiler/elab_memory.c
    compiler/elab_effects.c
    compiler/elab_fns.c
    compiler/elab_call.c
    compiler/elab_module.c
    compiler/elab_structs.c
    compiler/elab_types.c
    compiler/elab_typeclasses.c
    compiler/elab_concurrent.c
    compiler/elab_sessions.c
    compiler/elab_global.c
    compiler/project.c
    compiler/elab_dynvars.c
    compiler/elab_toplevel.c
    compiler/mono_specs.c
    compiler/emit_core.c
    compiler/emit_dk_runtime.c
    compiler/emit_cps_ir.c
    compiler/emit_effects.c
    compiler/emit_expr.c
    compiler/emit_stmt.c
    compiler/emit_fns.c
    compiler/emit_module.c
    compiler/expr.c
    compiler/fmt.c
    compiler/mangle.c
    compiler/forms.c
    compiler/span_audit.c
    compiler/pkg.c
    compiler/justrun.c
    compiler/global.c
    compiler/install.c
    compiler/reader.c
    compiler/reader_macros.c
    compiler/lang_layers.c
    compiler/stdlib_autoload.c
    # RT1--RT3 + S0--S3 (docs/upcoming/v1/refinement-types-plan.md): refinement
    # types.  refine_libz3.c is a DEV-ONLY scaffold -- it compiles to nothing
    # unless TUR_REFINE_Z3_ORACLE is defined, and that option refuses Release
    # and WASM builds, so no shipped artifact can ever link Z3.
    compiler/refine_vc.c
    compiler/refine_collect.c
    compiler/refine_smtlib.c
    compiler/refine_discharge.c
    compiler/refine_solver.c
    compiler/refine_solver_s0.c
    compiler/refine_solver_euf.c
    compiler/refine_solver_arith.c
    compiler/refine_solver_no.c
    compiler/refine_libz3.c
    compiler/symbols.c
    compiler/typeclass.c
    compiler/types.c
    # passes/ -- analysis and transformation passes
    passes/borrow_check.c
    passes/cps.c
    passes/cps_ir.c
    passes/effect.c
    passes/effect_check.c
    passes/effect_lower.c
    passes/kind_check.c
    passes/lifetime_elision.c
    passes/lifetimes.c
    passes/rc_elision.c
    # runtime/ -- memory, data structures, execution
    runtime/arc.c
    runtime/arena.c
    runtime/buf.c
    # RP2: hand-rolled FFI dispatcher table. Generated by
    # tools/gen_ffi_dispatch.py -- see that script's header for the
    # naming convention and regeneration steps.
    runtime/ffi_dispatch.c
    # RP4: generated shape-switch that bridges the interpreter's
    # marshaling layer to the typed trampolines above.
    runtime/ffi_dispatch_thunk.c
    runtime/cps_rt.c
    runtime/cps_prompt.c
    runtime/experiments.c
    runtime/gc.c
    runtime/globals.c
    runtime/hamt.c
    runtime/image.c
    runtime/interp.c
    runtime/rc.c
    runtime/rc_free_queue.c
    runtime/runtime.c
    runtime/serial.c
    runtime/stm.c
    # 6(a): atomics for front ends without the GCC builtins (the JIT). Host-side
    # by design -- see the file header and jit-engine-j0-findings.md 14.
    runtime/tur_atomics.c
    # TLS1: thread-local runtime state for front ends without TLS (the JIT).
    # Same host-residency pattern; see the file header and findings 15.
    runtime/tur_tls.c
    # SYM5: the interned-symbol registrar an emitted program calls from its
    # static init.  Dual-listed in TURT_RUNTIME_SOURCES like every other
    # runtime TU here; it was the one member of that list missing from this
    # one, which left tur_sym_register out of libturi.a entirely.  A static
    # archive only contributes members that resolve an undefined symbol, so
    # the duplicate definition costs an existing link nothing -- but a
    # whole-archive consumer (the JIT, which resolves runtime symbols by NAME
    # at run time) needs it present.  docs/archive/libturi-symbols-basename-collision.md.
    runtime/symbols.c
    runtime/tur_string.c
    # async/ -- async I/O, fibers, scheduler
    async/atomic_queue.c
    async/fiber.c
    async/io.c
    async/reactor.c
    async/scheduler.c
    async/timer_wheel.c
    # web/ -- WASM glue and playground tutorial
    web/tutorial.c
    ${TURI_EVAL_SOURCES}
    # lsp/ -- LSP server (JSON-RPC over stdio, part of the tur binary)
    lsp/lsp_io.c
    lsp/lsp_sink.c
    lsp/lsp_collect.c
    lsp/lsp_json.c
    lsp/lsp_docs.c
    lsp/lsp_util.c
    lsp/lsp.c
    lsp/lsp_session.c
    # cli/ -- standalone subcommands
    cli/lsp_lite.c
)

# ---------------------------------------------------------------------------
# Architecture detection
#
# Under MSYS2/MinGW, CMake leaves CMAKE_SYSTEM_PROCESSOR *and*
# CMAKE_HOST_SYSTEM_PROCESSOR empty, and the MSYS2 shell does not export the
# PROCESSOR_ARCHITECTURE that CMake would otherwise read them from -- so all
# three of the usual sources come back blank.  Fall through them in order and
# then ask the compiler directly, which always knows: GCC and Clang both answer
# `-dumpmachine` with a target triple ("x86_64-w64-mingw32").
#
# Case is normalised because native Windows reports "AMD64", not "amd64".
# ---------------------------------------------------------------------------
set(TUR_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(NOT TUR_ARCH)
    set(TUR_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
if(NOT TUR_ARCH)
    set(TUR_ARCH "$ENV{PROCESSOR_ARCHITECTURE}")
endif()
if(NOT TUR_ARCH)
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
        OUTPUT_VARIABLE TUR_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
string(TOLOWER "${TUR_ARCH}" TUR_ARCH)

# Architecture-specific assembly (added to both tur and libturi)
if(WIN32)
    # WIN3 tier C: real Windows x64 context switch (RCX first-arg, preserves
    # RSI/RDI/XMM6-15). Replaces the earlier aborting fiber_ctx_win.c stub.
    list(APPEND TUR_CORE_SOURCES async/fiber_ctx_x64_win.S)
elseif(TUR_ARCH MATCHES "arm64|aarch64")
    list(APPEND TUR_CORE_SOURCES async/fiber_ctx_arm64.S)
elseif(TUR_ARCH MATCHES "x86_64|amd64")
    list(APPEND TUR_CORE_SOURCES async/fiber_ctx_x64.S)
else()
    message(FATAL_ERROR "Unsupported architecture '${TUR_ARCH}'; expected arm64/aarch64/x86_64/amd64")
endif()

# Platform-specific I/O backends
if(WIN32)
    # Windows -- IOCP backend (io.h already selects IO_BACKEND_IOCP here).
    # Currently a stub: every entry point fails cleanly rather than silently
    # pretending to poll.  See docs/upcoming/v1/windows-support-plan.md (WIN3).
    list(APPEND TUR_CORE_SOURCES async/io_iocp.c)
elseif(UNIX AND NOT APPLE AND NOT FREEBSD AND NOT OPENBSD AND NOT NETBSD)
    # Linux - use epoll
    list(APPEND TUR_CORE_SOURCES async/io_epoll.c)
elseif(APPLE OR FREEBSD OR OPENBSD OR NETBSD)
    # BSD/macOS - use kqueue
    list(APPEND TUR_CORE_SOURCES async/io_kqueue.c)
endif()

# ffi_dispatch.c and ffi_dispatch_thunk.c are generated files that cast
# void* to function pointers -- forbidden by strict ISO C but intentional
# and safe here.  Suppress -Wpedantic for these two files only.
set_source_files_properties(
    runtime/ffi_dispatch.c
    runtime/ffi_dispatch_thunk.c
    PROPERTIES COMPILE_OPTIONS "-Wno-pedantic"
)

# ---------------------------------------------------------------------------
# tur_core: OBJECT library shared by tur executable and libturi
# ---------------------------------------------------------------------------
add_library(tur_core OBJECT ${TUR_CORE_SOURCES})

# ffi_dispatch.c is generated and uses void*->function-pointer casts that are
# valid POSIX/extension but not strictly ISO C; suppress -Wpedantic for it only.
set_source_files_properties(runtime/ffi_dispatch.c PROPERTIES
    COMPILE_OPTIONS "-Wno-pedantic")

# Subsystem subdirectories on the include path so bare #include directives
# (e.g. #include "arena.h") resolve regardless of which subdir the file lives in.
target_include_directories(tur_core PUBLIC ${TUR_INCLUDE_DIRS})

# Common warning flags
target_compile_options(tur_core PRIVATE
    -Wall -Wextra ${TUR_WERROR} -Wno-unused-parameter ${TUR_CSTD} -pedantic -pthread
    # GCC's -Wformat-truncation flags bounded, memory-safe snprintf path
    # building (pkg.c); clang has no such warning. Keep it non-fatal on GCC.
    $<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>
)
# Per-build-type flags
target_compile_options(tur_core PRIVATE
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
    $<$<CONFIG:TSan>:    -O1 -g -fsanitize=thread>
)
# Phase S3: ucontext_t is deprecated on macOS but still functional
if(APPLE)
    target_compile_options(tur_core PRIVATE -Wno-deprecated-declarations)
endif()
# Phase S1: editline feature flag
if(HAVE_EDITLINE)
    target_compile_definitions(tur_core PRIVATE TURI_HAVE_EDITLINE=1)
endif()

# Inject version string from VERSION file at compile time (E2)
file(READ "${CMAKE_SOURCE_DIR}/VERSION" _TUR_VERSION_RAW)
string(STRIP "${_TUR_VERSION_RAW}" _TUR_VERSION_STR)
target_compile_definitions(tur_core PRIVATE TUR_VERSION="${_TUR_VERSION_STR}")

# RT3: the dev-only refinement oracle.  OFF by default and refused for Release
# and WASM in the top-level CMakeLists, so this never reaches a shipped build.
# The LINK is directory-scoped above (an OBJECT library cannot propagate it);
# only the compile definition belongs on the target.
if(TUR_REFINE_Z3_ORACLE)
    target_compile_definitions(tur_core PRIVATE TUR_REFINE_Z3_ORACLE=1)
endif()

# ---------------------------------------------------------------------------
# tur: main compiler executable
# ---------------------------------------------------------------------------
add_executable(tur main.c lsp/mcp.c $<TARGET_OBJECTS:tur_core>)

# J1 (jit-engine-plan): the in-process MIR JIT engine behind `tur jit`.
# Only under -DTUR_JIT=ON: vendors MIR (cmake/mir.cmake, included by the
# top-level guard), compiles the engine TU, and exports tur's symbols so the
# engine's dlsym(RTLD_DEFAULT) resolver can see the linked-in runtime.
if(TUR_JIT)
  target_sources(tur PRIVATE jit_engine.c)
  # MIR's headers are not -pedantic-clean (extra semicolons from its DLIST
  # macros); suppress for the one TU that includes them, same treatment as
  # the generated ffi_dispatch.c above.
  set_source_files_properties(jit_engine.c PROPERTIES
    COMPILE_OPTIONS "-Wno-pedantic")
  target_compile_definitions(tur PRIVATE TUR_HAVE_JIT=1)
  target_link_libraries(tur PRIVATE tur_mir)
  set_target_properties(tur PROPERTIES ENABLE_EXPORTS TRUE)
endif()

# tur also needs the subsystem subdirs on the include path for main.c.
target_include_directories(tur PRIVATE ${TUR_INCLUDE_DIRS})

# Common warning flags
target_compile_options(tur PRIVATE
    -Wall -Wextra ${TUR_WERROR} -Wno-unused-parameter ${TUR_CSTD} -pedantic
    $<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>
)

# Per-build-type flags
target_compile_options(tur PRIVATE
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
    $<$<CONFIG:TSan>:    -O1 -g -fsanitize=thread>
)

# Phase T19: Fiber work requires pthread for spinlocks
target_compile_options(tur PRIVATE -pthread)
target_link_options(tur PRIVATE -pthread)
target_link_libraries(tur PRIVATE m)
# RP3: turi/spice_loader.c uses dlopen/dlsym; on Linux those live in
# libdl (libSystem on macOS already exports them, but linking -ldl
# there is a harmless no-op).
target_link_libraries(tur PRIVATE ${CMAKE_DL_LIBS})

target_link_options(tur PRIVATE
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
    $<$<CONFIG:TSan>:    -fsanitize=thread>
)

# Phase S1: link libedit when available
if(HAVE_EDITLINE)
    target_link_libraries(tur PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur PRIVATE TURI_HAVE_EDITLINE=1)
endif()

# E2: inject version into tur executable (main.c reads TUR_VERSION)
target_compile_definitions(tur PRIVATE TUR_VERSION="${_TUR_VERSION_STR}")


# Ensure the compiler binary ends up at build/tur (same path the test scripts
# and games expect).
set_target_properties(tur PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Phase S0: libturi — static library for C embedding of the eval API
# ---------------------------------------------------------------------------
add_library(libturi STATIC $<TARGET_OBJECTS:tur_core>)
set_target_properties(libturi PROPERTIES OUTPUT_NAME "turi")
target_include_directories(libturi PUBLIC ${TUR_INCLUDE_DIRS})
target_link_options(libturi INTERFACE -pthread)
# RP3: propagate libdl to anyone linking libturi (the REPL helpers
# transitively call dlopen via turi/spice_loader.c).
target_link_libraries(libturi INTERFACE ${CMAKE_DL_LIBS})
# turi_env_new now pulls in interpreter_natives.c.o (the stdlib native
# overrides auto-registered for every interpreter env), whose math shims
# reference sqrt/floor -- propagate libm to anyone linking libturi.
target_link_libraries(libturi INTERFACE m)
# WIN3: propagate ws2_32 to anyone linking libturi (io_iocp.c uses Winsock).
if(WIN32)
    target_link_libraries(libturi INTERFACE ws2_32)
endif()
if(HAVE_EDITLINE)
    target_link_libraries(libturi INTERFACE ${LIBEDIT_LIB})
endif()

# ---------------------------------------------------------------------------
# tur-link-and-build-split-plan Phase 6 prerequisite: libturt_runtime.a
# ---------------------------------------------------------------------------
# A lean, deliberately NON-sanitized static archive of exactly the runtime TUs
# that stdlib reaches via bare-source `__tur_autolink__` hints
# (src/runtime/{hamt,symbols,tur_string}.c).  `tur build --runtime=lib` links
# this instead of recompiling those sources on every build.
#
# Crucially it is built WITHOUT the Debug ASan/UBSan flags: linking the full
# (Debug) libturi.a pulls -fsanitize into the program and turns LeakSanitizer on
# for the whole binary, which trips fixtures that intentionally leak
# process-lifetime allocations.  This archive keeps the runtime-lib link
# behaviorally identical to the bare-source recompile (non-sanitized), so it can
# back `--runtime=lib` as a default.  See the plan's ASan caveat (Section 5/6).
#
# Compile flags mirror the default `tur build` cc_flags (-O2 -std=c99
# -fno-strict-aliasing) so the objects are ABI-identical to a bare-source
# compile; no -Werror so a stray warning here never fails the archive build.
# DEDUP-4b (gc-cycle-collection-plan): rc/gc/rc_free_queue join the archive so a
# compiled program can LINK the one maintained copy of the reference counter and
# cycle collector instead of carrying a hand-written replica of it in its
# preamble.  Safe to add ahead of the emit-side switch: while the emitted copy
# still defines these symbols itself, the linker simply never pulls these
# members in (a static archive member is only extracted to resolve an undefined
# symbol), so the archive grows but no existing link changes.
set(TURT_RUNTIME_SOURCES
    runtime/hamt.c
    runtime/symbols.c
    runtime/tur_atomics.c
    runtime/tur_tls.c
    runtime/tur_string.c
    runtime/rc.c
    runtime/gc.c
    runtime/rc_free_queue.c
)
add_library(turt_runtime STATIC ${TURT_RUNTIME_SOURCES})
set_target_properties(turt_runtime PROPERTIES OUTPUT_NAME "turt_runtime")
target_include_directories(turt_runtime PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(turt_runtime PRIVATE
    -O2 -std=c99 -fno-strict-aliasing -Wall -Wno-unused-parameter
    $<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>
)
if(APPLE)
    target_compile_options(turt_runtime PRIVATE -Wno-deprecated-declarations)
endif()

# ---------------------------------------------------------------------------
# Phase S8: install targets — libturi, public headers, tur binary, man page
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS libturi
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libturi
)

# DEDUP-4b: the lean runtime archive has to ship too.  `--runtime=lib` PREFERS
# it over libturi.a (it is the non-sanitized one), and AUTO links only it -- so
# leaving it out of the install set meant an installed SDK could never take the
# archive path at all, silently recompiling the runtime sources on every build.
# It also gates the rc<T>/GC de-dup: without this archive an installed toolchain
# has nothing to link the collector from, and falls back to the emitted replica.
install(TARGETS turt_runtime
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libturi
)

install(FILES
    turi/eval.h
    turi/env.h
    turi/value.h
    turi/fiber.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turi
    COMPONENT libturi
)

install(TARGETS tur
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT tur
)

install(FILES ${CMAKE_SOURCE_DIR}/man/man1/tur-repl.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    COMPONENT tur
)

# ---------------------------------------------------------------------------
# Phase S0: tur_eval_basic — C embedding smoke test
# ---------------------------------------------------------------------------
add_executable(tur_eval_basic ${CMAKE_SOURCE_DIR}/tests/turi/eval-basic.c)
target_include_directories(tur_eval_basic PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_eval_basic PRIVATE libturi)
target_compile_options(tur_eval_basic PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_eval_basic PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_eval_basic PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# web-repl-lang-switch-drops-stdlib: a `#lang` switch must keep the pinned
# stdlib preload (turi_env_pin_prelude) instead of emptying src_acc.
# ---------------------------------------------------------------------------
add_executable(tur_lang_switch_prelude ${CMAKE_SOURCE_DIR}/tests/turi/lang-switch-prelude.c)
target_include_directories(tur_lang_switch_prelude PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_lang_switch_prelude PRIVATE libturi)
target_compile_options(tur_lang_switch_prelude PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_lang_switch_prelude PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_lang_switch_prelude PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# TR2.0 (turi-incremental-elaboration-design): cross-eval reader-macro coverage
# ---------------------------------------------------------------------------
add_executable(tur_reader_macro_cross_eval ${CMAKE_SOURCE_DIR}/tests/turi/reader-macro-cross-eval.c)
target_include_directories(tur_reader_macro_cross_eval PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_reader_macro_cross_eval PRIVATE libturi)
target_compile_options(tur_reader_macro_cross_eval PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_reader_macro_cross_eval PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_reader_macro_cross_eval PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# TR2 (turi-incremental-elaboration-design): A/B differential -- the incremental
# parse path must be result-identical to the default whole-blob path
# ---------------------------------------------------------------------------
add_executable(tur_incremental_elab_diff ${CMAKE_SOURCE_DIR}/tests/turi/incremental-elab-differential.c)
target_include_directories(tur_incremental_elab_diff PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_incremental_elab_diff PRIVATE libturi)
target_compile_options(tur_incremental_elab_diff PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_incremental_elab_diff PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_incremental_elab_diff PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# DEDUP-4 (gc-cycle-collection-plan): behavioral parity assertions on the
# RUNTIME copy of the GC (src/runtime/{rc,gc,rc_free_queue}.c), pinning the
# semantics the hand-written emitted copy has always had.  Compiled fixtures
# only ever exercise the emitted copy, so divergence here is otherwise silent.
# ---------------------------------------------------------------------------
add_executable(tur_gc_runtime_copy_parity ${CMAKE_SOURCE_DIR}/tests/turi/gc-runtime-copy-parity.c)
target_include_directories(tur_gc_runtime_copy_parity PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_gc_runtime_copy_parity PRIVATE libturi)
target_compile_options(tur_gc_runtime_copy_parity PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_gc_runtime_copy_parity PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_gc_runtime_copy_parity PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# turi-interp-collections-libturi: Vec/Set/HAMT C-embedding parity harness
# ---------------------------------------------------------------------------
add_executable(tur_collections_embed ${CMAKE_SOURCE_DIR}/tests/turi/collections-embed.c)
target_include_directories(tur_collections_embed PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_collections_embed PRIVATE libturi)
target_compile_options(tur_collections_embed PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_collections_embed PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_collections_embed PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# turi-interp-stdlib-natives-libturi: option/result/str/math C-embedding parity
# ---------------------------------------------------------------------------
add_executable(tur_stdlib_natives_embed ${CMAKE_SOURCE_DIR}/tests/turi/stdlib-natives-embed.c)
target_include_directories(tur_stdlib_natives_embed PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_stdlib_natives_embed PRIVATE libturi)
target_compile_options(tur_stdlib_natives_embed PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_stdlib_natives_embed PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_stdlib_natives_embed PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# turi-env-owned-value-arena-pool-plan: env-teardown leak harness
# ---------------------------------------------------------------------------
add_executable(tur_env_teardown ${CMAKE_SOURCE_DIR}/tests/turi/env-teardown.c)
target_include_directories(tur_env_teardown PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_env_teardown PRIVATE libturi)
target_compile_options(tur_env_teardown PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_env_teardown PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_env_teardown PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# turi-value-pool-scratch-promotion-plan: long-lived-env steady-state harness
# ---------------------------------------------------------------------------
add_executable(tur_env_longlived ${CMAKE_SOURCE_DIR}/tests/turi/env-longlived.c)
target_include_directories(tur_env_longlived PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_env_longlived PRIVATE libturi)
target_compile_options(tur_env_longlived PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_env_longlived PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_env_longlived PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# libturi-per-embed-env-and-peripherals: Gaps 1-4 embed-API harness
# ---------------------------------------------------------------------------
add_executable(tur_embed_peripherals ${CMAKE_SOURCE_DIR}/tests/turi/embed-peripherals.c)
target_include_directories(tur_embed_peripherals PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_embed_peripherals PRIVATE libturi)
target_compile_options(tur_embed_peripherals PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_embed_peripherals PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_embed_peripherals PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# SB0: tur_eval_sandbox -- sandboxed environment test harness
# ---------------------------------------------------------------------------
add_executable(tur_eval_sandbox ${CMAKE_SOURCE_DIR}/tests/turi/sandbox-eval.c)
target_include_directories(tur_eval_sandbox PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_eval_sandbox PRIVATE libturi)
target_compile_options(tur_eval_sandbox PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_eval_sandbox PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_eval_sandbox PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# RP2: hand-rolled FFI dispatcher unit test
# ---------------------------------------------------------------------------
# The dispatchers live in tur_core via runtime/ffi_dispatch.c; the test
# is standalone (no other tur_core symbols needed) but easiest to link
# against the same object library so include paths and warning flags
# match the rest of the runtime.
add_executable(tur_ffi_dispatch_unit
    ${CMAKE_SOURCE_DIR}/tests/runtime/ffi-dispatch-unit.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_ffi_dispatch_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_ffi_dispatch_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_ffi_dispatch_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_ffi_dispatch_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_ffi_dispatch_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_ffi_dispatch_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_ffi_dispatch_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# RT2/S0--S3: refinement normalized-VC + decision-procedure unit test
# (docs/upcoming/v1/refinement-types-plan.md).  Exercises hash-consing,
# constant folding, each in-house stage, the cube expansion, the bounded
# counterexample search, and the SMT-LIB serializer -- including the
# "must NOT prove" direction, which is where the soundness invariant lives.
# ---------------------------------------------------------------------------
add_executable(tur_refine_solver
    ${CMAKE_SOURCE_DIR}/tests/unit/refine_solver.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_refine_solver PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_refine_solver PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_refine_solver PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_refine_solver PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_refine_solver PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_refine_solver PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_refine_solver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Z3 retirement: replay a LABELLED SMT-LIB2 corpus against the in-house chain,
# with no solver dependency of its own.  The oracle in refine_libz3.c is a live
# cross-check that only exists on a dev build with a system Z3; what has to
# survive its deletion is a corpus whose labels are DATA in the repo.  This
# target is that regression, so it is built unconditionally -- exactly the
# builds that have no Z3 are the ones it must run in.
# See tests/unit/refine_corpus.c and tests/corpus/smtlib/README.md.
# ---------------------------------------------------------------------------
add_executable(tur_refine_corpus
    ${CMAKE_SOURCE_DIR}/tests/unit/refine_corpus.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_refine_corpus PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_refine_corpus PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_refine_corpus PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_refine_corpus PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_refine_corpus PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_refine_corpus PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_refine_corpus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# RT3: differential fuzzing of the in-house refinement solver against Z3.
# Built only in an oracle build -- it links Z3, which no shipped build does.
# See the file header for why fixture coverage is not differential coverage.
# ---------------------------------------------------------------------------
if(TUR_REFINE_Z3_ORACLE)
    add_executable(tur_refine_fuzz
        ${CMAKE_SOURCE_DIR}/tests/unit/refine_fuzz.c
        $<TARGET_OBJECTS:tur_core>
    )
    target_include_directories(tur_refine_fuzz PRIVATE ${TUR_INCLUDE_DIRS})
    target_compile_options(tur_refine_fuzz PRIVATE
        -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
        $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    )
    target_compile_definitions(tur_refine_fuzz PRIVATE TUR_REFINE_Z3_ORACLE=1)
    target_link_options(tur_refine_fuzz PRIVATE -pthread
        $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
    )
    target_link_libraries(tur_refine_fuzz PRIVATE m)
    if(HAVE_EDITLINE)
        target_link_libraries(tur_refine_fuzz PRIVATE ${LIBEDIT_LIB})
        target_compile_definitions(tur_refine_fuzz PRIVATE TURI_HAVE_EDITLINE=1)
    endif()
    set_target_properties(tur_refine_fuzz PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# ---------------------------------------------------------------------------
# UC-2: user-level experiments file reader unit test
# (docs/upcoming/user-config-experiments-plan.md).  Exercises
# experiments_read_user_config against a temp $XDG_CONFIG_HOME.
# ---------------------------------------------------------------------------
add_executable(tur_experiments_user_config
    ${CMAKE_SOURCE_DIR}/tests/unit/experiments_user_config.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_experiments_user_config PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_experiments_user_config PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_experiments_user_config PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_experiments_user_config PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_experiments_user_config PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_experiments_user_config PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_experiments_user_config PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# ACB: emit_carrier_bridge unit test
# ---------------------------------------------------------------------------
add_executable(tur_codegen_carrier_bridge
    ${CMAKE_SOURCE_DIR}/tests/compiler/test_emit_carrier_bridge.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_codegen_carrier_bridge PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_codegen_carrier_bridge PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_codegen_carrier_bridge PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_codegen_carrier_bridge PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_codegen_carrier_bridge PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_codegen_carrier_bridge PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_codegen_carrier_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Theme E (TP3): arbitrary-arity Kind helper unit test
# ---------------------------------------------------------------------------
add_executable(tur_kind_arity_unit
    ${CMAKE_SOURCE_DIR}/tests/compiler/test_kind_arity.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_kind_arity_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_kind_arity_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_kind_arity_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_kind_arity_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_kind_arity_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_kind_arity_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_kind_arity_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# structdef-retirement DS-D: the :heap typed-pointer ABI lowering unit test
# (tur_heap_abi_unit) pinned type_c_name over a hand-built StructDef.  StructDef
# has been deleted; the :heap typed-pointer lowering now lives on the record-ADT
# monomorph/app path and is covered end-to-end by the fixture suite, so the
# isolated StructDef unit pin was retired with the type.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Variadic HKT rows (Direction 1): kind-level `List Type` (KIND_TYPEROW) unit test
# ---------------------------------------------------------------------------
add_executable(tur_kind_row_unit
    ${CMAKE_SOURCE_DIR}/tests/compiler/test_kind_row.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_kind_row_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_kind_row_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_kind_row_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_kind_row_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_kind_row_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_kind_row_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_kind_row_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Variadic HKT rows (Layer 2): TY_TYPEROW type-variant unit test
# ---------------------------------------------------------------------------
add_executable(tur_typerow_unit
    ${CMAKE_SOURCE_DIR}/tests/compiler/test_typerow.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_typerow_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_typerow_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_typerow_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_typerow_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_typerow_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_typerow_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_typerow_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# TS2: compiler-side EX_REINTERPRET codegen test
# ---------------------------------------------------------------------------
add_executable(tur_codegen_reinterpret
    ${CMAKE_SOURCE_DIR}/tests/compiler/reinterpret-codegen.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_codegen_reinterpret PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_codegen_reinterpret PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_codegen_reinterpret PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_codegen_reinterpret PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_codegen_reinterpret PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_codegen_reinterpret PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_codegen_reinterpret PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# TY4: lifetime elision + outlives-cycle unit test
# ---------------------------------------------------------------------------
add_executable(tur_lifetime_unit
    ${CMAKE_SOURCE_DIR}/tests/lifetime_unit.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_lifetime_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_lifetime_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_lifetime_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_lifetime_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_lifetime_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_lifetime_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_lifetime_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# CPS4 (cps-transform-plan): heap-reified continuation + trampoline runtime unit.
# Compiles cps_rt.c directly (no tur_core link) so leak detection validates only
# the CPS runtime -- it is self-contained and frees everything it allocates.
add_executable(tur_cps_rt_unit
    ${CMAKE_SOURCE_DIR}/tests/cps_rt_unit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/cps_rt.c
)
target_include_directories(tur_cps_rt_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_cps_rt_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_cps_rt_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_cps_rt_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_cps_rt_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_cps_rt_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_cps_rt_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# CPS5 (cps-transform-plan): multi-prompt delimited-control machine unit.
add_executable(tur_cps_prompt_unit
    ${CMAKE_SOURCE_DIR}/tests/cps_prompt_unit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/cps_prompt.c
)
target_include_directories(tur_cps_prompt_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_cps_prompt_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_cps_prompt_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_cps_prompt_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_cps_prompt_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_cps_prompt_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_cps_prompt_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# AI1 (application-image-dumps-plan): image header codec unit test.
add_executable(tur_image_unit
    ${CMAKE_SOURCE_DIR}/tests/image_unit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/image.c
)
target_include_directories(tur_image_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_image_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_image_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_image_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_image_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_image_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_image_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# LSP docstring-scanner unit test
# ---------------------------------------------------------------------------
add_executable(tur_docscanner_unit
    ${CMAKE_SOURCE_DIR}/tests/lsp/docscanner_test.c
    ${CMAKE_SOURCE_DIR}/src/lsp/lsp_docs.c
    ${CMAKE_SOURCE_DIR}/src/lsp/lsp_util.c
)
target_include_directories(tur_docscanner_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_docscanner_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_docscanner_unit PRIVATE
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_docscanner_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# LSP session unit test (try-turmeric-lsp-plan, §4)
#
# Links tur_core -- which is everything except main.c -- and supplies its own
# tur_collect_symbols. That is not a workaround for the link: it is the point.
# A fixed stub index makes completion ordering, path remapping, and last-good
# retention assertable, where the real compiler would tie every assertion to
# whatever the stdlib currently exports.
# ---------------------------------------------------------------------------
add_executable(tur_lsp_session_unit
    ${CMAKE_SOURCE_DIR}/tests/lsp/session_test.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_lsp_session_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_lsp_session_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_lsp_session_unit PRIVATE
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_compile_options(tur_lsp_session_unit PRIVATE -pthread)
target_link_options(tur_lsp_session_unit PRIVATE -pthread)
target_link_libraries(tur_lsp_session_unit PRIVATE m ${CMAKE_DL_LIBS})
# tur_core carries repl.c, whose line editing is libedit's whenever libedit was
# found. Omitting this links clean on a box without libedit and fails to link
# on one with it -- the wrong way round for a check meant to catch things early.
if(HAVE_EDITLINE)
    target_link_libraries(tur_lsp_session_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_lsp_session_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_lsp_session_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# WASM LSP analysis-backend test (try-turmeric-lsp-plan, L0)
#
# src/web/wasm_lsp.c is the front end the browser uses in place of main.c's
# compile_to_c.  Nothing in it is actually wasm-specific -- the Emscripten
# build differs only in where the stdlib is mounted, which is a TUR_STDLIB_DIR
# lookup -- so it runs here against the in-tree stdlib and the real compiler.
# Without this the file would only ever execute somewhere nothing can check it.
# ---------------------------------------------------------------------------
add_executable(tur_lsp_wasm_backend_unit
    ${CMAKE_SOURCE_DIR}/tests/lsp/wasm_backend_test.c
    ${CMAKE_SOURCE_DIR}/src/web/wasm_lsp.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_lsp_wasm_backend_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_lsp_wasm_backend_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_lsp_wasm_backend_unit PRIVATE
    -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_lsp_wasm_backend_unit PRIVATE m ${CMAKE_DL_LIBS})
if(HAVE_EDITLINE)
    target_link_libraries(tur_lsp_wasm_backend_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_lsp_wasm_backend_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_lsp_wasm_backend_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Injective name-mangler round-trip + injectivity unit test
# (reversible-name-mangling-plan, task T2).
add_executable(tur_mangle_unit
    ${CMAKE_SOURCE_DIR}/tests/mangle_test.c
    ${CMAKE_SOURCE_DIR}/src/compiler/mangle.c
)
target_include_directories(tur_mangle_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_mangle_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_mangle_unit PRIVATE
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_mangle_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Phase WASM: WebAssembly build target for try.turmeric-lang.io
# ---------------------------------------------------------------------------

# WASM glue source
set(WASM_GLUE_SOURCES
    web/wasm_glue.c
    web/tutorial.c
    # try-turmeric-lsp-plan: the LSP bridge, plus the WASM-side analysis
    # backend.  tur_core carries the whole server already; what it does not
    # carry is tur_collect_symbols, which lives in main.c (a CLI, and not part
    # of this source set).  Nothing referenced the server, so wasm-ld collected
    # it away and the dangling reference never surfaced -- exporting an entry
    # point makes it live, and this file is what resolves it.
    web/wasm_lsp.c
)

# Create a static library for the WASM glue + libturi
add_library(libturi_wasm STATIC ${WASM_GLUE_SOURCES} $<TARGET_OBJECTS:tur_core>)
set_target_properties(libturi_wasm PROPERTIES OUTPUT_NAME "turi_wasm")
target_include_directories(libturi_wasm PRIVATE ${TUR_INCLUDE_DIRS})
target_link_options(libturi_wasm INTERFACE -pthread)

# Option to build the WASM module (requires Emscripten)
option(TUR_WASM "Build WebAssembly module for try.turmeric-lang.io" OFF)

if(TUR_WASM)
    # Find emcc (Emscripten compiler)
    find_program(EMCC emcc)
    if(NOT EMCC)
        message(WARNING "emcc not found - WASM target will not be built. Install Emscripten from https://emscripten.org")
    else()
        message(STATUS "emcc found: ${EMCC} — building WASM module")
        
        # Get all the source files we need to compile
        set(WASM_SOURCES ${WASM_GLUE_SOURCES} ${TUR_CORE_SOURCES})

        # Remove architecture-specific assembly and platform I/O backends —
        # emcc targets wasm32 and cannot assemble native code; I/O is stubbed.
        list(REMOVE_ITEM WASM_SOURCES
            async/fiber_ctx_arm64.S
            async/fiber_ctx_x64.S
            async/io_epoll.c
            async/io_kqueue.c
        )
        # Deduplicate (tutorial.c appears in both WASM_GLUE_SOURCES and TUR_CORE_SOURCES)
        list(REMOVE_DUPLICATES WASM_SOURCES)
        
        # Build the WASM module using Emscripten
        # We compile all sources into object files first, then link with emcc
        add_custom_target(tur_wasm
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/wasm
            COMMAND ${EMCC}
                -I${CMAKE_CURRENT_SOURCE_DIR}
                -I${CMAKE_CURRENT_SOURCE_DIR}/compiler
                -I${CMAKE_CURRENT_SOURCE_DIR}/passes
                -I${CMAKE_CURRENT_SOURCE_DIR}/runtime
                -I${CMAKE_CURRENT_SOURCE_DIR}/async
                -I${CMAKE_CURRENT_SOURCE_DIR}/web
                ${WASM_SOURCES}
                # Bundle the stdlib into the module's virtual FS at /stdlib so the
                # interpreter can (load ...) macros + typed collections at init.
                # The default cwd is "/", so a cwd-relative "stdlib/<file>" load
                # (the form the stdlib modules use internally) resolves here too.
                # web-repl-missing-stdlib-preload.
                --embed-file ${CMAKE_SOURCE_DIR}/stdlib@/stdlib
                -o ${CMAKE_BINARY_DIR}/wasm/turmeric.js
                -sMODULARIZE=1
                -sALLOW_MEMORY_GROWTH=1
                -sALLOW_TABLE_GROWTH=1
                -sWASM=1
                -pthread
                -sPTHREAD_POOL_SIZE_STRICT=0
                -sEXIT_RUNTIME=0
                -sSTACK_SIZE=16777216
                -sDEFAULT_PTHREAD_STACK_SIZE=8388608
                -sINITIAL_MEMORY=67108864
                -sEXPORTED_FUNCTIONS=_turi_wasm_init,_turi_wasm_reset,_turi_wasm_shutdown,_turi_wasm_eval,_turi_wasm_eval_ex,_turi_wasm_version,_turi_wasm_format,_turi_wasm_lsp_request,_turi_wasm_lsp_flush,_turi_wasm_lsp_reset,_turi_wasm_set_lang,_turi_wasm_get_lang,_turi_doc_lookup,_turi_type_of,_turi_explain,_malloc,_free
                -sEXPORTED_RUNTIME_METHODS=stringToUTF8,UTF8ToString,lengthBytesUTF8
                -sEXPORT_NAME=TurmericModule
                -sMODULARIZE=1
                -O2
                -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pedantic
                -D_DEFAULT_SOURCE
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Building Turmeric WASM module (pthreads, lazy pool)"
            VERBATIM
        )

        # Copy the WASM output to the web directory for development
        add_custom_command(TARGET tur_wasm POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/wasm/turmeric.js
                ${CMAKE_SOURCE_DIR}/web/public/turmeric.js
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/wasm/turmeric.wasm
                ${CMAKE_SOURCE_DIR}/web/public/turmeric.wasm
            COMMENT "Copying WASM module to web/public"
        )
    endif()
endif()
