# ---------------------------------------------------------------------------
# J0 JIT spike harness (docs/upcoming/jit-engine-plan.md, Phase J0)
# ---------------------------------------------------------------------------
# Off by default and never reached by a normal build; the parent guards this
# whole subdirectory on TUR_JIT_SPIKE.
#
#   cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DTUR_JIT_SPIKE=ON
#   cmake --build build-jit -j --target tur-jit-spike
#   bash tools/jit-spike/run-spike.sh
include("${CMAKE_SOURCE_DIR}/cmake/mir.cmake")

add_executable(tur-jit-spike tur-jit-spike.c)
target_link_libraries(tur-jit-spike PRIVATE tur_mir)
# gnu11, not c11: strict-conformance mode hides clock_gettime/CLOCK_MONOTONIC
# and dlfcn behind feature-test macros, and the harness is a POSIX tool.
target_compile_options(tur-jit-spike PRIVATE -O2 -std=gnu11 -Wall)

# The whole point of plan section 3.2 step 4: the Turmeric runtime lives in the
# HOST process and c2mir resolves it by address, so the runtime TUs that need
# real atomics/TLS never have to survive c2mir's C11 subset.  Compiling the
# sources straight into the harness rather than linking libturt_runtime.a is
# deliberate -- a static archive contributes only members that resolve an
# undefined symbol in the harness itself, and the harness references none of
# them, so an archive link would export nothing.
#
# -O2 and NO sanitizer: this mirrors libturt_runtime.a's flags (src/CMakeLists),
# keeps the runtime ABI-identical to a bare-source `tur build`, and keeps ASan
# out of a measurement it would only skew (plan section 6).
# One list, used for both the sources and their per-file flags -- it was
# duplicated, which is a standing invitation for the two copies to drift.
#
# The src/async/ TUs join the host runtime so `tur_reactor_new` and friends
# resolve by address, exactly as hamt.c already does.  Ten fixtures failed the
# sweep purely because the harness linked the 9 runtime TUs and not these, which
# is a property of the harness rather than of the engine (findings 8.4.3).
set(TUR_JIT_HOST_RUNTIME
  "${CMAKE_SOURCE_DIR}/src/runtime/hamt.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/symbols.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/tur_string.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/rc.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/gc.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/rc_free_queue.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/runtime.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/arena.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/buf.c"
  "${CMAKE_SOURCE_DIR}/src/async/reactor.c"
  "${CMAKE_SOURCE_DIR}/src/async/fiber.c"
  "${CMAKE_SOURCE_DIR}/src/async/io.c"
  "${CMAKE_SOURCE_DIR}/src/async/scheduler.c"
  "${CMAKE_SOURCE_DIR}/src/async/scheduler_common.c"
  "${CMAKE_SOURCE_DIR}/src/async/timer_wheel.c"
  "${CMAKE_SOURCE_DIR}/src/async/atomic_queue.c"
)

# Architecture / platform selection, mirroring src/CMakeLists.txt.  The fiber
# context switch is assembly and the I/O backend is per-OS; picking them here
# rather than hard-coding the container's pair keeps the spike buildable
# wherever `tur` itself is (the macOS runs in section 9 need the kqueue/arm64
# arms).  ASM is enabled at the top level, so the .S file needs no extra setup.
if(APPLE)
  list(APPEND TUR_JIT_HOST_RUNTIME "${CMAKE_SOURCE_DIR}/src/async/io_kqueue.c")
elseif(UNIX)
  list(APPEND TUR_JIT_HOST_RUNTIME "${CMAKE_SOURCE_DIR}/src/async/io_epoll.c")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  target_sources(tur-jit-spike PRIVATE
    "${CMAKE_SOURCE_DIR}/src/async/fiber_ctx_arm64.S")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  target_sources(tur-jit-spike PRIVATE
    "${CMAKE_SOURCE_DIR}/src/async/fiber_ctx_x64.S")
endif()

target_sources(tur-jit-spike PRIVATE ${TUR_JIT_HOST_RUNTIME})
set_source_files_properties(
  ${TUR_JIT_HOST_RUNTIME}
  TARGET_DIRECTORY tur-jit-spike
  PROPERTIES COMPILE_OPTIONS "-O2;-std=gnu11;-fno-strict-aliasing;-w"
)
target_include_directories(tur-jit-spike PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/runtime"
  "${CMAKE_SOURCE_DIR}/src/async"
)

# import_resolver() reaches the runtime through dlsym(RTLD_DEFAULT, ...), which
# only sees executable symbols in the dynamic symbol table.
set_target_properties(tur-jit-spike PROPERTIES ENABLE_EXPORTS TRUE)
target_link_libraries(tur-jit-spike PRIVATE ${CMAKE_DL_LIBS})
