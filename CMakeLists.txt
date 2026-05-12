cmake_minimum_required(VERSION 3.20)
project(turmeric-games LANGUAGES C)

# ---------------------------------------------------------------------------
# CPM: package manager for fetching Raylib (and any future C dependencies).
# ---------------------------------------------------------------------------
include(cmake/CPM.cmake)

# ---------------------------------------------------------------------------
# Turmeric compiler
#
# The compiler is built with the existing Makefile.  We expose the resulting
# binary (build/tur) as an imported executable target so sub-directories can
# depend on it.
# ---------------------------------------------------------------------------
add_custom_target(tur_compiler
  COMMAND ${CMAKE_MAKE_PROGRAM} debug
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Building Turmeric compiler via Makefile"
  VERBATIM
)

# Convenience target: run the full test suite
add_custom_target(tur_tests
  COMMAND ${CMAKE_MAKE_PROGRAM} test
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Running Turmeric tests"
  VERBATIM
)

# ---------------------------------------------------------------------------
# Games
# ---------------------------------------------------------------------------
add_subdirectory(games)
