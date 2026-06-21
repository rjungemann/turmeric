---
title: Two typeclass instances over applied structs of the same class differing only in the element (`Enc [(Option cstr)]` vs `Enc [(Option int)]`) conflate -- dispatch always selects one
category: Typeclass instance selection -- applied-struct instance heads keyed only by head constructor
severity: Medium. Defining `Enc [(Option cstr)]` and `Enc [(Option int)]`
  (same class, same head constructor `Option`, different element) and
  dispatching on a concrete `(Option cstr)` vs `(Option int)` value selects the
  SAME instance for both (the last-defined wins) -- a silent misdispatch. A
  single applied-struct instance works (see gap G3). Not a carrier/ABI bug: the
  instance *selection* loses the element, so the wrong method body runs.
status: OPEN -- found 2026-06-21 while writing the G3 fixture; verified
  pre-existing (reproduces identically without the G3 fix). Tracked as gap G10
  in docs/carrier-concrete-abi-crossing-audit-plan.md.
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
