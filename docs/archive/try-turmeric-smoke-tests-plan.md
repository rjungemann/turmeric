# Try Turmeric: Smoke Test Fixes Plan

## Overview

The web REPL at `web/` has 14 Playwright smoke tests. As of this writing, 5 pass and 9
fail. This document describes the prerequisite investigations and fixes needed to get all
14 tests passing, in dependency order.

Current passing tests:
- page loads and WASM initializes
- evaluates `(+ 1 2)` and shows result 3
- evaluates println and shows output
- REPL history navigates with arrow keys
- hello world — prints Hello, Turmeric!

Current failing tests:
- REPL input evaluates expression in same context
- math — last expression `(** 2 8)` returns 256
- factorial — `(factorial 10)` returns 3628800
- fibonacci — `(fib 10)` returns 55
- type annotations — `(add 1 2)` returns 3
- higher-order — `(twice inc 5)` returns 7
- closures — `make-adder(5)(10)` prints 15
- effects — `(perform Ask)` resumes with 41 → prints 42
- pattern matching — `factorial(5)` returns 120

---

## Prerequisite Tasks

These must be resolved before or alongside the test fixes. Each prerequisite is an
investigation that determines what the correct fix is.

### P1 — Diagnose "table index is out of bounds" in multi-call WASM contexts

**Symptom:** When `(defn double [x :int] (* x 2))` is evaluated via the editor and then
`(double 21)` is submitted via the REPL input (a second `turi_wasm_eval` call), the
runtime throws a WebAssembly `RuntimeError: table index is out of bounds`.

**What to investigate:**
- Determine whether this is caused by a fixed-size WASM function table being exhausted.
  `-sALLOW_TABLE_GROWTH=1` was added to the emcc flags in `src/CMakeLists.txt`; verify
  whether this flag actually changes the `.wasm` binary or only the `.js` glue, and
  whether the error still occurs after rebuilding.
- If the error persists, check whether Turmeric closures store a C function pointer that
  goes through a WASM `call_indirect` instruction when invoked (Emscripten routes C
  function pointers through the function table). If so, the table may overflow when
  closures are created across multiple eval calls.
- Check whether closures created in a single `turi_wasm_eval` call (i.e. `defn` and the
  call site in the same source string) also hit this error. If not, the issue is specific
  to cross-call closure invocation.
- Review `eval_apply` in `src/turi/eval.c:702` to see whether `cl->native` function
  pointer dispatch goes through a WASM indirect call.

**Files:** `src/CMakeLists.txt`, `src/turi/eval.c`, `web/tests/smoke.spec.js`

**Expected outcome:** Either confirm that `ALLOW_TABLE_GROWTH=1` fixes the issue (and
rebuild+retest), or identify the specific C function pointer that overflows the table and
apply the correct Emscripten annotation (`EMSCRIPTEN_KEEPALIVE`, `EM_JS`, table
pre-sizing, etc.).

---

### P2 — Audit which operators are valid in the WASM evaluator

**Symptom:** The math smoke test sends `(** 2 8)` and `(% 10 3)` but these may not be
valid Turmeric infix builtins. Looking at `src/turi/eval.c` around the infix builtin
dispatch (`eval: unknown infix builtin '%s'`), it is unclear whether `**` (exponentiation)
and `%` (modulo) are implemented.

**What to investigate:**
- Search `src/turi/eval.c` for the list of recognised infix operators. Confirm whether
  `**`, `%`, and `/` are present.
- Determine what integer division and modulo look like in Turmeric (e.g. `(/ 100 4)`
  returns `25` or `25.0`?). The tutorial YAML notes "Division returns a float in
  Turmeric".
- Check what the `repr` format is for integer vs float results (e.g. is `3` printed as
  `3` or `3.0`?).
- Run a minimal WASM eval of `(+ 1 2)`, `(* 3 4)`, `(/ 10 2)` via a diagnostic test and
  inspect the raw console text to confirm repr format.

**Files:** `src/turi/eval.c`, `web/tests/smoke.spec.js`

**Expected outcome:** A definitive list of working arithmetic operators and their output
format, used to rewrite the math smoke test with correct inputs and expected substrings.

---

### P3 — Verify that `defn`-defined functions can be called in a single `turi_wasm_eval`

**Symptom:** Tests for factorial, fibonacci, type annotations, higher-order functions, and
closures all define a function with `defn` and call it in the same code block. It is
unknown whether these return the expected values or silently return `nil`/error in the
current WASM build.

**What to investigate:**
- Write a minimal diagnostic Playwright test (or use `page.evaluate` inline) that sends
  `(defn id [x :int] x)\n(id 7)` to `turi_wasm_eval` and reads back the raw result
  string before it goes through the `isError`/`nil` filter in `runCode`.
- Check whether the return value repr for an integer is `"7"` or `"7.0"`, and whether
  the function call produces a value at all.
- If the call silently returns `nil` or errors, check whether it is the same
  "table index" issue from P1 or a separate elaboration/evaluation failure.

**Files:** `src/turi/eval.c`, `web/main.js:384`, `web/tests/smoke.spec.js`

**Expected outcome:** Confirmed working invocation pattern and correct expected output
strings for use in the fixed smoke tests.

---

### P4 — Determine whether algebraic effects work in the WASM build

**Symptom:** The effects test uses `(defeffect Ask [] :int)`, `(perform (Ask))`, and
`(handle ...)`. In `src/turi/fiber.h`, `makecontext` is stubbed out to a no-op for the
WASM target, which may break continuation-based effect handling entirely.

**What to investigate:**
- Check `src/turi/fiber.h` for the `makecontext` stub and determine which code paths in
  `eval.c` depend on it (search for `makecontext`, `swapcontext`, `getcontext`).
- Try evaluating the effects test code from `tests/turi/eval-effects.tur` via
  `turi_wasm_eval` and observe whether it errors, panics, or succeeds.
- If effects are broken in WASM, decide: (a) fix WASM effects support (non-trivial, may
  require Asyncify or a fibre emulation layer), or (b) remove/skip the effects smoke test
  and document the limitation.

**Files:** `src/turi/fiber.h`, `src/turi/eval.c`, `tests/turi/eval-effects.tur`,
`web/tests/smoke.spec.js`

**Expected outcome:** Either a confirmed fix path for WASM effects, or a decision to
remove the effects test from the smoke suite with a documented limitation.

---

### P5 — Determine whether `match` wildcards work in the evaluator

**Symptom:** The pattern matching test uses `_ ` as a wildcard arm in `(match n 0 1 1 1
_ (...))`. The test failure shows `error: unbound symbol '_'`, meaning `_` is not
currently treated as a wildcard in the evaluator.

**What to investigate:**
- Search `src/turi/eval.c` and `src/turi/elab.c` for `match` expression handling and
  check whether `_` is a special-cased wildcard or just a regular variable name.
- Check whether there is a different wildcard syntax (e.g. `else`, `:else`, a named
  binding with no use).
- If `_` is intentionally unsupported as a wildcard, rewrite the test using the correct
  syntax. If it should be supported, file or reference an issue and skip the test until
  the fix lands.

**Files:** `src/turi/eval.c`, `src/turi/elab.c`, `web/tests/smoke.spec.js`

**Expected outcome:** Either a working wildcard syntax to use in the test, or a decision
to remove the pattern matching smoke test until the feature is implemented.

---

## Test Fix Tasks

Each task depends on the prerequisite(s) listed. Complete the prerequisite investigation
first, then apply the fix.

### T1 — Fix: REPL evaluates expression in same context

**Depends on:** P1

**Current failure:** Calling `(double 21)` from the REPL after defining `double` in the
editor produces `RuntimeError: table index is out of bounds`.

**Fix options (choose after P1 investigation):**
1. Confirm `-sALLOW_TABLE_GROWTH=1` in `src/CMakeLists.txt` resolves the issue, rebuild
   with `just wasm`, and rerun the test.
2. If the issue is specific to cross-call `cl->native` dispatch, apply the appropriate
   Emscripten export annotation and rebuild.
3. If unfixable in the short term, update the test to call `(* 21 2)` directly (no
   `defn`) as a regression-safe stand-in, and open a tracking issue.

**Test location:** `web/tests/smoke.spec.js:67`

---

### T2 — Fix: math — last expression returns 256

**Depends on:** P2

**Current failure:** The test sends `(** 2 8)` and expects "256" in the console. Either
`**` is not a valid operator or the output format differs.

**Likely fix:** Replace the test code with operators confirmed working in P2. For example,
if `**` is unsupported, use `(* (* 2 2) (* 2 2) (* 2 2) (* 2 2))` or a helper `defn`; or
change the expected value to match whatever `(* 2 3 4)` (= 24) actually outputs. Keep the
test simple — the goal is to verify that multi-expression evaluation returns the last
value.

**Test location:** `web/tests/smoke.spec.js:130`

---

### T3 — Fix: factorial — `(factorial 10)` returns 3628800

**Depends on:** P1, P3

**Current failure:** Evaluation appears to echo the code but produce no visible result in
the console within the assertion timeout.

**Likely fix:** Once P3 confirms the correct repr and any table-growth fix from P1 is in
place, the test should pass as-is. If the repr is `3628800.0` instead of `3628800`, update
`toContainText` to `'3628800'` (it is a substring match so both would pass). If the
evaluation is silently returning `nil`, investigate the `runCode` filter at
`web/main.js:732` (`result !== 'nil'`).

**Test location:** `web/tests/smoke.spec.js:144`

---

### T4 — Fix: fibonacci — `(fib 10)` returns 55

**Depends on:** P1, P3

**Current failure:** Same pattern as T3 — code echoed, result absent.

**Likely fix:** Same as T3. If the recursive call works after P1/P3 fixes, this test
passes without code changes. `(fib 10)` = 55; repr should be `"55"`.

**Test location:** `web/tests/smoke.spec.js:155`

---

### T5 — Fix: type annotations — `(add 1 2)` returns 3

**Depends on:** P3

**Current failure:** `(defn add [a :int b :int] :int (+ a b))` followed by `(add 1 2)`.

**Likely fix:** Same as T3. This is the simplest function-call test; if P3 confirms
single-call `defn`+invocation works, this test should need no changes.

**Test location:** `web/tests/smoke.spec.js:166`

---

### T6 — Fix: higher-order — `(twice inc 5)` returns 7

**Depends on:** P1, P3

**Current failure:** Higher-order function `twice` takes a function argument `f` and calls
`(f (f x))`. This involves passing a function value and calling it twice.

**Likely fix:** Confirm that function values can be passed as arguments and invoked via
`eval_apply` without hitting the table-growth issue. If P1's fix resolves the table
growth, this test should pass. If `(fn [x :int] :int (+ x 1))` is needed as the type
annotation for `inc`, verify the correct syntax.

**Test location:** `web/tests/smoke.spec.js:176`

---

### T7 — Fix: closures — `make-adder(5)(10)` prints 15

**Depends on:** P1, P3

**Current failure:** `make-adder` returns a closure `(fn [y :int] (+ x y))` which captures
`x`. Calling `(add5 10)` invokes the closure.

**Likely fix:** Closures capturing free variables are the most likely trigger for the
WASM function-table issue. Confirm with P1. If `ALLOW_TABLE_GROWTH=1` resolves it, the
test should pass as-is.

**Test location:** `web/tests/smoke.spec.js:187`

---

### T8 — Fix: effects — `(perform Ask)` resumes with 41 → prints 42

**Depends on:** P4

**Current failure:** `defeffect`, `perform`, and `handle` all produce "unbound symbol"
errors, suggesting the effect syntax in the test is missing required parentheses (the
test has `defeffect Ask [] :int` without surrounding parens, but the language requires
`(defeffect Ask [] :int)`).

**Likely fixes (two separate issues):**
1. **Syntax fix** — Wrap `defeffect` and `defn` in parentheses in the test code:
   ```js
   '(defeffect Ask [] :int)',
   '(defn use-ask [] :int',
   '  (+ 1 (perform (Ask))))',
   '(println (handle (use-ask)',
   '  (Ask [] k) (resume k 41)))',
   ```
2. **Runtime fix** — If after the syntax fix effects still fail because `makecontext` is
   stubbed in the WASM build (per P4), either implement WASM-compatible continuation
   support or remove this test from the smoke suite with a documented limitation.

**Test location:** `web/tests/smoke.spec.js:198`

---

### T9 — Fix: pattern matching — `factorial(5)` returns 120

**Depends on:** P5

**Current failure:** `match` uses `_` as a wildcard arm but the evaluator reports "unbound
symbol '_'".

**Likely fixes:**
- If P5 reveals a correct wildcard syntax (e.g. using a named binding), update the test.
- If `_` support needs to be added to the elaborator/evaluator, file a tracking issue and
  rewrite the test using explicit base cases (`0` and `1`) without a wildcard arm:
  ```js
  '(defn factorial [n :int] :int',
  '  (if (<= n 1) 1 (* n (factorial (- n 1)))))',
  '(factorial 5)',
  ```
  This removes the `match` dependency and tests the same computation.

**Test location:** `web/tests/smoke.spec.js:211`

---

## Implementation Order

```
P2 (operators audit)
  └── T2 (math test)

P5 (match wildcard)
  └── T9 (pattern matching test)

P4 (effects in WASM)
  └── T8 (effects test)

P1 (table index diagnosis)
  ├── T1 (REPL same-context)
  ├── T6 (higher-order)
  └── T7 (closures)

P3 (defn single-call verify)   ← depends on P1 result
  ├── T3 (factorial)
  ├── T4 (fibonacci)
  └── T5 (type annotations)
```

Start with P2, P4, P5 in parallel (no dependencies between them). Then P1. Then P3 once
P1 is resolved.

---

## Acceptance Criteria

- `npx playwright test` in `web/` reports 14/14 passing with no flakes on three
  consecutive runs.
- The dev server is started fresh (no `reuseExistingServer` cache) for the final
  verification run.
- Each test assertion uses the exact string Turmeric actually outputs (verified by
  reading the raw console DOM text, not assumed).
