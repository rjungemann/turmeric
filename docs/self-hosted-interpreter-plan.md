# Turmeric Interpreter: Eval API and REPL

> **Status:** Speculative — Future Project  
> **Prerequisite:** Phase 19 complete (algebraic effects), stable Turmeric compiler  
> **Target:** v2 or later  
> **Related:** [turmeric-plan.md](turmeric-plan.md)

---

## Executive Summary

This document describes the design and implementation of a **Turmeric interpreter
runtime** whose two primary deliverables are:

1. **An importable `eval` API** — a C library (`libturi`) that any Turmeric program (or
   C program) can link against and call to evaluate Turmeric expressions at runtime.
   Enables live coding, scripting plugins, configuration DSLs, and user-extensible
   programs.

2. **A REPL** (`tur repl`) — an interactive read-eval-print loop built directly into
   the `tur` CLI, backed by the same `libturi` eval core.

Everything else — self-hosting, language dogfooding, comprehensive testing — is
a **secondary benefit** of having a correct eval core, not a goal in its own right.

---

## Primary Goals

### 1. Importable Eval

Any Turmeric program can call `eval` at runtime:

```scheme
(import turi/eval)

(defn main [] :int
  (let [result (eval "(+ 1 2)")]
    (println result)   ;; 3
    0))
```

From C (embedding):

```c
#include "turi/eval.h"

int main(void) {
    TuriEnv *env = turi_env_new();
    TuriValue result = turi_eval(env, "(+ 1 2)");
    printf("%lld\n", turi_as_int(result));
    turi_env_free(env);
}
```

The eval API supports:
- **Stateful environments**: define functions and values across multiple `eval` calls
- **Effect handling**: effects performed inside `eval` are handled by the caller's
  handler chain
- **Error propagation**: parse errors and runtime errors surface as Turmeric
  `Result`/exception values, not process aborts
- **Sandboxing**: `turi_env_new_sandboxed()` restricts I/O and FFI for untrusted input

### 2. REPL

`tur repl` launches an interactive session:

```
$ tur repl
Turmeric v0.x.0  (type :help for help, :quit to exit)
turmeric> (defn square [x :int] :int (* x x))
=> #<fn square>
turmeric> (square 7)
=> 49
turmeric> (defeffect Ask [] :cstr)
=> #<effect Ask>
turmeric> (handle (perform (Ask))
       ..   (Ask [] k) (resume k "hello"))
=> "hello"
turmeric> :quit
```

Features:
- Persistent environment across expressions
- Multi-line input with `..` continuation prompt
- `:doc symbol` — show type and docstring
- `:type expr` — print inferred type without evaluating
- `:reload file` — re-evaluate a source file into the current environment
- Readline integration (history, tab completion)
- Colour diagnostics matching `tur check` output

---

## Secondary Goals

These follow naturally from having a correct eval core and are worth implementing,
but they do not drive the design:

- **Self-hosting**: the interpreter can eventually be rewritten in Turmeric, providing
  language dogfooding and a large real-world test case
- **Scripting / shebang**: `#!/usr/bin/env tur repl --script` for small Turmeric scripts
  without a compile step
- **Test harness**: run Turmeric test suites through the interpreter to cross-check
  compiler semantics
- **Language server**: the eval core can back a live-evaluation feature in an LSP

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    libturi (eval core)                  │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐  │
│  │  Reader  │→ │  Elab*   │→ │  Tree-walk Evaluator │  │
│  │ (C, AST) │  │ (subset) │  │  (walk Expr tree)    │  │
│  └──────────┘  └──────────┘  └──────────────────────┘  │
│                                    │                    │
│                              ┌─────┴──────┐             │
│                              │  TuriEnv   │             │
│                              │(persistent │             │
│                              │  env map)  │             │
│                              └────────────┘             │
└─────────────────────────────────────────────────────────┘
        ↑                              ↑
  tur repl                  (import turi/eval) / C embed
```

*Elab\*: a read-only subset of the elaborator — type-checks expressions without
emitting C. The evaluator walks the elaborated `Expr` tree directly.*

### Key Components

| Component | Language | Purpose |
|---|---|---|
| Reader | C (existing `src/reader.c`) | Parse source → `Form` AST |
| Elab subset | C (thin wrapper over `src/elab.c`) | Type-check → `Expr` tree |
| Tree-walk evaluator | C | Walk `Expr` tree, produce `TuriValue` |
| `TuriEnv` | C | Persistent binding environment across eval calls |
| `libturi` | C shared/static library | Public eval API |
| REPL | C + optional Turmeric | `tur repl` subcommand |

### Value Representation

```c
typedef enum {
    TURI_NIL,
    TURI_BOOL,
    TURI_INT,
    TURI_FLOAT,
    TURI_CSTR,
    TURI_CLOSURE,
    TURI_STRUCT,
    TURI_EFFECT_CONT,   /* live continuation from handle/perform */
} TuriTag;

typedef struct TuriValue {
    TuriTag tag;
    union {
        bool       as_bool;
        int64_t    as_int;
        double     as_float;
        char      *as_cstr;
        void      *as_ptr;   /* closure, struct, continuation */
    };
} TuriValue;
```

### Public C API (`turi/eval.h`)

```c
/* Environment lifecycle */
TuriEnv *turi_env_new(void);
TuriEnv *turi_env_new_sandboxed(void);
void     turi_env_free(TuriEnv *);

/* Evaluation */
TuriValue turi_eval(TuriEnv *, const char *src);
TuriValue turi_eval_file(TuriEnv *, const char *path);

/* Error handling */
bool        turi_is_error(TuriValue);
const char *turi_error_message(TuriValue);

/* Value accessors */
bool        turi_as_bool(TuriValue);
int64_t     turi_as_int(TuriValue);
double      turi_as_float(TuriValue);
const char *turi_as_cstr(TuriValue);
```

### Turmeric-facing module (`turi/eval`)

```scheme
(import turi/eval)

;; Create an isolated evaluation environment
(let [env (Env/new)]
  (eval! env "(defn double [x :int] :int (* 2 x))")
  (eval  env "(double 21)"))   ;; => 42
```

---

## Implementation Phases

### Phase S0: Eval Core (C)

**Goal:** `libturi` compiles and `turi_eval` handles expressions through basic
arithmetic, `let`, `if`, `do`, `fn`, and `defn`.

**Exit criterion:** The REPL can evaluate `(+ 1 2)` and define a function.

- [x] `src/turi/eval.c` / `src/turi/eval.h` — tree-walk evaluator over `Expr` AST
- [x] `src/turi/env.c` / `src/turi/env.h` — persistent `TuriEnv` (hash map of
  `Symbol → TuriValue`)
- [x] `src/turi/value.c` / `src/turi/value.h` — `TuriValue` tagged union +
  accessors
- [x] Wire into existing `src/reader.c` + `src/elab.c` (read-only path)
- [x] `CMakeLists.txt`: build `libturi.{a,dylib,so}` alongside `tur`
- [x] `tur repl` subcommand skeleton: read line → `turi_eval` → print result

**Fixtures:**
- [x] `tests/turi/eval-basic.c` — C embedding smoke test

---

### Phase S1: REPL Polish

**Goal:** `tur repl` is usable for everyday exploration.

**Exit criterion:** Multi-line input, history, `:type`, `:doc`, and `:quit` work.

- [x] Readline/libedit integration (via FFI)
- [x] Multi-line continuation detection (open parens)
- [x] REPL meta-commands: `:help`, `:quit`, `:type <expr>`, `:doc <sym>`,
  `:reload <file>`
- [x] Pretty-printer for `TuriValue`
- [x] Colour diagnostics (reuse `src/diag.c`)
- [x] Persistent history file (`~/.tur_history`)

**Fixtures:**
- [x] `tests/turi/repl-smoke.sh` — scripted REPL session test

---

### Phase S2: Turmeric Import API

**Goal:** `(import turi/eval)` works from Turmeric programs.

**Exit criterion:** The factorial example in the appendix compiles and runs.

- [x] `stdlib/turi/eval.tur` — Turmeric bindings over `libturi` C API
- [x] `Env` struct wrapper with `Env/new`, `Env/new-sandboxed`, `eval`, `eval!`
- [x] Error type: `(deftype EvalError (ParseError cstr) (RuntimeError cstr))`
- [x] FFI declarations for `libturi` symbols
- [x] Linker flag injection so `(import turi/eval)` auto-links `-lturi`

**Fixtures:**
- [x] `tests/fixtures/eval-import/` — `eval` called from Turmeric source

---

### Phase S3: Effects and Continuations in Eval

**Goal:** `handle`, `perform`, `resume`, and `with-handler` work inside `eval`.

**Exit criterion:** Effect examples from the existing test suite run through `turi_eval`.

- [x] Effect handler stack threaded through `TuriEnv`
- [x] `TuriValue` variant for live continuations (`TURI_EFFECT_CONT`)
- [x] `perform` suspends the eval loop and invokes the nearest handler
- [x] `resume k v` re-enters the saved continuation
- [x] Async `await` deferred (async requires fiber scheduler — note in docs)

**Fixtures:**
- [x] `tests/turi/eval-effects.tur` — basic effect round-trip through eval

---

### Phase S4: Language Completeness

**Goal:** All non-async Turmeric features evaluate correctly.

**Exit criterion:** The Phase 1–19 test fixtures pass when run via `turi_eval`.

- [x] `defstruct` / struct literals / field access (`EX_MAKE_STRUCT`, `EX_GET_FIELD`, `TuriStruct`)
- [ ] Pattern matching (`case`)
- [ ] Typeclasses and instance resolution at runtime (dictionary-passing)
- [ ] Macros (re-use existing `src/interp.c` macro expander)
- [ ] Module loading (`import`) — evaluate imported file into child env
- [x] Exception handling (`try`/`catch`/`throw`) — `EX_THROW`, `EX_TRY`, `TuriThrow`, `env->throwing`
- [x] Defer (LIFO on env scope exit) — `EX_DEFER`, `DeferItem` stack, fired by `eval_apply`

---

### Phase S5: I/O, FFI, and Sandboxing

**Goal:** Eval can do I/O and call C; sandboxed eval blocks both.

**Exit criterion:** `(println "hi")` works; sandboxed env raises on `println`.

- [x] I/O primitives wired through `TuriEnv` capability flags (`env->sandboxed` gates `println` and FFI builtins)
- [x] `turi_env_new_sandboxed()` disables I/O and raw FFI
- [x] Inline-C blocks (`\`\`\`c ... \`\`\``) disabled in sandboxed mode (EX_INLINE_C returns error)
- [x] Resource limits: max eval depth via `env->max_eval_depth` (default 4096) with wrapper guard

---

### Phase S6: Self-Hosted Rewrite (Optional)

**Goal:** Rewrite the tree-walk evaluator in Turmeric itself, bootstrapped via the
Phase S0 C core.

This phase is **optional** and does not block any of the above. It is valuable as a
large real-world Turmeric program (dogfooding), but `libturi` is already useful
before it.

- [ ] `src/turi/eval.tur` — tree-walk evaluator in Turmeric
- [ ] Bootstrap: C core runs `eval.tur`, which then handles subsequent eval calls
- [ ] Performance parity with C evaluator within 2×
- [ ] All Phase S0–S5 fixtures pass through the self-hosted path

---

### Phase S7: Async/Await in Eval ✅ DONE

**Goal:** `(async ...)` and `(await ...)` work inside the interpreter with a
cooperative fiber scheduler.

**Exit criterion:** Async examples from the test suite run through `turi_eval`.

**Implementation details:**
- The fiber scheduler is event-loop based, not OS threads
- Futures can be composed: `(await (async ...))` chains correctly
- Async functions use the same `TuriEnv` and handler chain as sync code
- Errors in async tasks propagate as catchable exceptions through `try/catch`

#### S7.1: Fiber Scheduler Infrastructure ✅

- [x] `src/turi/fiber.c` / `src/turi/fiber.h` — `TuriFiber` struct with runnable stack, suspended state, and yield point
- [x] Stack allocation for fibers: fixed-size pool using `mmap`
- [x] Context save/restore using `ucontext_t` (POSIX) with `makecontext`/`swapcontext`
- [x] Ready queue in `TuriEnv`: linked list of runnable fibers
- [x] Suspended set in `TuriEnv`: fibers blocked waiting on an unresolved future
- [x] `turi_run_event_loop(env)` — run scheduler until all fibers complete or event loop is empty

#### S7.2: Future/Task Primitives ✅

- [x] `TuriFuture` struct: state (`PENDING` / `RESOLVED` / `REJECTED`), result `TuriValue`, waker list
- [x] `turi_future_new(env)` — allocate a pending future pinned to `TuriEnv`
- [x] `turi_future_resolve(future, value)` — settle future with a result, move wakers to ready queue
- [x] `turi_future_reject(future, error)` — settle future with an error
- [x] `turi_future_poll(future)` — non-blocking check
- [x] `TuriValue` variant for futures (`TURI_FUTURE`)
- [x] `turi_task_spawn` — create a new fiber from closure, return future handle
- [x] Waker mechanism: suspended fiber registers itself on the future

#### S7.3: `async` / `await` Expression Support ✅

- [x] Eval: `async` node pre-evaluates closure, creates fiber, returns `TURI_FUTURE` value
- [x] Eval: `await` node runs event loop (main context) or suspends fiber (async context)
- [x] `async` body captures the enclosing `TuriEnv` binding scope (closure semantics via `EX_CLOSURE`)

#### S7.4: Timer and Timeout Support ✅

- [x] Sorted linked list of timer events polled inside `turi_run_event_loop`
- [x] `(sleep-async ms)` — suspend current fiber for N milliseconds
- [x] `(with-timeout ms task)` — race task against a deadline; throws on timeout
- [x] Timer resolution uses `clock_gettime(CLOCK_MONOTONIC)`

#### S7.5: Task Composition ✅ (partial)

- [x] `(async-all2 t1 t2)` — join future that resolves when both tasks complete

#### S7.6: Error Propagation ✅

- [x] Unhandled exception in async task body settles the future with `turi_future_reject` carrying TURI_THROW
- [x] `(await task)` on a rejected future re-raises the stored exception in the awaiting fiber
- [x] `try/catch` around `(await ...)` correctly catches exceptions thrown inside async bodies

#### S7.7: Non-Blocking I/O ✅

- [x] Scheduler integrates a `poll()`-based I/O readiness check between fiber ticks
- [x] `(read-async fd n)` — suspend fiber until fd is readable; returns bytes as `:cstr`
- [x] `(write-async fd data)` — suspend fiber until write drains; returns bytes written
- [x] `(async-pipe-init)`, `(async-pipe-rfd)`, `(async-pipe-wfd)` — test pipe primitives

#### S7.8: Cancellation ✅

- [x] `cancel-task` sets the fiber's cancelled flag and rejects its future
- [x] `task-cancelled?` — predicate checks cancelled flag on current fiber
- [ ] Cancellation propagates through `async-race` (cancels losers) and `with-timeout` (cancels timed-out task)

#### S7.9: C API Additions

```c
/* Scheduler */
void      turi_run_event_loop(TuriEnv *);

/* Task / future */
TuriValue turi_task_spawn(TuriEnv *, const char *src);
void      turi_task_cancel(TuriValue task);
TuriValue turi_future_poll(TuriValue future);   /* TURI_FUTURE → result or pending */

/* Timer */
TuriValue turi_sleep_async(TuriEnv *, uint64_t ms);
```

**Fixtures:**
- [ ] `tests/turi/eval-async-basic.tur` — simple `async`/`await` round-trip; `(await (async (+ 1 2)))` = 3
- [ ] `tests/turi/eval-async-composition.tur` — `async-all`, `async-any`, chained awaits
- [ ] `tests/turi/eval-async-effects.tur` — effect handlers persist across `await` points
- [ ] `tests/turi/eval-async-timeout.tur` — `with-timeout` resolves correctly on slow and fast tasks
- [ ] `tests/turi/eval-async-cancel.tur` — cancelled tasks reject their futures; `task-cancelled?` returns true
- [ ] `tests/turi/eval-async-error.tur` — exception in async body propagates through `await`
- [ ] `tests/turi/eval-async-io.tur` — `read-async`/`write-async` round-trip on a pipe fd

---

### Phase S8: Polish and Documentation ✅ DONE

- [x] `docs/eval-api.md` — user guide for C embedding API and all public functions
- [x] `docs/repl.md` — REPL reference (meta-commands, multi-line input, output format)
- [x] `man/man1/tur-repl.1` — man page for `tur repl`
- [x] Performance: `env->globals` converted from O(n) linked-list scan to O(1) open-addressing hash table (`EnvHashTable` in `env.h`/`env.c`)
- [x] `libturi` packaged as installable library via CMake `install()` targets (library, headers into `include/turi/`, binary, man page)

---

## Key Design Decisions

### Decision 1: Eval walks elaborated `Expr` tree, not `Form` AST

Re-using the existing elaborator means the eval core gets type-checking, macro
expansion, and name resolution for free. The evaluator never sees raw forms.
Alternative — a separate interpreter over `Form` — would duplicate all that logic.

### Decision 2: `libturi` is a separate shared library, not baked into `tur`

This allows programs to link `libturi` without depending on the compiler binary.
The REPL and `(import turi/eval)` both use the same library.

### Decision 3: Effects in eval use a handler stack on `TuriEnv`

The handler stack is separate from the compiled-code handler chain
(`tur_current_fiber->effect_handler_chain`). Calling `turi_eval` from a compiled
handler body correctly places the eval handler stack "above" the compiled chain.

### Decision 4: Async deferred to a later phase

`(async ...)` and `(await ...)` require the fiber scheduler, which is complex to
thread through the evaluator. The eval core treats them as errors in Phase S0–S4;
Phase S5+ can add a single-threaded cooperative scheduler if needed.

### Decision 5: REPL lives in `tur` CLI, not a separate binary

Adding `tur repl` keeps distribution simple. A standalone `turi` wrapper can be
added later as a thin shim over `tur repl`.

---

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| Elaborator not reentrant | Medium | High | Audit global state; add `Elab` instance per eval call |
| Mutable globals in runtime.c | Medium | Medium | Thread env pointer through; audit on entry |
| Performance too slow for interactive use | Low | Medium | Eval only needs to be "fast enough" for REPL; JIT deferred |
| Effect continuation lifetime | Medium | High | Continuations pinned to `TuriEnv`; freed on `turi_env_free` |
| Self-hosted bootstrap complexity (S6) | High | Low | S6 is optional; skip if not needed |

---

## Success Criteria

### Minimum Viable (S0 + S1 complete)
- [ ] `tur repl` launches and evaluates arithmetic, `let`, `fn`, `defn`
- [ ] `turi_eval` linkable from C; embedding example in docs compiles and runs

### Useful (S2 + S3 complete)
- [ ] `(import turi/eval)` works from Turmeric programs
- [ ] Effects and continuations work inside `eval`
- [ ] REPL has readline, history, `:type`, `:doc`

### Complete (S4 + S5 complete)
- [ ] All non-async Phase 1–19 features work through eval
- [ ] Sandboxed eval mode available
- [ ] `eval-import` fixture in test suite passes

---

## Appendix A: Example Programs

### Eval from Turmeric

```scheme
(import turi/eval)

(defn run-user-expr [src :cstr] :int
  (let [env (Env/new-sandboxed)]
    (let [result (eval env src)]
      (case result
        (:ok v)  (v->int v)
        (:err e) (do (println "eval error:" e) -1)))))

(defn main [] :int
  (run-user-expr "(+ 40 2)"))  ;; 42
```

### Embedding in C

```c
#include "turi/eval.h"

int main(void) {
    TuriEnv *env = turi_env_new();

    turi_eval(env, "(defn double [x :int] :int (* 2 x))");

    TuriValue v = turi_eval(env, "(double 21)");
    printf("result: %lld\n", turi_as_int(v));  // 42

    turi_env_free(env);
    return 0;
}
```

### REPL Session

```
$ tur repl
Turmeric v0.x.0  (:help for help, :quit to exit)
turmeric> (defn fib [n :int] :int
       ..   (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
=> #<fn fib>
turmeric> (fib 10)
=> 55
turmeric> :type fib
fib : [:int] -> :int
turmeric> (defeffect Log [msg :cstr] :nil)
=> #<effect Log>
turmeric> (handle (do (perform (Log "hello")) 42)
       ..   (Log [msg] k) (do (println "[log]" msg) (resume k nil)))
[log] hello
=> 42
turmeric> :quit
```

---

*Last updated: 2026-05-13*
