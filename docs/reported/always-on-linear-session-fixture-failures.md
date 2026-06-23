# Always-on linear/session checking: 5 residual fixture failures

**Summary:** After the `-X` feature flags became no-ops (all features
unconditionally on, commit `d7410a6`), five fixtures remain red under the
by-value suite. Each is a genuine semantic tension between always-on
linear/uniqueness/session checking and a pattern the fixture relies on.
**Severity:** low-medium (5 fixtures; the systemic regressions from the flag
flip are already fixed -- by-value suite is at 1768 passed, 5 failed).

These are grouped because they share a root theme: a discipline that used to
be opt-in (`-Xlinear`/`-Xsubstructural`/`-Xsessions`) now runs on programs
that were written without it, and the existing escape hatches do not cover
every shape. They are left for a maintainer design call because each carries
real regression risk -- e.g. naively making `@r` (deref) non-consuming breaks
~15 ref fixtures that rely on the deref-consumes-the-ref workaround.

## 1. `ref<T>` is must-consume linear, but auto-drop / deref relationships are inconsistent

Fixtures: `ref-explicit-drop`, `ref-if-branch-move-suppression`.

Minimal repro (`ref-explicit-drop`):

```turmeric
(defn main [] : int
  (let [r (ref 42)]
    (println @r)     ; deref CONSUMES r (linear-consume on the inner EX_VAR)
    (drop! r)        ; -> TUR-E0101: linear value 'r' used after being consumed
    0))
```

Root cause: a `ref<T>` binding is inferred linear (must consume exactly once;
`src/compiler/elab_fns.c:1554` infers `TY_REF` params SK_LINEAR, and let
bindings of `ref` type are linear). `@r` reads *through* the ref but the
generic EX_VAR path marks the linear binding consumed
(`src/compiler/elab_toplevel.c:420`). `ref-basic`
(`(let [r (ref 42)] (println @r) 0)`) only passes *because* deref consumes
`r`, satisfying the must-consume obligation -- the linear auto-drop is
deliberately skipped for linear refs (`src/compiler/elab_forms.c:847,946`).

Fix direction: make deref (and `::` reinterpret) non-consuming, and let the
ref auto-drop count as the single consumption at scope exit -- but the
auto-drop and an explicit `(drop! r)` must not both fire (double free).
This is a coordinated change across deref / scope-exit linear check /
auto-drop injection / `drop!`. Attempting only the deref half regresses
`ref-basic`, `ref-deref`, `ref-move`, `rc-auto-drop-*`, etc.

## 2. Linear channels aliased / shared across closures

Fixtures: `reactor-fibers-park-chan`, `schan-worker-pool`.

Minimal repro (shape):

```turmeric
(let [ch (chan-new 4)]
  (local-spawn g (fn [user] : nil ... (:: ch :ptr<void>) ...))  ; capture 1 consumes ch
  (local-spawn h (fn [user] : nil (chan-send ch 99) ...))       ; capture 2 -> use-after-consume
  (chan-free ch))
```

Root cause: a `Chan` is linear. Capturing it into two fiber closures (and the
`(:: ch :ptr<void>)` reinterpret, which also consumes) aliases a linear value,
which always-on linearity forbids. `chan-send`/`chan-recv` already take
`^borrow ch`, but the *closure capture itself* is the consuming move.

Fix direction: either give channel handles shared (rc-like) ownership so they
can be captured by multiple fibers, or rewrite the fixtures to share via an
explicit rc/clone. Design call.

## 3. `recv` on a session channel with still-abstract type parameters

Fixture: `generic-relay-aggregate-result`.

```turmeric
(defn fwd [T R] [c : (SChan (SRecv T R))] : (Pair T ptr<void>)
  (recv c))   ; TUR-E0212: recv requires a Session[...] channel, got
              ; (type-app SChan (type-app (type-app SRecv tyvar) tyvar))
```

Root cause: the `recv` session-op elaborator wants a concrete `Session[...]`
shape and does not accept a `SChan (SRecv T R)` whose protocol still contains
type variables (a generic forwarder). Session-type checking does not yet
handle abstract protocol parameters.

Fix direction: teach the session-op resolver to accept a `SChan (SRecv ...)`
type-application with tyvar leaves (treat it parametrically), or defer the
protocol-shape check until monomorphization.

## Recon notes (2026-06-23) -- answered questions and prereqs

A pre-implementation read of the relevant elaborator code. Each theme has
answers that are present in the code today plus prereq groundwork that
does *not* require the design call itself.

### Theme 1 -- ref<T> deref/auto-drop

Answered:

- **Existing infrastructure for "skip auto-drop if explicit drop! ran"
  already exists.** `is_binding_consumed()` in `src/compiler/elab_core.c:1150`
  detects `(drop! b)`, `(rc/drop b)`, and `(ref/from-rc b)` calls and is
  already wired into auto-drop injection at `elab_forms.c:952,980,1081,1105`.
  A non-consuming-deref fix can plug into the same path -- the
  coordination point already exists; what's missing is making deref
  *not* set `is_linear_consumed` in the EX_VAR walk
  (`elab_toplevel.c:420`).
- **The "~15 fixtures" claim is conservative.** `ref-*` fixtures using `@`
  (deref): `ref-basic`, `ref-deref`, `ref-explicit-drop`,
  `ref-if-branch-move-suppression`, `ref-in-closure`, `ref-move`,
  `ref-nested`, `ref-return`, `ref-return-early-branch` -- 9 ref fixtures
  plus the `rc-auto-drop-*` cluster. A precise audit (which depend on
  deref-as-consumption vs. which survive a non-consuming deref) is
  prereq #1.
- **`::` reinterpret is a separate consumption site** from deref and must
  be tracked alongside any deref-semantics change. Check `elab_memory.c`
  before claiming a fix is complete.

Prereqs (no design call needed):

1. Audit each `@`-using ref fixture: classify "depends on deref-consuming"
   vs. "survives non-consuming deref" by inspecting whether the body has
   a follow-up explicit `drop!`/`rc/drop` or relies on scope-exit auto-drop.
2. Confirm `::` reinterpret shares the EX_VAR consumption path (or doesn't)
   so any fix is coordinated.
3. Add a fixture that asserts the double-free case (auto-drop + explicit
   `drop!`) is rejected today, so the future fix has a regression anchor.

### Theme 2 -- channels captured into multiple closures

Answered:

- **The consuming site is `binding_mark_moved()` in `elab_sessions.c:273`
  (send) and `:328` (recv)**, not the closure-capture itself. The closure
  capture path uses `collect_free_vars()` (`elab_fns.c:4018`) but does
  not currently mark linear bindings consumed -- the report's diagnosis
  ("closure capture itself is the consuming move") is worth re-verifying
  against this finding.
- **`rc/clone` is fully implemented** at `elab_memory.c:215-244` and
  registered in `builtins.c:157`. An `rc<Chan>` wrapper is a viable
  workaround path with no new compiler work.
- **Precedent for sharing a channel safely exists:**
  `tests/fixtures/producer-consumer/input.tur:89-96` shares a channel
  across threads by wrapping it in a thread-arg struct as `ptr<void>`.
  Both failing fixtures could adopt this shape today as a holding
  pattern.
- **Multishot handlers already reject capturing linear values**
  (`elab_effects.c:1215-1234`, TUR-E0500). Either fn closures already do
  the same check (then the report's diagnosis is right and the fix is at
  capture time) or they don't (then the report's diagnosis is wrong and
  the consume is at first use inside the body). This is the single most
  load-bearing question to resolve before designing the fix.

Prereqs:

1. Determine empirically whether the failure fires at *capture time* or
   *first-use time* -- e.g. add a `(local-spawn g (fn [_] : nil 0))`
   that captures `ch` but never uses it; observe whether the error
   fires.
2. Prototype `(rc/clone ch)` per fiber in `schan-worker-pool` to confirm
   Rc-wrapped channels solve the case end-to-end.
3. Add the `producer-consumer`-style ptr<void> workaround to one of the
   two failing fixtures so the suite count drops to 4 while the design
   call is pending.

### Theme 3 -- recv on abstract-typed SChan

Answered:

- **All session ops route through one chokepoint:** `session_protocol_of()`
  at `elab_sessions.c:231-245` rejects anything that isn't `TY_SESSION`.
  `send`, `recv`, `close`, `offer`, `choose_left`, `choose_right` all
  call it (`:260,315,391,430,467,516`). Relaxing the check at this single
  site fixes all six ops in parallel -- the blast radius is narrower
  than the report implies.
- **Monomorphization is a separate emission-time pass**, not interleaved
  with elaboration -- so "defer until monomorphization" requires either
  threaded state or a second pass. The cheaper fix is to accept
  `TY_APP(... TY_TYVAR ...)` at the existing site and let
  monomorphization re-check on instantiation.
- **Minimal repro is already isolated** in
  `generic-relay-aggregate-result/input.tur:17-23` -- a forwarder
  `fwd [T R] [c : (SChan (SRecv T R))]` calling `(recv c)`. A reduced
  fixture (just the forwarder, no aggregate) would shrink the diff for
  the fix.

Prereqs:

1. Reduce `generic-relay-aggregate-result` to a single-defn repro so the
   fix's regression test is minimal.
2. Add a parallel reduced fixture for `send` on an abstract `SChan
   (SSend T R)`; if a future fix only patches `recv`, this asserts the
   sibling ops are addressed at the same time (since they share the
   chokepoint, this should be free).
3. Verify monomorphization currently re-elaborates the body at concrete
   types (so the deferred concrete-shape check actually runs); if it
   doesn't, the "defer" strategy is unsound and the parametric-accept
   path is the only viable one.
