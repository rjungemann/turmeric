cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Phase S1: detect libedit for REPL line editing + history
# ---------------------------------------------------------------------------
find_library(LIBEDIT_LIB edit)
if(LIBEDIT_LIB)
    set(HAVE_EDITLINE 1)
    message(STATUS "libedit found: ${LIBEDIT_LIB} — enabling REPL line editing")
else()
    set(HAVE_EDITLINE 0)
    message(STATUS "libedit not found — REPL will use plain fgets")
endif()

# ---------------------------------------------------------------------------
# Phase S0: turi eval core sources (shared between tur and libturi)
# ---------------------------------------------------------------------------
set(TURI_EVAL_SOURCES
    turi/value.c
    turi/env.c
    turi/fiber.c
    turi/eval.c
    turi/repl.c
)

# ---------------------------------------------------------------------------
# Compiler sources (all except main.c and turi eval core)
# ---------------------------------------------------------------------------
set(TUR_CORE_SOURCES
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
    expr.c
    fiber.c
    forms.c
    gc.c
    globals.c
    hamt.c
    interp.c
    kind_check.c
    lifetime_elision.c
    lifetimes.c
    rc_elision.c
    rc_free_queue.c
    rc.c
    io.c
    reader.c
    runtime.c
    serial.c
    scheduler.c
    scheduler_common.c
    stm.c
    timer_wheel.c
    symbols.c
    typeclass.c
    types.c
    ${TURI_EVAL_SOURCES}
)

# Architecture-specific assembly (added to both tur and libturi)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    list(APPEND TUR_CORE_SOURCES fiber_ctx_arm64.S)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND TUR_CORE_SOURCES fiber_ctx_x64.S)
else()
    message(FATAL_ERROR "Unsupported architecture '${CMAKE_SYSTEM_PROCESSOR}'; expected arm64/aarch64/x86_64/amd64")
endif()

# Platform-specific I/O backends
if(UNIX AND NOT APPLE AND NOT FREEBSD AND NOT OPENBSD AND NOT NETBSD)
    # Linux - use epoll
    list(APPEND TUR_CORE_SOURCES io_epoll.c)
elseif(APPLE OR FREEBSD OR OPENBSD OR NETBSD)
    # BSD/macOS - use kqueue
    list(APPEND TUR_CORE_SOURCES io_kqueue.c)
endif()

# ---------------------------------------------------------------------------
# tur_core: OBJECT library shared by tur executable and libturi
# ---------------------------------------------------------------------------
add_library(tur_core OBJECT ${TUR_CORE_SOURCES})

# turi/ sources need src/ on the include path (they use #include "arena.h" etc.)
target_include_directories(tur_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Common warning flags
target_compile_options(tur_core PRIVATE
    -Wall -Wextra -Werror -Wno-unused-parameter -std=c11 -pedantic -pthread
)
# Per-build-type flags
target_compile_options(tur_core PRIVATE
    $<$<CONFIG:Debug>:   -Og -g -fsanitize=address,undefined -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
    $<$<CONFIG:TSan>:    -O1 -g -fsanitize=thread>
)
# Phase S3: ucontext_t is deprecated on macOS but still functional
if(APPLE)
    target_compile_options(tur_core PRIVATE -Wno-deprecated-declarations)
endif()
# Phase S1: editline feature flag
if(HAVE_EDITLINE)
    target_compile_definitions(tur_core PRIVATE TURI_HAVE_EDITLINE=1)
endif()

# ---------------------------------------------------------------------------
# tur: main compiler executable
# ---------------------------------------------------------------------------
add_executable(tur main.c $<TARGET_OBJECTS:tur_core>)

# tur also needs src/ on the include path for main.c -> turi/eval.h
target_include_directories(tur PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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

# Phase S1: link libedit when available
if(HAVE_EDITLINE)
    target_link_libraries(tur PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur PRIVATE TURI_HAVE_EDITLINE=1)
endif()

# Ensure the compiler binary ends up at build/tur (same path the test scripts
# and games expect).
set_target_properties(tur PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Phase S0: libturi — static library for C embedding of the eval API
# ---------------------------------------------------------------------------
add_library(libturi STATIC $<TARGET_OBJECTS:tur_core>)
set_target_properties(libturi PROPERTIES OUTPUT_NAME "turi")
target_include_directories(libturi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_options(libturi INTERFACE -pthread)
if(HAVE_EDITLINE)
    target_link_libraries(libturi INTERFACE ${LIBEDIT_LIB})
endif()

# ---------------------------------------------------------------------------
# Phase S8: install targets — libturi, public headers, tur binary, man page
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS libturi
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libturi
)

install(FILES
    turi/eval.h
    turi/env.h
    turi/value.h
    turi/fiber.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turi
    COMPONENT libturi
)

install(TARGETS tur
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT tur
)

install(FILES ${CMAKE_SOURCE_DIR}/man/man1/tur-repl.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    COMPONENT tur
)

# ---------------------------------------------------------------------------
# Phase S0: tur_eval_basic — C embedding smoke test
# ---------------------------------------------------------------------------
add_executable(tur_eval_basic ${CMAKE_SOURCE_DIR}/tests/turi/eval-basic.c)
target_include_directories(tur_eval_basic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tur_eval_basic PRIVATE libturi)
target_compile_options(tur_eval_basic PRIVATE
    -Wall -Wextra -Wno-unused-parameter -std=c11 -pthread
    $<$<CONFIG:Debug>:   -Og -g -fsanitize=address,undefined -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_eval_basic PRIVATE -pthread
    $<$<CONFIG:Debug>:   -fsanitize=address,undefined>
)
set_target_properties(tur_eval_basic PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
