include_guard(GLOBAL)
# ---------------------------------------------------------------------------
# MIR (c2mir + MIR-gen) -- vendored for the J0 JIT spike.
# docs/upcoming/jit-engine-plan.md, Phase J0.
# ---------------------------------------------------------------------------
# MIR is the engine chosen in section 2 of the plan: a C11 front end (c2mir)
# plus an optimizing JIT back end, so `tur`'s existing emit-C path is reused
# verbatim and no per-architecture instruction selection is written at all.
#
# This whole file is INERT unless -DTUR_JIT_SPIKE=ON.  The spike is a scratch
# harness (tools/jit-spike/), not part of `tur`; nothing in a default build --
# Debug, Release, or WASM -- fetches, compiles, or links MIR.  A network fetch
# in the default configure path is exactly what the Z3 block in the top-level
# CMakeLists refuses, and the same reasoning applies here.
#
# Pinned to a commit, never a branch: c2mir's accepted C subset is the spike's
# whole subject matter, so a floating dependency would silently change the
# result being measured.
# The pin points at the rjungemann/mir fork: upstream a8ab7c31 (master tip and
# full history mirrored there) plus three fixes on fix/make-one-ret-distinct-targets:
#   b79e3681 -- make_one_ret merged multi-value rets through the LAST ret's
#     operand list, which aliases when simplify canonicalizes a trailing
#     `ret 0, 0` to `ret t, t`, returning { second-word, second-word } for a
#     two-word struct -- exactly the emitted tail-loop for a self-recursive
#     carrier-struct function
#     (docs/archive/mir-two-word-struct-return-goto-loop-miscompile.md).
#   41ff4d94 -- try_spilled_reg_mem overran its 2-entry op_nums[] when one
#     insn used the same spilled register in three operand positions
#     (`mul r,r,r` from coalesced `r = r * r`), smashing rewrite_insn's frame
#     (jit-engine-j0-findings.md section 15).
#   90633091 -- c2mir gave the aarch64 target header's fake __uint128_t
#     (`struct {unsigned long hi, lo;}`) alignment 8 where AAPCS64 requires
#     16.  On Apple that skews the whole signal-context chain, since
#     _STRUCT_ARM_NEON_STATE64 is `__uint128_t __v[32]`: ucontext_t comes out
#     864 vs clang's 880, so any struct embedding one disagrees between
#     JIT-compiled and host-compiled code with no diagnostic.  Carries two
#     supporting c2mir fixes -- _Alignas was unparseable in spec_qual_list
#     (struct members) and ignored for layout when it did parse
#     (jit-engine-j0-findings.md section 30.1,
#     docs/archive/jit-arm64-uint128-align-struct-layout-skew.md).
#   d7e19e8d -- c2mir accepted `#pragma pack` and silently ignored it, laying
#     the struct out at natural alignment.  Unlike c2mir's other gaps this one
#     does not refuse the input: it compiles, runs, and is wrong (a pack(4)
#     struct measured 16 bytes where clang gives 12), with no diagnostic beyond
#     "unknown pragma".  <mach/message.h> wraps every Mach message trailer in
#     `#pragma pack(push, 4)`, so mach_msg_context_trailer_t came out 64 against
#     the SDK's own asserted 60.  The packing is tracked in the preprocessor
#     (the only stage that sees the directive) and stamped onto each emitted
#     token, so it stays correct across #include nesting; the parser lifts it
#     onto the struct node and the layout code caps member alignment
#     (docs/reported/jit-c2mir-ignores-pragma-pack.md).
# Point TUR_MIR_GIT_REPOSITORY/TAG back at vnmakarov/mir when upstream lands
# equivalents.
# CACHE-VARIABLE TRAP: `set(... CACHE ...)` does NOT update an entry that is
# already in an existing build directory's CMakeCache.txt.  Editing the pin
# here changes what a FRESH configure fetches; an existing build dir keeps
# fetching its old pin silently -- even after `rm -rf <dir>/_deps`, which
# re-clones from the CACHED repo/tag, not from this file.  After repointing,
# either configure with -DTUR_MIR_GIT_REPOSITORY=... -DTUR_MIR_GIT_TAG=... or
# use a fresh build dir, and VERIFY with `git -C <dir>/_deps/mir-src log`.
# (This nearly shipped a spike binary built from unpatched upstream once:
# the cache still said vnmakarov/a8ab7c31 while this file said the fork.)
set(TUR_MIR_GIT_REPOSITORY "https://github.com/rjungemann/mir.git"
    CACHE STRING "MIR repository for the JIT spike (fork carrying the ret + RA fixes)")
set(TUR_MIR_GIT_TAG "d7e19e8d6159fb0dd242c2347fe165d552ae2b80"
    CACHE STRING "MIR commit pin: upstream a8ab7c31 + make_one_ret + try_spilled_reg_mem + aarch64 __uint128_t align + #pragma pack")

include(FetchContent)

# SOURCE_SUBDIR names a directory that does not exist in the MIR tree, which is
# the supported way to fetch sources WITHOUT add_subdirectory'ing the upstream
# build.  That matters: MIR's own CMakeLists adds c2m, m2b, b2m, an llvm2mir
# target that runs find_package(LLVM REQUIRED), and a full ctest suite -- none
# of which belong in this project's test list or configure output.  We declare
# exactly the three TUs the spike links instead: the IR/loader core, the
# generator, and the C front end.
FetchContent_Declare(
  mir
  GIT_REPOSITORY "${TUR_MIR_GIT_REPOSITORY}"
  GIT_TAG        "${TUR_MIR_GIT_TAG}"
  GIT_SHALLOW    FALSE
  SOURCE_SUBDIR  "cmake-build-is-not-used-here"
)

FetchContent_MakeAvailable(mir)

if(NOT TARGET tur_mir)
  add_library(tur_mir STATIC
    "${mir_SOURCE_DIR}/mir.c"
    "${mir_SOURCE_DIR}/mir-gen.c"
    "${mir_SOURCE_DIR}/c2mir/c2mir.c"
  )
  target_include_directories(tur_mir PUBLIC
    "${mir_SOURCE_DIR}"
    "${mir_SOURCE_DIR}/c2mir"
  )
  # MIR wants gnu11 + -fsigned-char and is built -O3 regardless of the enclosing
  # build type: an -O0 MIR-gen would make every compile-latency number in the
  # J0 report meaningless.  It is also deliberately NOT sanitized -- the spike
  # measures generated-code latency, and ASan-instrumenting the generator both
  # skews that and (per plan section 6) cannot see inside JIT'd code anyway.
  target_compile_options(tur_mir PRIVATE
    -O3 -std=gnu11 -fsigned-char -fPIC -w
  )
  find_package(Threads)
  if(Threads_FOUND)
    target_compile_definitions(tur_mir PUBLIC "MIR_PARALLEL_GEN")
    target_link_libraries(tur_mir PUBLIC Threads::Threads)
  endif()
  if(UNIX)
    target_link_libraries(tur_mir PUBLIC m ${CMAKE_DL_LIBS})
  endif()
endif()
