---
title: Two typeclass instances over applied structs of the same class differing only in the element (`Enc [(Option cstr)]` vs `Enc [(Option int)]`) conflate -- dispatch always selects one
category: Typeclass instance selection -- applied-struct instance heads keyed only by head constructor
severity: Medium. Defining `Enc [(Option cstr)]` and `Enc [(Option int)]`
  (same class, same head constructor `Option`, different element) and
  dispatching on a concrete `(Option cstr)` vs `(Option int)` value selects the
  SAME instance for both (the last-defined wins) -- a silent misdispatch. A
  single applied-struct instance works (see gap G3). Not a carrier/ABI bug: the
  instance *selection* loses the element, so the wrong method body runs.
status: RESOLVED 2026-06-21 (gap G10). Fixed on
  branch claude/g2-carrier-concrete-abi-audit-3yzkhm; fixture
  tests/fixtures/applied-struct-instance-element-discrimination. See
  "Resolution" below.
---

## Resolution (2026-06-21)

Fixed. The defect was NOT in `typeclass_env_lookup_instance` (the call's
dispatch never routes through it) but in the **method-call instance selection**
in `elab_typeclasses.c` (the `(.method obj)` / `(enc obj)` resolver). Its
applied-head element-discrimination compared the receiver's and instance's first
app arg, but only treated **TY_STRUCT / TY_ADT** elements as "concrete." A
concrete **primitive** element (`cstr` vs `int`) failed that test, so the
element difference was ignored and both `Enc [(Option cstr)]` and
`Enc [(Option int)]` matched any `(Option X)` receiver -- the last-defined
silently won.

Fix: a new helper `typeclass_type_arg_concrete` treats a concrete primitive
element (int/cstr/bool/float/...) as discriminating in addition to struct/ADT
elements; a tyvar element stays a wildcard so parametric instances
(`Enc [Option]`) still match any element. Two instances differing only in a
primitive element are now told apart, for both local and struct-field receivers.

`g3multi` now prints `s` / `i` (was `i` / `i`). Fixture
`tests/fixtures/applied-struct-instance-element-discrimination` (locals + struct
fields, both elements). Suite green (1743/0).

---

# Applied-struct instances of one class keyed only by head constructor

## One-line summary

`Enc [(Option cstr)]` and `Enc [(Option int)]` -- two instances of one class
whose heads are applied structs sharing the head constructor `Option` but
differing in the element -- are not distinguished at dispatch: a concrete
`(Option cstr)` and a concrete `(Option int)` both select the same instance
(the last defined), so one of them runs the wrong method body.

## Minimal repro

```turmeric
(defclass Enc [a] (enc [x] : cstr))
(definstance Enc [(Option cstr)] (enc [x] : cstr (if (.is-some x) "s" "n")))
(definstance Enc [(Option int)]  (enc [x] : cstr (if (.is-some x) "i" "n")))
(defn main [] : int
  (do
    (println (enc (:: (some "hi") (Option cstr))))   ;; EXPECT "s"
    (println (enc (:: (some 7)   (Option int))))     ;; EXPECT "i"
    0))
```

Output:

```
i
i        <- both select Enc [(Option int)] (last defined); want "s" then "i"
```

## Diagnosis (direction)

The instance-selection key for an applied-struct head appears to collapse to
the head **constructor** (`Option`) and drop the element, so
`Enc [(Option cstr)]` and `Enc [(Option int)]` hash/match to the same slot --
the dispatch suffix reconstruction (`__inst_Enc_enc__Option`) and/or the
dict-singleton keying do not include the element type. Contrast a single
applied-struct instance (gap G3), which works because there is no sibling to
collide with.

This is an instance-*selection* defect (which `__inst_*` to call), not a
carrier<->concrete ABI/representation one. It is adjacent to the audit family
because G3 exercises the same applied-struct instance head, but the fix is in
instance keying/mangling (include the element type in the selection key), not in
the carrier bridge.

## Related

- **G3** (`docs/archive/instance-method-byvalue-struct-field-receiver-abi-mismatch.md`):
  the single-applied-struct-instance case; its fixture deliberately uses ONE
  instance to avoid this conflation.
- Likely the same root as
  `docs/reported/instance-suffix-mangler-tyvar-element-legacy-struct-token.md`
  (instance suffix mangling losing the element); cross-check when fixing.

## Validation

When fixed, the repro prints `s` then `i`, and the two-instance form of the G3
fixture (a `Rec` with both `(Option cstr)` and `(Option int)` fields) can be
restored. Cross-reference: gap G10 in
`docs/carrier-concrete-abi-crossing-audit-plan.md`.
