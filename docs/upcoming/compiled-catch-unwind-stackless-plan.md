---
title: Stackless catch-unwind in the compiled backend (D3) -- Plan
category: Planning
description: Phase D3 of compiled-c-crossing-tco-plan. D1 (heap handler chain) and D1a (--enable=panic-return-signal) both lifted nested catch-unwind from a SIGSEGV below 50000 to ~150000 but no further, because the wall is frame COUNT -- two live C frames per nesting level -- not the jmp_buf. This plan scopes the stackless / partial-CPS lowering of the catch-unwind boundary that removes those frames, the only thing that reaches the 200000/1,000,000 target for the nested shape.
---

# Stackless catch-unwind (D3) -- Plan

## What D1 / D1a settled, and the wall they hit

[compiled-c-crossing-tco-plan.md](./compiled-c-crossing-tco-plan.md):

- **D1** moved handler discovery onto a thread-local chain of heap nodes that
  each own their `jmp_buf`. Nested `cu-rec` went from SIGSEGV below 50000 to
  ~150000.
- **D1a** (`--enable=panic-return-signal`) replaced the `longjmp` unwind with a
  `tur_panicking` return-path signal. Nested `cu-rec` still tops out at ~150000
  -- *no deeper than D1*.

The two together prove the wall is **frame count, not frame size**. For

```
(defn cu-rec [n :int] :int
  (if (= n 0) 0 (do (catch-unwind (fn [] : int (cu-rec (- n 1)))) n)))
```

every nesting level holds **two live C frames** that neither the heap `jmp_buf`
nor the return signal removes:

1. `cu-rec(n)`'s own frame, live because it must `return n` *after* the
   `catch-unwind` completes (the `catch-unwind` is in non-tail position -- there
   is a `do ... n` after it).
2. `tur_catch_unwind_box`'s frame, live because it must run code *after* the
   thunk returns -- pop the handler node and box the `ok`/`err` result.

`cu-catch-deep` (a single top-level catch over a deep non-tail `deep-panic`)
already reaches 200000 under both D1 and D1a, precisely because it has *one*
catch frame, not one per level. So D3 is specifically about the **nested**
shape: getting the two per-level frames off the native C stack.

## Why this is a backend change, not a peephole

The compiled backend lowers every call to a native C call on the C stack. To
make the `catch-unwind` continuation (`... n` after the catch; the pop+box after
the thunk) live somewhere other than the C stack, that continuation must be
**heap-allocated and driven by a trampoline** -- exactly the interpreter's
driver work-stack model (DK_CATCH_UNWIND in the C1 landing), ported to compiled
code. A trampoline is contagious: any function on a call chain that must stay
heap-bounded has to return control to the driver instead of C-calling, so its
own continuations become heap frames too. That is a **calling-convention
change** for the affected functions, comparable in scope to D1 and D2 combined
(as the parent plan notes).

## Prototype (landed) -- the trampoline reaches 20,000,000 with a flat stack

A hand-lowered C model of both shapes lives at
[prototypes/d3-stackless-catch-unwind.c](./prototypes/d3-stackless-catch-unwind.c)
(build/run instructions in its header). It integrates the shipped D1 heap
handler chain and the D1a `tur_panicking` signal, and drives the two per-level
frames onto a heap continuation chain stepped by a single flat trampoline loop.

Measured:

| Shape | Native compiled (D1/D1a) | Trampoline prototype |
| --- | --- | --- |
| `cu-rec` (nested catch, no panic) | SIGSEGV ~150000 | 20,000,000 OK |
| `cu-rec-p` (nested catch, per-level caught panic) | SIGSEGV ~150000 | 20,000,000 OK |
| either, under a **64 KiB** stack `ulimit` | dies almost immediately | 1,000,000 OK |

Running 1,000,000 deep under a 64 KiB stack is the categorical proof: the
recursion depth no longer touches the native C stack at all. This validates
direction (a)'s mechanism end-to-end and de-risks the codegen work.

### The validated driver shape

The trampoline is a two-action state machine over a heap continuation chain --
the compiled lowering emits exactly this, with the per-function segments named:

- **Continuation node** (`Cont`): a tagged heap record `{ tag, saved locals,
  boundary, next }`. `next` is the caller's continuation; `boundary` is the D1
  `tur_handler_node` this segment must pop when it resumes.
- **`DESCEND(n, k)`**: run a function segment. At a non-tail `catch-unwind`, push
  the boundary, heap-allocate a continuation node capturing the live locals and
  the current `k`, and `DESCEND` into the thunk body under it -- no C recursion.
- **`RETURN(v, k)`**: deliver a value to a continuation. The `catch-unwind`-after
  segment pops its boundary, consumes the `tur_panicking` signal into an
  `ok`/`err` box, restores its saved locals, and `RETURN`s to `k->next`.

Each source function that crosses a catch boundary splits into one segment per
`catch-unwind` site (entry segment + one after-segment each), plus its tail; the
codegen names them and threads live locals through the `Cont` node instead of the
C frame. `cu-rec` needs two segments (entry, after); `cu-rec-p` two (entry,
add1-after).

## Two directions (decide after prototyping -- prototype now done)

### (a) Selective CPS -- transform only functions that cross a catch boundary

Identify functions that (transitively) contain a `catch-unwind` in non-tail
position and CPS-transform *those* (and their non-tail callers up to the nearest
boundary), leaving the rest of the program on the ordinary native-call ABI. The
boundary between CPS'd and direct code is a shim that reifies the C continuation
as a heap closure on entry and re-enters the trampoline.

- Pro: bounded blast radius; hot non-catch code keeps the native ABI.
- Con: the CPS/direct boundary shim is fiddly; the "which functions" analysis
  must be conservative (a catch reachable through a higher-order call forces
  CPS on the callee).

### (b) Whole-program trampoline for the effect/async surface

Fold `catch-unwind` into the same machine D3 of the parent plan will build for
`async`/`await`/`handle` (heap-allocated continuations / a partial CPS transform
of the effect surface). `catch-unwind` becomes one more prompt on that machine.

- Pro: one mechanism for catch + effects + async; matches the interpreter's
  unified work-stack driver.
- Con: the largest piece; only worth it if D3-async lands the machine anyway.

Recommendation: **direction (a).** The prototype above confirms it reaches the
target on the self-contained `cu-rec`/`cu-rec-p` shapes; escalate to (b) only if
the async/effect D3 work is being built concurrently and the shared machine is
cheaper than two lowerings.

## Slices 1-5 (landed) -- codegen behind `--enable=stackless-catch-unwind`

The codegen of direction (a) is wired behind `--enable=stackless-catch-unwind`
(implies `panic-return-signal`; registered in `EXPERIMENTS[]`, fires TUR-W0060).
It recognises the self-recursive grammar over **1..TUR_SC_MAXP (8) scalar
params**, in either a result-**discarding** or result-**using** recursive branch:

```
(defn f [p0 : S0 ... pk : Sk] : R
  (if COND BASE (do  (catch-unwind (fn [] (f RECUR0 ... RECURk))) AFTER)))        ; discard
(defn f [p0 : S0 ... pk : Sk] : int
  (if COND BASE (let [r (catch-unwind (fn [] (f RECUR0 ... RECURk)))] AFTER)))    ; use r
```

where each `Si` and `R` is a scalar the trampoline can round-trip through an
int64 `saved[]` slot (`sc_scalar_kind`): `int` / `bool` / `cstr` / raw pointer
(via an `intptr_t` cast) and `float` / `float32` (via BIT reinterpretation --
`tur_sc_bits_f64` / `tur_sc_f64_from_bits`, and the f32 pair -- so the value
survives the int64 slot instead of being truncated by a cast). In the result-
**using** `let` form `AFTER` may inspect the bound catch result `r` through the
pure predicate accessors (`ok?` / `err?` / ...); an eligible function is
panic-free, so `r` is always `ok(<recursion value>)`, and the box is emitted as
`tur_box_ok(__v)` (int return only, since the box carries an int). That box
leaks one node per level exactly as the normal `tur_catch_unwind_box` result
does (the panic fixtures carry `requires.no-leak-check` for the same reason) --
a documented prototype trait, not a regression.

- **Eligibility** (`stackless_catch_eligible`, emit_fns.c): 1..8 scalar params
  and a scalar return with a known C return type, no ABI-spec / dict-clone, body
  is the `if/do/catch-unwind` shape above, the thunk is a 0-arg
  `EX_FN`/`EX_CLOSURE` whose captures are all params and whose body is a
  self-call with one arg per param, and COND/BASE/RECUR*/AFTER are "simple"
  (literals / vars / arithmetic-comparison **builtins** / cast / ascribe / if --
  crucially NOT a general call, which could panic and trip the driver-return).
  Everything else falls back to the normal (D1/D1a) emission.
- **Emit** (`emit_stackless_catch_body`): a `for(;;)` driver over a heap
  `tur_cont` chain (emitted in the preamble; `saved[TUR_SC_MAXP]` holds the
  level's params as int64 bits). Per-type save/restore go through `sc_save_expr`
  / `sc_restore_expr` (intptr_t cast for int/bool/ptr, `tur_sc_*` bit helpers
  for float); the return value likewise via the recorded `current_fn_ret_ctype`.
  RECUR args are computed into `__auto_type` temps (their own types) before any
  param is reassigned, since a later arg may read an earlier param. Reuses the
  D1 `tur_handler_node` chain for the boundary and the D1a `tur_panicking`
  signal for a caught panic.
- **Integration**: a first branch in `emit_fn_def`'s body-emit chain, so all the
  signature / param / ctx setup is reused and only the body is swapped.

Measured (compiled backend): with the flag, single-param `cu-rec` runs
**10,000,000** deep and **1,000,000 under a 64 KiB stack `ulimit`** (flat native
stack); two-param, `bool`-param, `float`-param and the result-using variants also
run flat (the result-using form validated to **200000** deep), and every shape
matches the native result at small depth (differential-checked, incl.
`bool`/`cstr`/`float`/`float32` params and returns, and the `let`-bound result
form). The default (flag-off) codegen is byte-identical and `bash tests/run.sh`
stays green (1958). Fixtures `stackless-catch-unwind-deep` (1 int param),
`-multiparam` (2 int params), `-scalar` (int + `bool`), `-float` (int + `float`)
and `-result` (result-using `let` form), each with a `flags` file enabling the
experiment, guard them.

### What is NOT done yet (follow-on)

- Extracting the caught **value** (`ok-val`/`err-val`) in the result-using form
  (only the pure predicates are whitelisted; `ok-val` on a `Panic` result also
  has an unrelated inference gap in the language today), and the `err` branch
  (dead for eligible functions, so untested).
- Carrier / opaque / aggregate params and returns (ownership / RC concerns, so
  they stay excluded).
- Panicking COND/BASE/RECUR/AFTER (a non-accessor call is held non-simple, so
  this is closed for the accepted grammar; a general lowering would need the
  driver to not early-return on the `panic-return-signal` check).
- Mutual recursion, and recursion through a catch that is not a direct self-call.
- The general segment-splitting emit for arbitrary catch-crossing functions
  (this slice special-cases one grammar rather than splitting an arbitrary body).

Graduation still needs the general lowering plus the fiber/effect/cancel
integration `panic-return-signal` also defers; until then it stays a prototype.

The next concrete step is generalising eligibility (multi-param, then arbitrary
catch-crossing bodies via real segment splitting), validated against the
compiled `cu-rec` / `cu-catch-deep` probes at 200000 then 1,000,000 (the D4
sign-off).

## Dependencies and reuse

- Builds on **D1a's return signal** as the propagation transport (a caught panic
  is already a return-path signal; the trampoline consumes it at the reified
  boundary instead of at a C frame). Graduating `panic-return-signal` should be
  coordinated with this work, and D3 should extend it to the fiber/effect/cancel
  unwinds it currently punts.
- Reuses the **heap handler node** from D1 as the reified boundary record; the
  node gains the heap continuation pointer the D1 bullet already anticipated
  ("a pointer to the target continuation").

## Validation / sign-off (mirrors D4)

- `cu-rec` and `cu-catch-deep` at 200000 during development, then 1,000,000 for
  the D4 sign-off, with no SIGSEGV, matching the interpreter's `eval-tco` probes.
- `bash tests/run.sh` green with the mechanism on (once it graduates from behind
  a flag).

## Out of scope

- Source-level semantics of `catch-unwind` (unchanged).
- The `async`/`await`/`handle` continuation rewrite itself -- direction (b) only
  *reuses* that machine if it exists; building it is the async half of D3.
