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
target_sources(tur-jit-spike PRIVATE
  "${CMAKE_SOURCE_DIR}/src/runtime/hamt.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/symbols.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/tur_string.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/rc.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/gc.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/rc_free_queue.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/runtime.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/arena.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/buf.c"
)
set_source_files_properties(
  "${CMAKE_SOURCE_DIR}/src/runtime/hamt.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/symbols.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/tur_string.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/rc.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/gc.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/rc_free_queue.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/runtime.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/arena.c"
  "${CMAKE_SOURCE_DIR}/src/runtime/buf.c"
  TARGET_DIRECTORY tur-jit-spike
  PROPERTIES COMPILE_OPTIONS "-O2;-std=gnu11;-fno-strict-aliasing;-w"
)
target_include_directories(tur-jit-spike PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/runtime"
)

# import_resolver() reaches the runtime through dlsym(RTLD_DEFAULT, ...), which
# only sees executable symbols in the dynamic symbol table.
set_target_properties(tur-jit-spike PROPERTIES ENABLE_EXPORTS TRUE)
target_link_libraries(tur-jit-spike PRIVATE ${CMAKE_DL_LIBS})
