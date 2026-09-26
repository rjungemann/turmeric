# `list-length` on a `(Cons any)` chain segfaults compiled

**RESOLVED 2026-09-26.** Worse than filed, and fixed by the first direction.
The repro's ascription was not needed: every list in a Saffron program is a
`(Cons any)` -- a `& xs : any` rest list and a `(list 1 2 3)` literal are
both typed that way statically -- so a plain `(length xs)` or `(list-length
(list 1 2 3))` segfaulted, and `(list-head xs)` quietly answered the head's
tag word (4) instead of `7.1`.

The fix is at the call, not in the walker. A call to one of the carrier
helpers `list-length` / `length`, `list-head` / `car`, `list-tail` / `cdr`
whose argument is a `(Cons A)` with a head wider than the carrier word -- an
`any` or union element (a two-word `tur_tagged_t`), or a by-value aggregate
laid out inline -- is elaborated as a call to the element-aware twin
(`tlength`, `thead`, `ttail`), which reads `.head` / `.tail` at the
monomorph's own layout (`typed_list_twin_redirect`, `src/compiler/elab_call.c`).
Every other element keeps the carrier call and its C byte for byte. An
explicit `(:: xs :int)` is left alone: it is the documented escape hatch
that discards the element type (the archived
[heap-cons-byvalue-aggregate-head-breaks-int-carrier-list-helpers](history/heap-cons-byvalue-aggregate-head-breaks-int-carrier-list-helpers.md)).

`tlength` was made tail-recursive (`tlength-acc__`) in the same change,
since `length` on a Saffron list now reaches it: the old `(+ 1 (tlength
...))` blew the stack on a million-element list. The new defn shifted the
elaborator's id numbering, so the 155 codegen snapshots were regenerated
(renumbering only).

Pinned by `tests/fixtures/list-helpers-wide-head-element` (Saffron: rest
lists, a `(list ...)` literal, the empty list, head/tail/length, on both
engines) and three new lines in `list-length-byvalue-aggregate-element`
(`list-length` / `length` / `list-head` on a `(Cons (Option int))`).

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
