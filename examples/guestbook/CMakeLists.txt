cmake_minimum_required(VERSION 3.20)
project(guestbook LANGUAGES C)

# ---------------------------------------------------------------------------
# guestbook -- multi-page web form via serializable continuations
# ---------------------------------------------------------------------------
#
# Requires: Turmeric compiler (tur) built in the parent build tree.
# No external HTTP library -- uses the bundled httpd.c raw-socket shim.
#
# Build:
#   just configure-examples
#   cmake --build build --target guestbook
#
# Run:
#   GUESTBOOK_SECRET="my-secret" ./build/examples/guestbook/guestbook

# Locate the Turmeric compiler from the parent build.
set(TUR_BINARY "$<TARGET_FILE:tur>")

# Source files to compile with the Turmeric compiler.
set(TUR_SOURCES
    src/main.tur
    src/handlers.tur
    src/router.tur
    src/conts.tur
    src/store.tur
    src/templates.tur
    src/security.tur
)

# The C shim that provides the HTTP listener.
set(C_SOURCES
    httpd.c
)

# Emit C from Turmeric sources, then compile and link with the C shim.
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/guestbook_tur.c"
    COMMAND "${TUR_BINARY}" emit-c
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main.tur"
            --output "${CMAKE_CURRENT_BINARY_DIR}/guestbook_tur.c"
    DEPENDS tur ${TUR_SOURCES}
    COMMENT "Compiling Turmeric sources -> guestbook_tur.c"
)

add_executable(guestbook
    "${CMAKE_CURRENT_BINARY_DIR}/guestbook_tur.c"
    ${C_SOURCES}
)

target_include_directories(guestbook PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(guestbook PRIVATE turi_static)

# On Windows, link Winsock.
if(WIN32)
    target_link_libraries(guestbook PRIVATE ws2_32)
endif()

# Create the data directories at configure time so they exist on first run.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/conts")
