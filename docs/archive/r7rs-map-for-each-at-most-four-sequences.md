# `#lang r7rs`: `map` and `for-each` take at most four sequences

**RESOLVED 2026-09-25.** `r7rs-mapn-go__` and `r7rs-for-eachn-go__` carry
arms 5 through 8, spelled inline like 1 through 4 (`r7rs-head-of__` reads a
head past the fourth), so the cap is the shim arity
(`TUR_FAT_SHIM_MAX_ARITY`, eight) and the fall-through message names it. The
five- and eight-sequence cases of `map`, `for-each`, `string-map`,
`string-for-each`, `vector-map` and `vector-for-each` are in
`tests/fixtures/r7rs-base-library`; the million-element `(map + a b)` still
runs at the default `-O2` (4.1 s wall for two 10^6-element lists, most of it
building them). The guide's bullet is deleted. Original report follows.


**Severity:** low-medium. R7RS 6.10 puts no limit on the number of lists
`map` and `for-each` accept; a fifth is a runtime error on both back ends.
`vector-map`, `vector-for-each`, `string-map` and `string-for-each` share the
same walker and the same limit.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(write (map + '(1) '(2) '(3) '(4)))         ; (10) -- fine
(write (map + '(1) '(2) '(3) '(4) '(5)))    ; panics
```

```
$ tur run p.tur
panic ...: map: more than 4 lists is not supported yet (r7rs-lang-plan R6)
(10)
$ tur --interpret p.tur
panic: map: more than 4 lists is not supported yet (r7rs-lang-plan R6)
```

The same message with `for-each` in place of `map` comes from
`vector-for-each` and `string-for-each` as well, so the whole family is
capped:

```scheme
(write (string-map (lambda (a b c d e) a) "1" "2" "3" "4" "5"))
(vector-for-each (lambda (a b c d e) a) #(1) #(2) #(3) #(4) #(5))
```

## Root cause

`stdlib/r7rs/prelude.tur:254-288`. `r7rs-mapn-go__` and `r7rs-for-eachn-go__`
call `f` on the heads of the lists with its arguments spelled out INSIDE the
loop body -- an `if` chain on `n`, the number of lists, with an arm for 1, 2, 3
and 4 and `r7rs-fail-any__` as the fall-through (prelude.tur:272 and :286).
`vector-map`, `string-map` and the two `-for-each` procedures convert to lists
and come through the same walker, so they inherit the cap.

The spelled-out arms are deliberate and the comment above them (T8) says why:
calling `f` through `r7rs-apply-list__`, or through any helper, made each step
a call into a separate CPS procedure that resumed the loop from inside its own
frame, so every element nested a C frame and a million-element `(map + a b)`
overflowed even at `-O2`. So the arms cannot simply be replaced by the
rest-chain packing `apply` uses.

The same comment calls 4 "the dynamic call's argument limit (apply's too)",
which is stale: since r7rs-apply-more-than-four-arguments the dynamic call, the
fat shims and `apply` all carry eight (`TUR_FAT_SHIM_MAX_ARITY`).

## Fix directions

- Cheapest, and it closes the gap most programs would hit: write arms 5
  through 8 the way 1 through 4 are written, and the cap becomes the shim
  arity rather than a number below it. The `if` chain is already the shape;
  this only lengthens it. Update the stale "(apply's too)" comment in the same
  change.
- Past the shim arity the loop needs a step that does not cost a frame per
  element. One direction is a walker specialized per arity by a macro, so the
  spelling stays inline while the source does not repeat; another is to teach
  the CPS lowering to treat the helper call as a tail resume, which is the
  general fix and would also let `apply`'s packing be used here.
- Either way, `tests/fixtures/r7rs-base-library` is the place for a
  five-sequence case, and the million-element `(map + a b)` in the T8 comment
  is the regression to re-run: a fix that lifts the cap and reintroduces the
  frame-per-element nesting is not a fix.

## Guide upkeep

`docs/guides/r7rs-guide.md` ("Where it differs from R7RS") carries a bullet
beginning "**`map` and `for-each` take at most four sequences.**" When this
resolves, delete that bullet whole; the `apply` bullet below it already states
the eight-argument cap, and nothing else in the guide refers to the four.
