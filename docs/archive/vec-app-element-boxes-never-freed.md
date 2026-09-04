# `(Vec (Option int))` leaked one element box per push

**Severity:** memory leak, silent, proportional to element count. No
miscompile -- values round-trip correctly; they are simply never released.
**Status:** FIXED 2026-08-16 by the increment-4 container-element collapse
(`docs/archive/repr-decision-function-plan.md`).
**Found by:** the CONTAINER_ELEM representation shadow, while diagnosing what
looked like a harmless scope mismatch.

## Repro

```turmeric
(defstruct Sm [a : int])
(defn main [] : int
  (let [va (:: (vec-new) (Vec (Option int)))]
    (vec-push! va (:: (some 11) (Option int)))
    (vec-push! va (:: (some 22) (Option int)))
    (vec-free va))
  (let [vb (:: (vec-new) (Vec Sm))]      ; control: nominal ADT element
    (vec-push! vb (Sm 33))
    (vec-push! vb (Sm 44))
    (vec-free vb))
  0)
```

Built with ASan/LSan
(`TUR_CC_FLAGS="-O1 -g -fsanitize=address -std=c99 -L<build>/src"`):

```
ERROR: LeakSanitizer: detected memory leaks
Direct leak of 16 byte(s) in 1 object(s) ...
Direct leak of 16 byte(s) in 1 object(s) ...
SUMMARY: AddressSanitizer: 32 byte(s) leaked in 2 allocation(s).
```

Exactly the two `(Option int)` boxes. The `Sm` vector in the same program
freed both of its.

Note the default `tur build` does NOT link a sanitizer, so this leak is
invisible to an ordinary run and to the fixture harness; it takes an explicit
`TUR_CC_FLAGS` sanitizer build to see.

## Root cause

Two mechanisms decided one thing and agreed on half of it.

- **Push side.** A concrete by-value app element is heap-boxed into the slot:
  `malloc(sizeof(tur_adt_Option__int))`, store, `vec_hypush_ex`.
- **Ownership side.** `vec-free` threads `(tur-vec-elem-wide? v)` as bit 1 of
  its `owned` flag, and that fold consults
  `type_is_boxed_container_elem` (`src/compiler/types.c`), which recognised
  **TY_ADT only**. For a TY_APP element it answered 0, so `vec-free-o` freed
  the data buffer and the header but not the per-element boxes.

Visible in the emitted C -- the two folded flags in the repro above:

```c
int64_t __ps_165 = (INT64_C(0));   /* (Vec (Option int)) -> boxes NOT freed */
int64_t __ps_172 = (INT64_C(1));   /* (Vec Sm)           -> boxes freed     */
```

The same predicate also drives the map/set release fold (`tur-wide-byval?`),
so the hole was not specific to `Vec`.

## Why it survived

The boxing half and the ownership half agree for every *nominal* by-value
element, which is what the fixtures exercise; the split only opens for a
parametric one. This is the "representation splits hide behind one-sided test
coverage" pattern the meta-plan warns about, and the shadow that surfaced it
initially read as a false positive: repr_of said BOXED, the predicate said
not-boxed, and the push side visibly boxed either way -- so the first
diagnosis was "a scope mismatch between two agreeing mechanisms". That
diagnosis was right about the boxing and wrong about the ownership, and only
looking at what each of the six call sites *does* with the answer showed
which half was which.

## Fix

`type_is_boxed_container_elem` is now defined as its answer rather than a
second derivation of it:

```c
bool type_is_boxed_container_elem(Type t) {
    return repr_of(&t, REPR_POS_CONTAINER_ELEM) == REPR_BOXED_AGG;
}
```

All six consulting sites -- the push bridges, the read recovery, the two
ownership folds, and the field-read receiver recovery -- therefore read one
answer. The position's shadow is retired by construction: want and got are
the same expression.

## Coverage

- `tests/fixtures/vec-app-element-box-lifecycle` -- round-trips and frees both
  an app-element and an ADT-element vector. It pins the *double-free*
  direction, which an unsanitized harness run can actually observe; the leak
  direction needs the sanitizer build above.
- `tests/run-repr-trace.sh` asserts the app-element vector's `vec-free` is
  emitted with a non-zero `owned` flag.
- Suite 2599/0; type fuzzer 2 fresh seeds x 250 cases, 0 BUG classes.
