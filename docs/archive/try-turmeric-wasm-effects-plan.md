# Try Turmeric: WASM Effects Implementation Plan

## Background

Algebraic effects (`defeffect`, `perform`, `handle`, `resume`) are implemented in the
native Turmeric evaluator using `ucontext` — specifically `makecontext`, `swapcontext`,
and `getcontext` — to suspend and resume the body of a `handle` expression when a
`perform` is encountered.

In the Emscripten/WASM build, `ucontext.h` is not available. The current workaround in
`src/turi/fiber.h` stubs all three calls to no-ops inside `#ifdef __EMSCRIPTEN__`. As a
result, `(handle ...)` never actually suspends the body computation: `swapcontext` returns
immediately, `cont->done` is still `false`, `cont->perf_name` is NULL, and the evaluator
returns `#<error: eval: unhandled effect: (null)>`.

The fix requires replacing the ucontext stubs with a WASM-compatible coroutine primitive.
Emscripten provides two viable options; the tasks below choose between them and implement
the winner.

---

## Tasks

### I1 — Evaluate Emscripten Fibers vs Asyncify

Emscripten provides two paths for coroutine-style suspension in WASM:

- **Emscripten Fibers** (`emscripten/fiber.h`): explicit cooperative multitasking via
  `emscripten_fiber_init` / `emscripten_fiber_swap`. Conceptually a direct drop-in for
  `makecontext`/`swapcontext`. Requires `-sASYNCIFY=1` at link time.
- **Asyncify alone** (`-sASYNCIFY=1`): instruments the entire call graph so any function
  can be suspended and rewound. Higher code-size overhead but requires no restructuring of
  the C side.

`eval_handle` in `src/turi/eval.c` already has a clean `#ifndef __EMSCRIPTEN__` / `#else`
split for stack allocation (mmap vs malloc), making either path easy to slot in.

**Decision criteria:**
- Prefer Fibers if the overhead of full-graph Asyncify instrumentation is unacceptable for
  the WASM bundle size or init time.
- Prefer Asyncify-only if Emscripten Fibers prove unstable or require additional exported
  symbols that complicate the build.

**Output:** A short written decision (one paragraph) appended to this file before E1
begins.

---

### I1 — Decision: Emscripten Fibers

**Chosen approach: Emscripten Fibers** (`emscripten/fiber.h`). The existing algebraic-effect implementation already structures suspension and resumption as explicit cooperative swaps between two named contexts (`body_ctx` and `handler_ctx`). Emscripten Fibers map directly onto this model: `emscripten_fiber_init` replaces the `getcontext`/`makecontext` pair, and `emscripten_fiber_swap` is a direct substitute for `swapcontext`. The only structural change required is passing `cont` as the `void *arg` to the fiber entry point (removing the `g_pending_cont` side-channel) and allocating a per-context asyncify-stack buffer alongside each existing C stack. Full-graph Asyncify-only would instrument every function in the entire call graph to enable suspension from arbitrary depth, which significantly bloats code size and adds per-call overhead; Fibers incur that cost only for the fibers themselves while leaving all other code untouched.

---

### E1 — Add Asyncify to the WASM build

**File:** `src/CMakeLists.txt`

Both paths require `-sASYNCIFY=1`. Add it (and a reasonable stack size hint) to the emcc
flags in the `tur_wasm` custom target:

```cmake
-sASYNCIFY=1
-sASYNCIFY_STACK_SIZE=65536
```

Verify that the build still produces a working module for non-effect code (run the
existing passing smoke tests) before proceeding.

---

### E2 — Replace `ucontext` stubs with Emscripten Fibers in `src/turi/fiber.h`

**File:** `src/turi/fiber.h`

Inside the `#ifdef __EMSCRIPTEN__` block, replace the current no-op stubs:

```c
/* Current stubs — no-ops that break effects */
static inline int getcontext(ucontext_t *u) { (void)u; return 0; }
static inline int swapcontext(ucontext_t *o, const ucontext_t *n) { ... }
#define makecontext(u, fn, argc, ...) ((void)(u), (void)(fn), (void)(argc))
```

With real Emscripten fiber calls using a thin compatibility shim:

```c
#include <emscripten/fiber.h>

typedef emscripten_fiber_t ucontext_t;

static inline int getcontext(ucontext_t *u) {
    emscripten_fiber_init_from_current_context(u, asyncify_stack, ASYNCIFY_STACK_SIZE);
    return 0;
}
static inline int swapcontext(ucontext_t *from, ucontext_t *to) {
    emscripten_fiber_swap(from, to);
    return 0;
}
/* makecontext handled separately — see E3 */
```

The stack for the handler context (`cont->handler_ctx`) needs its own asyncify stack
buffer; allocate it alongside the body stack in `eval_handle`.

---

### E3 — Fix `g_pending_cont` thunk pattern for WASM

**File:** `src/turi/eval.c`

`eval_body_thunk` currently retrieves its continuation via a global `g_pending_cont`
because `makecontext` takes no user-data argument. `emscripten_fiber_init` accepts a
`void *arg` directly, removing the need for the global.

Changes:
- Update `emscripten_fiber_init` call (the WASM `makecontext` replacement) to pass `cont`
  as the user-data argument.
- Update `eval_body_thunk` to receive `cont` via its argument instead of reading
  `g_pending_cont`.
- Remove or guard the `g_pending_cont` global behind `#ifndef __EMSCRIPTEN__`.

---

### E4 — Export `eval_body_thunk` to the WASM function table

**File:** `src/turi/eval.c`

Emscripten Asyncify requires that functions used as fiber entry points are reachable from
the WASM function table so the instrumentation can correctly track them. Annotate
`eval_body_thunk` (and `async_fiber_thunk` if applicable) with `EMSCRIPTEN_KEEPALIVE`:

```c
#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE
static void eval_body_thunk(void *arg) { ... }
```

Alternatively, add the function names to `-sASYNCIFY_EXPORTS` in `CMakeLists.txt` if
annotating the source is not desirable.

---

### E5 — Verify effects round-trip with an isolated diagnostic test

**File:** `web/tests/diag.spec.js` (temporary) or a new `web/tests/effects-diag.spec.js`

Before touching the main smoke suite, add a focused Playwright test that evaluates only
the first effects test from `tests/turi/eval-effects.tur`:

```js
test('effects diagnostic — Ask perform/resume prints 42', async ({ page }) => {
    await page.goto('/');
    await waitForReady(page);
    await setCode(page, [
        '(defeffect Ask [] :int)',
        '(defn use-ask [] :int',
        '  (+ 1 (perform (Ask))))',
        '(println (handle (use-ask)',
        '  (Ask [] k) (resume k 41)))',
    ].join('\n'));
    await runAndGetConsole(page);
    await expect(page.locator('#console')).toContainText('42', { timeout: 10_000 });
});
```

This test must pass before E6 and E7 are attempted.

---

### E6 — Restore the effects smoke test in the main suite

**File:** `web/tests/smoke.spec.js`

Once E5 passes, add the effects test back to the `Examples` describe block using the
correct syntax (parentheses around `defeffect` and `defn`, which were missing in the
original version):

```js
test('effects — (perform Ask) resumes with 41 → prints 42', async ({ page }) => {
    await setCode(page, [
        '(defeffect Ask [] :int)',
        '(defn use-ask [] :int',
        '  (+ 1 (perform (Ask))))',
        '(println (handle (use-ask)',
        '  (Ask [] k) (resume k 41)))',
    ].join('\n'));
    await runAndGetConsole(page);
    await expect(page.locator('#console')).toContainText('42');
    expect(await hasNoEvalError(page)).toBe(true);
});
```

---

### E7 — Fix the effects example in `web/main.js`

**File:** `web/main.js`

The `EXAMPLES.effects` string has the same missing-parentheses bug as the original smoke
test. Update it so the Load Example dropdown actually runs:

```js
effects: `(defeffect Ask [] :int)

(defn use-ask [] :int
  (+ 1 (perform (Ask))))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))`,
```

---

## Implementation order

```
I1  (research: Fibers vs Asyncify)
 └── E1  (add -sASYNCIFY=1 to build; verify non-effect tests still pass)
      └── E2  (replace ucontext stubs with Emscripten Fiber shim)
           └── E3  (fix g_pending_cont / thunk arg passing)
                └── E4  (export eval_body_thunk to WASM table)
                     └── E5  (isolated effects diagnostic test)
                          ├── E6  (restore effects smoke test)
                          └── E7  (fix effects example in dropdown)
```

---

## Acceptance criteria

- `npx playwright test` in `web/` reports all smoke tests passing, including the effects
  test.
- The effects Load Example in the dropdown evaluates without error and prints `42`.
- The Asyncify-instrumented WASM bundle size increase is documented (before/after `.wasm`
  byte count noted in the PR).
- Non-effect evaluations (arithmetic, closures, factorial, fibonacci) show no measurable
  performance regression.
