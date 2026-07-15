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
