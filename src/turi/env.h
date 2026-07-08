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

/* interp-collections-never-freed: an interpreter-created collection buffer
 * (currently a Vec; see below re: Set/Map).  The tree-walker allocates these
 * with raw calloc/malloc and hands back a bare TURI_INT carrier, so neither the
 * rc-drop path nor turi_env_free reclaims them -- they leak for the process
 * lifetime unless the program explicitly calls vec-free.  Each buffer is
 * registered here at native_*_new time and its `destroy` runs at turi_env_free,
 * bounding the create/teardown leak (and the interpreter harness leak gate).  An
 * explicit vec-free / set-free / map-free tombstones its node (box = NULL) so
 * teardown skips an already-freed buffer -- no double free.  The node itself is
 * pool-allocated (reclaimed with the env).
 *
 * Vec and Set/Map are both wired up.  A Vec box uniquely owns its data buffer,
 * so freeing each at teardown is trivially safe.  Set/Map (HAMT-backed) share
 * nodes across boxes, but each box owns exactly one reference to its persistent
 * HAMT, so a per-box tur_hamt_free at teardown reclaims shared structure through
 * the node refcounts (correct now that the delete path retains pulled-up
 * siblings -- docs/archive/hamt-delete-sibling-refcount.md). */
typedef void (*TuriCollBufFreeFn)(void *box);
typedef struct TuriCollBuf {
    void                *box;      /* wrapper allocation; NULL once freed/tombstoned */
    TuriCollBufFreeFn    destroy;  /* frees box (and any heap buffer it owns) */
    struct TuriCollBuf  *next;
} TuriCollBuf;

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

/* Per-env diagnostic sink callback (libturi-per-embed-env-and-peripherals
 * Gap 3).  Declared here because the function-pointer field lives on TuriEnv;
 * the public setter turi_env_set_diag_sink is in turi/eval.h.  `level` matches
 * DiagLevel; see eval.h for the field contract. */
typedef void (*TuriDiagSinkFn)(struct TuriEnv *env, int level, const char *code,
                               const char *file, uint32_t line,
                               uint32_t col_start, uint32_t col_end,
                               const char *message, void *ud);

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
    uint32_t    prior_toplevel;  /* Count of top-level PARSED forms from prior evals */
    /* Count of already-evaluated non-file-scope-def PROGRAM items from prior
     * evals. Distinct from prior_toplevel: a (load ...) form expands inline to
     * many program items, so parsed-form count and program-item count diverge.
     * The evaluation range must skip already-run PROGRAM items, so it is keyed
     * off this, not the parsed count -- otherwise a re-elaborated (load ...)
     * shifts the boundary and previously-run top-level forms get evaluated
     * again (e.g. a prior (gen-next g) double-advances a suspended generator). */
    uint32_t    prior_prog_items;
    ArenaNode  *eval_arenas;     /* Linked list of per-call arenas (never freed) */
    /* turi-env-owned-value-arena-pool-plan: dedicated pools for TuriValue heap
     * payloads (closures, structs, captured frames/bindings, cons cells, ...),
     * distinct from eval_arenas (which holds AST/elaboration memory). Created in
     * turi_env_new, reclaimed wholesale in turi_env_free, so an embedding host
     * gets leak-clean teardown. Allocate via the turi_val_* helpers in value.h.
     *
     * turi-value-pool-scratch-promotion-plan splits the single pool into two:
     *  - value_scratch : default target of every turi_val_* allocation. When
     *    scratch promotion is OFF (the default) it is never rewound and behaves
     *    exactly like the old single value_arena -- everything lives until
     *    turi_env_free.  When promotion is ON, it is arena_reset at each
     *    top-level eval boundary after escapees are copied out.
     *  - value_perm : receives the promoted deep-copies of values that must
     *    survive a scratch reset (result + globals). Empty until promotion runs. */
    Arena       value_scratch;
    Arena       value_perm;
    /* turi-value-pool-scratch-promotion-plan: opt-in bound on steady-state memory
     * for a single long-lived env (notebook-kernel pattern). When true, turi_eval
     * promotes every escaping value into value_perm and rewinds value_scratch at
     * each top-level boundary, so transient per-eval allocation does not
     * accumulate across evals. OFF by default: the per-unit-env embedding pattern
     * (create/eval/free) needs no promotion and the default path is unchanged.
     * Set via turi_env_set_scratch_promotion. */
    bool        scratch_promotion;
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
    /* SB3: step-fuel resource limit (0 in both fields = unlimited).  Since C4
     * (turi-c-scoped-forms-heap-bounding) retired the eval_depth recursion
     * guard, step-fuel is the sole per-eval resource limit (it bounds total
     * work regardless of recursion shape; recursion itself is heap-bounded). */
    uint64_t    step_fuel;        /* remaining fuel units; decremented each eval step */
    uint64_t    step_fuel_limit;  /* initial limit set by turi_env_set_fuel */
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
    /* interp-collections-never-freed: interpreter-created collection buffers
     * (currently Vec), tracked so turi_env_free reclaims the ones the program
     * never freed. */
    TuriCollBuf   *coll_bufs;
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
    /* libturi-per-embed-env-and-peripherals Gap 3: per-env diagnostic sink.
     * When non-NULL, turi_eval installs it into the process-global diag sink
     * for the duration of the call so this env's diagnostics route here instead
     * of stderr.  Set via turi_env_set_diag_sink. */
    TuriDiagSinkFn diag_sink;
    void          *diag_sink_ud;
    /* Gap 4: true when module_base_dir was set via turi_env_set_module_base_dir
     * (heap-owned, freed by turi_env_free).  Direct field assignments by the CLI
     * leave this false and retain their existing borrowed-pointer semantics. */
    bool        module_base_dir_owned;
    /* Gap 5: list of (free_fn, ud) finalizers for natives registered via
     * turi_env_register_native_ex.  Each fires once, in LIFO order, from
     * turi_env_free -- so an embedder can let a native's `ud` lifetime ride
     * along with the env it was registered on.  turi_env_reset does NOT fire
     * them (natives survive a reset).  Opaque node list (NativeFinalizer*,
     * defined in env.c). */
    void       *native_finalizers;
    /* Gap 7: per-env interpret-mode bit, snapshotted into the process-global
     * g_interpret_mode for the duration of each turi_eval on this env (restored
     * afterward).  Defaults to true (every turi_env_new caller is an interpreter
     * embedder).  Lets two co-resident libturi embedders -- one wanting
     * interpret-mode eval, one wanting compile-mode elaboration -- each see their
     * own setting per eval call without the deeper elaborator-threading refactor.
     * Set via turi_env_set_interpret_mode. */
    bool        interpret_mode;
    /* Gap 8: true when spice_image is BORROWED from another (prototype) env via
     * turi_env_set_shared_spice_image -- turi_env_free then leaves it alone
     * instead of calling tur_spice_image_free, so many per-script envs can share
     * one read-only loaded image rather than each carrying its own copy. */
    bool        spice_image_borrowed;
} TuriEnv;

/* Create a new unrestricted environment. */
TuriEnv *turi_env_new(void);

/* Create a sandboxed environment (I/O and FFI builtins disabled). */
TuriEnv *turi_env_new_sandboxed(void);

/* turi_env_new_with_natives is declared in turi/eval.h (it depends on
 * TuriNativeSpec / TuriNativeFn). */

/* Free all resources owned by env. Any closures captured from it become
 * dangling after this call. */
void turi_env_free(TuriEnv *env);

/* turi-value-pool-residual-sites: register a coroutine execution stack (a
 * fiber's or generator's mmap'd/malloc'd stack) so it is reclaimed by
 * turi_env_free instead of leaking for the process lifetime. The node is
 * allocated from the env value pool; `base`/`size` are the dealloc args. */
/* Returns the tracking node so callers (async fibers) can hold a back-pointer
 * for early reclaim; generators may ignore the return value. */
TuriCoroStack *turi_env_track_coro_stack(TuriEnv *env, void *base, size_t size);

/* interp-collections-never-freed: register an interpreter-created collection
 * buffer (a wrapper allocation, e.g. a Vec box) so turi_env_free reclaims it via
 * `destroy` if the program never freed it explicitly.  Returns the tracking
 * node; callers stash it inside the wrapper so an explicit free (e.g. vec-free)
 * can tombstone it in O(1) via turi_env_untrack_collection.  Returns NULL
 * (buffer untracked, no crash) when env or box is NULL. */
TuriCollBuf *turi_env_track_collection(TuriEnv *env, void *box,
                                       TuriCollBufFreeFn destroy);

/* interp-collections-never-freed: tombstone a tracking node whose buffer is
 * about to be freed explicitly, so the turi_env_free teardown walk skips it and
 * does not double-free.  NULL-safe. */
void turi_env_untrack_collection(TuriCollBuf *node);

/* Gap 8 (libturi-per-embed-env-and-peripherals): share one loaded spice image
 * across many per-script envs read-only, instead of each env auto-discovering
 * and owning its own copy.  Point every per-script env at a single prototype
 * env's already-loaded image; the borrowing env will NOT free it in
 * turi_env_free (the prototype owns it and must outlive every borrower).
 *
 * Pass `image` obtained from the prototype env's `spice_image` field (or NULL to
 * detach).  Borrowing replaces any image the env currently owns (that owned
 * image is freed first).  This is a measure-first hook: the report flags the
 * per-env image cost as speculative, so this gives embedders the sharing knob
 * without committing to copy-on-write arena machinery.  Safe only when no
 * borrower triggers `(reload)` on the shared image. */
void turi_env_set_shared_spice_image(TuriEnv *env, struct TurSpiceImage *image);

/* turi-value-pool-scratch-promotion-plan: opt into bounded steady-state memory
 * for a single long-lived env shared across many top-level evals (a notebook
 * kernel / long-lived REPL service).  With promotion ON, turi_eval deep-copies
 * every value that escapes a top-level eval (its result plus every global) into
 * a permanent pool and rewinds the scratch pool, so transient per-eval
 * allocations are reclaimed instead of accumulating.
 *
 * OFF by default -- the create-per-unit-of-work embedding pattern needs no
 * promotion and behaves exactly as before.  Enable only for the immortal-env
 * pattern.  Safe to toggle between top-level eval cycles, not from inside an
 * async/handler frame.  The promotion is conservative: when an eval leaves live
 * state the walk cannot prove safe to relocate (carrier-encoded pointer values,
 * live continuations/generators/fibers, pending async work), that eval's scratch
 * is left intact rather than corrupted -- it simply does not shrink that cycle. */
void turi_env_set_scratch_promotion(TuriEnv *env, bool enable);

/* Look up a global binding by name.  Returns TURI_ERROR if not found. */
TuriValue turi_env_get(TuriEnv *env, const char *name);

/* Set (or replace) a global binding by name. */
void turi_env_set(TuriEnv *env, const char *name, TuriValue value);

/* Helper for debugger: find an EnvBinding in env's globals. */
struct EnvBinding *turi_env_find_binding(TuriEnv *env, const char *name);

/* Helper for debugger: rebuild env's globals hash table after local overrides are cleared. */
void turi_env_rebuild_hash_table(TuriEnv *env);

#endif /* TURI_ENV_H */
