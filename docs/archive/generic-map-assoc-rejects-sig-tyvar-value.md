# `map-assoc` of an `A`-typed value onto `(map-new)` is rejected inside a generic

**Severity: low-medium** as filed (a legal program refused at `tur check`). Once
accepted, it also hid a miscompile, so the real severity was high. Found
2026-09-30 by `tests/generic-spec-matrix.py` (its `map` sink, every producer and
type). **RESOLVED 2026-09-30.**

## Repro

```turmeric
(defn keep [A] [x : A] : A
  (map-get (map-assoc (map-new) 1 x) 1))
(defn main [] : int (println (keep 7.1)) 0)
```

```
stdlib/map.tur:578:8: error [TUR-E0001]: function 'map-assoc-eq-o' arg 4:
  expected V, got A
```

The same body in a non-generic function, with a concrete value, checked and
ran. So did a map whose type is written down:
`(map-assoc (:: (map-new) (Map int A)) 1 x)`.

## Root cause

`map-assoc` is a macro expanding to `(map-assoc-eq-o m h key val keyeq owned)`,
with `m : (Map K V)` and `val : V`. With `m = (map-new)`, nothing has fixed
`(map-new)`'s slots, so `call_collect_type_bindings` (`elab_call.c`) binds the
callee's `V` to itself. A later CONCRETE value against such a self-binding was
already accepted (the `m5-eq-vec-rewrite` rule). The enclosing signature's own
`A` is as fixed as a concrete type in every instantiation, but it was compared
by identity and refused.

The filed fix direction ("unify an open-slot tyvar") was wrong about the
mechanism. The binding is a self-binding, not an `open_slot` tyvar.

## The hidden miscompile

Accepting `A` while leaving the binding as `V := V` checked, but the call's
result stayed `(Map int V)`. `map-get` then read back the unfixed `V`, which
collapsed to `int`. `elab_defn` replaces a declared tyvar result with a concrete
body type, so the generic's `A` result became `int`. At `A := (fn [int] int)`,
the caller lost the "result of a tyvar-returning call" signal, which is what
makes it fat-dispatch a function value read out of the carrier. It thin-called
the fat handle: SIGSEGV (the matrix's `vecget/map/fn`, `okval/map/fn` and
`mapget/map/fn`).

## Fix

A self-binding met by a later argument typed with a signature tyvar (checked
with `ng_tyvar_in_sig` against the elaborator the argument check hands in)
takes that tyvar as the binding. The result is then `(Map int A)`, exactly
as the ascribed spelling gives.

## Verified

- `tests/fixtures/generic-map-assoc-sig-tyvar-value` (float, cstr, int, struct,
  Option, two assocs), compiled and `--interpret`.
- `tests/fixtures/generic-spec-carrier-crossings-4` (the function-value
  instantiation that segfaulted once the check passed).
- The matrix's `map` sink: 192 cells failing before this fix, 0 after.
