#ifndef TURI_ENV_H
#define TURI_ENV_H

/* Platform macros before system headers */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 700
#  endif
#endif

#include <setjmp.h>
#include <stdbool.h>
#include "diag.h"

#ifndef __EMSCRIPTEN__
#  if defined(__APPLE__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  endif
#  include <ucontext.h>
#  if defined(__APPLE__)
#    pragma clang diagnostic pop
#  endif
#else
/* WASM: ucontext.h not available; use Emscripten Fibers as the coroutine
 * primitive.  Requires -sASYNCIFY=1 at link time.
 * Full shims (getcontext/swapcontext/makecontext) live in turi/fiber.h. */
#  include <emscripten/fiber.h>
#  ifndef TURI_ASYNCIFY_STACK_SIZE
#    define TURI_ASYNCIFY_STACK_SIZE 65536
#  endif
#  ifndef TURI_UCONTEXT_STUB_DEFINED
#    define TURI_UCONTEXT_STUB_DEFINED
typedef struct {
    emscripten_fiber_t fiber;
    char asyncify_stack[TURI_ASYNCIFY_STACK_SIZE];
} ucontext_t;
#  endif
#endif

#include "arena.h"
#include "buf.h"
#include "symbols.h"
#include "value.h"

/* Phase S7: forward-declare async scheduler types (defined in turi/fiber.h) */
typedef struct TuriFiber    TuriFiber;
typedef struct TuriFuture   TuriFuture;
typedef struct TuriTimer    TuriTimer;
typedef struct TuriIoPending TuriIoPending;

/* A per-eval-call arena node, kept alive until TuriEnv is freed.
 * Closures may hold Expr* pointers into these arenas. */
typedef struct ArenaNode {
    Arena            arena;
    struct ArenaNode *next;
} ArenaNode;

/* A name→value binding in the global environment. */
typedef struct EnvBinding {
    const char       *name;   /* points into sym_arena (permanent) */
    TuriValue         value;
    struct EnvBinding *next;
} EnvBinding;

/* Open-addressing hash table for O(1) global lookup.
 * Each slot holds a pointer to an EnvBinding in the linked list,
 * or NULL for an empty slot. Sized as a power-of-two capacity. */
typedef struct EnvHashTable {
    EnvBinding **slots;
    uint32_t     cap;    /* always a power of 2 */
    uint32_t     count;
} EnvHashTable;

/* Persistent evaluation environment.
 * All per-eval arenas are kept alive here; callers must not free TuriEnv
 * while any closures from it are still referenced. */
typedef struct TuriEnv {
    Arena       sym_arena;       /* SymbolTable string storage — permanent */
    SymbolTable st;              /* Persistent symbol table */
    Buf         src_acc;         /* Accumulated prior source text */
    uint32_t    prior_toplevel;  /* Count of top-level exprs from prior evals */
    ArenaNode  *eval_arenas;     /* Linked list of per-call arenas (never freed) */
    EnvBinding *globals;         /* Global name→TuriValue map (linked list) */
    bool        sandboxed;       /* When true, I/O builtins are disabled */
    /* Return-signal state: set by EX_RETURN, cleared by function application */
    bool        returning;
    TuriValue   return_value;
    /* Phase S3: effect handler stack (TuriHandlerFrame*, defined in eval.c) */
    void       *handler_stack;
    /* Phase S4: in-flight exception state (like returning, but for throw) */
    bool        throwing;
    TuriValue   throw_value;
    /* Phase S4: defer stack (DeferItem*, defined in eval.c) */
    void       *defer_stack;
    /* Phase S5: recursion depth guard */
    uint32_t    eval_depth;
    uint32_t    max_eval_depth;
    /* Panic state: set by EX_PANIC before firing defers; detects double-panic */
    bool        panicking;
    /* Set when currently inside a #[no-unwind] function call */
    bool        in_no_unwind;
    /* Phase S7: cooperative async scheduler */
    TuriFiber  *sched_ready_head;   /* ready queue head (FIFO) */
    TuriFiber  *sched_ready_tail;   /* ready queue tail */
    TuriFiber  *current_fiber;      /* fiber currently executing (NULL = main) */
    ucontext_t  sched_ctx;          /* context to swap back to from fibers */
    TuriTimer  *timers_head;        /* sorted timer list (ascending deadline) */
    TuriIoPending *io_pending_head; /* pending non-blocking I/O entries */
    uint32_t    io_pending_count;
    /* All allocated futures (linked list for bulk free in turi_env_free) */
    TuriFuture *all_futures;
    /* Pipe fds for the built-in test I/O pipe (S7.7 tests) */
    int         test_pipe_rfd;
    int         test_pipe_wfd;
    /* Performance S8: hash table for O(1) global lookup */
    EnvHashTable globals_ht;
    /* Active reader syntax mode — settable via #lang in the REPL */
    ReaderType   reader_type;
    /* Base directory for resolving module imports (NULL = ".").
     * Set this before turi_eval_file when the input uses (import ...). */
    const char  *module_base_dir;
    /* Phase R2: catch-unwind support — setjmp boundary for interpreter panic handling */
    jmp_buf     *catch_jmp;           /* active catch-unwind jmp_buf, or NULL */
    char         catch_panic_msg[512]; /* copy of panic message when longjmp fires */
} TuriEnv;

/* Create a new unrestricted environment. */
TuriEnv *turi_env_new(void);

/* Create a sandboxed environment (I/O and FFI builtins disabled). */
TuriEnv *turi_env_new_sandboxed(void);

/* Free all resources owned by env. Any closures captured from it become
 * dangling after this call. */
void turi_env_free(TuriEnv *env);

/* Look up a global binding by name.  Returns TURI_ERROR if not found. */
TuriValue turi_env_get(TuriEnv *env, const char *name);

/* Set (or replace) a global binding by name. */
void turi_env_set(TuriEnv *env, const char *name, TuriValue value);

#endif /* TURI_ENV_H */
