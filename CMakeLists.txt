cmake_minimum_required(VERSION 3.20)
project(turmeric LANGUAGES C ASM)

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
add_test(
  NAME tur_flags_tests
  COMMAND bash tests/run-flags.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Make sure tur is built before any test is run
set_tests_properties(tur_tests tur_cli_tests tur_span_tests tur_flags_tests
  PROPERTIES DEPENDS tur
)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
add_subdirectory(examples)
