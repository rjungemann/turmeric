---
title: defdata parametric return-type inference is weak
severity: MEDIUM. Forced boilerplate ascription throughout parametric-defdata code.
status: RESOLVED 2026-07-01. #1 and #2 fixed; #3 no longer reproduces.
---

# defdata parametric inference gap (+ historical elab_match SEGV)

Three linked issues around parametric `defdata` types, surfaced while
writing `tests/fixtures/parsec-tutorial/input.tur` (a pure-Turmeric
parser).

## 1. `defdata` requires keyword-prefixed field types; `defgadt` does not -- FIXED 2026-07-01

Historically:

```turmeric
(defdata PRes [a]
  (PFail)
  (POK a int))     ;; used to error: constructor field type must be a keyword
```

Fix landed in `src/compiler/elab_structs.c` -- the ok_tag check no longer
gates F_SYM on record-style. Positional and record-style variants both
accept `:int` and bare `int`, matching `defgadt` / `defstruct`.

The obsolete regression fixture `errors/defdata-malformed-ctor-field-type/`
(which guarded a NULL-deref that only occurred after the now-removed
error) was deleted.

## 2. Parametric `defdata` return-type inference is weak -- FIXED 2026-07-01

A bare nullary constructor of a parametric ADT (`(PFail)` from
`(defdata PRes [a] (PFail) (POK a int))`) carries no field arguments to
bind its type parameters from, so it used to default to the bare TY_ADT
`PRes` -- mismatching a peer arm whose `(POK v rest)` inferred to
`(PRes Expr)` and triggering "match: arm types are incompatible --
expected adt (from earlier arm), got app".

Fix landed in `src/compiler/elab_call.c` (~line 2200): the 0-arg
constructor path now consults `e->expected_type`, and when it's a
concrete `TY_APP` over the same ADT def (a function return type, a
match arm's peer type, an ascription target, etc.) uses it as the
result type.  Bare `(PFail)` now unifies with concrete `(POK ...)`
peers whenever the outer expected type is visible.

**Known follow-on:** when the enclosing scrutinee type is a
sibling forward-referenced defn whose parametric return type wasn't
recorded by the Pass-1 forward-decl scan (which only commits primitive
kinds), the pattern arm bindings still fall back to the placeholder
carrier and downstream `(POK inner ...)` uses the wrong element type.
Tracked separately in
[defdata-parametric-forward-decl-inference.md](../reported/defdata-parametric-forward-decl-inference.md).

## 3. `elab_match` SEGV on unascribed parametric match -- NOT REPRODUCIBLE 2026-07-01

The original report noted a segfault near `elab_structs.c` line ~3990
during unascribed nested-match combinations. As of 2026-07-01 the
shapes described (bare `(PFail)` / `(POK n rest)` arm bodies) produce
a proper diagnostic instead of a crash. The SEGV path appears to have
been closed by other elaborator work.

If a new SEGV shape surfaces, file a fresh report -- do not reopen this
one for it.

## Workaround (no longer needed)

`tests/fixtures/parsec-tutorial/input.tur` used to ascribe every
`(PFail)` / `(POK ...)` constructor call with `(:: ... (PRes Expr))`.
With #2 fixed, only one ascription remains -- in `factor`'s
`(POK inner (list-tail rest))` -- and it is load-bearing for the
sibling forward-reference gap tracked above.
