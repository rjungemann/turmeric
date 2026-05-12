cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Compiler sources
# ---------------------------------------------------------------------------
set(TUR_SOURCES
    arc.c
    arena.c
    atomic_queue.c
    borrow_check.c
    buf.c
    builtins.c
    cps.c
    diag.c
    effect_check.c
    effect_lower.c
    effect.c
    elab.c
    emit.c
    exn.c
    expr.c
    fiber.c
    forms.c
    gc.c
    interp.c
    kind_check.c
    lifetime_elision.c
    lifetimes.c
    main.c
    rc_elision.c
    rc_free_queue.c
    rc.c
    reader.c
    io.c
    runtime.c
    symbols.c
    typeclass.c
    types.c
)

# Architecture-specific assembly
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    list(APPEND TUR_SOURCES fiber_ctx_arm64.S)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND TUR_SOURCES fiber_ctx_x64.S)
else()
    message(FATAL_ERROR "Unsupported architecture '${CMAKE_SYSTEM_PROCESSOR}'; expected arm64/aarch64/x86_64/amd64")
endif()

# Platform-specific I/O backends
if(UNIX AND NOT APPLE AND NOT FREEBSD AND NOT OPENBSD AND NOT NETBSD)
    # Linux - use epoll
    list(APPEND TUR_SOURCES io_epoll.c)
elseif(APPLE OR FREEBSD OR OPENBSD OR NETBSD)
    # BSD/macOS - use kqueue
    list(APPEND TUR_SOURCES io_kqueue.c)
endif()

add_executable(tur ${TUR_SOURCES})

# Common warning flags
target_compile_options(tur PRIVATE
    -Wall -Wextra -Werror -Wno-unused-parameter -std=c11 -pedantic
)

# Per-build-type flags
target_compile_options(tur PRIVATE
    $<$<CONFIG:Debug>:   -Og -g -fsanitize=address,undefined -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
    $<$<CONFIG:TSan>:    -O1 -g -fsanitize=thread>
)

# Phase T19: Fiber work requires pthread for spinlocks
target_compile_options(tur PRIVATE -pthread)
target_link_options(tur PRIVATE -pthread)

target_link_options(tur PRIVATE
    $<$<CONFIG:Debug>:   -fsanitize=address,undefined>
    $<$<CONFIG:TSan>:    -fsanitize=thread>
)

# Ensure the compiler binary ends up at build/tur (same path the test scripts
# and games expect).
set_target_properties(tur PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
