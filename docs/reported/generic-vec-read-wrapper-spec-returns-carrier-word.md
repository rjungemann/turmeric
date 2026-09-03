# A generic `vec-get` wrapper's spec returns the slot word where its by-value element is expected

**Severity: medium** -- a hard `cc` error on the DEFAULT path for a shape
`tur check` accepts.  Found 2026-09-03 while building CE2
([container-element-form-plan](../upcoming/container-element-form-plan.md));
reproduced on the compiler BEFORE that change, so it is pre-existing and
independent of the option-niche experiment.

## Repro

```turmeric
(load "stdlib/string.tur")
(defn get-it [A] [v : (Vec A) i : int] : A
  (vec-get v i))
(defn main [] : int
  (let [v (:: (vec-new) (Vec (Option String)))]
    (vec-push! v (some (string/from-cstr "bb")))
    (println (string/to-cstr (unwrap (:: (get-it v 0) (Option String)))))
    0))
```

```
error: incompatible types when returning type 'int64_t' but 'tur_adt_Option__String' was expected
```

The Path A spec `get_it__spec__..._Option__String` is declared to return the
by-value `tur_adt_Option__String` (right: that is the element monomorph),
but its body's tail is `vec_hyget(...)` -- the raw int64 slot word, which for
a by-value aggregate element holds the element's heap box pointer -- and no
carrier->concrete readback (`*(tur_adt_Option__String *)(intptr_t)w`) is
emitted at the return.  A direct `(:: (vec-get v 0) (Option String))` at a
concrete site gets that readback from the ascription bridge; the spec's
return position does not.

Under `--enable=option-niche` the same wrapper is FINE after CE2: the slot
word is the niche pointer, and `(void *)(intptr_t)w` is the correct
readback (the `option-niche-vec-word` fixture pins it).  So the gap is
specifically "a spec whose declared result is a BOXED by-value aggregate
element, with the raw slot read in tail position".

## Not the store side

The push twin -- `(defn push-it [A] [v : (Vec A) x : A] (vec-push! v x))`
over `(Option String)` -- also failed to build on the default path before
CE2 (its spec heap-promoted the element and then bridged the cell pointer a
second time).  CE2 fixed that in passing by resolving the bridge type inside
the spec and gating the second bridge; this report is the read half only.

## Fix direction

The spec's tail-return bridge (emit_fns.c, the by-value-result branches)
should recognise a raw-slot-read tail (`vec-get` / `vec-pop!` /
`vec-data-get-checked__`) whose declared element resolves to a
`container_elem_form == CE_BOX` type and emit the box readback, exactly as
the `(:: ... T)` ascription bridge does at a concrete site -- or route the
call through `vec-get-byval`, whose spec body already carries that
ascription.
