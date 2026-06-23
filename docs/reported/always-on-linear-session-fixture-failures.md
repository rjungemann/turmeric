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
