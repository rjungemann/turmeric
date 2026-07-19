---
title: "CPS/DK runtime -- the finish plan (N6.5 endgame: delete the fallback, retire fibers for colored code)"
category: Planning
status: finished (2026-07-19) -- the N6.5 endgame reached its finish line. BODY-* = 0 corpus-wide (Slice PY), the colored non-SIG-* fallback is a build-time hard error (Slice PZ), cps-tramp-resume GRADUATED making CPS/DK the sole effect lowering, and the now-dead fiber effect runtime C was deleted (Stage G). The only residuals are the permanent SIG-* carve-out (by design, keeps the direct emitter for uncolored + signature-rejected code) and the separately-tracked cps-async experiment. Ready to archive; see the 2026-07-19 progress note at the top of the log.
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

#### THE dominant STRUCT-OR-TAINT root: heap-join-over-recursion (Rule A) -- measured

A per-function census (`TUR_TRACE_EVICT` counting fixtures) found the surface is
overwhelmingly **three stdlib functions**, each evicting in **~1762 fixtures**
(i.e. every fixture that imports stdlib): `set-eq-loop`, `map-eq-loop`,
`__cons-fmap`. Instrumenting `ensure_S` shows all three pass the initial
candidate gate (sig + `term_core_ok`) and are then evicted by the **fixpoint
Rule A: `needs_heap_join`** -- NOT by taint. Root cause is precise
(`needs_heap_join` case 2, `jbody_has_cps_tailcall`):

- Each is the pattern **"call a colored helper, then recurse (or return)"**:
  `set-eq-loop` calls `hamt/has-dynamic?` (colored: it invokes a `keyeq` fat
  comparator) then tail-recurses; `__cons-fmap` calls its `^fat f` then recurses
  in the `tcons` arg. The colored call's continuation becomes a `letcont` heap
  join (`emit_heap_join` reifies it as a DK frame -- already supported).
- The join is rejected because its **jbody itself contains a cps->cps tail call**
  (the recursion, threading the function's `KK_RET` continuation). The heap-join
  frame is lifted `LH_PERFORM_CONT` (KK_RET -> `return value`), which has NO `k`
  in scope to thread to the recursive `set_eq_loop__cps(args, k)` -- so the lifted
  frame would reference an undeclared `k`. The jbody also **captures** outer
  locals (`iter`/`s2-hamt`/`keyeq`), so it is a *capturing* heap join too
  (`needs_heap_join` case 1).

**This one shape dominates the entire STRUCT-OR-TAINT surface** -- it is the
highest-leverage slice in the whole plan (3 fns x ~1762 fixtures, plus every
caller that taint-cascades off them). Fix direction: lift a heap join whose
jbody has a `KK_RET` cps->cps tail call as a **RESET-style frame**
(`LH_RESET_CONT` already carries `k = enclosing continuation` in its env and
lowers `KK_RET -> dk_run(k, ..)`), building the frame env with `__k = cur_k` plus
the jbody's scalar captures; then relax `needs_heap_join` to admit the
capturing + cps->cps-tailcall case for that lift mode. High regression surface
(1762 fixtures currently pass via the direct fallback), so land behind a careful
suite pass. This supersedes the "scattered value-representation" framing above as
the STRUCT-OR-TAINT priority.

**Attempt 1 (reverted) -- two blockers the RESET-style lift must first solve.**
The `LH_RESET_CONT` lift above was implemented and DOES admit + correctly run the
three target functions (`set-eq-loop`/`map-eq-loop`/`__cons-fmap`: verified
`set_eq_loop__cps` emits, `data-literal-set-eq`/`map-eq`/`hamt-lisp-eq` green).
But relaxing `needs_heap_join` admits MANY more heap joins corpus-wide (140
codegen snapshots moved), and two of them miscompiled -- so the lift is not yet
general enough to land. The next attempt must handle both:

1. **Nil-typed call in the jbody (`contract-nested`, C build error "void value
   not ignored").** A jbody whose lifted body binds a `:nil`/`:void` cps->direct
   call (e.g. `tur_contract_check`, which returns void) emits `__t0 = void_fn(...)`
   -- the CT_LETCALL emit in the lifted frame does not apply the nil-result
   special-case (emit-as-statement, bind the unit placeholder) that
   `emit_letraw`/the top-level path use. Fix: route a nil-typed call through the
   statement form inside the lifted frame too.
2. **Delivery bug in a multi-join sequential function (`hamt-delete`
   `test-del-present`: a chain of `hamt/free` colored calls -> heap-joins j0..j8;
   segfault, and empty output once `next` was set to `dk_done()`).** A function
   that is a *straight-line sequence* of colored calls (each its own heap join,
   each capturing the growing live set) chains RESET-style frames, and the value
   fails to reach the final continuation. The frame's own `dk_run(__k, v)` /
   `f__cps(args, __kont)` delivery composes wrong with the `dk_frame` `next` here
   (unlike `emit_reset`, which sits under a prompt). The RESET-style splice needs
   the correct `next`/prompt discipline for a *bare* (non-delimited) join before
   it is sound for arbitrary sequential-colored-call bodies.

So: the admission relaxation and the 3-function win are real, but must be gated
to the shapes the lift actually handles (or the lift generalized to cover nil
jbody calls + sequential multi-join delivery) before landing. Do NOT re-land the
blanket `needs_heap_join` relaxation without fixing both.

**Attempt 2 (LANDED). Suite 2179/0.** Both blockers fixed; the blanket
relaxation lands green:
- The lift is an **LH_RESUME_CONT resume-frame** (`dk_frame_resume`), not
  LH_RESET_CONT.  A `DKK_RESUME_FRAME` rfn RECEIVES its run-time downstream chain
  as the `__kont` PARAMETER (`dk_run_impl` passes `k->next`) and CONSUMES it
  (returns rfn's result, never re-processing `next`) -- so a KK_RET delivery
  lowers `dk_run(__kont, v)` and a recursive `f__cps(args, __kont)` threads it,
  delivered EXACTLY ONCE.  This is the same frame kind the Track-A nested-perform
  continuation uses; captures ride a caps-only env (no `__k` field -- `__kont` is
  the runtime param).  The earlier RESET_CONT double-delivery (bug 2) is gone.
- **Nil jbody call** (bug 1): `emit_term`'s `CT_LETCALL` now emits a
  `:nil`/`:void` cps->direct call as a bare statement + unit-placeholder bind
  (`fn(args); x = 0;`) instead of `x = void_fn(...)`.
- Result: `set-eq-loop`, `map-eq-loop`, `__cons-fmap` CPS-emit; the STRUCT-OR-TAINT
  per-function census drops from 87 distinct fns to 53, and the three
  ~1762-fixture-wide evictions are eliminated (top evictor is now `main` at 38).
  140 codegen snapshots regenerated.

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

1. **DONE (Slice PZ).** Flip `emit_cps_ir_try_fn`: a colored function that is not
   a `SIG-*` routing and whose body left the subset now raises
   `diag_emit(DIAG_ERROR, ...)` naming the function + residual form instead of
   returning false; main.c's post-emit `diag_had_error()` gate makes it a build
   failure. Experimental admission-expanding flags (`cps-async`) are exempt until
   they graduate.
2. **CONSTRAINED BY THE SIG-* CARVE-OUT -- no clean deletable surface (analysis
   below).** `emit_fn_def` does NOT branch on `cps_colored` after the
   `emit_cps_ir_try_fn` call at emit_fns.c:2624 -- once `try_fn` returns false the
   remainder is a SINGLE uniform direct-emit path (with the Phase-19 per-fiber
   effect routing) shared by BOTH uncolored functions AND `SIG-*` colored
   functions. There is no distinct "non-SIG-* colored fallback" block to delete:
   the non-SIG-* colored case is now a build error (step 1), and every other
   fall-through (uncolored, and the permanent `SIG-*` colored routings the plan
   explicitly KEEPS on the direct emitter) still needs that exact path.
3. **LIKEWISE CONSTRAINED.** The fiber effect runtime is not "unreachable from
   colored code": `SIG-*` colored functions are colored precisely because they
   perform/handle effects (e.g. `deferred-ask` = SIG-TAINT performs `Ask`;
   `main` handlers; exported effectful fns), and they run that effect machinery on
   the fiber runtime by design. So the fiber effect runtime is PERMANENTLY needed
   for `SIG-*` colored + uncolored effectful code; there is no colored-only subset
   to remove. (The old whole-program delimited-lowering machine `emit_cps.c` was
   already deleted pre-Phase-4; that was the deletable delimited surface.) The
   "CPS/DK is the SOLE lowering for colored functions" endgame statement is
   precise only for **non-`SIG-*`** colored functions -- the `SIG-*` carve-out is
   permanent and keeps the fiber runtime alive.
4. Verify: `TUR_TRACE_EVICT` shows only `SIG-*` (met); full suite green (met,
   2179/0); stackless sign-off probe; ASan clean.

**Net Phase 4 outcome:** the readiness gate (BODY-* = 0) is achieved and now
build-time ENFORCED for the shipping backend (steps 1 + 4-partial). Steps 2-3 as
originally written (delete the fiber runtime for colored code) are not applicable
as stated: the direct/fiber path is shared with the permanent `SIG-*` colored
routings and cannot be removed without moving those off the direct emitter, which
the plan deliberately does not do. The remaining optional work is diagnostic
(stackless sign-off probe, an ASan sweep) rather than deletion.

## Ordering / dependencies

- Phase 1 before Phase 2 (closures/forms are Phase-2 roots' building blocks).
- Phase 3 can run in parallel with 1-2 but must finish before the ASan gate.
- Phase 4 is last and is mechanical once the gate is empty modulo `SIG-*`.
- The resuming-shift plan (`../archive/cps-backend-n6-resuming-shift-plan.md`) and the
  fallback-deletion plan (`../archive/cps-backend-n6-fallback-deletion-plan.md`) are
  subsumed here: the former is done, the latter is Phase 4.

## Honest distance

Control flow: **done.** The `BODY-UNSUPPORTED` surface has been driven **65 -> 4
per-emit** (see the Progress log: P1.a pure-delegation forms; P1.b/P1.c the
closure keystone for every non-escaping shape; P3.a-d the DK-node / snapshot /
receiver-`k` / receiver-env leaks; PA/PB/PC the B1 DK-native resumable-payload
bridge -- the WHOLE B1 family now CPS-emits, `effect-cont` AND raw-`int`
continuation payloads; PD the unsafe-marker whole-body delegation clearing 2 of
the 4 B2 closure-as-value fixtures; PE the EX_DEFER whole-body delegation
clearing 3 of 4 defers plus the parser `mbind` borrow drop-glue clearing 1 of 2
remaining closures). What remains, and why each is a distinct slice rather than a
quick admission:

- **`EX_CLOSURE` (7) -- actually TWO families (verified, P-inv2 below).** The 7
  split into two distinct blockers; the earlier "all effect-payload" framing was
  wrong.

  **B1. Resumable fn-PAYLOAD effects (was 3): DONE (PA/PB/PC).**
  `effect-cont-kv-sugar`, `multishot-effect-cont-kv-sugar`,
  `effect-fn-payload-capturing`, `cross-function-resume-via-effect` -- the
  `(perform (E g))` shape where the handler resumes THROUGH the payload
  (`(E [f] k) (f k)`) -- all now CPS-emit (direct == cps == turi, ASan-clean). The
  `__Shift` cloneable-cont machinery was generalized to user resumable-payload
  effects (admission + form-level cont reflavor + handler cloneable-wrap, landed as
  one unit), covering `effect-cont`, `multishot-effect-cont`, AND raw-`int`
  continuation payloads (identified by the payload body resuming its param). The
  earlier belief that this was "Phase-2 taint work, not a bridge" (P-inv3) was
  itself too pessimistic: the taint chain rooted at the performer's perform-arg
  gate, which the bridge opens soundly (the handler cloneable-wrap makes the
  earlier `atom_ok` widening sound). Full history:
  [cps-dk-multishot-user-effects-plan.md](cps-dk-multishot-user-effects-plan.md)
  and Progress-log PA/PB/PC. NOTE: PB (`handle_delim_ok`) was a GENERAL win beyond
  B1 -- any effect whose handled body computes through a colored call (e.g.
  `(handle (+ 10 (inner)) ...)`) now CPS-emits.

  **B2. Capturing closure as a VALUE in colored code (was 4; 2 done via PD).**
  `unsafe-closure-capture` / `free-lift-bind` (a closure literal passed to
  `free-run` inside an `unsafe` block) now CPS-emit: the `unsafe` block is a pure
  compile-time MARKER (`is_unsafe_marker`), so an unsafe-marker handle with a
  delegatable body is itself delegatable (PD) and the whole region -- including the
  fat-closure arg the direct emitter owns -- whole-body-delegates.  (The fat
  closure is not dropped -- the deferred escaping-fat-closure-env leak, now marked
  `requires.no-leak-check` on those two fixtures; present in the emitted C on both
  paths, the direct `int main` merely hid it from LSan.)  REMAINING (2):
  `currying-effect-partial` (a partial-application closure `add10 = (log-add 10)`
  called inside a REAL `Log` handle -- not an unsafe marker) and
  `hkt-stdlib-parser-instances` (parser-combinator closures).  These need the
  deferred general **closure env drop glue**, scoped in
  [closure-drop-glue-plan.md](closure-drop-glue-plan.md): S1 a scoped free for
  NON-escaping closures (partial-app + non-retaining HOF arg -- clears
  `currying-effect-partial` and the two PD leak fixtures, no ownership tracking)
  and S2 move-based drop glue for ESCAPING (stored) closures -- clears
  `hkt-stdlib-parser-instances` and the httpd middleware family. Tracked in
  `docs/reported/escaping-fat-closure-env-leak.md`.
- **`EX_DEFER` (4) -- two entangled sub-shapes, neither standalone-landable.**
  (i) USER side-effecting defers (`effect-defer` `(defer (println ...))`,
  `unsafe-defer`) must fire their side effect at a specific scope-exit point; a
  boundary reap would move that (observable) -- they need a real CPS-side defer
  frame. (ii) rc AUTO-drop defers (`unsafe-basic`/`-nested`) evict on the
  `EX_DEFER` only because `expr_has_unsafe_control` conservatively rejects the
  pure `(& p)` (`EX_BORROW_IMMUT`, un-modelled -> `default: true`) inside the
  `unsafe`->`Unsafe` handle, so `plan_autodrop`'s single-shot-crossing
  drop-at-exit path bails. **Modeling the pure P1 forms there does NOT admit the
  fixture** -- the `ptr<int>` borrow (`p = rc->ptr r`, `b = (& p)`) crossing the
  handle is a co-located `BODY-STRUCT-OR-TAINT` blocker, so the eviction just
  moves category (confirmed; a prior session reverted this as reshuffling). The
  rc<scalar> boundary-reap alternative hits the same co-located struct blocker.
- **`EX_WHILE` (0). DONE (Slice PF).** `effect-handler-capture-loop` now
  CPS-emits: the loop was never the blocker (`EX_WHILE` delegates when its
  cond+body do); the self-contained per-iteration `handle` was. A real handle
  that fully discharges its effect delegates to the direct emitter's fiber frame
  (which runs inside a `__cps` body), gated by a `g_wbd_handled` effect stack so
  no DK-threaded perform is enclosed. See the Progress log.
- **`EX_INLINE_C` (2).** A session-channel primitive inlined into a colored
  `main`; niche, tied to the session runtime.

**Revised assessment (this session, verified):** the earlier "no conceptual
blockers remain -- each is a scoped admission+emit slice" was **too optimistic**,
and the follow-up P-inv3 pessimism ("B1 is Phase-2, not a bridge") was ALSO wrong.
The reality, resolved by PA/PB/PC: **the whole B1 resumable-payload family (4
fixtures) now CPS-emits** via the generalized `__Shift` cloneable-cont bridge, and
PB's `handle_delim_ok` was a general win beyond it, and PD cleared 2 of the 4 B2
fixtures via unsafe-marker whole-body delegation, and Slice PE cleared 3 of 4
`EX_DEFER` (whole-body-delegated) + 1 of 2 `EX_CLOSURE` (parser `mbind` borrow
drop-glue), and Slice PF cleared `EX_WHILE` via self-contained handle
delegation. `BODY-UNSUPPORTED` is now **4** (was 14): `EX_CLOSURE` x1
(`currying-effect-partial` -- an indirect call through a capturing closure that
itself performs `Log`; the harder B1-style case), `EX_DEFER` x1 (`effect-defer`
-- native defer-frame + co-located `perform`), and `EX_INLINE_C` x2
(session-runtime-tied). The remaining genuinely-scoped work is
the colored-capturing-closure indirect-call case, native lowering for the
defer/while/inline-C residuals, and the dominant Phase-2 `BODY-STRUCT-OR-TAINT`
surface. The EVICT gate now reads `SIG-*` plus these 4 named residuals.

## Progress log

### 2026-07-19 -- FINISH LINE REACHED (Stage G): the endgame is complete

The N6.5 finish line is met. Beyond Slices PY (BODY-* = 0) and PZ (the hard-error
gate) already logged below, two further landings after those entries closed the
last of Phase 4:

- **`cps-tramp-resume` GRADUATED (2026-07-19).** The CPS/DK trampolined
  tail-resume is now the DEFAULT and SOLE lowering for effectful colored code
  (`g_opt_cps_tramp_resume` defaults on; row moved to `GRADUATED[]` in
  `experiments.c`). The whole corpus DK-lowers every effect -- **zero
  `tur_effect_perform` call sites** remain.
- **Stage G: the dead fiber effect runtime C is DELETED.** With zero fiber-effect
  call sites, the fiber effect runtime that had been emitted unconditionally into
  every program was dead code, and it was removed from `emit_module.c`:
  `tur_effect_perform`, `tur_effect_cont_resume` / `tur_effect_cont_valid`, the
  first-class-handler-value fiber dispatch (`__tur_msdyn_*` / `tur_handler_dispatch`),
  the `TurContK` / `EffectHandler*` structs, `global_effect_handler_chain`, and the
  two `FiberBlock` effect fields. STAYS: `FiberBlock` + scheduler/reactor/futures
  (concurrency, unrelated to effects), `tur_handler_table_t` (the DK handler-value
  path reads it), and `tur_cloneable_cont_*` (the DK `__Shift` bridge). Emitted C
  now has zero references to any deleted symbol; ~139 snapshots regenerated as the
  preamble shrank. Suite 2203/0.

This resolves the Phase-4 steps-2-3 question the "Net Phase 4 outcome" note below
left as "not applicable as stated." The refinement that unblocked deletion: the
fiber effect runtime was NOT permanently needed after all -- once
`cps-tramp-resume` graduated, even `SIG-*` colored effectful code no longer routes
its effects through the fiber path, so the effect-runtime C had zero call sites and
was deletable. The fiber `FiberBlock`/scheduler stays (concurrency, not effects),
which is a different subsystem than the "fiber effect runtime" the plan targeted.

**Remaining after the finish line (neither a v1 blocker nor tracked here):**

- The **permanent `SIG-*` carve-out** -- uncolored functions and colored functions
  whose C signature the CPS backend cannot spell stay on the direct emitter by
  design. This is not eviction; it is the intended routing and was never part of
  the finish line.
- **`cps-async`** -- the one still-gated experiment touching this backend (heap
  `async`/`await`). Its recursive-await residual was investigated and declined as
  works-at-intended (see `compiled-stackless-recursive-await-plan.md`); the
  experiment is tracked independently and does not hold this plan open.
- A **pre-existing owning handler-value table leak** (unrelated to Stage G),
  noted in the Stage G commit and its own report -- a general memory-model gap,
  not a CPS-specific defect.

Net: the CPS/DK backend is the sole lowering for colored non-`SIG-*` functions,
the BODY-* fallback is a hard error, and the fiber effect runtime is gone. This
plan is complete and ready to archive.

### Slice PZ (LANDED) -- Phase 4 step 1: the N6.5 gate is now ENFORCED

The direct/fiber whole-function fallback for colored code is now a HARD ERROR
when it would be taken for a non-signature reason. `emit_cps_ir_try_fn`
categorizes every colored fallback (the same SIG-*/BODY-* split the
`TUR_TRACE_EVICT` trace uses) and, for a BODY-* fallback in the shipping config,
raises a `diag_emit(DIAG_ERROR, ...)` naming the function + residual form instead
of silently returning false. main.c's post-emit `diag_had_error()` gate turns that
into a build failure, so any regression that reintroduces a fixable BODY root is
caught at build time rather than quietly routing to the retired fiber path.
Permanent SIG-* routings (export / main / ABI-reject signature / permanent taint /
opaque inline-C body) still fall back cleanly. Suite 2179/0.

Scope: the hard error governs the SHIPPING backend only. An experimental
`--enable` feature that EXPANDS the colored surface -- currently `cps-async`, which
CPS-lowers `async`/`await` -- may still carry in-flight BODY residuals its own
admission has not closed (e.g. `add-cap-let`'s `(let [c (fn [] ...)] (await (async
c)))` capturing-closure-as-async-arg), so `g_opt_cps_async` exempts the flag from
the hard error; those keep the fallback until the feature graduates. This is Phase
4 step 1 of 4; steps 2-3 (delete the direct-emitter colored-body fallback in
emit_fns.c; remove the now-unreachable fiber effect-runtime paths) remain.

### Slice PY (LANDED) -- erased-generic carrier admission: BODY-* REACHES ZERO

**The N6.5 readiness gate is MET: `TUR_TRACE_EVICT` shows ONLY SIG-* across the
whole corpus** (BODY-UNSUPPORTED = 0 AND BODY-STRUCT-OR-TAINT = 0). Suite 2179/0.
The last BODY root, `test-option-eq-nones` (`(option-eq? (none) (none) cmp)`), is
now CPS-native. The erased `(none) : (Option A)` has an unresolved element tyvar
(both args are the nullary `none`, so nothing pins `A`), so it cannot key a
concrete monomorph clone -- but in a polymorphic/erased context it rides the
uniform int64 CARRIER ABI (the direct emitter already lowered the call to the
generic `option_hyeq_qu(int64_t, int64_t, int64_t)`). The ONLY blocker was
admission: `atom_ok((Option A))` = false because `slot_ok_t` recognized neither a
flat product (`slot_box_ty`, needs a known element size) nor a heap handle
(`carrier_handle_ok`). Added `erased_adt_carrier`: an ADT/struct application with
a `TY_TYVAR`/`TY_UNKNOWN` argument crosses a slot as a plain int64 word, so
`slot_ok_t` now admits it. The emit path needed NO change -- the CT_TAILCALL
no-clone fallback (`mclone == NULL` && callee not `in_s` -> cps->direct) already
routes the erased call to the generic carrier callee. `test-option-eq-nones` emits
`__t0 = none(); __t1 = none(); __t2 = <cmp>; option_hyeq_qu(__t0,__t1,__t2)
/* cps->direct */; dk_run(...)` -- and `option-basic` prints the full expected
sequence (last line `true` = None == None). No other fixture's codegen moved.

With BODY-* empty, the plan's readiness gate for the N6.5 deletion (flip
`emit_cps_ir_try_fn` so a colored non-SIG-* fn is CPS-only; delete the direct/
fiber whole-function fallback; hard-error residual forms) is now satisfied.

### Slice PX (LANDED) -- coloring precision: inline-C callee is not an indirect call

**BODY-STRUCT-OR-TAINT collapses from 3 distinct roots to 1** -- both sized
`main`s AND `re-parse-class` cleared in one fix; only `test-option-eq-nones`
remains. Suite 2179/0 (one codegen snapshot, `load-inside-defmodule-injects-names`,
regenerated -- ~3 functions correctly lost their needless `__cps` variant).

`cps_collect_calls` set `has_indirect` (-> colors the function) for ANY call whose
callee is not a top-level coloring node -- including a RESOLVED call to an
inline-C-bodied `defn`. The doc `docs/archive/history/cps-coloring-overcolors-
nonnode-calls.md` already carved out constructors (a non-node leaf that reaches no
control op); an inline-C body is the same case -- opaque C with NO Turmeric
control op, so it can never reach a perform/handle/shift. Extended the carve-out:
a resolved call whose `fn_binding->body_is_inline_c` is set no longer colors its
caller. This stops a contract macro (`require-msg!` -> `tur-contract-check`, an
embedded inline-C `defn` absent from the node set) from spuriously coloring an
otherwise-pure function like `sized-bitvec-assert-len!`; that de-coloring cascades
correctly -- `assert-len!` uncolored -> `main`'s call to it delegates to the direct
emitter (which handles the by-value `Size` GADT natively) -> the sized `main`s and
`re-parse-class` all drop out of BODY-*. Corpus-wide the fix also un-colored a
class of spuriously-colored inline-C-calling functions (SIG-REJECT 8209->8156,
SIG-MAIN 430->406) -- the coverage win the history doc predicted. Over-coloring was
always SAFE (never a miscompile), so this is purely a coverage/precision gain,
verified by the green runtime suite.

### Slice PW (LANDED) -- unsafe-marker handle lowers transparently to its body

An `(unsafe ...)` block desugars to a handle on the built-in `Unsafe` effect, but
`Unsafe` is a compile-time MARKER that is never performed at runtime
(`is_unsafe_marker`) -- the handler never fires. `build_handle` now lowers such a
handle directly to its body (`cps_bind(h->body, ...)`) instead of a `CT_HANDLE`.
This keeps the body's operations in the plain function body, where `term_core_ok`
admits a `CT_LETRAW`, rather than inside a handle delim where `handle_delim_ok`
(correctly) rejects a fiber-runtime `CT_LETRAW`. Suite 2179/0, no regressions;
the `sized-sz3-bitvec`/`-matrix` mains now translate to a flat let-chain with no
spurious handles (a real simplification + the correct lowering).

This does NOT by itself clear the sized `main` STRUCT-OR-TAINT root: the residual
blocker, now precisely diagnosed, is the tail `tailcall sized-bitvec-assert-len!
(bv __t3 j4)` where (a) `__t3 : Size` is a recursive GADT lowered as a BY-VALUE
ADT WITH DROP GLUE (`is_heap` is only set by an explicit `:heap` kw, never auto-
detected for a recursive type; so `slot_ok_t(Size)` = false -> `atom_ok` rejects
the arg), and (b) `assert-len!` SIG-REJECTs on its `Size` param, so the colored
tailcall threading `j4` targets a callee with no `__cps` variant. This is the same
by-value-aggregate-with-drop-glue crossing as `re-parse-class` (area 1), plus a
colored-SIG-REJECT-callee-with-continuation wrinkle -- genuine drop-glue +
signature codegen, not a predicate widening.

**Why `assert-len!` is colored (the cascade source), diagnosed further.** Its
body `(require-msg! (= (sized-bitvec-len bv) (size-eval expected)) "...")` has no
real control op (`cps_directly_uses_control` = 0 once the unsafe-marker handle is
excluded -- verified). It is colored purely by `cps_collect_calls`' `has_indirect`
seed: `require-msg!` expands to `(tur-contract-check condition msg)`, and
`tur-contract-check` (a `defmodule tur/contract` inline-C `defn`) does NOT resolve
via `cps_find_node` at the call site, so the call is treated as an UNRESOLVED
(indirect) call and conservatively colors the function. That coloring cascades:
`assert-len!` colored -> SIG-REJECTs on its `Size` param -> `main`'s tail call to
it is a colored tailcall to a SIG-REJECT callee -> `main` evicts. So the sized
`main` root has a SECOND, independent lever besides the by-value-Size crossing:
resolve (or safely non-color) the `tur-contract-check` call. Two sub-questions to
pin next: (a) why does `cps_find_node` miss the module-qualified
`tur-contract-check` binding when other `defmodule` fns (hamt/*) resolve fine --
a binding-identity mismatch between the call site and the top-level node? and (b)
is a known-safe-runtime-call whitelist (like the existing `ctor == NULL` skip at
cps.c ~464) the right shape for contract/runtime helpers. NOTE: excluding the
unsafe-marker handle from `cps_directly_uses_control` (the natural companion to
Slice PW) is CORRECT but INSUFFICIENT alone -- `has_indirect` still colors
`assert-len!` -- so it was not landed on its own.

### Slice PV (LANDED) -- partial-application inlining clears the EX_CLOSURE keystone

**BODY-UNSUPPORTED -> 0** (the last `EX_CLOSURE` root gone) and STRUCT-OR-TAINT
`log-add` + `main` (currying-effect-partial) cleared in ONE fix. Suite 2179/0, no
codegen churn elsewhere. A let-bound closure that is a PARTIAL APPLICATION
(`(let [add10 (log-add 10)] (add10 32))`) and whose sole use is a saturated direct
call is rewritten to the underlying saturated call `(log-add 10 32)` -- a native
DK colored tailcall that threads `k`, with NO fat closure and NO env drop-glue.
The elaborator lowers `(log-add 10)` to `(let [__papc 10] CLOSURE{__pap1288})`;
`pap_extract` peels that capture-prelude, reads the underlying `log-add` +
captured bindings off the `__pap` wrapper body, and `pap_maybe_rewrite`
reconstructs `(log-add __papc rest...)` at each call site (the capture bindings
stay in scope, emitted from the prelude; the closure is dropped). Soundness rests
on the proven, complete, conservative `closure_binding_escapes` (from emit_core.c;
`default -> escapes`) -- inlining fires ONLY when the closure var never appears as
a value, plus `pap_calls_saturated` (conservative `default -> false`) confirming
every use is a full-arity call. `log-add` was only STRUCT-OR-TAINT as a taint
victim of `main`; making `main` native cleared both. `currying-effect-partial`
prints `step` / `42`; all 10 currying fixtures pass. Remaining BODY surface: 3
STRUCT-OR-TAINT roots -- `main` (a DIFFERENT fixture), `re-parse-class`,
`test-option-eq-nones`.

### Slice PU (LANDED) -- un-lowerable inline-C is a permanent SIG-* carve-out

BODY-UNSUPPORTED `EX_INLINE_C` roots (`session-effects`, `session-mp-effects`
mains) cleared: 2 -> 0. Suite 2179/0, no codegen churn. An inline-C block declares
a fixed C signature and cannot thread a DK continuation, so no BODY-* admission
can ever pull a colored function whose body contains an un-lowerable inline-C into
the CPS set -- it is a PERMANENT carve-out, exactly like an ABI-reject signature,
not a fixable BODY root. When a colored function fails `term_core_ok` and its
`first_unsupported` residual is an `EX_INLINE_C` form, classification now sets
`sig_perm = true` (the direct emitter owns it unchanged; the routing does not
move, only the taint class and the EVICT label). The EVICT trace reports it as
`SIG-INLINE-C`. Only the genuinely un-delegatable case reaches here: a whole-body-
delegatable inline-C leaf (`unsafe-*` mains, an rc/of auto-drop) translates to a
`CT_LETRAW` owning-op, never a residual `CT_UNSUPPORTED`, so those stay native/
SIG-TAINT as before. BODY-UNSUPPORTED is now down to a single `EX_CLOSURE` root.

### Slice PT (LANDED) -- defer-as-continuation: `(do (defer D) <control>)` native

BODY-UNSUPPORTED `EX_DEFER` root (`effect-defer`'s `deferred-ask`) cleared. Suite
2179/0, no codegen churn on any other fixture. A `do` block carrying an explicit
`(defer D)` alongside a control op (perform / colored call) the whole-body path
does not own used to evict on `EX_DEFER` -- a `__cps` function establishes no
defer frame. But an explicit defer's meaning is exactly "run `D` at this block's
scope exit, LIFO", which the DK continuation models directly: thread each defer
body into the block's continuation so it fires after the tail value is produced
(through any perform) and before the value is delivered, in reverse-declaration
order. Implemented in `cps_ir.c`'s `cps_tail` EX_DO case, scoped to a value-
producing tail whose every defer body is control-free (`safe_to_delegate`);
anything else falls through and evicts as before. It runs ONLY on the per-node
path (`g_whole_body_delegate` already delegates the whole `do`), so it can only
turn an eviction into a native emit -- never alter a working delegated defer.
`deferred-ask` now lowers to `let __t0 = perform Ask(); let __t1 = (println
"cleanup"); (k __t0)` -- and, since the program's `main` handles `Ask` on the
FIBER runtime (uncolored/direct), `deferred-ask` is now honestly SIG-TAINT
(permanent DK<->fiber non-interop routing, same class as the PL-PQ effectful
cluster), not a fixable BODY root. `effect-defer` still prints `cleanup` / `42`.

### Slice PS (LANDED) -- native abortive cross-function shift (`inner`)

STRUCT-OR-TAINT distinct roots **5 -> 4**. Suite 2179/0, no codegen churn on any
other fixture. The `inner` root in `shift-abort-crossfn`
(`(shift (fn [k : cont] (+ x 5)) 0)` -- an abortive shift whose `reset` is in the
caller `outer`) was NOT a control-flow gap after all: the CT-IR translator already
lowers it to a native `CT_SHIFT` whose `dk_shift` captures up to the
*dynamically* nearest prompt (the caller's reset) -- which is exactly correct
cross-function abort semantics on the DK chain. The ONLY blocker was admission:
`term_core_ok`'s CT_SHIFT case runs `shift_body_ok`, and the abortive receiver's
applied body starts `let k = 0` -- binding the receiver's `cont`-typed parameter
`k` to a placeholder literal (the shift's value operand `0`, typed TY_CONT). The
generic `atom_ok` slot gate rejects a TY_CONT literal (a continuation is not
slot-representable), evicting an otherwise-native shift. The binding is DEAD (an
abortive receiver never references `k`), so it never crosses a slot -- the DK
backend materializes it as an unused local. Fix: `shift_cont_placeholder` admits a
`CA_INT`/`CA_UNIT`/`CA_BOOL` literal typed TY_CONT in `shift_body_ok`'s CT_LETVAL
case (scoped to the shift body, never the generic `atom_ok`). `inner` now emits
`inner__cps`; `shift-abort-crossfn` prints 105/7/42 natively. `discard-ctx`
(same-function abortive shift) was already native; the resuming cross-function
family (`shift-crossfn-resume-works`) is unchanged. This confirms the plan's
"control-flow surface is closed" claim -- the last shift-flavored eviction was an
admission-predicate gap, not missing machinery.

### The whole remaining BODY-* surface (after PS+PT+PU+PV) -- TWO areas

Corpus scan after this session (PS/PT/PU/PV): **BODY-UNSUPPORTED = 0** (cleared)
and **BODY-STRUCT-OR-TAINT = 6 fns / 3 distinct roots** (`main` in
`sized-sz3-bitvec` + `sized-sz3-matrix`, `re-parse-class`, `test-option-eq-nones`).
SIG-* carries the `SIG-INLINE-C` permanent category (2 fns). The `currying-effect-
partial` area (was 3 of 5 rows) is LANDED via PV; what remains are two independent
infrastructure areas:

1. **Owning value crossing a control op** (`main` in sized-sz3-bitvec/matrix,
   `re-parse-class`). The sized mains handle `#fx{Unsafe}` blocks whose bodies are
   owning bitvec/matrix ops (`<owning-op>` CT_LETRAW); `re-parse-class` is a non-
   generic defn returning a by-value STRUCT `RxParse = (RxPR :Regex :int)` whose
   heap `Regex` field gives it DROP GLUE (not a `slot_box_ty` flat product; PR's
   gate correctly excludes it). Both need the owning-value / drop-glue signature +
   scope-exit-drop path (the open owning plans:
   `cps-backend-owning-autodrop-lowering-plan.md`,
   `cps-backend-ref-scope-exit-drop-plan.md`).
2. **Erased-generic monomorph resolution** (`test-option-eq-nones`).
   `(option-eq? (none) (none) cmp)` -- the erased `(none) : (Option A)` is a TY_APP
   whose element tyvar is unresolved, so `slot_ok_t`/`atom_ok` reject the cps->cps
   tailcall arg (`option-eq?` is a mono-template) AND `find_mono_clone_for_call`
   has no aggregate arg to key the `(Option int)` clone on. Needs admitting the
   erased Option carrier + a fallback-clone pick for the element-free `none`.

### The remaining 5 STRUCT-OR-TAINT roots (after this session's PK-PR, 26 -> 5)

Four distinct infrastructure areas, none a bounded predicate widening:
- **`log-add` + `main`** (currying-effect-partial): the EX_CLOSURE partial-app
  keystone -- a capturing closure `(log-add 10)` the handle body binds + calls;
  admitting it leaks the fat-closure env (ASan) without the Phase-3 drop-glue.
- **`re-parse-class`**: a NON-generic defn returning a by-value STRUCT `RxParse =
  (RxPR :Regex :int)` -- a Regex (heap) field gives it DROP GLUE, so it is not a
  `slot_box_ty` flat product (PR's gate correctly excludes it).  Needs the owning-
  value / drop-glue signature path, same family as the EX_CLOSURE track.
- **`test-option-eq-nones`**: `(option-eq? (none) (none) cmp)` -- the erased
  `(none) : (Option A)` is an int64 CARRIER (not a `TY_APP` aggregate), so
  `slot_ok_t` rejects the arg AND `find_mono_clone_for_call` has no aggregate arg
  to key the `(Option int)` clone on.  Needs erased-generic monomorph resolution.
- **`inner`** (shift-abort-crossfn): a CROSS-FUNCTION capturing shift
  (`(shift (fn [k] (+ x 5)) 0)` whose `reset` is in `outer`) -- control-flow
  (shift/reset) territory, the cross-function-resume machinery, not effect taint.

### Slice PR (LANDED) -- CPS-emit by-value-flat-product monomorphs of ordinary defns

STRUCT-OR-TAINT distinct roots **7 -> 5**. Suite 2179/0 (4 by-value Option
snapshots regenerated, output unchanged). The owning-track mono-template route, now
landed with the two gates the investigation below identified: `mono_sig_ok` widens
to a by-value FLAT PRODUCT (`slot_box_ty`) for an ORDINARY defn only, EXCLUDING (a)
a typeclass-instance method (`spec->typeclass_inst` -- dict carrier ABI collides)
and (b) a heap-ADT/struct handle (`carrier_handle_ok` -- interior carrier fields
mishandled).  Plus `binding_cps_reachable` (so `cps_to_direct` sees a mono-template)
and aggregate-only `find_mono_clone_for_call` matching.  Cleared `test-option-eq-
same` / `test-option-eq-diff`.  Remaining owning roots: `test-option-eq-nones` (the
erased `(none)` : `(Option A)` is an int64 carrier, not a `TY_APP` aggregate, so no
clone resolves) and `re-parse-class` (a by-value STRUCT return/threading -- check
whether `RxParse` is a `slot_box_ty` flat product or carries drop glue).

### Owning by-value-ADT track -- investigation findings (root cause, now fixed by PR)

Ground truth (the PR above implements the "bounded fix" this identified):

- **Root cause confirmed.** `test-option-eq-*` call `option-eq?` (SIG-REJECT: its
  `(Option A)` params are by-value aggregates `sig_slot_ok` cannot spell), so the
  call is cps->direct and the by-value `(Option int)` arg cannot cross.
- **The mono-template route works for a DIRECTLY-called by-value-ADT generic.**
  Three coordinated changes made `option-eq?` a mono-template and its callers
  native-CPS: (1) `mono_sig_ok` admits a by-value-aggregate CONCRETE param/return
  via the wider `slot_ok_t` (a concrete clone name is unique, no carrier/concrete
  divergence); (2) a `binding_cps_reachable` helper (`in_s || mono_template`) so
  `term_core_ok`'s `cps_to_direct` agrees with emit_term's `binding_in_s || mclone`;
  (3) `find_mono_clone_for_call` must match on AGGREGATE args ONLY (`TY_APP` /
  `TY_ADT` / `TY_STRUCT`) -- a fn-value comparator arg is carried as `void *` and
  never matches the generic fn-param C type, so matching on it spuriously fails.
  With these, option-basic COMPILED and printed the correct `true/false/true`.
- **Why it reverted: the mono_sig_ok relaxation collides for DICT-reached clones.**
  A typeclass-instance monomorph (Eq/Show: `eqmap-*`, `set-*`, `show-collections`,
  ~12 fixtures) has its `<clone>` emitted BOTH by the CPS backend (concrete `__cps`
  + wrapper) AND via the carrier/dict path -> duplicate-definition / type-mismatch
  `cc` failures (exactly the `sig_slot_ok` comment's warning).  The direct-called
  `option-eq?` had no dict path, so it was collision-free.
- **The bounded fix** is to gate the `mono_sig_ok` by-value relaxation to a clone
  that is NOT also emitted via a carrier/dict path (or make the direct emitter
  skip a specific CPS-owned clone -- the skip is currently per-binding, not
  per-clone).  `test-option-eq-nones` additionally needs the erased `(none)`
  `(Option A)` (an int64 carrier, not a `TY_APP` aggregate) to resolve a clone.
  `re-parse-class` is the same by-value-struct-signature family.

### Slice PQ (LANDED) -- capability (struct-field fn-value) call delegation

STRUCT-OR-TAINT distinct roots **10 -> 7**. Suite 2179/0. A `handle` whose body
invokes an effect-annotated fn-value stored in a STRUCT FIELD -- `(.print-line cap
"..")`, `Printer.print-line : fn #fx{Write}`, handler handles Write -- is an
EX_CALL with `fn_binding == NULL` (an indirect `.field` callee) that
safe_to_delegate rejected outright.  Now: (1) a binding-less indirect callee
routes through `fnvalue_call_wbd_delegatable`; (2) `expr_fn_effect_row` reads a
capability field's row off the record ctor field (`CtorField.effect_row`); (3)
that row is ERK_UNRESOLVED at CT-IR time, so `row_concrete_all_wbd_handled` now
matches an UNRESOLVED row by symbolic effect name too.  `main` whole-body-
delegates -> PL seeds Write fiber -> `do-write-line` + the `__fn_128*` callback
victims reclassify to SIG-TAINT.  Cleared `do-write-line`, `__fn_1282`,
`__fn_1283`.

**Remaining 7 roots** (this session: STRUCT-OR-TAINT 26 -> 7 across PK-PQ, suite
2179/0 throughout): `log-add` + `main` (currying-effect-partial) need the
EX_CLOSURE partial-app keystone -- `(log-add 10)` is a capturing closure (a
partial application of a colored fn) that `main`'s handle body binds and calls;
`main` evicts BODY-UNSUPPORTED on that EX_CLOSURE, so it never reaches the
handler-installer delegation.  `test-option-eq-*` (3) + `re-parse-class` are the
owning by-value-ADT track (carrier-box a by-value ADT arg in the cps->direct
delegation).  `inner` is a taint victim.

### Slice PP (LANDED) -- cross-HOF leaf-fiber delegation clears apply-logged

STRUCT-OR-TAINT distinct roots **11 -> 10**. Suite 2179/0. The cross-function
analogue of PO: a colored fn whose body calls ANOTHER leaf-fiber HOF with an
effectful callback (`apply-logged = (apply callback x)`, `apply : #fx{e}` indirect-
calls its param, `callback : #fx{Log}`) is itself permanently fiber and now whole-
body-delegates. `colored_call_wbd_delegatable` admits a call to a colored GLOBAL
callee whose own body `expr_has_indirect_fnvalue_call`s, when >=1 argument is a
concrete-effectful fn-value -- no local handle required (the effect is permanently
fiber, escaping to a caller's fiber handler; a DK handler over a fn-value-reached
effect is impossible, so taint keeps it fiber). Cleared `apply-logged`.

**This session's arc: STRUCT-OR-TAINT distinct roots 26 -> 10** (Slices PK, PL,
PM, PN, PO, PP; suite 2179/0 throughout). The remaining 10 need three NEW shapes,
each a distinct slice:
- **`do-write-line`** (capability-effect-poly): a bare `(perform (Write ..))`
  invoked INDIRECTLY through a STRUCT FIELD (a `Printer` capability). Its handler-
  installer `main` calls the capability via `EX_GET_FIELD` + call, a fn-value shape
  `fnvalue_call_wbd_delegatable` does not yet cover -> extend it to a struct-field
  fn-value callee.
- **`log-add`** (currying-effect-partial): a concrete-effect fn PARTIALLY applied
  (`(log-add 10)`) into a capturing closure that `main` then calls -- `main` evicts
  BODY-UNSUPPORTED on `EX_CLOSURE`. This is the documented capturing-closure-as-
  value keystone; the handler-installer delegation needs the closure admitted.
- **`test-option-eq-*` (3) + `re-parse-class`**: the owning by-value-ADT track
  (carrier-box a by-value ADT arg in the cps->direct delegation).
`__fn_1282/1283`, `inner`, `main` are taint victims that clear once their root does.

### Slice PO (LANDED) -- leaf-fiber self-recursive HOF whole-body delegation

STRUCT-OR-TAINT distinct roots **12 -> 11**. Suite 2179/0, no codegen churn.
Implements the gated self-call delegation the PN note called for: a colored HOF
that INDIRECT-calls an EFFECTFUL fn-value and self-recurses (`map-list =
(+ (f n) (map-list (- n 1) f))`, `f : #fx{e}`) is permanently fiber and now
whole-body-delegates its recursion instead of evicting (moved into S; same direct
body). The gate `expr_has_indirect_fnvalue_call` requires the fn-value callee's
effect row NON-EMPTY -- which is exactly what keeps P6's `__cons-fmap` (indirect-
calls a PURE fmap, empty row) on its native heap-join path, and keeps normal
recursive functions native. Cleared `map-list`.

**Remaining 11 roots.** `apply-logged` (a CROSS-HOF leaf-fiber case: calls
`apply`, itself a leaf-fiber HOF, passing an effectful callback -- not self-
recursive, no local handle) + its taint victims (`__fn_1282/1283`,
`do-write-line`, `inner`, `log-add`, `main` in those fixtures); and the owning
by-value-ADT track `test-option-eq-*` (3) + `re-parse-class`. Next slices: (1)
extend leaf-fiber delegation to a call to ANOTHER leaf-fiber HOF with an effectful
callback and an escaping effect (apply-logged); (2) carrier-box a by-value ADT arg
in the cps->direct delegation (test-option-eq-*).

### Slice PN (LANDED) -- delegate a fully-handled fn-value call in a handler-installer

STRUCT-OR-TAINT distinct roots **14 -> 12**. Suite 2179/0. A `handle` whose body
INDIRECT-calls a fn-value whose entire (concrete, non-empty) effect row is
discharged locally -- `run-with` = `(handle (f) (E [] k) (resume k 5))`,
`f : (fn [] #fx{E} int)`. `fnvalue_call_wbd_delegatable` relaxes guard (2) for this
shape; Slice PL's base-taint seeding keeps E fiber, closing the historical
fiber<->DK split that made the guard necessary. Cleared `run-with`, `my-eff`.
Both historical-regression fixtures (handle-effectful-fn-param-same-fn,
currying-effect-partial) recategorize AND print correct output.

### The remaining 12 STRUCT-OR-TAINT roots + the leaf-fiber-HOF next slice

After PK-PN the still-BODY roots are: **(a) leaf-fiber HOFs** `map-list`
(`(+ (f n) (map-list (- n 1) f))`) and `apply-logged` (`(apply callback x)`) --
a colored function that INDIRECT-calls an effectful fn-value (permanently fiber)
and either self-recurses or calls another polymorphic HOF, with the effect
ESCAPING (no local handle); plus their taint victims (`__fn_1282/1283`,
`do-write-line`, `inner`, `log-add`, `main` in those fixtures). **(b) the owning
by-value-ADT track** `test-option-eq-*` (3) + `re-parse-class`.

**Attempted + REVERTED: a blanket self-recursive-call delegation.** Allowing ANY
self-call (`fn == cur_fn`) to whole-body-delegate DOES clear `map-list` (verified
correct output), but it is far too broad: it routes NORMAL recursive functions
(defn-basic, letrec-*, continuation-*, dozens more) from native CPS onto the
direct-emitter delegation path, a step BACKWARD (delegation relies on the very
direct fallback the endgame deletes) -- a large codegen churn / regression. The
sound version must GATE the self-call (and cross-HOF) delegation to a function
that is genuinely leaf-fiber -- i.e. its body contains an indirect call THROUGH A
FN-VALUE (a fn-typed param / local closure), which makes its effect permanently
fiber. That whole-function property (scan the body for an indirect fn-value call)
is the next slice; without the gate the relaxation regresses.

### Slice PM (LANDED) -- delegate an effect-polymorphic HOF call in a handler-installer

STRUCT-OR-TAINT distinct roots **16 -> 14**. Suite 2179/0. Extends PL to a callee
with a POLYMORPHIC (`#fx{e}`) effect row: it performs only its fn-value arguments'
effects, so `main`'s `(map-list 3 callback)` inside a delegated handle is safe to
delegate when every fn-typed arg's effect row is concrete + fully handled (`main`
handles Log; `callback : #fx{Log}`). `colored_call_wbd_delegatable` walks the
args via `expr_fn_effect_row` (the referent lifted closure's `inferred_effect_row`
-- the lifted binding's own fn type drops the row). Cleared `__fn_1281`,
`__fn_1285`; `main`/`__fn_128*` clear in many more fixtures. `map-list` itself
stays a genuine BODY root (a polymorphic HOF that threads + indirect-calls an
effectful fn-value with no concrete effect to perm-taint -- fat-closure __cps ABI
territory).

### Slice PL (LANDED) -- permanent-fiber classification for whole-body-delegated effects

STRUCT-OR-TAINT distinct roots **22 -> 16**. Suite 2179/0. No codegen change (the
recategorized fns were already evicted to the direct emitter; only the EVICT
CATEGORY moves BODY-STRUCT-OR-TAINT -> SIG-TAINT).

An effect reached through a direct-emitted indirect (fn-value) call is
PERMANENTLY fiber for the current backend -- the DK machine cannot thread a
continuation through an opaque fn-value call -- so its taint cascade is permanent
SIG routing, NOT a fixable BODY root. The taint model did not recognize this: a
whole-body-delegated colored fn (`apply-cb`, body = one CT_LETRAW around the
indirect call; `run`, handle delegated) runs its effects on the FIBER runtime,
yet its effects were not in the base (permanent) fiber taint.

- **ensure_S** seeds a whole-body-delegated candidate's effects
  (`term_is_whole_body_delegation`: a single CT_LETRAW tailed by an appcont to
  KK_RET) into `base_lo/base_hi`, which feeds BOTH the main and permanent taint
  fixpoints -> the effect classifies permanently fiber. A control-free single-op
  body matches the shape but has empty effects (no-op).
- **safe_to_delegate** lets a handler-installer (`run` = `(handle (apply-cb ..)
  (Ask ..))`, apply-cb : #fx{Ask}) whole-body-delegate a call to a colored GLOBAL
  callee when ALL the callee's declared effects are discharged by the enclosing
  delegated handle (`callee_effects_all_wbd_handled`) -- so run's term becomes a
  whole-body delegation and the seeding above fires.

Cleared (BODY -> SIG-TAINT): `run`, `run-all`, `counted-sum`, `f`, `g`, `greet`.
Verified: effectful-callback repro still prints 27; effect-poly / effect-row /
currying-effect-partial fixtures match expected output.

**Remaining 16 STRUCT-OR-TAINT roots + the extension that clears most.** The
still-BODY effectful-fn-value roots (`__fn_1281/1282/1283/1285`, `apply-logged`,
`map-list`, `run-with`, `my-eff`, + taint victims `do-write-line`, `inner`,
`log-add`, `main`) have a handler-installer whose delegated body calls a callee
with a POLYMORPHIC effect row (`map-list : #fx{e}`), which `callee_effects_all_
wbd_handled` rejects (it requires ERK_CONCRETE). The instantiated effect at the
call site is the fn-value ARGUMENT's effect (the callback's `Log`), which the
handle discharges -- so the sound extension is: for a callee with an ERK_VAR row,
require every fn-typed argument's effect row to be concrete and subset of
`g_wbd_handled`. `test-option-eq-*` (3) stay on the owning by-value-ADT-arg track
(unrelated).

### Slice PK (LANDED) -- pure fn-value threading into an effect-free callee

STRUCT-OR-TAINT distinct roots **26 -> 22**. Suite 2179/0.

A colored function that threads a fn-VALUE (a plain `TY_FN` closure/fn-pointer or
a poly-fat closure) as a call ARGUMENT used to always evict on that arg:
`call_arg_ok` rejects a fat-fn atom, and a plain `TY_FN` atom fails the slot gate.
That kept the pure higher-order-value-threading family on the fiber path.

New `call_args_ok(fn, args, n, cps_to_direct)` admits a fn-value arg when the
CALLEE is provably effect-free (`callee_effect_free`: its DECLARED effect row is
empty -- NULL / `ERK_EMPTY`; a row variable or any concrete effect reads
non-empty and is rejected, so the gate is conservative and fixpoint-independent,
reading the elaboration-time row, not the taint pass). Wired into the three
call-arg admission sites (`term_core_ok` CT_LETCALL/CT_TAILCALL, `handle_delim_ok`
CT_TAILCALL).

**Soundness.** An effect-free callee cannot perform an effect THROUGH a fn-value
argument: were the callback effectful and invoked, that effect would propagate
into the callee's own signature by row polymorphism (`apply-cb` calling
`f : (fn [] #fx{Ask} int)` is itself `#fx{Ask}`). So no fiber<->DK effect mismatch
can arise (the `unhandled effect` hazard in the `handle_delim_ok` NOTE), and the
fat / one-word carrier crosses the call by value inline via `atoms_csv` -- no
one-word slot spill. Verified the EFFECTFUL callback family stays correctly
evicted: a minimal `apply-cb` repro (callback performs Ask through a direct-
emitted indirect call, `run` installs a DK prompt) keeps `run` + the lifted
callback on the fiber path and still prints 27.

Cleared roots (all match `expected.stdout`): `test-map` / `test-filter`
(hamt-lisp-map-filter, pass `double-val`/`keep-big` into `hamt/map`/`hamt/filter`),
`test-merge-with` (hamt-lisp-merge-with), `then-parser-impl`
(hkt-stdlib-parser-instances).

**Post-PK census -- the 22 remaining STRUCT-OR-TAINT roots split into THREE
tracks (ground truth for the next slice):**

1. **Effectful fn-value / fiber<->DK interop (~16, the dominant wall).** `run`,
   `run-all`, `run-with`, `map-list`, `apply-logged`, `my-eff`, the lifted
   callbacks `__fn_1281/1282/1283/1285`, and their taint victims (`counted-sum`,
   `do-write-line`, `log-add`, `greet`, `f`, `g`, `inner`, `main`). Shape: a HOF
   (`apply-cb`) reaches an effect through a DIRECT-EMITTED indirect (fn-value)
   call `(f)`; the DK backend cannot thread a continuation through an opaque
   fn-value call, so the effect must be performed on the FIBER runtime and its
   handler (`run`) must be a fiber handler. Verified sound + intended (the
   `cps-backend-effectful-callback` fixture comment states Ask goes through the
   fiber dynamic-handler chain). This is genuinely un-CPS-emittable for the
   current backend; solving it needs a fat-closure `__cps` ABI so an indirect
   call can thread the DK -- the "indirect-callee admission" the top-of-doc table
   anticipates. A large slice, not a widening.
2. **By-value-ADT arg into a SIG-REJECT (direct) callee (`test-option-eq-*`, 3).**
   `option-eq?` is permanently `SIG-REJECT` (its `(Option A)` params are by-value
   aggregates the CPS signature cannot spell), so `test-option-eq-same/diff/nones`
   call it cps->DIRECT, threading a by-value `(Option int)` arg (or, for `(none)`,
   a generic Option ERASED to the bare `int64_t` carrier) that `call_arg_ok`
   rejects for a cps->direct call. The direct emitter BOXES such an arg
   (`emit_byvalue_carrier_abi`); the CPS delegation does not. Fix direction: box a
   by-value-ADT arg in the cps->direct delegation (admission + `atoms_csv` emit),
   the carrier-ABI/owning track.
3. **Struct return/threading (`re-parse-class`, 1).** A pure `defstruct`-returning
   function colored may-capture; the owning/by-value-aggregate track.

#### Track 1 design -- the fat-closure `__cps` ABI (the critical path)

This is the dominant remaining wall (~16 roots) and the confirmed hard core of
N6.5. It is NOT a recategorization: no honest `SIG-*` relabel applies, because a
function like `run` has a REAL control op (a `handle`) that needs an effect
runtime -- unlike a `SIG-REJECT` fn, which by definition contains no control op
and lets the direct emitter own its ABI. `run` genuinely must lower its handle on
SOME machine; the only reason it can't be the DK machine today is that the effect
it handles is reached through a direct-emitted indirect (fn-value) call inside a
callee (`apply-cb`'s `(f)`), and the DK backend cannot thread `__kont` through an
opaque fn-value call. So the effect stays fiber, taints every fn touching it, and
the whole cluster evicts.

The fix makes an indirect fn-value call DK-threadable so the effect becomes a DK
effect and the cluster CPS-emits together:

1. **Colored callbacks already have a `__cps` body** (`__fn_1283` has
   `term_core_ok = 1` -- it is admissible; it evicts ONLY by taint). Emit the
   `__cps` variant even for a callback that is currently taint-evicted, so a
   `<fn>__cps(env, args, DK *__kont)` entry exists.
2. **The fat closure (`tur_poly_fn_t` / the ordinary closure record) must carry
   the `__cps` entry** alongside the direct `fn` pointer -- a second function
   pointer field (`fn_cps`), populated when the closed-over function is colored
   and CPS-emitted. A pure/uncolored closure leaves it NULL.
3. **The indirect-call lowering** (`cps_ir.c` `indirect_callee_ok` / the
   `CT_LETRAW` delegation for `(f ...)`) must, inside a `__cps` body, emit
   `f.fn_cps(f.env, args, __kont)` when `fn_cps` is non-NULL, threading the
   enclosing continuation -- instead of the direct `f.fn(f.env, args)`.
4. **Effect classification** (`ensure_S`): an effect reached through an indirect
   call whose callee carries a `fn_cps` entry is NO LONGER forced fiber -- drop
   the indirect-call fiber-taint seed for that case so `Ask` classifies as a DK
   effect, `run`'s handle lowers as a DK prompt, and `apply-cb` / `__fn_1283`
   join `S`.
5. **Fallback**: a fn-value with a NULL `fn_cps` (pure, or an uncolored/foreign
   closure) still lowers to the direct indirect call and keeps its effect (if
   any) fiber -- so mixed programs stay correct during the transition.

Validate on `cps-backend-effectful-callback` first (expect `run` + `__fn_1283`
to leave the EVICT list and the program to still print 27), then the
`effect-poly-*` / `effect-row-*` family. High regression surface (touches the
closure record layout + every indirect call), so land behind a full suite pass
and expect a large snapshot regen. This is the last large slice before the
Phase-4 deletion; Tracks 2/3 (carrier-ABI arg boxing / monomorph resolution) are
smaller and independent.

### Slice PG/PH -- `with-abort-panic` family (TY_NEVER + nested-handle-in-continuation)

Two composable STRUCT-OR-TAINT slices via the root-profiling method (`ensure_S`
instrument -> isolate the one failing predicate -> widen soundly). Suite 2179/0.

- **PG (`TY_NEVER` panic placeholder).** A handler case ending in `(panic ...)`
  diverges; the CPS translation still threads a `!`-typed (bottom) placeholder
  word to the prompt, but the panic aborts first.  `atom_ok` rejected `TY_NEVER`,
  evicting the handling fn -> tainting the effect -> evicting the performer.  Fix:
  admit `TY_NEVER` like `TY_NIL` in `atom_ok`; lower a `TY_NEVER` binder as the
  int64_t nil word in `binder_ctype` + `emit_letraw` (not `void __t;` /
  `= ((void)0)`).  Cleared `effect-abort-panic`.
- **PH (nested handle in a handle/reset continuation).** Two sequential handles
  (`effect-abort` `main`) failed because neither `joins_closed_rec` nor
  `collect_caps_rec` had a `CT_HANDLE` case (both hit `default`).  Added it to
  both, mirroring `CT_RESET`/`CT_AWAIT`/`CT_PERFORM`: the continuation is
  same-scope; the delim/case frames are lifted (fresh join scope) and their env
  captures ride the enclosing helper; each case binds its params + `k`.  Cleared
  `effect-abort`.

STRUCT-OR-TAINT distinct fns 53 -> 52.

### Slice PI (LANDED, PI-1/PI-2/PI-3) -- printing handlers + three DK soundness fixes

Dropping the `is_println_shape` rejection in `handle_case_ok` (a handler that
prints is the COMMONEST effect shape; `emit_term`'s CT_LETPRIM already emits
`println` in a lifted frame) cleared **9 fixtures** but first exposed **three
latent DK soundness bugs** that eviction had been masking (these fixtures ran on
the fiber path before, and each bug reproduces WITHOUT println on a minimal
repro).  All three fixed, then the relaxation landed:

1. **Taint via fn-value data flow (PI-1, `60b7e11`).** A colored performer reached
   as a fn-VALUE passed to a higher-order callee (`apply-logged` hands a
   Log-performing lifted closure to `apply`, which calls `(f x)`) left the
   `ensure_S` call graph with no edge, so Rule C's handler->performer path never
   spanned the fiber intermediary -> handler on DK, performer's chain severed ->
   `unhandled`.  Fix: `expr_collect_effects_acc` records an `EX_VAR` naming a
   colored fn-value as a reachable callee (the precise data-flow edge -- NOT the
   `edges_all` over-approximation, which regressed ~19 fixtures in a discarded
   attempt).
2. **Multi-effect deep-handler re-installation (PI-2, `2c41f84`).** A handle with
   N cases emits one `dk_handler` node per effect; on a deep resume `dk_perform`
   re-installed only the SINGLE performed tag, so a sibling effect performed in the
   resumed continuation escaped (perform-order dependent).  Fix: re-install the
   maximal consecutive `DKK_HANDLER` run (the whole group).
3. **Enclosing-handler propagation (PI-3, `a7d3b4e`).** A deep handle's
   continuation-frame `next` was `dk_done()`, burying the enclosing handlers in
   the frame's `__k` env -- so an effect an inner handle does NOT handle, performed
   in its body, could not propagate to an outer handler (`inner` handles Write, its
   body performs Log -> `main`'s Log handler).  Fix: the frame `next` carries the
   enclosing handler markers (`dk_copy_enclosing_handlers(cur_k)`), same as shallow
   and `emit_reset`; the markers are transparent to the returning value.

Gate: cleared `effect-dump`, `effect-handle-reduce`, `effect-handler-subtype`,
`effect-hierarchy`, `effect-let-subsumption`, `effect-log`, `effect-stdlib-io`,
`effect-strict-mode`, `try-with-basic` (and `effect-poly-infer` now correct on
DK).  STRUCT-OR-TAINT 52 -> 47 distinct fns, 71 -> 62 evicting fixtures.  Suite
2179/0.  The three fixes are independent soundness wins beyond the println payoff.

### Slice PF -- self-contained handle delegation clears EX_WHILE

`BODY-UNSUPPORTED` **5 -> 4** (`EX_WHILE` -> 0). Suite 2179/0.
`effect-handler-capture-loop` -- a `while` loop whose body installs a fresh
per-iteration `handle` (Ask, resumed with `cur*10`) -- now CPS-emits (`run__cps`),
prints 100.

The loop was never the blocker (`EX_WHILE` was already delegatable when its
cond+body are); the blocker was the real (non-`unsafe`) `handle` in the body.
A handle that fully discharges its effect is SELF-CONTAINED: the direct emitter
emits its own fiber handler frame (which falls back to
`global_effect_handler_chain` when no fiber is active, so it runs inside a
`__cps` body), and the whole while+handle region delegates as one CT_LETRAW,
then `dk_run` delivers to the DK continuation. `safe_to_delegate` now admits a
real handle under `g_whole_body_delegate`, pushing its handled effects onto a
`g_wbd_handled` stack; an interior `perform` is delegatable ONLY when its effect
is on that stack (discharged locally), a `resume` only inside a delegated handle.

The soundness hazard is fiber<->DK non-interop: a delegated (fiber) handle must
not enclose a perform that threads the DK. Two guards enforce this: (1) a
`perform` of an effect NOT locally handled stays native (its function keeps a
non-empty net effect and never whole-body-delegates -- verified: a `while`+bare
`perform` handled by the CALLER stays evicted); (2) inside a delegated handle, a
call through a fn-VALUE (a colored param/closure, or any indirect callee -- which
`callee_colored` cannot see) is rejected, since it could perform via the DK
past the fiber frame. Guard (2) was found by regression: without it,
`currying-effect-partial` (handle body calls a colored closure) and
`handle-effectful-fn-param-same-fn` (handle body calls a colored fn-param) were
wrongly delegated and crashed `unhandled effect`; both are now correct (the
former stays a residual `EX_CLOSURE` eviction, the latter CPS-emits natively).

Residual `BODY-UNSUPPORTED` (4): `EX_CLOSURE` x1 (`currying-effect-partial`),
`EX_DEFER` x1 (`effect-defer`), `EX_INLINE_C` x2 (session-channel).

### Slice PE -- EX_DEFER whole-body delegation + parser closure drop-glue

Two independent eviction cuts, `BODY-UNSUPPORTED` **9 -> 5**. Suite 2179/0.

- **EX_DEFER (4 -> 1).** `safe_to_delegate` now admits `EX_DEFER` -- but ONLY
  under `g_whole_body_delegate` (the whole-body probe), not per-node decompose.
  A `__cps` function establishes no defer frame of its own, so a lone delegated
  defer would register into a sub-region frame that pops early. Gated to
  whole-body delegation, the direct emitter emits the ENTIRE body region --
  `tur_frame` init / `push_defer` / `fire_lifo` -- as one CT_LETRAW, then the DK
  continuation runs, so the defer fires at its true lexical scope. This
  genuinely admits `unsafe-basic` / `unsafe-nested` / `unsafe-defer` (colored
  only by an `unsafe` marker carrying an `rc/of` auto-drop defer; `main__cps`
  emits and runs correctly, verified 200/100/42 ordering on a discriminating
  probe). `effect-defer` (a defer sharing a `do` with a `perform`) is NOT
  whole-body-delegatable, so it stays honestly evicted on `EX_DEFER` -- the gate
  avoids the reshuffle-into-`STRUCT-OR-TAINT` that a gateless widening causes
  (the anti-pattern the Phase-1 findings warned against). This CORRECTS the
  earlier finding that "every EX_DEFER eviction ... crosses a control op": with
  the unsafe-marker handle already delegatable (PD), the 3 unsafe-* bodies are
  control-op-free and whole-body-delegatable; only `effect-defer` genuinely
  crosses.
- **EX_CLOSURE parser combinator (2 -> 1).** Annotated stdlib `mbind`'s callback
  param `^borrow ^fat` (parsec.tur). `mbind` invokes the callback in a loop and
  returns a fresh Cell list -- it borrows, never retains -- so the existing S1.2
  `hoist_borrowed_closure_args` + `cps_closure_env_freeable` path now admits AND
  frees the inline capturing closure in `then-parser-impl`, clearing its
  `EX_CLOSURE` eviction (`then-parser-raw`/`then-parser` closures likewise free).
  `currying-effect-partial` (a partial-app of a COLORED fn -- an indirect call
  through a capturing closure that itself performs `Log`) is the harder B1-style
  case and remains.

Residual `BODY-UNSUPPORTED` (5): `EX_CLOSURE` x1 (`currying-effect-partial`,
colored capturing-closure indirect call), `EX_DEFER` x1 (`effect-defer`, native
defer-frame + perform), `EX_WHILE` x1 (`effect-handler-capture-loop`), and
`EX_INLINE_C` x2 (`session-effects` / `session-mp-effects`, session-channel).

### Slice PD -- unsafe-marker whole-body delegation (2 of 4 B2 fixtures)

`free-lift-bind` and `unsafe-closure-capture` now CPS-emit (30 / 37, direct == cps
== turi). `BODY-UNSUPPORTED` 11 -> 9. Both are functions colored ONLY by an
`(unsafe ...)` block whose body passes a capturing/fat closure to `free-run`
(uncolored). An `unsafe` block desugars to a handle on the built-in Unsafe effect,
but Unsafe is a pure compile-time MARKER never performed (`is_unsafe_marker`, set
by elab_unsafe): its fiber-lift never suspends and the direct emitter emits the
body in place. So `safe_to_delegate` now admits an unsafe-marker handle whose body
is itself delegatable -- the whole region (unsafe scope + `free-run` call + fat
closure) whole-body-delegates (P1.b) to the direct emitter instead of evicting on
the closure. The fat closure is not dropped (the deferred
escaping-fat-closure-env leak, `docs/reported/escaping-fat-closure-env-leak.md`):
it is present in the emitted C on BOTH the direct and CPS paths -- the direct
`int main` merely hid it from LeakSanitizer via stack reachability, so the two
fixtures carry a `requires.no-leak-check` marker until closures get drop glue.
Suite 2179/0. The other 2 B2 fixtures (`currying-effect-partial`,
`hkt-stdlib-parser-instances`) are colored by real effects, not `unsafe`, and
remain on the drop-glue backstop.

### Slice PC -- B1 raw-int continuation payloads (whole B1 family now CPS-emits)

`effect-fn-payload-capturing` and `cross-function-resume-via-effect` (both
`f : (fn [int] int)` payloads resumed via `(resume k v)`) now CPS-emit (1107 / 11,
ASan-clean, direct == cps == turi). `BODY-UNSUPPORTED` 13 -> 11; the whole B1
family is cleared. The `int` carries no cont flavor for `defeffect` to detect (and
a genuine `(fn [int] R)` payload must not be disturbed), so the continuation is
identified by the reliable FORM signal that the payload body RESUMES its first
param -- `(resume k ...)` / `(k ...)` (`form_lambda_resumes_first_param`). On that
signal the payload's `int` param is reflavored to `multishot-effect-cont` (type-
checks: TY_CONT unifies with the declared int-carrier param) and the shared effect
object is marked `resumable_payload_param` at the perform, so the handler reuses
all Phase A wiring. Order note: the int case relies on performer-before-handler
elaboration (source order); the annotated case stays order-independent. Suite
2179/0.

### Slice PB -- top-level handle-body admission for computation-join-to-prompt (general; completes multishot-effect-cont-kv-sugar)

`multishot-effect-cont-kv-sugar` now CPS-emits (23, ASan-clean). The blocker was
NOT the double-resume (Phase A's substrate already handles `(+ (k 1) (k 2))` -- a
direct-body probe returns 3) but its handled body `(+ 10 (inner))`: a top-level
`handle`'s handled body was admitted with the strict `term_core_ok`, which forbids
the interior KK_PROMPT delivery a computation join emits
(`letcont j(t){+10 t; <prompt>} in tailcall inner(j)`). This is a GENERAL gap --
a plain `(defeffect Ask [] :int)` with the same `(handle (+ 10 (inner)) ...)`
shape evicted identically. Fix: `handle_delim_ok` (emit_cps_ir.c) admits the
handled body like `term_core_ok` but permitting KK_PROMPT delivery through
pure-computation / join / cps->cps-tailcall shapes, while deliberately NOT
admitting an interior control op or a delegated (`CT_LETRAW`) effectful call
(which performs on the fiber runtime, not the DK prompt -- so `(handle (f) ...)`
with an effectful fn-param stays on fiber). Iterative landing: the first cut
(reuse `delim_ok`) regressed effect-error-codes / effect-handler-capture-nested /
handle-effectful-fn-param-same-fn; `handle_delim_ok` is the precise predicate.
Suite 2179/0. This is a `BODY-STRUCT-OR-TAINT` reduction (the multishot sibling
was never `BODY-UNSUPPORTED`), and a general one -- any effect fixture whose
handle body computes through a colored call now CPS-emits.

### Slice PA -- B1 cont-substrate bridge LANDED for effect-cont-typed resumable payloads

`effect-cont-kv-sugar` now CPS-emits end-to-end (performer + handler), the first
user resumable-payload effect to do so outside the synthesized `__Shift` path.
`BODY-UNSUPPORTED` 14 -> 13. Corrects P-inv3's "not a bridge slice, Phase-2
only": the bridge IS landable for `effect-cont`-typed payloads (P-inv3's own
caveat), and this proves it. Mechanism (one unit, per
[cps-dk-multishot-user-effects-plan.md](cps-dk-multishot-user-effects-plan.md)
Phase A): (1) `reflavor_effect_payload` upgrades a `(fn [k : effect-cont] ...)`
payload's cont param to `multishot-effect-cont` at the perform site so its
`(k v)` lowers to the DK-backed `tur_cloneable_cont_resume`; (2) a per-effect
`resumable_payload_param` (detected at `defeffect` from the param type FORM --
the cont flavor collapses to its TY_INT carrier in the stored Type) drives both
halves; (3) the handler case auto-upgrades an un-annotated `k` to `CK_MULTISHOT`
(this also flips the cloneable-runtime emission gate); (4) the perform-arg gate
admits the boxed-fn payload atom (`PerformExpr.resumable_payload`), and
`is_delegatable_value` admits a capturing payload (`Closure.is_effect_payload`);
(5) the handler-case cloneable-cont wrap + boxed-payload `arg` reap
(`emit_cps_ir.c` ~3350) fire on `hcase->resumable_payload`, generalizing the
`is_shift_effect` gate but scoped precisely so a hand-written `^multishot`
handler on a non-payload effect is untouched. Verified: output 1107 on
direct == cps == turi, ASan-clean on the CPS path, suite 2179/0, the `int`-typed
B1 fixtures (Phase C) unchanged (still evict to fiber). `multishot-effect-cont-kv-sugar`
(payload resumes twice) still evicts -- the multi-shot-resume admission is the
Phase B remainder.

### Investigation P-inv3 -- B1 is not a bridge slice: user resumable-payload effects already evict to fiber (no code landed; corrects P-inv2)

Attempted the B1 bridge for `effect-cont-kv-sugar` (P-inv2's "single most
tractable slice") and found the premise false. P-inv2 read
`multishot-effect-cont-kv-sugar`'s `--dump-cps` output as "already CPS-emits" --
but `--dump-cps` is dump-only (never wired to codegen), and under
`TUR_TRACE_EVICT` that clean, capture-free, multishot-annotated fixture EVICTS on
`BODY-STRUCT-OR-TAINT` and runs on the fiber runtime (`tur_handler_dispatch`
building a `tur_cloneable_cont` from a fiber STACK IMAGE via `__tur_msdyn_cont`,
not a DK chain). A minimal capture-free ONE-SHOT `effect-cont` payload
(`inner`/`outer`, `(k 7)`) evicts the same way and runs correctly (17) via fiber.
So the taint fixpoint **correctly evicts every user resumable-payload effect to
the proven fiber path** -- only the fully-synthesized `__Shift` desugar CPS-emits.
The 3 B1 fixtures surface as `BODY-UNSUPPORTED` (not `-STRUCT-OR-TAINT`) only
because their payloads CAPTURE, tripping `EX_CLOSURE` in CT-IR translation before
the taint fixpoint evicts them. Re-applied the `is_effect_payload` admission
patch to confirm: with it, `effect-cont-kv-sugar`'s performer stops evicting and
CPS-emits -- then miscompiles at runtime (`continuation error: not a capturable
continuation`), because the patch's perform-arg `atom_ok` widening flips the
taint `in_s` true and bypasses the correct fiber eviction. That widening is
UNSOUND; the patch is reverted and dropped (not preserved -- it forces an
un-bridged CPS emission, not merely a gated one). Net: no code landed; B1's real
blocker is the DK-side multishot-cont support the clean multishot sibling also
lacks (Phase-2), corrected in "Honest distance" above.

### Investigation P-inv2 -- EX_CLOSURE is two families; the cont-bridge is DK-backed via tur_cloneable_cont (no code landed)

Follow-up on P-inv, tracing the continuation substrate to decide whether the B1
bridge is feasible. Two findings. (1) The 7 `EX_CLOSURE` evictions are NOT one
family: 3 are resumable fn-payload effects (B1, cont-bridge) and 4 are capturing
closures used as indirect-callee / HOF-argument values in colored code (B2,
closure-drop-glue) -- confirmed by dumping each (`currying-effect-partial` evicts
on the partial-app closure `__papc1282` in its handle body, not on a payload;
`unsafe-closure-capture` / `free-lift-bind` evict on the `(fn [inner] ...)` passed
to `free-run`; `hkt-stdlib-parser-instances` on parser-combinator closures). (2)
The B1 bridge is real and DK-backed: `tur_cloneable_cont` is a generic
`cont_fn(env,value)` wrapper that __Shift builds with `__dk_cont_fn` (=
`dk_invoke`) over a `dk_copy_range` chain env, and the direct emitter's
`emit_effects_resume` already routes a `CK_MULTISHOT` continuation through
`tur_cloneable_cont_resume` (that DK-backed path) -- so a user resumable-payload
effect whose cont param is reflavored to `multishot-effect-cont` resumes through
the SAME substrate the CPS handler produces. The bridge is thus "generalize
__Shift's reflavor + handler cloneable-wrap to user effects," tractable for
`effect-cont`-typed payloads and blocked on a runtime dispatch tag for raw-`int`
payloads. Documented in "Honest distance" (B1/B2); no code landed -- the bridge
is a real semantic unification that wants its own scoped slice, not a tail-end
change.

### Investigation P-inv -- the residual-14 blockers are deeper than "scoped admission" (no code landed)

A focused pass on the largest residual family (`EX_CLOSURE`, 7) to land the
plan's designated "next tractable slice" (reap at the handler gated on a local
escape check). **Finding: the slice as scoped does not exist -- the blocker is
the continuation substrate, not the env free.** Implemented the clean admission
(new `Closure.is_effect_payload` flag set at the user-effect perform payload in
`elab_effects.c`; admitted in `is_delegatable_value`; perform-arg gate widened
from `is_shift_perf && shift_recv_atom_ok` to `shift_recv_atom_ok` for any
effect). This correctly stopped `producer` (`effect-fn-payload-capturing`)
evicting -- but the build then **miscompiled**: the CPS handler case binds `k`
as a raw `DK *subk` and passes it to the direct-emitted payload `f`, whose
`(resume k ...)` expects a fiber effect-cont handle (`int64_t`), not a `DK *`
(`note: expected 'int64_t' but argument is of type 'DK *'`; runtime
`continuation error: not a capturable continuation`). The __Shift path only
works because its desugar reflavors the receiver's `cont` param to cloneable so
the resume matches the handler's cloneable bridge; a user payload's `k` (`int` /
`effect-cont`) has no annotation to reflavor. Making user resumable-fn-payload
effects work under CPS therefore needs a DK<->fiber continuation bridge (or the
reflavor generalized), a substrate change well beyond admission + reap. Reverted
to a clean tree; preserved the admission patch at
`/tmp/effect-payload-admission.patch` for when the bridge lands. Also traced the
`EX_DEFER` family: `unsafe-basic`/`-nested` evict on `EX_DEFER` only because
`expr_has_unsafe_control` rejects the pure `(& p)` inside the `Unsafe` handle,
but the `ptr` borrow is a co-located `BODY-STRUCT-OR-TAINT` blocker so a defer
fix only moves the eviction category (matches the prior revert). Net: no code
landed; the "Honest distance" section above is rewritten with these verified
blockers so the roadmap stops under-scoping the residuals.

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
delegation. **Slice P1.c below closes the freeable subset of those.**

### Slice P1.c -- closure KEYSTONE (effect-bearing shape): leaf-admit + boundary-reap the freeable subset

The effect-bearing closures P1.b left evicting -- a closure built in a handler
case / shift / perform continuation -- are now admitted when they are provably
**freeable**, generalizing the P3.d receiver-reap tactic. Such a closure is
leaf-admitted via `CT_LETRAW` and its heap fat-env is registered for a single-node
free at the outermost DK entry boundary (a new `letraw.reap_env` flag ->
`__dk_reap_ptr` in `emit_letraw`), closing the leak that made a general
leaf-admitted closure unsound on the CPS path.

Admission gate `cps_closure_env_freeable` mirrors the direct emitter's
`let_binding_env_freeable`: the init is a **capturing** closure (capture-free is
already delegatable; a `__Shift` receiver is **excluded** -- it is reaped by the
handler-case path P3.d, so never twice), it returns a **scalar** (its result
cannot alias the env), and the bound name does **not escape** the let body or any
sibling init. Escape is decided by `closure_binding_escapes`, which is
conservative -- `EX_PERFORM` and every unmodeled control form default to
"escapes" -- so a reaped closure PROVABLY does not escape (no early free / UAF),
and boundary reap of distinct per-construction mallocs never double-frees.

Validated under ASan/LSan: `run-handler` and `rc-auto-drop-closure-capture` are
admitted and leak-clean; the `closure-env-no-leak` ASan target (multi-shot
handler-case closure + per-iteration loop closure) stays green; a probe passing a
closure as a `perform` arg correctly still evicts (no reap -> no UAF), output
correct. **Gate: `BODY-UNSUPPORTED` 16 -> 14** (`EX_CLOSURE` 9 -> 7). The
remaining 7 `EX_CLOSURE` evictors escape (used as an effect payload) or return a
non-scalar -- correctly left evicting, as reaping them would be a UAF or an
env-aliasing free; they are the GENERAL escaping-closure case (needs closure
drop-glue, `escaping-fat-closure-env-leak.md`). Suite 2179/0.

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

- **`EX_DEFER`** (1 left after Slice PE; was 4). A lone `EX_DEFER` delegated via
  `CT_LETRAW` in per-node position would register into a sub-region frame that
  pops early (a `__cps` function establishes no defer frame of its own) -- so a
  gateless widening is wrong and only reshuffles the eviction into the unnamed
  `BODY-STRUCT-OR-TAINT` bucket. BUT under `g_whole_body_delegate` the direct
  emitter emits the WHOLE body region (frame init / push_defer / fire_lifo) as
  one CT_LETRAW, and the defer fires at its true scope. Slice PE landed exactly
  that gating and admitted `unsafe-basic` / `-nested` / `-defer` (once the
  `unsafe`-marker handle is delegatable per PD, those bodies are control-op-free
  and whole-body-delegatable). What remains is `effect-defer`: a defer sharing a
  `do` with a `perform` is genuinely control-crossing, is not
  whole-body-delegatable, and needs native `term_core_ok` + `emit_term`
  defer-frame lowering rather than delegation.
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
