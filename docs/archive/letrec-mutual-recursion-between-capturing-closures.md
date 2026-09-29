# `letrec`: two capturing closures that call each other do not compile

**RESOLVED 2026-09-28: the letrec closure knot, fix direction 1.**

Two halves, because the report's root cause has two:

- **Capture (elaborator).** Before any init is elaborated, `elab_letrec`
  predicts which `fn` members will capture (`letrec_predict_closures`,
  src/compiler/elab_forms.c): a member captures when its init names a local
  outside the group, names a value (non-`fn`) member, or calls a member that
  captures -- the last to a fixpoint.  The prediction lives on the binding
  (`letrec_predicted_closure`), and `collect_free_vars` now treats a call to a
  group member as recursion only when that member is the one being elaborated
  (the S5 env-pointer self-call, `letrec_elaborating`) or will be captureless
  (`letrec_member_is_recursion`, elab_core.c).  So `ev` captures `od` even
  though `od` has no init yet.  The scan is syntactic and over-approximates (a
  shadowed name still counts), which is safe: a member predicted to capture
  that turns out captureless is lifted to a global, and a capture of a global
  member is already skipped at the env struct and the fill (Edge 1).
- **Knot (emitter).** `ev`'s env is built before `od` exists.  While a letrec's
  bindings are emitted, `EmitCtx.letrec_knot` holds the members not bound yet;
  a member's own EX_CLOSURE init that captures one zeroes that slot and records
  the real fill, rendered against the owner's binding, which
  `letrec_knot_bound` emits the moment the target is bound (emit_expr.c, both
  `emit_let_value` and `emit_letrec_value`).  Nothing can call `ev` in between:
  members are bound in order, and `od`'s init is a closure construction.

Pinned by `tests/fixtures/letrec-mutual-capturing-closures` (the repro, a
three-member cycle, the later member reached from a closure nested in the
earlier one, a transitive capture through a call, an over-predicted
captureless member, a float capture, and a value member that calls the tied
knot) and `tests/fixtures/r7rs-internal-defines-mutual-capture` (the `#lang
r7rs` internal-define shape).  Saffron's untyped `letrec` works the same way.

Not changed: neither env in a knot is dropped at scope exit (each is captured
by the other, so the env-free rule sees both as escaping) -- a leak of the two
envs per evaluation, the same posture as any captured closure today.

The original report follows.

**Severity:** medium. A `letrec` (and so a `#lang r7rs` body's internal
defines, and a named `let` inside one) whose members capture a local and call
each other fails in the C compile:

```
error: 'od_1666' undeclared (first use in this function)
```

Filed 2026-09-27 while landing r7rs-srfi-plan S6.

## Repro

```clojure
(defn parity [k : int n : int] : int
  (letrec [ev (fn [i : int] : int (if (= i 0) k (od (- i 1))))
           od (fn [i : int] : int (if (= i 0) (- 0 k) (ev (- i 1))))]
    (ev n)))
(defn main [] : int (println (parity 1 10)) 0)
```

The `#lang r7rs` shape: `even?`/`odd?` internal defines that also use a
variable of the enclosing procedure.

## Root cause

`collect_free_vars` (src/compiler/elab_core.c, the EX_CALL case) leaves a
call to a member of the letrec group being elaborated out of the capture set
(`letrec_self_group`, "Edge 1"), because the recursion machinery handles a
member's own self-call and captureless members, which are lifted to globals.
S6 narrowed that to members not yet known to be closures, so a call to an
EARLIER capturing sibling is captured now
(`tests/fixtures/letrec-sibling-closure-capture`). A call to a LATER sibling
still is not: when `ev` is elaborated, `od` has no init yet, so nothing says
it will capture, and capturing it would need its value before it exists.

## Fix directions

- Allocate every capturing member's env first, then fill the slots (the
  classic letrec closure knot): `ev`'s env holds `od` and `od`'s holds `ev`.
  The emitter's env fill would run after all allocations.
- Or lower such a group to `let` of cells plus `set!` (as the Scheme lowering
  already does for a value define that refers to itself), with the calls
  going through the cells.
