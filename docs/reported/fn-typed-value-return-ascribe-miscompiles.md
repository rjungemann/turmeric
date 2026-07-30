---
status: open
severity: high
discovered: 2026-07-30
area: codegen (first-class fn-typed VALUES at return/ascription boundaries)
---

# Fn-typed values as data: returning a fn param or ascribing a let-wrapped closure miscompiles

## Summary

A closure used as a **first-class value** survives only the boundaries the
fixtures happen to exercise. Consuming it through a fn-typed parameter works
(thin and `^fat` alike); but a function that **returns** its fn-typed
parameter emits invalid C, a `^fat` pass-through **segfaults**, ascribing a
let-wrapped closure value segfaults, and a `^fat` HOF over a *nested* fn type
(`(fn [] (fn [] int))`) segfaults. All four compile with `tur check` rc=0.

Found by `tests/type-fuzz-src.py` (seed 7, cases 0/26/28/37/38, then
hand-minimized). Adjacent to but distinct from
[poly-result-hof-capturing-closure-sigbus](poly-result-hof-capturing-closure-sigbus.md):
that report is about closure *literals* flowing into non-carrier fn params;
this one is about closure *values* -- even into carrier-safe signatures and
even under `^fat`.

## Matrix (all with `(defn mk [x : int] : (fn [] int) (fn [] x))`)

| Shape | Result |
|---|---|
| `((mk 5))` -- direct invoke of returned closure | ok |
| `((let [v (mk 5)] v))` -- let-bound, invoked | ok |
| `((:: (mk 5) (fn [] int)))` -- ascribed, invoked | ok |
| `((gid (mk 5)))` -- through generic identity `[A] x:A->A` | ok |
| `(use1 (mk 5))` where `use1 [v : (fn [] int)] : int (v)` -- thin consume | ok |
| same with `^fat v` -- fat consume | ok |
| `((:: (let [v (mk 5)] v) (fn [] int)))` -- ascription AROUND a let | **SIGSEGV** |
| `((thru (mk 5)))` where `thru [v : (fn [] int)] : (fn [] int) v` -- thin pass-through | **invalid C** |
| same with `^fat v` -- fat pass-through | **SIGSEGV** |
| `((thru (fn [] 5)))` -- thin pass-through, NON-capturing literal | **invalid C** |
| `((hof (fn [] c)))` where `hof [^fat f : (fn [] (fn [] int))] : (fn [] int) (f)`, `c` a closure value | **SIGSEGV** |

Same results with `cstr` payloads; scalar type does not matter.

## Repros

Thin pass-through (invalid C -- the belief made visible):

    (defn mk [x : int] : (fn [] int) (fn [] x))
    (defn thru [v : (fn [] int)] : (fn [] int) v)
    (defn main [] : int (println ((thru (mk 5)))) 0)

    /tmp/tur-build/..._tur.c: In function 'thru':
    error: aggregate value used where an integer was expected
         return (int64_t)(intptr_t)v;

Ascription around a let (segfault):

    (defn mk [x : int] : (fn [] int) (fn [] x))
    (defn main [] : int (println ((:: (let [v (mk 5)] v) (fn [] int)))) 0)
    ;; Segmentation fault

Fat pass-through (segfault):

    (defn mk [x : int] : (fn [] int) (fn [] x))
    (defn thru [^fat v : (fn [] int)] : (fn [] int) v)
    (defn main [] : int (println ((thru (mk 5)))) 0)
    ;; Segmentation fault

## Root cause (direction)

The cc error names it: inside `thru` the parameter is the by-value fat struct,
and the return path casts it to `int64_t` -- producer and consumer disagree
about which of the (at least) three fn-value representations (bare code
pointer / fat `{thunk, env}` handle / by-value fat struct) crosses each
boundary. The same disagreement read in the other direction explains the
segfaults: an invoke reads a code pointer where an env struct (or vice versa)
was stored. This is the closure-flavored instance of the int64-carrier
erasure family (`result-monad-bind-typed-boundary-miscompiles` is the ADT
instance), and one more data point for the direction already argued in
poly-result-hof-capturing-closure-sigbus: normalize every fn-typed VALUE
boundary onto the fat `{thunk, env}` protocol, shimming bare fns on the way
in, rather than deciding representation per-boundary.

## Where it bites

Any combinator library: a middleware stack that stores handlers and returns
them, a parser library passing parsers through identity/compose functions --
"return the function you were given" is the first thing such code does.

## Fix directions

1. Fold into the calling-convention plan (fat protocol for every non-carrier
   fn boundary) --
   [docs/upcoming/fn-value-fat-normalization-plan.md](../upcoming/fn-value-fat-normalization-plan.md);
   these repros are its stage-2 acceptance tests -- the pass-through and
   ascribe-around-let shapes specifically.
2. Fixtures: the full matrix above, ok rows included -- the working/broken
   boundary is sharp enough that a regression in either direction should pin.
3. Until fixed, `tests/type-fuzz-src.py` avoids the shapes via
   `known_bug_slug` (slug: this file) and pins them in `--known-probes`.

## Guide upkeep

When this report is resolved -- or any representation/bridge it describes
changes shape on the way -- update
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
in the same PR: fix the representation inventory, move this report's row out
of the missing-cells table, and correct the link when the report moves to
`docs/archive/`.
