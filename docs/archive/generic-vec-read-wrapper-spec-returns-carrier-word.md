---
title: A generic vec-get wrapper's spec returns the slot word where its by-value element is expected
category: Archive
description: A generic wrapper whose tail is a raw container read failed to compile when specialized to a boxed by-value element -- the spec returned the int64 slot word where its declared result was the element aggregate. RESOLVED 2026-09-04 by routing the spec's return through the same bridge the concrete ascription site already used.
---

# A generic `vec-get` wrapper's spec returns the slot word where its by-value element is expected

**Severity: medium** -- a hard `cc` error on the DEFAULT path for a shape
`tur check` accepts.  Found 2026-09-03 while building CE2
([container-element-form-plan](../upcoming/container-element-form-plan.md));
reproduced on the compiler BEFORE that change, so it is pre-existing and
independent of the option-niche experiment.

**RESOLVED 2026-09-04**, along the fix direction it filed.

## Repro

```turmeric
(defstruct Pt [x : int])
(defn get-it [A] [v : (Vec A) i : int] : A
  (vec-get v i))
(defn main [] : int
  (let [v (:: (vec-new) (Vec (Option Pt)))]
    (vec-push! v (some (make-struct Pt :x 7)))
    (println (if (some? (:: (get-it v 0) (Option Pt))) "some" "none"))
    0))
```

```
error: incompatible types when returning type 'int64_t' but 'tur_adt_Option__Pt' was expected
```

(Filed against `(Option String)`; since the niche graduated on 2026-09-03
that element is the niche word and the wrapper works by default, so the
repro is any BOXED by-value element -- a struct payload here.  Re-verified
2026-09-03 on `(Option Pt)`.)

The Path A spec `get_it__spec__..._Option__Pt` is declared to return the
by-value `tur_adt_Option__Pt` (right: that is the element monomorph),
but its body's tail is `vec_hyget(...)` -- the raw int64 slot word, which for
a by-value aggregate element holds the element's heap box pointer -- and no
carrier->concrete readback (`*(tur_adt_Option__String *)(intptr_t)w`) is
emitted at the return.  A direct `(:: (vec-get v 0) (Option String))` at a
concrete site gets that readback from the ascription bridge; the spec's
return position does not.

For a niche element (`(Option String)`, the default now) the same wrapper is
FINE after CE2: the slot word is the niche pointer, and `(void *)(intptr_t)w`
is the correct readback (the `option-niche-vec-word` fixture pins it).  So the gap is
specifically "a spec whose declared result is a BOXED by-value aggregate
element, with the raw slot read in tail position".

## Not the store side

The push twin -- `(defn push-it [A] [v : (Vec A) x : A] (vec-push! v x))`
over `(Option String)` -- also failed to build on the default path before
CE2 (its spec heap-promoted the element and then bridged the cell pointer a
second time).  CE2 fixed that in passing by resolving the bridge type inside
the spec and gating the second bridge; this report is the read half only.

## Resolution

`fn_return_needs_carrier_result_bridge` (emit_fns.c) is the predicate that
routes a function's return through the carrier->concrete bridge.  It already
existed and already emitted exactly the readback this wanted -- it was gated on
`expr_tail_is_catch_box`, one specific tail shape, so a raw container read in
the same position never reached it.  It now also answers yes for a raw
container-read tail whose declared result is a CE_BOX element, and the existing
`emit_carrier_bridge(..., CK_CARRIER, CK_CONCRETE, sink_rt)` does the rest.
The spec's tail becomes the same NULL-guarded deref the concrete site gets:

```c
int64_t __t201 = (int64_t)(intptr_t)(__ps_200);
tur_adt_Option__Pt __t202 =
    (__t201 ? (*(tur_adt_Option__Pt *)(intptr_t)(__t201)) : (tur_adt_Option__Pt){0});
```

Two details the fix direction did not have:

- **Ask the DECLARED result, not the body's type.**  `vec-get`'s own result is
  already lowered to the int64 carrier, so `fd->body->type` reads back as plain
  `int` -- `repr_of` on it answers the question about the wrong type and the
  predicate silently stays false.  The declared result
  (`fn_e->type.as.fn.result_full_type`, resolved through the active
  specialization) is the element monomorph, which is what CE_BOX is a fact
  about.  This cost one debugging round and is the kind of thing that looks
  like "the fix didn't work" rather than "the fix asked the wrong question".
- **CE_WORD must stay excluded.**  For a niche element the slot word IS the
  value, so the readback would unbox a box that was never there -- the same
  distinction the bridge's own niche row draws, asked here about the declared
  result rather than about a marked temp.  The fixture carries niche and
  scalar controls so a change that widens this to every element form fails
  loudly.

The three raw-slot readers (`vec-get`, `vec-pop!`, `vec-data-get-checked__`)
were named inline in emit_expr.c's hoist-marking site; that list is now the
shared `emit_call_is_raw_slot_read`, so the two sites that must recognise a
raw slot word cannot drift.  `vec-get-byval` is deliberately absent: its own
body ascribes the word back to the element, so it hands back the element
rather than the slot, and bridging it would double-unbox.

## What the fix reached past the report

- **`vec-pop!` had the same failure** and is covered by the same predicate.
  Its ownership is unchanged and matches the concrete site: neither frees the
  box the popped element rode in.  Measured under ASan, the concrete site
  leaks 88 bytes in 4 allocations against the spec's 72 in 3 -- and all of
  those are the program's own un-freed vec, data buffer and `some` box, i.e.
  "this program never calls `vec-free`", not a readback leak.
- **A bare by-value struct element** (`(Vec Pt)`, no Option wrapper) is CE_BOX
  too and was broken identically; it works now and is pinned.

## Validation

- `bash tests/run.sh` -- **2785 passed, 0 failed**, zero snapshot drift.
- `tests/fixtures/generic-vec-read-wrapper-spec` pins all four element forms:
  `(Option Pt)` through both `vec-get` and `vec-pop!`, a bare `Pt`, and the
  niche `(Option String)` and scalar `int` controls.  It reads `.x` and `.y`
  rather than asserting `some?`: a readback that misses the box still answers
  "some" off the first word, so a shape-only assertion passes while the
  payload is garbage.
- `run-option-niche-seam.sh` 10/0, `run-sr2-seam.sh` 55/0, `run-sr4-seam.sh`
  24/0, `run-leak-check.sh` 79/0.
