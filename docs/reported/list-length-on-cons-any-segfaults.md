# `list-length` on a `(Cons any)` chain segfaults compiled

**Severity: low-medium.** `stdlib/list.tur`'s `list-length` is inline C over
the untyped cons cell -- `{ int64_t head; int64_t tail; }` -- and a `(Cons
any)` monomorph's cell is `{ tur_tagged_t head; tur_adt_Cons__any *tail; }`:
a 16-byte head, so the walker reads the tail from the middle of the head
and dereferences it.  The interpreter answers correctly (its native walks
either representation).  Found 2026-09-23 as section 3 of
docs/archive/r7rs-compiled-dynamic-shapes.md and misread there as a boxing
defect; the boxing is fine (r7rs-lang-plan R6).

## Repro

```turmeric
#lang saffron
(defn g [& xs : any] (list-length (:: xs (Cons any))))
(defn main [] : int (println (g 7.1 2)) 0)
```

```
$ tur run g.tur
Segmentation fault          # list_hylength, stdlib/list.tur:179
$ tur --interpret g.tur
2
```

A typed walker over the same chain is right on both back ends
(`tests/fixtures/saffron-variadic-dynamic-call`'s `len`), and the R7RS
prelude uses its own (`r7rs-length`).

## Fix directions

- Give `(Cons any)` (and every monomorph whose element is wider than one
  word) a per-monomorph `list-length`, the way the typed accessors are
  specialised, or make the inline C read the cell through the monomorph's
  layout (`sizeof` the head from the element type) instead of assuming the
  untyped cell.
- Until then, `list-length` is the untyped-list walker; a `(Cons any)`
  wants a Turmeric-level walk.
