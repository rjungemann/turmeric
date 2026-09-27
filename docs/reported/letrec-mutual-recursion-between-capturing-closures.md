# `letrec`: two capturing closures that call each other do not compile

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
