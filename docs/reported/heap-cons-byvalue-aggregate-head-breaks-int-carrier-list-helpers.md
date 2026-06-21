---
title: Generic int-carrier list helpers (list-length, ...) over a :heap Cons with a by-value aggregate head read tail at the wrong offset and segfault
category: Carrier <-> Concrete ABI -- :heap cons cell layout vs the int64-carrier list API
severity: Medium. Runtime segfault (not a compile error). A `(Cons (Option int))`
  -- a list whose element is a by-value aggregate -- now *builds* and is usable
  through the TYPED accessors (`.head`, ascribed `.tail`), but feeding it to a
  generic int-carrier list helper such as `(list-length (:: xs :int))` crashes.
  Blocks treating a by-value-aggregate-element list through the `:int`-carrier
  list API (list-length, list-eq?, etc.).
status: OPEN -- found 2026-06-21 while closing gap G1
  (docs/carrier-concrete-abi-crossing-audit-plan.md). Exposed once the
  homogeneity-check fix let such a list construct.
---

# `:heap` Cons with a by-value aggregate head breaks the int-carrier list API

## One-line summary

`Cons` is `(defstruct Cons :heap [A] (head A) (tail :int))` -- the generic
int-carrier list helpers walk a cons chain as a fixed
`struct { int64_t head; int64_t tail; }`. When the element `A` is a by-value
aggregate (`Option__int`, a multi-word struct), the real cell is
`struct { Option__int head; int64_t tail; }`, so `tail` no longer sits at
offset 8. The carrier walk reads `tail` from inside the `head` aggregate,
follows a bogus pointer, and segfaults.

## Minimal repro

```turmeric
(defn main [] : int
  (let [xs (:: (list (some 42) (some 7)) (Cons (Option int)))]
    (println (list-length (:: xs :int))))   ;; segfault
  0)
```

The list builds fine (gap G1 fixed); the crash is in `list-length` walking the
chain at the carrier layout. The typed path is unaffected:

```turmeric
(let [h0 (.head xs)
      t0 (:: (.tail xs) (Cons (Option int)))
      h1 (.head t0)]
  ...)                                       ;; works: reads 42 then 7
```

(see `tests/fixtures/list-homog-byvalue-aggregate-element`).

## Root cause (direction)

The `:int`-carrier list helpers (`list-length`, `list-eq?`, the inline-C cons
walkers) assume the `__tur_cons`/`Cons` cell has an int64 `head` at offset 0
and an int64 `tail` at offset 8. That holds for scalar / `:heap`-pointer
elements (head fits the carrier) but not for a by-value aggregate head, where
the C struct embeds the aggregate inline and pushes `tail` past offset 8.

This is the consumer-side companion of the carrier<->concrete family: the
*producer* (`list-build__` via `tcons-of`) correctly specializes the cell to
`Cons__Option__int { Option__int head; int64_t tail; }`, but the *generic*
consumer still reads it at the carrier layout.

### Not just the raw escape hatch -- the typed `(List A)` API collapses too

The typed `(List A)` wrapper (`stdlib/list-typed.tur`) is itself carrier-backed:
`list-empty?`, `list->carrier`, and any typed traversal bottom out in
`(:: xs :int)` and then the same `{int64 head; int64 tail}` carrier walk
(`tnil?`, `list-length`, ...). So a by-value-aggregate-element list is broken for
*every* carrier-level traversal, not only the explicit `(:: xs :int)` escape
hatch. Only the direct typed field accessors (`.head`, ascribed `.tail`) read the
real concrete layout and stay correct. This widens the impact: the fix has to
make the *carrier representation itself* correct for aggregate elements, not just
patch one helper.

## Fix directions

1. Box the by-value aggregate head into the carrier inside `tcons-of` for
   aggregate elements (so the cell stays `{ int64 head; int64 tail; }` and the
   head is a heap pointer) -- keeps the generic int-carrier API working, at the
   cost of an allocation per element.
2. Or monomorphize the int-carrier list helpers per element type (a `list-length`
   that walks `Cons__Option__int`), the same end-to-end-monomorphization
   direction as the rest of the audit. The `(:: xs :int)` coercion that erases
   the element type to the carrier is the point to intercept.

Option 2 keeps the by-value thread end-to-end and is consistent with the
crossing-audit's P2; option 1 is the smaller local change.

### Why this is architecturally significant (not a one-line patch)

Both options touch the cons-cell **layout invariant** established by #482 / gap
G1 (a by-value aggregate element is stored *inline* in the cell, which is what
makes the typed `.head` a direct read with no allocation):

- **Option 1 (box the head)** reverts that decision for aggregate elements: the
  cell becomes uniformly `{int64 head; int64 tail}` with a heap pointer head, so
  every carrier helper works -- but `.head` and `(:: head A)` must now *deref*
  (a carrier->concrete bridge), and every aggregate element costs an allocation.
  This changes the field-read codegen for aggregate-element cons and regenerates
  fixtures.
- **Option 2 (monomorphize the helpers)** keeps the inline layout but requires a
  per-element-type `list-length`/`list-eq?`/`tnil?`/... family minted at the
  `(:: xs :int)` coercion -- a substantial codegen feature, the end-to-end
  direction of the audit but not a small change.

Either way this is a design decision about the cons layout on the one track to
v1, with real regression risk to the currently-working typed path -- it is the
kind of architecturally significant change to confirm direction on before
undertaking, not a rough edge to patch in passing.

## Cross-reference

`docs/carrier-concrete-abi-crossing-audit-plan.md` -- downstream of gap G1;
distinct from gap G2 (nested instance-method dispatch).
