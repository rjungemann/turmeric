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
    fmt.c
    forms.c
    gc.c
    globals.c
    hamt.c
    interp.c
    kind_check.c
    pkg.c
    tutorial.c
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

# ---------------------------------------------------------------------------
# Phase WASM: WebAssembly build target for try.turmeric-lang.io
# ---------------------------------------------------------------------------

# WASM glue source
set(WASM_GLUE_SOURCES
    wasm_glue.c
    tutorial.c
)

# Create a static library for the WASM glue + libturi
add_library(libturi_wasm STATIC ${WASM_GLUE_SOURCES} $<TARGET_OBJECTS:tur_core>)
set_target_properties(libturi_wasm PROPERTIES OUTPUT_NAME "turi_wasm")
target_include_directories(libturi_wasm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_options(libturi_wasm INTERFACE -pthread)

# Option to build the WASM module (requires Emscripten)
option(TUR_WASM "Build WebAssembly module for try.turmeric-lang.io" OFF)

if(TUR_WASM)
    # Find emcc (Emscripten compiler)
    find_program(EMCC emcc)
    if(NOT EMCC)
        message(WARNING "emcc not found - WASM target will not be built. Install Emscripten from https://emscripten.org")
    else()
        message(STATUS "emcc found: ${EMCC} — building WASM module")
        
        # Get all the source files we need to compile
        set(WASM_SOURCES ${WASM_GLUE_SOURCES} ${TUR_CORE_SOURCES})

        # Remove architecture-specific assembly and platform I/O backends —
        # emcc targets wasm32 and cannot assemble native code; I/O is stubbed.
        list(REMOVE_ITEM WASM_SOURCES
            fiber_ctx_arm64.S
            fiber_ctx_x64.S
            io_epoll.c
            io_kqueue.c
        )
        # Deduplicate (tutorial.c appears in both WASM_GLUE_SOURCES and TUR_CORE_SOURCES)
        list(REMOVE_DUPLICATES WASM_SOURCES)
        
        # Build the WASM module using Emscripten
        # We compile all sources into object files first, then link with emcc
        add_custom_target(tur_wasm
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/wasm
            COMMAND ${EMCC}
                -I${CMAKE_CURRENT_SOURCE_DIR}
                ${WASM_SOURCES}
                -o ${CMAKE_BINARY_DIR}/wasm/turmeric.js
                -sMODULARIZE=1
                -sALLOW_MEMORY_GROWTH=1
                -sALLOW_TABLE_GROWTH=1
                -sWASM=1
                -pthread
                -sPTHREAD_POOL_SIZE_STRICT=0
                -sEXIT_RUNTIME=0
                -sEXPORTED_FUNCTIONS=_turi_wasm_init,_turi_wasm_reset,_turi_wasm_shutdown,_turi_wasm_eval,_turi_wasm_eval_ex,_turi_wasm_version,_turi_wasm_format,_turi_doc_lookup,_malloc,_free
                -sEXPORTED_RUNTIME_METHODS=stringToUTF8,UTF8ToString,lengthBytesUTF8
                -sEXPORT_NAME=TurmericModule
                -sMODULARIZE=1
                -O2
                -Wall -Wextra -Wno-unused-parameter -std=c11 -pedantic
                -D_DEFAULT_SOURCE
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Building Turmeric WASM module (pthreads, lazy pool)"
            VERBATIM
        )

        # Copy the WASM output to the web directory for development
        add_custom_command(TARGET tur_wasm POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/wasm/turmeric.js
                ${CMAKE_SOURCE_DIR}/web/public/turmeric.js
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/wasm/turmeric.wasm
                ${CMAKE_SOURCE_DIR}/web/public/turmeric.wasm
            COMMENT "Copying WASM module to web/public"
        )
    endif()
endif()
