# Residual spice-uplift soft blockers W1/W2 (letrec carrier self-call; fresh `vec-new` arg unification)

**Status:** RESOLVED. Both filed by turmeric-spices Track C (branch
`claude/track-c-turmeric-xaky6y`) as low/medium soft blockers, each with a
one-line workaround already landed in the spice. Fixed here in turmeric main.

- W1 -- elaboration: `src/compiler/elab_forms.c`, `elab_letrec` Pass A.
- W2 -- elaboration: `src/compiler/elab_call.c`, the saturated-call argument
  check (GS2 block) + two new static helpers.
- Codegen (surfaced while landing W1's `(Vec T)` case): `src/compiler/emit_module.c`,
  six `EX_LET` walks extended to `EX_LETREC`.

Regression fixtures:
`tests/fixtures/letrec-self-recursive-carrier-struct-return/`,
`tests/fixtures/letrec-self-recursive-carrier-vec-return/`,
`tests/fixtures/vec-new-fresh-arg-concrete-unify/`.
`bash tests/run.sh`: 1722 passed, 0 failed (with `../turmeric-spices` cloned).

Verified against the real spice: with both fixes in place, plot's
`renderers-bbox` and `anyrenderers->legacy` (`spices/plot/src/plot/core.tur`)
type-check **and** emit C as `letrec`-bound closures threading a `BBox` /
`(Vec int)` accumulator, seeded with a bare `(vec-new)` -- both prior
workarounds removed.

---

## W1 -- `letrec` self-recursive closure with a carrier-lowered accumulator types its own call at the int carrier

**Severity:** Low/Medium. The `letrec`-closure analogue of S6 (fixed for
top-level `defn` by #460's RR1 block). A `letrec`-bound self-recursive `fn`
goes through a different binding path and was not covered, so its self-call
collapsed to the int64 carrier when the accumulator/return is a carrier-lowered
type (a `:copy` struct, a `(Vec T)`).

### Repro

```turmeric
(defstruct Box :copy [lo : int  hi : int])
(defn go-letrec [n : int] : Box
  (letrec [go (fn [i : int  acc : Box] : Box
                (if (>= i n) acc (go (+ i 1) acc)))]
    (go 0 (make-struct Box 10 99))))
;; before: error: if branches have mismatched types: then=Box else=int
```

`(Vec int)` accumulator reproduces identically (`then=(type-app Vec int) else=int`).

### Root cause

`elab_letrec` (`src/compiler/elab_forms.c`) pre-registers each binding in
**Pass A** with a placeholder `TY_FN` stub so the fn body's self-call sees the
right arity. The placeholder's return type came from a lightweight keyword
peek that resolved only `int/bool/void/nil/cstr` and **collapsed every other
declared return to the int64 carrier (`TY_INT`)**. The self-call inside the fn
body (elaborated in **Pass B**) reads that carrier placeholder, so the
recursive call comes back typed `int`; an `if` whose other arm is the real
return type then fails with a spurious branch mismatch. The binding is patched
to the real fn type only *after* Pass B, too late for the self-call.

### Fix

In Pass A, after building the placeholder, resolve the declared return *form*
fully with `fn_type_from_form` and stamp the carrier-lowered result
(`result_kind` + `result_full_type`) onto the placeholder when it resolves to a
`TY_STRUCT` / `TY_APP` / `TY_ADT`. Mirrors #460's RR1 declared-result-shape
propagation. A bare `F_LIST` at index 2 is a body form (e.g. `(go ...)`), not
a return annotation, so the gate accepts only `F_KEYWORD`/`F_SYM`/`F_TYPE_ANN`.

---

## W2 -- a bare `(vec-new)` passed directly as a concrete `(Vec T)` argument did not unify its element tyvar

**Severity:** Low. S4/#461 back-propagates the element type onto a *let-bound*
`vec-push!` receiver only; a fresh `(vec-new)` handed straight to a `(Vec T)`
parameter (no `vec-push!` in the caller to pin it) left the element tyvar open.

### Repro

```turmeric
(defn sink [v : (Vec int)] : int (vec-len v))
(defn main [] : int (sink (vec-new)))
;; before: error [TUR-E0001]: function 'sink' arg 1:
;;   expected (type-app Vec int), got (type-app Vec tyvar 'A')
```

### Root cause

In the saturated-call argument check (`elab_call.c`, GS2 block), when the
callee parameter is a *concrete* `(Vec int)` (no named tyvar) and the argument
is `(Vec A)` (tyvar `A` unpinned), the structural `type_eq` fails -- even
though an empty container parametrically inhabits `(Vec int)` for any element
type.

### Fix

When the structural compare fails and the argument is a **return-only
polymorphic call result** -- an `EX_CALL` none of whose own arguments carry the
tyvar (so the value is genuinely parametric, *not* an abstract `(Vec A)`
parameter the body must keep abstract) -- unify the argument's tyvar against the
concrete parameter (`call_collect_type_bindings`), accept, and record the
resulting substitution (`A := int`) on the `(vec-new)` call's `abi_bindings` so
emit monomorphizes it to `vec_new__spec__Vec__int__` rather than passing the
int64 carrier into a `Vec__int*` parameter (avoiding a `-Wint-conversion`
warning -- matching the clean codegen the `(:: (vec-new) (Vec int))` ascription
produced). Two helpers gate the soundness:

- `w2_arg_is_free_poly_call` -- the argument is an `EX_CALL` whose result tyvars
  are all free of its own arguments.
- `w2_tyvars_free_of_args` -- the recursive freedom check.

Soundness preserved: an abstract `(Vec A)` *parameter* reference (`EX_VAR`) and
a call that *carries* the tyvar through an argument (e.g. `(sink (idv xs))`)
both still fail with `TUR-E0001`, as they must.

---

## Codegen follow-on -- carrier call in a `letrec` body dropped from emission

Surfaced end-to-end while landing W1's `(Vec T)` case: a carrier call appearing
only in a `letrec` body -- e.g. the `(vec-new)` seed in
`(letrec [go (fn ...)] (go 0 (vec-new)))` -- linked with `undefined reference to
vec_hynew`. The carrier-call registration walk `emit_abi_scan_expr`
(`src/compiler/emit_module.c`) handled `EX_LET` but had **no `EX_LETREC` case**,
so it never descended into the letrec body and the generic definition was
pruned. All six `EX_LET` walks in `emit_module.c` (which share the `as.let_`
layout) were extended to also handle `EX_LETREC`. `vec-push!` survived only
because it sits inside the lifted closure body, scanned as a top-level item.

## Out of scope (pre-existing, not introduced here)

json `derive-json-sum-decode` routes its `(ok <adt>)` tail through a standalone
`defn` because a typeclass method whose tail is a bare `(ok <adt-value>)`
mis-boxes the ADT-typed result slot (value-struct case fixed, sum/ADT case
not). Separate open gap; untouched by this work.
