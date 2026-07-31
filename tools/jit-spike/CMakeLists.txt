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
# S2 (findings 11.5): link the runtime ARCHIVE, not a curated TU list.
#
# This started as nine hand-picked src/runtime TUs and grew by adding whatever
# the last sweep failure named.  That is how it ended up in the state 11.5
# describes: linking *some* of the async runtime let the emitted preamble --
# which carries its own fiber/scheduler implementation -- resolve part of its
# reactor calls into a host implementation expecting the host's structures, and
# ten reactor fixtures crashed where they had previously failed cleanly at link.
#
# `tur build` autolinks `-lturi`, the whole archive.  Matching that is both the
# faithful reading of plan section 3.2 step 4 (the MIR_load_external table IS
# the runtime boundary) and the thing that makes the boundary a DEFINED set
# rather than a moving target.
#
# --whole-archive / -force_load because a static archive otherwise contributes
# only members that resolve an undefined symbol in the harness itself, and the
# harness references almost none of them -- the symbols have to be present and
# exported for dlsym(RTLD_DEFAULT) to find them at JIT link time.
if(APPLE)
  target_link_options(tur-jit-spike PRIVATE
    "LINKER:-force_load,$<TARGET_FILE:libturi>")
  target_link_libraries(tur-jit-spike PRIVATE libturi)
else()
  target_link_libraries(tur-jit-spike PRIVATE
    -Wl,--whole-archive libturi -Wl,--no-whole-archive)
endif()
target_include_directories(tur-jit-spike PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/runtime"
  "${CMAKE_SOURCE_DIR}/src/async"
)

# import_resolver() reaches the runtime through dlsym(RTLD_DEFAULT, ...), which
# only sees executable symbols in the dynamic symbol table.
set_target_properties(tur-jit-spike PROPERTIES ENABLE_EXPORTS TRUE)
target_link_libraries(tur-jit-spike PRIVATE ${CMAKE_DL_LIBS})
