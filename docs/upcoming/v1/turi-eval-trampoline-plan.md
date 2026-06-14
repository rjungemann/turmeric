---
title: Turi Eval Trampoline / Explicit-Stack Evaluator Plan
category: Planning
description: Remove the tree-walking interpreter's dependence on the native C stack by converting `eval_expr` into an explicit-stack (trampolined) evaluator, so deep non-tail recursion runs to completion (or hits a heap-sized, configurable limit) instead of being capped by the C stack. Supersedes the stopgap of sizing `max_eval_depth` to the C stack.
---

# Turi Eval Trampoline -- Plan

## Status and scope

This is the long-form follow-up to **Direction D** of
[docs/archive/history/turi-deep-recursion-c-stack-overflow.md](../../archive/history/turi-deep-recursion-c-stack-overflow.md).

**Already landed (Direction A, the stopgap):** `turi_env_new` now derives
`max_eval_depth` from `getrlimit(RLIMIT_STACK)` divided by a conservative
per-`eval_expr`-frame cost (`src/turi/env.c`, `turi_default_max_eval_depth`),
so deep non-tail recursion produces a clean `eval: recursion limit exceeded`
instead of a raw SIGSEGV. The trade-off is that the achievable depth is now
capped well below the C-stack crash point (~300 logical recursion levels on a
12.5 MB stack, with a 2x safety margin below the measured ~650-level crash).

**This plan (Direction D)** removes the native-C-stack dependency entirely by
making evaluation iterative over an explicit, heap-allocated work stack. The
goal: deep recursion runs to completion bounded only by heap (and a much
higher, configurable logical limit), and `eval_depth` stops being a proxy for
C-stack nesting because there is no per-level C-stack growth to bound.

Out of scope: changing language semantics, the value representation, or the
async/fiber scheduler. This is purely a restructuring of the synchronous
`eval_expr` recursion into a driver loop.

## Why now / motivation

1. **Parity gap.** A program that recurses a few hundred deep runs fine
   *compiled* but errors (formerly crashed) under `--interpret` and in
   `tur repl`. Direction A turned the crash into a clean error but did not
   close the parity gap -- it lowered the achievable depth.
2. **Blocked fixtures.** `tests/fixtures/escape-deep-capture` builds up 5000
   non-tail frames to prove escape-continuation capture is O(1) at any depth.
   The escape *semantics* already work under `--interpret` (verified at
   50/500); only the 5000-frame build-up is unreachable. The fixture is parked
   `requires.compiled`. A trampoline lifts the ceiling past 5000 and lets the
   fixture rejoin the `run-turi.sh` allowlist.
3. **Foundation.** An explicit evaluation stack is also the natural substrate
   for first-class delimited continuations in the interpreter (reify the work
   stack slice between a `reset` and a `shift`), so this is enabling work for
   the broader turi continuation story, not just a robustness fix.

## Current architecture (what we are replacing)

The synchronous evaluator is a mutually-recursive tree walk over `Expr`
(pointers re-verified 2026-06-14 against `src/turi/eval.c`):

```
eval_expr        (eval.c:4036)  -- depth-guard wrapper, ++/-- eval_depth (:4046-4050)
  -> eval_expr_impl (eval.c:4054)        -- giant switch on e->kind
       -> eval_apply  (eval.c:4020)      -- function application
            -> eval_apply_inner (eval.c:3869) -- TCO trampoline loop (args_buf[64] :3873)
            -> eval_body_tco (eval.c:3651)    -- tail-position dispatcher (tco_args[64] :3842)
                 -> eval_expr ...         -- recurse (non-tail keeps the frame live)
```

Key facts that shape the design:

- `eval_expr_impl` is a single large `switch (e->kind)` whose stack frame
  reserves several `TuriValue args[MAX_EVAL_ARGS]` / `fields[MAX_EVAL_ARGS]`
  scratch arrays (`MAX_EVAL_ARGS == 64`). This is what makes each C frame
  expensive (~10 KB under Debug/ASan).
- `eval_depth` increments once per `eval_expr` call and is the faithful proxy
  for C-stack nesting today.
- `eval_body_tco` already implements **tail-call optimization** by looping
  instead of recursing on the final form of a body -- so tail recursion is
  *already* C-stack-flat. The crash is specifically **non-tail** recursion
  (e.g. `(+ 1 (sum-to (- n 1)))`), where the pending `(+ 1 _)` keeps a frame
  live across the recursive call.
- Control-flow signalling already uses out-of-band env flags
  (`env->returning` / `env->return_value`, `env->throwing` / `env->throw_value`,
  step-fuel), checked at the top of `eval_expr`. The trampoline must preserve
  these checks at each driver step.

## Scoping audit (2026-06-14)

Grounded measurements and a per-site map taken before starting T1, against
`./build/tur` (Debug + ASan) at this branch.

### Baseline measurements

- **Non-tail ceiling: ~312 logical levels.** A non-tail `sum-to`
  (`(+ n (sum-to (- n 1)))`) returns correctly at 300 but hits
  `eval: recursion limit exceeded` by ~312-315 (binary-searched). The error is
  the `eval_depth` guard (`eval.c:4046`), *not* a SIGSEGV -- Direction A holds.
- **Tail recursion is already flat.** `sum-acc 100000` (a proper tail call)
  returns `5000050000` with no ceiling: `eval_apply_inner`'s TCO loop
  (`eval.c:3878`) never grows the C stack. **The trampoline targets non-tail
  recursion only** -- tail calls are already O(1) C-stack.
- **Per-level C-stack cost is dominated by `TuriValue[64]` scratch.** `TuriValue`
  is a tagged union of 16 bytes (`value.h:40-54`), so each `args[MAX_EVAL_ARGS]`
  / `fields[MAX_EVAL_ARGS]` is **1 KB** of C frame. The depth model uses
  `TURI_EVAL_FRAME_BYTES = 12288` (`env.c:34`); `max_eval_depth` is
  `RLIMIT_STACK * 3/5 / 12288` (`env.c:43`), floored at 256.
- On the deep non-tail path, **three** of these 1 KB arrays are live per logical
  level simultaneously (see map below): `eval_expr_impl` EX_BUILTIN + EX_CALL,
  plus `eval_apply_inner`'s `args_buf`. That ~3 KB (plus ASan redzones + other
  locals) is what the 12 KB/level model captures.

### T1 site map (the `TuriValue[64]` scratch arrays to heap-spill)

| # | Site | `eval.c` | Hot on deep path? | Used in | Return paths to free at |
| - | --- | --- | --- | --- | --- |
| 1 | `eval_expr_impl` EX_BUILTIN `args` | :4259 | **yes** | final arg loop :4282-4286 only (short-circuit AND/OR return before touching it) | mid-loop `:4284`, tail `:4286` |
| 2 | `eval_expr_impl` EX_CALL `args` | :4402 | **yes** | :4407-4411 | mid-loop `:4409`, tail `:4411` (after `eval_apply` copies it) |
| 3 | `eval_apply_inner` `args_buf` | :3873 | **yes** | TCO loop work area, copied across iterations | every loop exit |
| 4 | `eval_body_tco` EX_CALL-tail `tco_args` | :3842 | tail-only | :3846-3856 | `:3849`, `:3854`, `:3856` |
| 5 | `eval_expr_impl` EX_MAKE_STRUCT `fields` | :4454 | cold | struct construction | construction + error paths |
| 6 | `eval_expr_impl` EX_PERFORM `args` | :4755 | cold | effect perform | perform + error paths |

**Recommended T1 shape:** keep a small inline buffer for the common low-arity
case and spill to the heap only above it -- avoids malloc/free churn on every
call while removing the 1 KB worst-case from the frame:

```c
TuriValue inl[8];
TuriValue *args = (n <= 8) ? inl : malloc((size_t)n * sizeof(TuriValue));
/* ... fill, use ... */
if (args != inl) free(args);   /* at EVERY return that can be reached after this */
```

`MAX_FN_ARITY` is 16, so calls (#2/#3/#4) never exceed 16; an inline-8 buffer
covers the overwhelmingly common 0-8 arg case with zero allocation and a 128 B
(not 1 KB) frame contribution. Sites #1/#5/#6 (builtins, struct fields, perform
args) can exceed 16, so the heap spill also lets us *drop* the
`n > MAX_EVAL_ARGS` hard error there. **The one correctness hazard is
free-on-all-paths**: each case has 2-3 `return`s reachable after the alloc
(tabulated above) -- a missed free is a leak the interpreter harness will not
catch (it runs `detect_leaks=0`), so audit against the table, not the sanitizer.

Validate T1 by re-running the ceiling probe (expect the ~312 ceiling to roughly
double) and `bash tests/run-turi.sh` green.

### T4 re-entrancy surface (natives that call back into eval)

The trampoline must keep these as *bounded* nested driver calls (C recursion
proportional to native-HOF nesting, not turi program depth):

- **`turi_call` (`eval.c:6349` -> `eval_apply`)** is the public re-entry point.
  Call sites: **8 in `eval.c`**, **29 in `src/main.c`** (the `wk_register_*`
  native HOFs and comparators: `map`/`fold` closures, `map_turi_eq_tramp`,
  backtrack `mbind`/`bt-apply-fat`, tuple2-eq, etc.). Each must invoke a fresh
  driver loop, not splice frames onto the caller's explicit stack.
- **`eval_apply` direct re-entry:** the fiber thunk (`eval.c:203`), the two
  `with-handler`/thunk evals (`:5395`, `:5441`), the EX_CALL tail (`:4411`), and
  `turi_call` (`:6353`).
- **Shallow re-entry guard (T4):** add a small counter so a pathological
  native-HOF nest errors cleanly rather than reintroducing unbounded C recursion.

### Phase status

- **T1 -- LANDED 2026-06-14.** Heap-spilled / shrank all six scratch arrays:
  EX_BUILTIN, EX_CALL, EX_MAKE_STRUCT use an inline-8 buffer with a heap spill
  above it (`EVAL_SCRATCH_INLINE`, also lifting the old `n > MAX_EVAL_ARGS`
  caps); the arity-bounded `eval_apply_inner` `args_buf`, `eval_body_tco`
  `tco_args`, and EX_PERFORM `args` shrank to a fixed `EVAL_MAX_FN_ARITY` (16)
  inline buffer (no heap -- EX_PERFORM's must stay inline because it is borrowed
  across the `swapcontext`). **Key finding:** the achievable depth was *not*
  frame-limited but **guard-limited** -- Direction A's `eval_depth` guard
  (`max_eval_depth`, sized via `TURI_EVAL_FRAME_BYTES = 12288`) tripped at ~311
  levels, well below the C-stack crash. So realizing T1's win required
  **re-tuning `TURI_EVAL_FRAME_BYTES`** to the now-smaller frame: the post-T1
  Debug+ASan crash moved from ~650 to ~1120 levels (sum-to), so the constant
  dropped 12288 -> 7168 (~1.25x of the measured ~5.8 KB real frame, preserving
  the ~2x crash margin). Result: ceiling **~311 -> ~531 levels**, beyond it a
  clean `recursion limit exceeded` (rc=1, never SIGSEGV); regression-clean
  (`run-turi.sh` 1186 passed, same 8 unrelated monomorphization fails; 14/14
  `eval|sandbox` ctests). The heap spill is `n <= 8`-inline so the common path
  allocates nothing; `eval_apply_inner` copies args at entry and frames copy
  TuriValues by value, so freeing the EX_CALL/builtin/struct containers after
  use is safe.
- **T2 -- STARTED 2026-06-14 (slice 1: EX_IF + EX_DO/EX_PROGRAM).** Added the
  explicit-stack driver `eval_drive` (`src/turi/eval.c`): a heap work-stack
  (`DriveCont`/`DRIVE_INLINE` inline-32 + spill) with a descend/return loop that
  flattens directly-nested EX_IF branch chains and EX_DO/EX_PROGRAM sequences
  off the C stack. `eval_expr_impl`'s EX_IF and EX_DO/EX_PROGRAM cases now
  delegate to it; every other kind (incl. EX_CALL/EX_BUILTIN) is still evaluated
  as a black box via the recursive `eval_expr`, so this is behaviour-preserving.
  Control-flow signals (error/returning/throwing) unwind the work-stack to the
  bottom; the EX_DO defer-skip-for-last-value rule is reproduced exactly.
  Validated: full `run-turi.sh` 1186 passed (same 8 unrelated monomorphization
  fails), 14/14 `eval|sandbox` ctests, early-`return` through nested if/do
  correct. A non-tail nested if-else chain ~800 deep now evaluates (was past the
  ~531 eval ceiling); beyond that the limiter is the **elaborator's** own
  recursion over deeply-nested source (a SIGSEGV at ~1000 nested forms in
  `elab_if`/`elab_form`), which is a *separate, pre-existing* limitation -- the
  elaborator is not trampolined and is out of scope for this plan; worth a
  follow-up if deeply-nested generated source becomes a real workload.
  **Remaining T2 slices:** EX_LET/EX_LETREC (frame + defer lifetime on the
  work-stack), EX_AND/EX_OR, EX_MAKE_STRUCT.
- **T2 slice 2 -- EX_LET / EX_LETREC -- LANDED 2026-06-14.** The driver now owns
  the new `EvalFrame` and its defer scope on the work-stack
  (`DK_LET_BIND` -> `DK_LET_BODY`): bindings evaluate in the owned frame, the
  body runs after them, and on the return phase each LET continuation runs its
  own cleanup (free frame; fire body defers on normal exit) -- so a control
  signal unwinding through it still frees the frame correctly. The blanket
  "unwind to DONE" was replaced with per-continuation signal handling. EX_LETREC
  pre-binds its names to nil and re-homes captureless fn literals onto the frame,
  matching the recursive version. Validated: `run-turi.sh` 1186 passed (same 8
  unrelated monomorphization fails); nested let / shadowing / letrec mutual
  recursion correct; early-`return` through a let frees the frame; non-tail
  let-defers fire LIFO (matching compiled, `defer-order` fixture green). Surfaced
  a **pre-existing, unrelated** interpreter divergence -- single-scope defers at
  *function exit* (tail position) fire FIFO instead of compiled LIFO, in the
  `fire_defers_to_mark_reversed` tail path T2 does not touch -- filed as
  [docs/reported/turi-tail-scope-defers-fire-fifo-not-lifo.md](../../reported/turi-tail-scope-defers-fire-fifo-not-lifo.md).
  **Remaining T2 slices:** EX_MAKE_STRUCT (EX_AND/EX_OR are not distinct kinds --
  they are `BS_AND_SC`/`BS_OR_SC` shapes inside EX_BUILTIN, handled at T3).
- **T2 slice 3 -- EX_MAKE_STRUCT -- LANDED 2026-06-14. T2 linear-form conversion
  complete.** Added `DK_MAKE_STRUCT`: the descend allocates a heap field
  accumulator hung off the work-stack (via the renamed multi-purpose `aux`
  pointer on `DriveCont`), evaluates each field in turn, and on completion calls
  `make_struct_val_def` and frees the accumulator; a control signal frees it and
  propagates. This is the same descend-and-accumulate shape T3 will use for call
  args, so it doubles as the T3 template. (The T1 inline-8 fast path no longer
  applies here -- the accumulator must persist across the per-field descents --
  so small structs now take one accumulator malloc; acceptable, pooling is a
  later perf item.) Validated: `run-turi.sh` 1186 passed (same 8 unrelated
  monomorphization fails), 14/14 `eval|sandbox` ctests, nested `make-struct` and
  early-`return` inside a field expr (accumulator freed) correct.
  **T2 is done** for every linear form the plan named; the next step is **T3**
  (fold `eval_apply`/`eval_body_tco` into the driver -- the non-tail
  FUNCTION-call ceiling remover -- which also subsumes the EX_BUILTIN and
  and/or short-circuit paths).
- **T3-T5** -- unchanged from the design below; T3 folds eval_apply into the
  driver (the actual non-tail FUNCTION-call ceiling remover); T5 makes
  `TURI_EVAL_FRAME_BYTES` dead code for the synchronous path.

## T3 scoping (2026-06-14)

Grounded against the current `src/turi/eval.c` after T1/T2 landed. T3 is the
headline -- it removes the non-tail FUNCTION-call ceiling -- and the largest,
riskiest slice. Post-T2 baseline: a non-tail `sum-to` still tops out at **~531
levels** (T2 did not touch calls).

### Why both EX_CALL *and* EX_BUILTIN must fold (the sum-to chain)

`(+ n (sum-to (- n 1)))` is the canonical non-tail recursion. Tracing one level
on today's code:

```
eval_drive(if)            -- if body driven (T2)
  -> else branch (+ n (sum-to ..))  is EX_BUILTIN -> driver default ->
     eval_expr(EX_BUILTIN)          [C frame]  evals arg1 ...
       -> eval_expr(EX_CALL sum-to) [C frame]
            -> eval_apply -> eval_apply_inner [C frame]  (TCO loop)
                 -> eval_body_tco(sum-to body = if) [C frame]
                      -> tail if -> else (+ ..) is default -> eval_expr(+)  ...repeat
```

So **per level** the live C frames are the `EX_BUILTIN(+)` evaluation *and* the
`eval_apply`/`eval_apply_inner`/`eval_body_tco` chain. The ceiling only drops
when **both** the builtin arg-eval and the whole call/apply/body path move onto
the work-stack. A partial fold (e.g. only EX_CALL) will not move the ceiling.

### Application machinery to fold (inventory, with pointers)

`eval_apply_inner` (`eval.c:3885`, the TCO loop) + `eval_apply` wrapper
(`:4044`) do, in order:

1. copy args to an arity-bounded buffer; `env->current_module = cl->module`
   (restored by the `eval_apply` wrapper) (`:3902`);
2. step-fuel check (`:3905`);
3. **native dispatch** -- `cl->native(...)`, no TCO (`:3912`) -- a *leaf*;
4. arity check with `skip_env_param`/`param_offset` (`:3917`);
5. **inline-C native override** -- `turi_env_get(fname)->native` (`:3926`) -- leaf;
6. **inline-C pattern executor** -- `try_exec_simple_inline_c` + the ADT/struct
   re-tag (`:3940`) -- leaf;
7. build `call_frame`, bind args (`:3986`);
8. save `defer_mark` / `was_returning` / `was_no_unwind`; `env->returning=false`;
   set `env->in_no_unwind` (`:3992`);
9. `eval_body_tco(call_frame, fn->body)` (`:3999`);
10. restore `in_no_unwind`; `fire_defers_to_mark_reversed(defer_mark)` (function
    -exit defers, FIFO -- see the defer-order report) (`:4001-4004`);
11. **TCO bounce**: if the body returned a `TcoFrame` (`:4007`), rebind args and
    loop (no C growth);
12. normal return: pick `return_value`/`throw_value`/`result`, restore
    `was_returning` (`:4024`).

`eval_body_tco` (`:3666`) is the **tail dispatcher**: it `goto restart`s on
tail-position EX_IF/EX_DO/EX_LET/EX_MATCH and emits a `TcoFrame` (`tco_bounce`,
`:106`) for a tail call; non-tail sub-exprs fall to `default -> eval_expr`
(`:3877`) -- which is exactly the recursion T3 removes.

### Design: a unified application loop in the driver

Add three continuations and evaluate the callee body *in the driver loop*:

- **`DK_BUILTIN_ARG`** -- accumulate EX_BUILTIN args (same shape as
  `DK_MAKE_STRUCT`), then call `eval_builtin` (a leaf). Also handles the
  `BS_AND_SC`/`BS_OR_SC` short-circuit (descend next operand, stop early on the
  decisive value). Removes the `EX_BUILTIN(+)` C frame from the chain.
- **`DK_CALL_ARG`** -- accumulate EX_CALL args (resolve the callee first), then
  "enter the callee".
- **Enter callee** -- steps 1-8 above. Native / inline-C-override / inline-C-
  pattern callees are **leaves** (call directly, push the value back into the
  loop, no body descend). A real closure: save module/returning/no_unwind/
  defer_mark into a **`DK_CALL_RET`** frame, build `call_frame`, bind args, and
  **descend `fn->body`** (so the body's non-tail sub-exprs use the loop).
- **`DK_CALL_RET`** -- steps 10/12 on body return: restore `in_no_unwind`, fire
  defers, restore module + `was_returning`, pick the return value; on a control
  signal, the same cleanup runs as it unwinds (mirrors the per-continuation
  cleanup added for `DK_LET_BODY` in T2).

### The crux: preserving TCO without `eval_body_tco`

Once the body descends in the driver, `eval_body_tco`'s tail handling no longer
runs, so the driver must reproduce it -- otherwise tail recursion grows the
*work-stack* linearly (heap-bounded but a regression of today's O(1) tail
calls). Approach: thread an explicit **tail flag** through the descend. The body
descends in tail mode; the T2 linear forms already pick their tail sub-position
(if->branches, do->last item, let->body, match->arm body), so tail-ness
propagates; non-tail positions (builtin args, `if` cond, non-last `do` items,
call args) descend in non-tail mode. A **tail** EX_CALL whose enclosing frame is
`DK_CALL_RET` does *frame reuse*: fire the current call's defers, rebind the new
callee's args into a fresh `call_frame`, and re-descend the new body -- i.e. the
`eval_apply_inner` loop expressed as in-place reuse of the `DK_CALL_RET` frame
(the existing `TcoFrame` mechanism can be retired or reused as the carrier).
Getting the **defer-firing order on tail calls** right (each iteration fires the
previous call's defers before reuse, per step 10) is the subtle part.

### Prerequisite / scope-cap: EX_MATCH (and EX_WHILE)

Callee bodies are very often an `EX_MATCH`; it is **not** yet driven
(`eval.c:4815` non-tail, `:3737` tail). If the body descends and hits a
black-boxed EX_MATCH, the arm body re-enters `eval_expr` and the ceiling returns
for match-recursive code (the common functional style). So **drive EX_MATCH**
(arm-body in the loop, tail-aware) as part of T3, or the ceiling only lifts for
`if`-recursive code. `EX_WHILE` (`:4421`) is an internal loop already C-flat; its
body sub-exprs still recurse via `eval_expr` -- lower priority.

### Re-entrancy (T4)

`turi_call` (`eval.c:6349`; 8 sites in eval.c, **29 in main.c** native HOFs /
comparators) must spawn a *fresh* driver loop per call so native-HOF nesting
costs bounded C recursion, not turi-program depth. Add a shallow re-entry guard.

### Suggested sub-slicing (each regression-green; ceiling drops only at the end)

1. **T3.0 -- drive EX_MATCH -- LANDED 2026-06-14.** Extracted the arm resolution
   (scrutinee + pattern scan + guards, all shallow via `eval_expr`) into
   `eval_match_resolve` and added a `DK_MATCH_BODY` continuation: the driver
   resolves the winning arm, then descends the arm *body* in the loop (freeing
   the arm frame on return, like `DK_LET_BODY`). `eval_expr_impl`'s EX_MATCH now
   delegates to `eval_drive`; `eval_body_tco`'s tail-match copy is untouched.
   Validated: `run-turi.sh` 1186 passed (same 8 unrelated monomorphization
   fails), 14/14 `eval|sandbox` ctests, all `adt-*` match fixtures green,
   `return` through an arm body correct. Needed so the T3 call fold flattens
   match-recursive callee bodies.
2. **T3.1 -- `DK_BUILTIN_ARG` -- LANDED 2026-06-14.** EX_BUILTIN now runs on the
   driver: regular builtins accumulate args on a heap buffer (via `aux`, like
   `DK_MAKE_STRUCT`) then apply `eval_builtin`; `BS_AND_SC`/`BS_OR_SC`
   short-circuit by descending operands and stopping on the decisive value
   (empty-and->true / empty-or->false preserved). Also shrank `DRIVE_INLINE`
   32->8 since the driver is now on hot paths (if/let/match/builtin) -- this kept
   the driver's C frame lean enough that routing EX_BUILTIN through it left the
   non-tail ceiling **unchanged at ~530** (no regression). Validated:
   `run-turi.sh` 1186 passed (same 8 unrelated monomorphization fails), 14/14
   `eval|sandbox` ctests, and/or short-circuit + side-effect ordering correct.
   Calls still recurse, so the ceiling stays ~530 until T3.2.
3. **T3.2 -- `DK_CALL_ARG` + `DK_CALL_RET` + body-in-loop + TCO**. The ceiling
   remover and the hard part (TCO + defers + module + no_unwind). Validate the
   `sum-to 5000` probe succeeds and `escape-deep-capture` runs. **Split:**
   - **T3.2a -- `DK_CALL_ARG` -- LANDED 2026-06-14.** EX_CALL now runs on the
     driver: it resolves the callee (fn_binding/fn_expr + recover_carrier_closure
     + reword_unbound_call_head), accumulates args on the work-stack (closure in
     `last`, accumulator in `aux`), then applies via the existing (recursive)
     `eval_apply`. Because the builtin *and* the call now share one `eval_drive`
     loop, the call's `eval_expr`/`eval_expr_impl` C frames vanish from the
     per-level chain, so the **non-tail ceiling ~doubled (~530 -> ~1064)** as a
     bonus -- and **TCO is preserved** (eval_apply's TcoFrame loop is untouched:
     `sum-acc 1_000_000` stays O(1)). Regression-green (1186 passed, same 8
     monomorphization fails; 14/14 `eval|sandbox` ctests; calls/recursion/
     closure-returning HOF correct). The arg-accumulation here is reused by:
   - **T3.2b -- non-tail body fold + tail-flag -- LANDED 2026-06-14. The non-tail
     ceiling is removed.** Used a **hybrid** that avoids reimplementing TCO:
     only **non-tail turi-body closures are folded** (a `DK_CALL_RET` saves
     module/returning/no_unwind/defer_mark, builds the call frame, and the body
     is descended in the loop with `tail=true`); **tail calls and leaf calls
     (native / inline-C) still go through `eval_apply`**, which already does O(1)
     TCO via its `TcoFrame` loop. A `bool tail` flag is threaded through the
     tail-transparent forms (if-branches, do last item, let body, match arm
     body) so an EX_CALL is classified tail vs non-tail; only non-tail folds.
     Because folded non-tail calls descend the body in the *same* `eval_drive`
     C frame (work-stack on the heap), deep non-tail recursion no longer grows
     the C stack -- it is **heap-bounded**.
     - Validated: **sum-to 5000 / 50000 / 500000 all run** (was capped ~1064);
       **TCO preserved** -- `sum-acc 1_000_000` (if-tail), `loop-let`/`loop-do`
       1_000_000 (let/do-tail) all O(1), no OOM; early-`return` propagates
       through folded calls (h(3)=342); defers fire; `run-turi.sh` 1186 passed
       (same 8 unrelated monomorphization fails); 14/14 `eval|sandbox` ctests.
     - **`escape-deep-capture` unparked** (T3.4): the 5000-non-tail-frame fixture
       that was `requires.compiled` (C-stack overflow) now PASSes under
       `--interpret`; its marker was removed so it runs on both paths.
     - Note: the `eval_depth` guard / `TURI_EVAL_FRAME_BYTES` still bound the
       *residual* C recursion (eval_apply tail-chain bodies' non-tail subexprs);
       folded non-tail recursion is unguarded but heap-bounded (a runaway hits a
       clean driver-stack OOM, never SIGSEGV). Retiring the guard for the
       synchronous path (T5) is no longer urgent.
4. **T3.3 -- re-entrancy audit + guard** (T4).
5. **T3.4 -- raise `max_eval_depth`, retire the `TURI_EVAL_FRAME_BYTES` stopgap,
   unpark `escape-deep-capture`** (T5).

### Risks

- **TCO regression**: easiest mistake is letting tail recursion grow the
  work-stack. Gate every step on a tail-recursive probe (`sum-acc 1_000_000`)
  staying O(1)/heap-flat.
- **Defer order at function exit**: the fold must keep
  `fire_defers_to_mark_reversed` semantics (and not "fix" the pre-existing
  single-scope FIFO bug as a side effect -- track that separately).
- **Leaf dispatch fidelity**: native / inline-C-override / inline-C-pattern
  paths (incl. the ADT/struct re-tag) must be reproduced exactly at "enter
  callee", or struct-returning inline-C silently miscompiles.
- **Scope**: this is the biggest single change to the hottest function. T1+T2
  plus the Direction-A guard remain a coherent safe resting point if T3.2 stalls.

## Design: explicit-stack CEK-style driver

Convert the recursion into a loop that drives an explicit stack of
**continuation frames** (work items describing "what to do with the value of
the sub-expression currently being evaluated"). This is a CEK-machine-shaped
refactor scoped to the synchronous evaluator.

### Core types

```c
/* What to resume after a sub-expression produces a value. One variant per
 * compound Expr kind that currently makes a nested eval_expr call. */
typedef enum {
    K_DONE,            /* bottom of stack: stash result, halt the loop      */
    K_IF_BRANCH,       /* test evaluated -> pick then/else                  */
    K_CALL_ARG,        /* nth arg evaluated -> eval next arg or apply       */
    K_BUILTIN_ARG,     /* like K_CALL_ARG but for primitive ops             */
    K_LET_BIND,        /* binding value evaluated -> bind, eval next/body   */
    K_DO_SEQ,          /* sequence element evaluated -> eval next           */
    K_MAKE_STRUCT_FLD, /* field evaluated -> eval next field or construct   */
    K_AND, K_OR,       /* short-circuit boolean chains                      */
    /* ... one per compound EX_* that recurses today ...                    */
} ContKind;

typedef struct ContFrame {
    ContKind   kind;
    EvalFrame *frame;       /* lexical env at this point                    */
    const Expr *expr;       /* the compound expr being decomposed           */
    int         index;      /* progress cursor (which arg/field/elem)       */
    TuriValue  *scratch;    /* HEAP-allocated arg/field accumulator         */
    int         scratch_len;
    /* small extra payload union for branch targets, bound names, etc.      */
} ContFrame;

typedef struct ContStack {
    ContFrame *items;
    size_t     len, cap;    /* grows via realloc -- bounded by heap         */
} ContStack;
```

The crucial change: `scratch` (the per-call argument accumulator that today is
a `TuriValue[64]` on the C stack) moves to the **heap**, sized to the actual
arity. That alone removes the dominant per-frame cost; the explicit stack then
removes the C recursion.

### Driver loop

```c
TuriValue eval_expr(TuriEnv *env, EvalFrame *frame, const Expr *e) {
    ContStack k; contstack_init(&k);
    contstack_push(&k, (ContFrame){ .kind = K_DONE });
    TuriValue cur = TURI_NONE;     /* "value being returned up the stack"   */
    const Expr *control = e;       /* "expr being evaluated down"           */
    EvalFrame *cf = frame;
    bool evaluating = true;        /* true: descend; false: returning a val  */

    for (;;) {
        if (env->returning || env->throwing) { /* propagate: pop to a handler */ }
        if (env->step_fuel_limit > 0 && env->step_fuel-- == 0) { /* error */ }

        if (evaluating) {
            switch (control->kind) {
            case EX_INT: cur = ...; evaluating = false; break;     /* leaf */
            case EX_IF:
                contstack_push(&k, (ContFrame){K_IF_BRANCH, cf, control, ...});
                control = control->if_test;       /* descend into test      */
                break;
            case EX_CALL:
                contstack_push(&k, (ContFrame){K_CALL_ARG, cf, control,
                                               .index=0, .scratch=heap_args(n)});
                control = control->call_args[0];  /* descend into arg 0      */
                break;
            /* ... */
            }
        } else { /* returning `cur` to the frame on top of the stack */
            ContFrame *top = &k.items[k.len-1];
            switch (top->kind) {
            case K_DONE: { TuriValue r = cur; contstack_free(&k); return r; }
            case K_IF_BRANCH:
                control = truthy(cur) ? top->expr->if_then : top->expr->if_else;
                cf = top->frame; contstack_pop(&k); evaluating = true; break;
            case K_CALL_ARG:
                top->scratch[top->index++] = cur;
                if (top->index < arity) { control = next_arg; evaluating = true; }
                else { /* all args ready: enter the callee */
                       /* push a K_DONE-like boundary, set up callee body,   */
                       /* free top->scratch, continue                        */ }
                break;
            /* ... */
            }
        }
    }
}
```

### Function application without C recursion

`eval_apply` + `eval_body_tco` fold into the driver: applying a closure pushes
the callee's body forms as a `K_DO_SEQ`-like continuation over a freshly bound
`EvalFrame`, and the existing TCO behaviour is preserved by *not* pushing a
return frame when the call is in tail position (the same condition
`eval_body_tco` checks today). Non-tail calls push a continuation; tail calls
reuse the current one -- which is exactly TCO, now expressed as "don't grow the
explicit stack."

### Native / inline-C / builtin boundaries

Native functions and the simple inline-C evaluator are leaves from the driver's
perspective: they are called directly (a bounded, non-recursive C call) and
their result is returned to the loop. The only subtlety is natives that
*re-enter* evaluation (e.g. higher-order natives that call a turi closure,
`map`/`fold`-style). Those re-enter via a nested driver-loop call -- a bounded
amount of C recursion proportional to the *nesting of native HOFs*, not to the
turi program's recursion depth, so it does not reintroduce the unbounded-stack
problem.

## New depth/limit semantics

- `eval_depth` no longer tracks C-stack nesting; it tracks **explicit-stack
  depth** (`k.len`). The guard stays, but `max_eval_depth` can be raised
  dramatically (heap-bounded) -- default to a large logical value
  (e.g. 1,000,000) with `turi_env_set_max_depth` still honored for sandboxes.
- `TURI_DEFAULT_SANDBOX_DEPTH` keeps its small value (256) -- sandboxes want a
  *logical* recursion cap regardless of the stack.
- The Direction-A `getrlimit`-derived sizing in `turi_default_max_eval_depth`
  becomes **dead code for the synchronous path** and should be removed (or
  repurposed only for the residual native-HOF re-entry recursion, which is
  shallow). Note this explicitly in the env.c comment when ripping it out.

## Implementation phases

1. **T1 -- Heap-allocate the arg/field scratch. LANDED 2026-06-14.** Changed
   `eval_expr_impl`'s `TuriValue args[MAX_EVAL_ARGS]` (and `fields`, perform
   args, and the TCO buffers) to an inline-8-with-heap-spill / fixed-16 scheme,
   plus the required `TURI_EVAL_FRAME_BYTES` re-tune (the limiter was the depth
   guard, not the frame). Roughly doubled the achievable depth (~311 -> ~531)
   on the *current* recursive evaluator -- shipped on its own; a good bisection
   point if T2 regresses. See **Phase status** above for the full write-up.
2. **T2 -- Driver loop for the leaf + linear forms.** Introduce `ContStack`
   and convert the non-application compound forms (`EX_IF`, `EX_DO`, `EX_LET`,
   `EX_AND`/`EX_OR`, `EX_MAKE_STRUCT`) to the loop, keeping `eval_apply` as a
   recursive call for now. Validate semantics with the full turi harness after
   each form group.
3. **T3 -- Trampoline function application.** Fold `eval_apply` /
   `eval_body_tco` into the driver, preserving the existing TCO condition.
   This is where the non-tail-recursion ceiling actually disappears.
4. **T4 -- Re-entrancy audit.** Enumerate every native/builtin that calls back
   into `eval_expr` and confirm each goes through a fresh bounded driver call;
   add a shallow re-entry guard so a pathological native-HOF nest still errors
   cleanly rather than crashing.
5. **T5 -- Raise the limit, drop the stopgap.** Bump default `max_eval_depth`,
   remove/repurpose `turi_default_max_eval_depth`, and unpark
   `escape-deep-capture` (drop `requires.compiled`, add to the allowlist).

## Validation

- **Regression gate at every phase:** `ASAN_OPTIONS=detect_leaks=0 bash
  tests/run-turi.sh` and the eval ctest targets
  (`ctest --test-dir build -R "eval|sandbox"`) must stay green. Baseline at this
  branch (2026-06-14): **1186 passed**; the 8 failures present are a separate
  in-flight monomorphization workstream (`m2b-default-of`, `rt-return-dispatch-*`,
  etc.), unrelated to the evaluator -- the gate is "no *new* failures," i.e. the
  pass count does not drop and no non-monomorphization fixture regresses.
- **Depth ceiling probe** (the report's repro), expected to *succeed* (not
  error) at 5000 after T3:
  ```sh
  ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret /tmp/deeprec.tur  # sum-to 5000
  ```
- **No SIGSEGV under any build** for arbitrarily deep non-tail recursion:
  beyond the (now heap-bounded) limit it must print `eval: recursion limit
  exceeded`, rc nonzero -- never rc 139.
- **escape-deep-capture** runs green under `--interpret` once T5 lands.
- **Compiled suite:** `bash tests/run.sh` (~1442 fixtures) unaffected -- this
  change is interpreter-only and touches no codegen, so fixture snapshots must
  not move.

## Risks and trade-offs

- **Scope.** This is a large, invasive rewrite of the hottest function in the
  interpreter. The phased plan (T1 shippable alone; T2/T3 form-group by
  form-group behind the harness) is the mitigation. If T2/T3 stall, T1 plus the
  already-landed Direction-A guard is a coherent, safe resting point.
- **Performance.** An explicit stack with heap scratch may be slower than the C
  recursion for shallow programs (allocation churn). Mitigate by pooling
  `ContFrame.scratch` allocations (free-list keyed by arity) and only spilling
  to the heap above a small inline threshold.
- **Continuations.** The explicit stack is a prerequisite for, but does not by
  itself deliver, first-class delimited continuations in the interpreter --
  that is separate follow-up work (reifying stack slices), noted here only as
  the strategic payoff.

## Cross-references

- Report / origin: [docs/archive/history/turi-deep-recursion-c-stack-overflow.md](../../archive/history/turi-deep-recursion-c-stack-overflow.md)
- Broader interpreter parity tracking: [turi-parity-post-v1-plan.md](../turi-parity-post-v1-plan.md), [turi-interpreter-gap-closure-plan.md](../../archive/history/turi-interpreter-gap-closure-plan.md)
- Parked fixture: `tests/fixtures/escape-deep-capture` (`requires.compiled`)
