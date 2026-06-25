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
#include "compiler/diag.h"

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

#include "runtime/arena.h"
#include "runtime/buf.h"
#include "compiler/symbols.h"
#include "turi/value.h"

/* Phase S7: forward-declare async scheduler types (defined in turi/fiber.h) */
typedef struct TuriFiber    TuriFiber;
typedef struct TuriFuture   TuriFuture;
typedef struct TuriTimer    TuriTimer;
typedef struct TuriIoPending TuriIoPending;

/* RP5: list node for env-retained spice images. The full layout
 * lives here so both env.c (teardown) and ffi_thunk.c (push) can
 * traverse it without an extra header dependency. The TurSpiceImage
 * struct itself is still opaque (declared via the forward decl on
 * the field below). */
struct TurSpiceImageNode {
    struct TurSpiceImage      *image;
    struct TurSpiceImageNode  *next;
};

/* A per-eval-call arena node, kept alive until TuriEnv is freed.
 * Closures may hold Expr* pointers into these arenas. */
typedef struct ArenaNode {
    Arena            arena;
    struct ArenaNode *next;
} ArenaNode;

/* turi-value-pool-residual-sites: a coroutine execution stack (fiber/generator).
 * These back a ucontext_t, so they must be mmap'd (native) / malloc'd (WASM) at
 * a stable address rather than bump-allocated from the value pool. Each one is
 * tracked here so turi_env_free can munmap/free it instead of leaking it for the
 * process lifetime. The node itself is pool-allocated (reclaimed with the env). */
typedef struct TuriCoroStack {
    void                 *base;   /* mmap/malloc base pointer of the stack */
    size_t                size;   /* byte length (needed for munmap) */
    struct TuriCoroStack *next;
} TuriCoroStack;

/* SB4: Capability bits -- controls which operations are permitted in a
 * sandboxed environment.  TURI_CAP_ALL grants every capability (unrestricted).
 * TURI_CAP_NONE grants nothing (fully sandboxed). */
typedef uint32_t TuriCaps;
#define TURI_CAP_IO       (1u << 0)  /* println-*, file I/O builtins */
#define TURI_CAP_FFI      (1u << 1)  /* dlopen/dlsym/dlclose */
#define TURI_CAP_INLINE_C (1u << 2)  /* inline-C expressions */
#define TURI_CAP_ASYNC    (1u << 3)  /* (async ...) forms */
#define TURI_CAP_UNSAFE   (1u << 4)  /* raw-malloc, ptr-deref, unsafe-cast, ... */
#define TURI_CAP_IMPORT   (1u << 5)  /* (import ...) module loading */
#define TURI_CAP_ALL      (~(TuriCaps)0)
#define TURI_CAP_NONE     ((TuriCaps)0)

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
    /* turi-env-owned-value-arena-pool-plan: a dedicated pool for TuriValue heap
     * payloads (closures, structs, captured frames/bindings, cons cells, ...),
     * distinct from eval_arenas (which holds AST/elaboration memory). Created in
     * turi_env_new, reclaimed wholesale in turi_env_free, so an embedding host
     * gets leak-clean teardown. Allocate from it via the turi_val_* helpers in
     * value.h. The Arena chains its own slabs, so one is enough. */
    Arena       value_arena;
    EnvBinding *globals;         /* Global name→TuriValue map (linked list) */
    bool        sandboxed;       /* Deprecated alias: true when caps == TURI_CAP_NONE */
    TuriCaps    caps;            /* SB4: capability bitmask (TURI_CAP_ALL = unrestricted) */
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
    /* SR N4 (turi-cek-stackless-reentry): abortive-shift transfer signal.
     * An abortive (shift f _) sets these instead of longjmp-ing to a setjmp
     * reset boundary; the signal propagates like `throwing` (short-circuiting
     * the same `returning || throwing` guards) and is consumed by the nearest
     * matching reset boundary (a work-stack DK_RESET, or eval_reset_boundary
     * for the non-driver path).  This keeps delimited-control nesting on the
     * heap work-stack instead of one C frame per reset.  abort_prompt_kind is
     * a TuriPromptKind (int here; the enum lives in eval.c). */
    bool        aborting;
    TuriValue   abort_value;
    int         abort_prompt_kind;
    /* SR N4 Slice 2: when non-NULL, the abort is a call/cc *escape* targeting
     * this specific boundary (a TuriEscapeBoundary*), matched by pointer rather
     * than by abort_prompt_kind; reset boundaries pass it through.  NULL for a
     * plain shift abort (matched by prompt kind). */
    void       *abort_target;
    /* SB3: step-fuel resource limit (0 in both fields = unlimited) */
    uint64_t    step_fuel;        /* remaining fuel units; decremented each eval step */
    uint64_t    step_fuel_limit;  /* initial limit set by turi_env_set_fuel */
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
    /* turi-value-pool-residual-sites: coroutine execution stacks (fiber +
     * generator), tracked so turi_env_free reclaims them. */
    TuriCoroStack *coro_stacks;
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
    /* Phase TI5: typed panic payload carried across the catch boundary so that
     * catch-panic-of can filter by type and the panic-payload-* accessors can
     * read the panicked value/file/line.  type_tag is a TypeKind (stored as int
     * to keep env.h free of the compiler type headers); 0 == TY_NIL/none. */
    int          catch_panic_type;    /* TypeKind tag of the in-flight panic */
    TuriValue    catch_panic_value;   /* the panicked value (cstr for plain panic) */
    const char  *catch_panic_file;    /* source file (best-effort; may be NULL) */
    int          catch_panic_line;    /* source line of the panic */
    /* SI4: TypeClassEnv* from latest turi_eval; used by turi_try_show for Show dispatch.
     * Points into an eval_arena (never freed). Cast to TypeClassEnv* in eval.c. */
    void        *last_tc_env;
    /* RM Q#5: session-scoped reader-macro registry. Persists across REPL
     * turns so `(reader-macros/define ...)` on one line is visible to the
     * reader on the next. Allocated from sym_arena; entries' templates
     * point into eval_arenas (kept alive by TuriEnv until free). Opaque
     * to callers that don't include reader_macros.h. */
    struct ReaderMacroRegistry *reader_macros;
    /* RP3: loaded spice image (auto-discovered from cwd at REPL start).
     * NULL outside a project, or when --no-auto-spice was passed. Owned
     * by TuriEnv: freed by turi_env_free. Opaque to callers that don't
     * include spice_loader.h; the REPL's binding layer (RP4) walks it
     * to resolve `(import M :refer [...])`. */
    struct TurSpiceImage *spice_image;
    /* RP5: retired spice images held alive for the env's lifetime.
     * When (reload) swaps in a fresh image, the previous one stays
     * pinned because old TuriNativeFn bindings still reference its
     * strdup'd module/defn name strings via the globals hash table.
     * Freed in turi_env_free in reverse order. Opaque list node. */
    struct TurSpiceImageNode *retired_spice_images;
    /* Module-private name resolution (interpreter parity with the compiled
     * per-module mangling). `defining_mod` is the DefModule* whose body is
     * currently being evaluated (set by EX_DEFMODULE), so EX_FN_DEF can tell
     * exported from private defns. `current_module` is the module that owns the
     * closure currently executing (set by eval_apply), so a call to a private
     * name resolves to "<module>/<name>" before falling back to the bare name.
     * Both are borrowed Symbol/strdup'd strings with env (or longer) lifetime. */
    const void *defining_mod;     /* const DefModule* — opaque to avoid expr.h dep */
    const char *current_module;   /* owning module of the running closure, or NULL */
    /* Debugger Phase 2: when non-NULL, the eval loop yields to the interactive
     * debugger REPL on breakpoint / step. Allocated by turi_debug_enable,
     * reclaimed by turi_debug_disable / turi_env_free. Opaque (TuriDebugger* is
     * internal to turi/eval.c). A plain NULL check on the eval hot path keeps
     * non-debugger interp runs free of any per-node cost. */
    void       *debugger;
} TuriEnv;

/* Create a new unrestricted environment. */
TuriEnv *turi_env_new(void);

/* Create a sandboxed environment (I/O and FFI builtins disabled). */
TuriEnv *turi_env_new_sandboxed(void);

/* Free all resources owned by env. Any closures captured from it become
 * dangling after this call. */
void turi_env_free(TuriEnv *env);

/* turi-value-pool-residual-sites: register a coroutine execution stack (a
 * fiber's or generator's mmap'd/malloc'd stack) so it is reclaimed by
 * turi_env_free instead of leaking for the process lifetime. The node is
 * allocated from the env value pool; `base`/`size` are the dealloc args. */
void turi_env_track_coro_stack(TuriEnv *env, void *base, size_t size);

/* Look up a global binding by name.  Returns TURI_ERROR if not found. */
TuriValue turi_env_get(TuriEnv *env, const char *name);

/* Set (or replace) a global binding by name. */
void turi_env_set(TuriEnv *env, const char *name, TuriValue value);

#endif /* TURI_ENV_H */
