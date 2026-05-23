cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Raylib — prefer a system-installed copy to avoid recompiling from source.
# Install once with:  brew install raylib   (macOS)
#                     apt install libraylib-dev  (Debian/Ubuntu)
# Falls back to CPM download+compile when no system copy is found.
# ---------------------------------------------------------------------------
find_package(raylib 5.0 QUIET)
if (NOT raylib_FOUND)
    CPMAddPackage(
      NAME raylib
      GITHUB_REPOSITORY raysan5/raylib
      GIT_TAG 5.0
      OPTIONS
        "BUILD_EXAMPLES OFF"
        "BUILD_GAMES    OFF"
        "WITH_PIC       ON"
    )
endif()

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
set(TUR_SRCDIR   "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(TUR_OUTDIR   "${CMAKE_CURRENT_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Step 1: Compile main.tur → snake_gen.c (generated C)
#
#   build/tur emit-c src/main.tur  >  snake_gen.c
#
# The compiler is run from the repo root so that the stdlib path
# (stdlib/macros.tur etc.) is resolved correctly.
# ---------------------------------------------------------------------------
add_custom_command(
  OUTPUT  "${TUR_OUTDIR}/snake_gen.c"
  COMMAND /bin/sh -c "$<TARGET_FILE:tur> emit-c \"${TUR_SRCDIR}/main.tur\" > \"${TUR_OUTDIR}/snake_gen.c\""
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  DEPENDS tur "${TUR_SRCDIR}/main.tur" "${TUR_SRCDIR}/raylib.tur"
  COMMENT "Compiling main.tur → snake_gen.c"
  VERBATIM
)

# ---------------------------------------------------------------------------
# Step 2: Build the game executable
#
#   snake_gen.c  +  rayLibShim.c  →  snake  (linked with raylib)
#
# snake_gen.c contains:
#   • All Turmeric runtime (emitted by the compiler)
#   • extern declarations for the shim functions (from `extern-c` in .tur)
#   • A main() entry point
#
# rayLibShim.c provides the definitions that satisfy those extern decls,
# wrapping Raylib's API and owning all mutable C game state.
# ---------------------------------------------------------------------------
add_executable(snake EXCLUDE_FROM_ALL
  "${TUR_OUTDIR}/snake_gen.c"
  "${TUR_SRCDIR}/rayLibShim.c"
)

target_compile_options(snake PRIVATE -w)   # suppress warnings in generated C
target_include_directories(snake PRIVATE "${TUR_SRCDIR}")
target_link_libraries(snake PRIVATE raylib)

# ---------------------------------------------------------------------------
# Copy assets to build directory
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
       DESTINATION "${TUR_OUTDIR}")
endif()
