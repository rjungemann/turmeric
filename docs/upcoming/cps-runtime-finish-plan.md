---
title: "CPS/DK runtime -- the finish plan (N6.5 endgame: delete the fallback, retire fibers for colored code)"
category: Planning
status: open -- the finishing sequence. Control-flow gaps are closed; what remains is a measured BODY-* coverage grind + leak discipline, then the fallback deletion. Readiness gate = TUR_TRACE_EVICT shows only SIG-* across the corpus.
description: One consolidating plan to drive the CPS/DK backend from its current half-finished state to done. The endgame is N6.5 -- make the CPS/DK backend the SOLE lowering for colored (may-capture / effectful) functions, delete the direct/fiber whole-function fallback, and hard-error residual forms. This plan states, with evidence, what is already finished, the exact remaining eviction surface, the ordered slices to empty it, the leak discipline needed for an ASan-clean finish, and the deletion + verification steps. It supersedes the scattered N6 follow-up docs as the single execution track.
---

# CPS/DK runtime finish plan

Working nickname: "Phase Fuckshit." The point of this doc is to stop the
half-finished drift: name the finish line, prove what is already done, enumerate
exactly what is left, and order it so it can be executed slice by slice to zero.

## The finish line (unambiguous)

**N6.5: the CPS/DK backend is the SOLE lowering for colored functions.** Concretely:

- `emit_cps_ir_try_fn` never returns false for a colored function except on a
  `SIG-*` routing (see below). The direct-emitter path for colored bodies
  (`emit_fns.c:emit_fn_def`'s fallback) is deleted.
- Any residual source **form** the CT-IR subset cannot lower is a **hard error**
  with a form-named diagnostic (`cps_form_name`), not a silent fall-through.
- The fiber effect runtime (`tur_effect_cont_resume` et al.) is dead code for
  colored functions and is removed.
- Exit gate (measured, not vibes): across `tests/fixtures/*`,
  `TUR_TRACE_EVICT=1` shows **only `SIG-*`** categories. `BODY-STRUCT-OR-TAINT`
  and `BODY-UNSUPPORTED` are **zero**.
- Full suite green; ASan/LSan clean (no `requires.no-leak-check` on the CPS
  effect/continuation fixtures); the stackless-recursion sign-off probe green
  with the fallback gone.

`SIG-*` is NOT part of the finish line -- it is permanent, deliberate routing:
a colored function whose C SIGNATURE cannot be spelled by the CPS backend
(poly-fat / by-value-aggregate param / borrow ABI: `SIG-REJECT`; exported C
symbol: `SIG-EXPORT`; program entry: `SIG-MAIN`) stays on the direct emitter
forever. These functions contain no control op that needs the DK machine; the
direct emitter owns their ABI. "Delete the fallback" means "delete the BODY-*
fallback," never "route everything through CPS."

## What is already finished (with evidence -- do not relitigate)

- **The delimited-control carve-out is GONE.** `src/compiler/emit_cps.c` (the old
  direct/whole-program delimited-lowering machine) is **deleted**. cloneable /
  serial / async / raw reset / shift all lower **natively** through the CT-IR/DK
  backend: e.g. `outer` in `cps-backend-shift0` emits `outer__cps` and is not in
  the EVICT list. The `cps_uses_delimited` / `_cloneable_dk` / `_serial` flags
  gate **runtime-prelude emission** (emit the DK machine / cloneable / serial C
  support), not function routing. Any doc prose calling delimited control a
  "carve-out owned by a whole-program transform" is stale pre-D4 language.
- **The control-flow surface is closed.** Cross-function / resuming `shift` --
  long flagged as "the sole remaining control-flow gap" and even "not
  expressible" -- now emits natively on DK, single- and multi-shot, including a
  capturing receiver (`../archive/cps-native-handle-in-reset-plan.md`, Reductions A+B;
  `shift-crossfn-resume-works` = 11/105/23/306 all `__cps`). What remains in
  BODY-* is **value representation and form coverage**, not control flow. There
  are no known conceptual blockers left in the effect/continuation machinery.
- **Effect-result Tier-C crossings are native** (`v1/cps-tier-c-effect-result-native-plan.md`, landed).

## The remaining surface (measured -- this is the whole job)

Run the readiness gate to reproduce; do it before and after every slice:

```sh
TUR=./build/tur
for d in tests/fixtures/*/; do i="$d/input.tur"; [ -f "$i" ] || i="$d/$(basename $d).tur";
  [ -f "$i" ] && TUR_TRACE_EVICT=1 "$TUR" emit-c "$i" >/dev/null 2>>/tmp/ev.txt; done
grep '^\[EVICT\]' /tmp/ev.txt | awk '{print $2}' | sort | uniq -c    # category totals
grep '^\[EVICT\] BODY-UNSUPPORTED' /tmp/ev.txt | sed 's/.*unsupported form: //' | sort | uniq -c
```

Current `BODY-*` (the only rows that must reach zero):

### BODY-UNSUPPORTED -- specific source forms (each is a scoped slice)

| Form | ~count | Note |
| --- | --- | --- |
| `EX_CLOSURE` (capturing closure as a **value**) | ~16 | The keystone. `make-adder`, `producer`, `then-parser-impl`, `run-handler`, ... A first-class capturing closure returned/stored/passed (not a __Shift receiver -- that path is done). |
| httpd higher-order call (`httpd-call`, prints `null`) | ~30 | One fixture family; an indirect/closure shape. Investigate: likely resolves with the closure-value slice + an indirect-callee admission. |
| `EX_CONS_LIST` | ~10 | Cons-list literal built in a colored body. |
| `EX_PANIC` | ~3 | `panic`/abort in a colored body -- terminates, no continuation; should be trivially admissible (emit the call, no successor). |
| `EX_WHILE` | ~2 | Loop in a colored body. |
| `EX_DEFER` | ~2 | `defer` in a colored body. |
| `EX_BORROW_IMMUT` | ~2 | `&T` borrow crossing. |

### BODY-STRUCT-OR-TAINT -- ~90 distinct fns, TWO sub-classes

1. **Genuine (root) evictions.** Bodies with a non-slot value, an unsupported
   heap-join, or one of the forms above. The clusters visible in the names:
   - **Higher-order value threading**: `apply-fat`, `apply-parser`, `apply-twice`,
     `apply-logged`, `both`, `either` -- fat/closure values crossing slots.
   - **Owning values crossing control ops**: fed by the already-open owning plans
     (`cps-backend-owning-autodrop-lowering-plan.md`,
     `cps-backend-owning-env-teardown-e3-plan.md`,
     `cps-backend-ref-scope-exit-drop-plan.md`,
     `cps-backend-multishot-continuations-owning-capture-plan.md`). These are the
     `BODY-STRUCT-OR-TAINT` root causes for rc/ref/weak captures.
   - **Effect + aggregate bodies**: `ask-and-log`, `log-io`, `do-write-read`,
     `add-with-effect`, `counted-sum` -- an effect whose result/continuation
     carries a non-slot value.
2. **Taint cascade.** A function that shares an effect with a genuine evictor is
   pulled to fiber so the two machines never split (`ensure_S` Rule B/C). These
   are NOT independent work: fixing a root un-taints its whole cluster. Proven by
   Reduction B -- admitting the __Shift performer+handler cleared all 8 functions
   in the cluster at once. So the real root count is materially smaller than ~90;
   **measure roots by fixing them, not by counting names.**

## Ordered execution (each slice: land + full suite + re-measure the gate)

The shape of every slice is the one Reductions A/B used: find the failing
admission predicate with a `TUR_TRACE_EVICT` + a temporary `term_core_ok` probe,
widen the predicate for the specific shape (scoped, never a blanket widening --
see the Dead Ends in `../archive/cps-native-handle-in-reset-plan.md`), add emit if the
shape is genuinely new, regenerate snapshots, keep the suite green.

**Phase 1 -- BODY-UNSUPPORTED forms (small, contained, high signal).**
Order by leverage:

1. `EX_PANIC`, `EX_WHILE`, `EX_DEFER`, `EX_BORROW_IMMUT` -- four independent,
   contained form-admission slices. `panic` is a no-successor terminator (easiest).
   `while`/`defer`/borrow each need a `term_core_ok` case + an `emit_term` case.
2. `EX_CONS_LIST` -- admit a cons-list literal as a delegated value (it is a
   pure heap construction; likely a `safe_to_delegate` / `letraw` extension).
3. **First-class capturing closures as values (`EX_CLOSURE`)** -- the keystone.
   Approach (from the N6.5 plan's own note): lift the closure into a `CT_LETRAW`
   delegation whose captured env rides the continuation, admitting a capturing
   closure in bind/tail **value** position -- distinct from the __Shift-receiver
   flag (which admits it only as a `(recv k)` callee). Extend `is_delegatable_value`
   / `safe_to_delegate` for a capturing closure that is a plain value (not
   indirect-called at an unsafe site), with `collect_caps` walking scalar captures
   and bailing on non-Copy. Validate against `indirect_callee_ok`'s hazard.
4. httpd higher-order call family -- likely falls out of (3) + an indirect-callee
   admission; investigate after (3).

**Phase 2 -- BODY-STRUCT-OR-TAINT roots.** With Phase 1 done, most remaining
BODY-STRUCT-OR-TAINT are (a) higher-order fat-value threading and (b) owning
values crossing control ops. Execute the open owning plans as the (b) track;
(a) needs slot/heap-join admission for fat/closure values in bind position.
Re-measure after each: taint-cascade fns vanish for free.

**Phase 3 -- leak discipline (prerequisite for an ASan-clean finish).** The
native continuation path is memory-safe but leaks the per-resume snapshot: the
receiver emits `tur_cloneable_cont_resume(tur_continuation_snapshot(k), v)` and
nothing frees the snapshot (same class the fiber path had). Add a
resume-and-drop discipline in the shared receiver codegen; also the
`dk_frame_resume` per-resume node
(`cps-resume-frame-node-leak.md`) and the escaping-fat-closure env
(`escaping-fat-closure-env-leak.md`). Exit: drop every `requires.no-leak-check`
on CPS effect/continuation fixtures; ASan/LSan clean corpus-wide.

**Phase 4 -- N6.5 deletion + verification.**

1. Flip `emit_cps_ir_try_fn`: for a colored function that is not a `SIG-*`
   routing and whose body is not in the subset, **hard error** naming the form
   (`cps_form_name`) instead of returning false.
2. Delete the direct-emitter colored-body fallback in `emit_fns.c:emit_fn_def`
   (keep uncolored + `SIG-*` colored on the direct emitter).
3. Delete the fiber effect runtime paths now unreachable from colored code.
4. Verify: `TUR_TRACE_EVICT` shows only `SIG-*`; full suite green; stackless
   sign-off probe green; ASan clean.

## Ordering / dependencies

- Phase 1 before Phase 2 (closures/forms are Phase-2 roots' building blocks).
- Phase 3 can run in parallel with 1-2 but must finish before the ASan gate.
- Phase 4 is last and is mechanical once the gate is empty modulo `SIG-*`.
- The resuming-shift plan (`../archive/cps-backend-n6-resuming-shift-plan.md`) and the
  fallback-deletion plan (`../archive/cps-backend-n6-fallback-deletion-plan.md`) are
  subsumed here: the former is done, the latter is Phase 4.

## Honest distance

Control flow: **done.** Remaining: a bounded coverage grind -- ~6-8 Phase-1
slices, the owning-value plans + higher-order threading in Phase 2 (the bulk of
the ~90, minus the taint cascade that clears for free), Phase-3 leak discipline,
then the Phase-4 deletion. No conceptual blockers remain -- every item is a
scoped admission+emit slice of the kind already shipped twice. It is many
slices, not one push; but it is now fully enumerated and each has a measurable
exit (the EVICT gate ticking toward `SIG-*`-only).

## Progress log

### Slice P1.b -- closure KEYSTONE (control-free colour-only shape) landed via whole-body delegation

The `EX_CLOSURE` keystone is landed for the shape that dominates the count: a
function "colored" **only** because it constructs a capturing closure (it threads
no continuation and calls nothing that does). Rather than leaf-admit the closure
on the CPS path (which leaks its fat-closure env -- the scoped-env free is not
applied there), such a function now emits as a **single `CT_LETRAW` delegating
its whole body** to the direct emitter, whose `emit_value(EX_LET)` lowers the
closure with the `let_binding_env_freeable` scoped free. No leak, no reap, no
escape analysis on the CPS side -- the direct emitter's existing, sound machinery
does it all.

Mechanism (`cps_ir_translate_fn`): a `whole_body_delegatable` probe runs
`safe_to_delegate` with a scoped flag (`g_whole_body_delegate`) that admits a
capturing closure as a delegatable value. The flag is confined to the probe and
never reaches the per-node `cps_tail`/`cps_bind` path (a lone-leaf closure there
would still leak). The probe succeeds only when the body has **no control op and
no colored call**, so direct emission is behaviour-equivalent -- no
effect-threading / fiber concern. A function with a real `perform`/`handle`/
`shift` or a colored callee stays on the per-node DK path.

Gate effect: **`BODY-UNSUPPORTED` 52 -> 16.** Cleared the control-free colour-only
`EX_CLOSURE` evictions (`make-adder`, `let-star`, `letrec-self-in-nested-closure`),
the **httpd higher-order call family (~30)**, and one `EX_WHILE`. Validated:
`define-in-fn` / `let-star` / `letrec-*` are ASan-clean with correct output, the
`closure-env-no-leak` ASan target stays green, suite 2179/0. Remaining `EX_CLOSURE`
(9) are **effect-bearing** closures (a closure built in a handler-case / shift /
perform body) -- those genuinely need the lifted-body path (leaf-admission + a
lifted-env free, the P3.d receiver-reap tactic generalized), NOT whole-body
delegation, and stay gated on that free. So the keystone splits cleanly: the
control-free majority is done here; the effect-bearing minority is the remaining
slice.

### Slice P3.a -- Track-A perform-continuation `dk_frame_resume` node leak fixed (Phase 3)

The nested-control / multi-shot perform-continuation branch of `emit_perform`
(`emit_cps_ir.c`) returned `dk_perform(..., dk_frame_resume(...))` inline and
never reclaimed the spliced resume-frame node -- one 88-byte DK node per
perform execution. Now wrapped in `__dk_reap_node(...)` (single-node free at the
outermost entry boundary; the node's `->next` is `cur_k`, so a chain-walking
free is wrong), matching the reset/handle structural-node reaping. Multi-shot
safe (node outlives every re-entrant `dk_perform`). Validated under ASan/LSan:
the report's minimal repro plus `cps-backend-two-perform` /
`cps-backend-owning-struct-capture-multishot` are now leak-clean; suite
2179/0; the report is archived (`docs/archive/cps-resume-frame-node-leak.md`).
### Slice P3.b -- per-resume continuation snapshot freed after a multishot resume (Phase 3)

The MS1 multishot resume (`emit_effects_resume`) emitted
`tur_cloneable_cont_resume(tur_continuation_snapshot(k), v)` and never reclaimed
the snapshot -- one cont clone (struct + a deep `dk_copy_range` of the chain)
leaked per `(k v)`. Since the resume's `cont_fn` (`__dk_cont_fn` = `dk_invoke`)
copies the chain internally and frees only its own copy, the snapshot's env is
caller-owned; snapshot -> resume -> `tur_cloneable_cont_drop` reclaims it safely
(no double-free; original `k` untouched, so a subsequent multishot resume still
works). Validated under ASan/LSan: `shift-crossfn-resume-works` drops
**3320 -> 1248 bytes** with no UAF; the other multishot fixtures stay clean;
suite 2179/0.

### Slice P3.c -- reap the shift receiver's bound continuation `k` (Phase 3)

The other half of the `shift-crossfn-resume-works` residual: the shift handler
case's bound `k` (a `tur_cloneable_cont` = struct + an owned `dk_copy_range` of
`subk`, `emit_cps_ir.c:~3343`) was never reclaimed -- one cont struct + chain per
receiver invocation. `k` is only ever *cloned* per resume (never itself resumed),
so it is dead once the receiver returns, but the case body is emitted with
terminal returns inline (no single exit to drop at). Hoist the env and register
both halves for the outermost entry-boundary reap -- the chain via
`__dk_reap_keep` (`dk_free`), the struct via `__dk_reap_ptr` (`free`) -- i.e.
`tur_cloneable_cont_drop` deferred to the boundary (safe: every resume has
settled by then). Validated under ASan/LSan: `shift-crossfn-resume-works` drops
**1248 -> 64 bytes**, no UAF, output correct; suite 2179/0.

### Slice P3.d -- reap the __Shift receiver closure env; `shift-crossfn-resume-works` fully leak-clean

The 64-byte residual was the shift receiver itself -- the `(fn [k] ...)` the
`__Shift` desugar (`elab_effects.c:~796`) ALWAYS boxes into a fresh heap value: a
capturing-closure env, or an `EX_FN_TO_FAT` fatshim (`malloc(2*int64)`) for a
bare / capture-free fn. It is never a raw code pointer, so it is always safe to
free. It is passed as the `__Shift` effect arg, consumed exactly once by the
handler case (`recv(k)`), and was not reclaimed at the perform site -- so reap
its env (the handler param `arg`) at the entry boundary alongside `k`
(`emit_cps_ir.c`). A scalar-captured receiver frees cleanly; an owning capture
leaks its captured value (no closure drop glue yet) but never double-frees -- the
conservative interim of `escaping-fat-closure-env-leak.md`, applied to the one
non-escaping closure the CPS backend itself constructs.

With P3.a-d, `shift-crossfn-resume-works` is **fully ASan/LSan-clean (3320 -> 0
bytes)**, output 11/105/23/306, no UAF -- its `requires.no-leak-check` is
**dropped**. Suite 2179/0.

Phase 3 leak status: the DK-node (P3.a), per-resume snapshot (P3.b), receiver-`k`
(P3.c), and shift-receiver-env (P3.d) leaks are all fixed. Remaining is the
GENERAL escaping-fat-closure-env leak (`escaping-fat-closure-env-leak.md`, the
keystone gate) -- a closure that ESCAPES its constructor (returned / stored,
possibly with no effects at all, e.g. `make-scaler`), which cannot be reaped at a
DK boundary (there may be none) and needs closures to carry RC/drop glue so a
shared closure is not double-freed. That is the deferred item still blocking the
closure keystone and the httpd / fat-closure leak fixtures. The reap tactic used
for the shift receiver works precisely because that closure is NON-escaping and
CPS-backend-constructed; it does not generalize to an escaping closure.

### Slice P1.a -- pure-delegation forms landed (`EX_PANIC` / `EX_CONS_LIST` / `EX_BORROW_IMMUT`)

Three control-op-free leaf forms now delegate through `CT_LETRAW` instead of
evicting a colored body: a `panic` in a handler case, a `& rest` variadic
cons-list argument, and an immutable `(& x)` borrow. Each is admitted in
`safe_to_delegate` (`src/passes/cps_ir.c`) by recursing into its operands, so
the direct emitter emits it wholesale as the pure expression / no-successor
terminator it already is. `collect_free_vars` (`elab_core.c`) was extended to
descend `EX_CONS_LIST` items so a delegated cons-list riding a lifted CPS
continuation env never misses a captured item; the three forms (plus
`EX_INLINE_C`) are now named in `cps_form_name`. Gate: `BODY-UNSUPPORTED`
`EX_PANIC`/`EX_CONS_LIST`/`EX_BORROW_IMMUT` -> 0. Suite green.

### Findings for the remaining Phase-1 forms (still open)

- **`EX_DEFER`** is NOT a pure-delegation slice. A CPS-emitted function
  establishes no defer frame, so delegating a lone `EX_DEFER` via `CT_LETRAW`
  would register a defer that never fires. The `plan_autodrop` hoist only
  covers a *straight-line* owning auto-drop; every `EX_DEFER` eviction left in
  the corpus (`effect-defer`, `unsafe-defer`, `unsafe-basic`, `unsafe-nested`)
  crosses a control op -- and notably `(& x)` inside an `unsafe` block
  elaborates through an `EX_HANDLE` (that borrow-region handle is what colors
  `main`), so these are genuine control-crossing cases. This needs the native
  `term_core_ok` + `emit_term` treatment (or a CPS-side defer-frame), not a
  predicate widening. (Widening `expr_has_unsafe_control` to model borrows as
  non-control only reshuffles the eviction from `BODY-UNSUPPORTED` into the
  unnamed `BODY-STRUCT-OR-TAINT` bucket without admitting the function -- not
  real progress, and it hides the form from the trace; do not land that alone.)
- **`EX_WHILE`** (2 left) and **`EX_INLINE_C`** (2 left, session-channel
  bodies) likewise contain / neighbour control ops in the colored bodies that
  reach them, so they need native lowering rather than delegation.
- **`EX_CLOSURE`** (the keystone): the admission itself is FUNCTIONALLY DONE and
  validated, but it is **gated on Phase 3**, not independent of it. Widening
  `is_delegatable_value` to admit a general capturing closure as a plain value
  (`return e->as.closure_.closure != NULL`) makes the corpus's colour-only
  closure functions emit and run correctly -- `make-adder` becomes fully
  CPS-admitted and prints 15; 9/10 closure fixtures pass unchanged. The
  representation is safe (`emit_value(EX_CLOSURE)` yields a single-word env
  pointer that rides the DK slot via `emit_letraw`'s `TY_FN` carrier store; the
  2-word `tur_poly_fn_t` fat form only arises through the separate
  `EX_FN_TO_FAT`/`EX_POLY_WRAP` nodes), the captures are surfaced correctly
  (`collect_free_vars` reads the closure's explicit `captures` list), and
  `cap_add` already bails a non-Copy multi-shot capture to fallback. **The
  blocker is the leak**: the CPS delegation path does NOT apply the direct
  emitter's scoped-env free (`let_binding_env_freeable`, `emit_expr.c`), so an
  admitted closure `malloc`s its fat-closure env and never frees it --
  `make_adder__cps` leaks its env (uncaught, `define-in-fn` has no ASan target),
  and `run-handler` regresses the `tur_closure_env_leak` ASan target red
  (verified: 120 bytes leaked in the handler-case helper). So the keystone must
  land **together with** the escaping-fat-closure-env free on the CPS path
  (Phase 3), which means Phase 3 is a hard prerequisite for the closure slice,
  not the "parallel, finish-before-the-ASan-gate" option the ordering section
  implies. Two shapes need the free: (1) a control-op-free colour-only function
  (`make-adder`) is cleanest fixed by whole-body-delegating to the direct
  emitter (which already frees the env) rather than decomposing it into CPS; (2)
  a closure in a genuinely control-bearing function's lifted body (`run-handler`
  handler case) needs the free emitted on the CPS lifted-helper path. Note this
  is the **non-escaping** scoped-free that the direct emitter already solves
  (`let_binding_env_freeable`); it is NOT the deferred *escaping*-closure
  drop-glue tracked in `docs/reported/escaping-fat-closure-env-leak.md` (that one
  needs closures to carry RC/drop glue and stays interim-`requires.no-leak-check`).
  So the keystone's leak gate is a bounded "replicate the direct-path scoped-env
  free on the CPS delegation path" slice, not the open-ended closure-drop-glue
  work. The admission change was reverted pending that free; the
  `is_delegatable_value` comment carries a pointer here.
