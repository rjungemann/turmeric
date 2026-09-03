cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# guestbook -- multi-page web form via serializable continuations
# ---------------------------------------------------------------------------
#
# Requires: the Turmeric compiler (tur) built in the parent build tree.
# No external HTTP library -- the bundled httpd.c raw-socket shim is the
# listener.  The same program builds without CMake at all:
#
#   tur build examples/guestbook          # -> examples/guestbook/build/bin/guestbook
#   tur run   examples/guestbook/src/main.tur
#
# (build.tur's :c-sources links httpd.c on both paths.)  This target exists so
# `cmake --build build --target guestbook` works alongside the other examples.
#
# Run:
#   GUESTBOOK_SECRET="my-secret" ./build/examples/guestbook/guestbook

set(TUR_SRCDIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(TUR_OUTDIR "${CMAKE_CURRENT_BINARY_DIR}")

# main.tur imports the other modules; `tur emit-c` resolves them through the
# enclosing build.tur (auto-spice discovery) and emits one translation unit.
file(GLOB TUR_SOURCES "${TUR_SRCDIR}/*.tur")
add_custom_command(
  OUTPUT  "${TUR_OUTDIR}/guestbook_gen.c"
  COMMAND /bin/sh -c "$<TARGET_FILE:tur> emit-c \"${TUR_SRCDIR}/main.tur\" > \"${TUR_OUTDIR}/guestbook_gen.c\""
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  DEPENDS tur ${TUR_SOURCES}
  COMMENT "Compiling main.tur -> guestbook_gen.c"
  VERBATIM
)

add_executable(guestbook EXCLUDE_FROM_ALL
  "${TUR_OUTDIR}/guestbook_gen.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/httpd.c"
)
target_compile_options(guestbook PRIVATE -w)   # suppress warnings in generated C
target_include_directories(guestbook PRIVATE "${CMAKE_SOURCE_DIR}/src")

# On Windows, link Winsock.
if(WIN32)
  target_link_libraries(guestbook PRIVATE ws2_32)
endif()
