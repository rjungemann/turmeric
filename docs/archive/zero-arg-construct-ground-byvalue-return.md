---
title: 0-arg `(none)` in a ground non-generic `(Option T)` return emits the carrier handle into a by-value slot
category: Compiler / Codegen / Option none-as-NULL retirement (Track A, step 2 follow-up)
severity: Medium. The step-2 fix in
  `docs/reported/option-consumer-retype-byvalue.md` resolves the 0-arg
  `(none)` return in a **parametric** TY_APP context (e.g. `(Option (NonEmpty A))`
  where `A` is a body tyvar). The matching case for a **ground non-generic**
  return type -- e.g. `(Option BoundedIdx)` -- still falls through the carrier
  base, so the emitted C assigns `int64_t = none()` into an `Option__BoundedIdx`
  slot and `cc` rejects with `incompatible types`.
status: RESOLVED 2026-06-18. Fixed by a ground-TY_APP sibling to the step-2
  constructor-result-tyvar binding (`elab_call.c`) plus a struct-element
  carrier-skip gate (`emit_module.c`).  Gated to a by-value struct/opaque
  element (`call_app_has_struct_elem`) so a primitive-element `(Option int)`
  stays on the existing carrier+bridge path -- only struct/opaque payloads,
  where the carrier base straddles the sibling `some`/`ok` by-value spec, opt
  in.  `stdlib/refined.tur` `bidx-of?` / `bidx-unwrap` retyped to pure
  Turmeric by-value; `tests/fixtures/refined-bounded-idx/` reverted to
  ascription-free calls; new codegen fixture
  `tests/fixtures/option-construct-ground-byvalue-none/` pins the by-value
  `none__spec__Option__BoundedIdx`.  Full suite green (1680 passed, 0 failed).
---

# 0-arg `(none)` in a ground `(Option T)` return: by-value spec missing

## Repro

`stdlib/refined.tur` `bidx-of?`, rewritten to pure Turmeric with a by-value
return:

```turmeric
(defn bidx-of? [n : int i : int] : (Option BoundedIdx)
  (if (or (< i 0) (>= i n))
    (none)
    (some (:: i :BoundedIdx))))
```

Builds fail with:

```
/tmp/tur-build/.../input_tur.c:NNNN:21: error: incompatible types when assigning
to type 'Option__BoundedIdx' from type 'int64_t' {aka 'long int'}
        __t32 = none();
```

## Expected vs. observed

- **Expected**: the false arm constructs a by-value
  `none__spec__Option__BoundedIdx` aggregate (analogous to the parametric
  fix `none__spec__Option__int` for `option-map`'s by-value return path).
- **Observed**: `(none)` resolves to the carrier-base `none` (returning
  `int64_t`) and is assigned straight into the `Option__BoundedIdx` C slot.

## Why the existing step-2 fix doesn't catch it

The step-2 fix attaches `constructor-result-tyvar -> caller-tyvar` bindings
on a 0-arg `#{Construct}` when the enclosing return type is a **parametric
TY_APP** -- e.g. `(Option (NonEmpty A))` where the body has a free tyvar `A`.
emit then composes that mapping through the active spec
(`A -> int`) and mints a by-value clone.

For `bidx-of?` the return is a ground TY_APP -- no free tyvar to bind --
so the elab pathway that records the per-Expr* binding never fires, and
emit falls back to the carrier base. The structural-match guard added in
step 2 (`find_matched_abi_spec` requiring the per-Expr* recording for 0-arg
constructors) then disqualifies the by-value spec entry, so the carrier
base wins.

## Proposed fix

For a 0-arg `#{Construct}` whose declared result is a ground TY_APP and
whose enclosing return position is also ground TY_APP, mint (or look up) a
by-value spec keyed by the ground element type and force emit to use it.
The simpler case than step 2: there is no tyvar to substitute, just a
spec-name match (`none__spec__Option__BoundedIdx`).

The minimal change is in `elab_call.c` (record the ground binding even
when there is no tyvar to compose) and `emit_core.c` /
`find_matched_abi_spec` (allow a per-Expr* match even with no abi_bindings
when the result-type is fully ground and matches an existing spec).

## Validation

- `stdlib/refined.tur` `bidx-of?` body retyped to pure Turmeric returning
  `(Option BoundedIdx)` (currently reverted to inline-C carrier with a
  pointer to this report).
- `bidx-unwrap [o : (Option BoundedIdx)] : BoundedIdx` retyped similarly.
- `tests/fixtures/refined-bounded-idx/` reverts to ascription-free
  `(some? o)` / `(bidx-unwrap o)` calls (currently has
  `(some? (:: o (Option int)))` workarounds).

## Related

- `docs/reported/option-consumer-retype-byvalue.md` step 2 (the parametric
  TY_APP fix this generalizes).
- `src/compiler/emit_module.c` `construct_recovered_byvalue`.
