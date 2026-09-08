---
title: A `#map{...}` literal leaks its intermediate maps even when the result is freed
category: Reported
description: hamt-of chains map-assoc, and each step mints a fresh `{void* hamt}` box. Only the LAST reaches map-free, so an N-entry map literal leaks N-1 boxes plus their trie nodes -- 1080 bytes in 11 allocations for `#map{:a 1 :b 2 :c 3}`, with no `any` involved. Pre-existing; measured while adding the Saffron map literal.
---

# A map literal leaks its intermediate maps

**Severity: medium.** Bounded per literal, unbounded in a loop that builds one.
Not a wrong answer -- the map is correct and freeing it frees the final
generation -- but a `#map{...}` inside a loop leaks proportionally to
iterations x entries, and there is nothing the author can free.

## Repro

```turmeric
(defn main [] : int
  (let [m #map{:a 1 :b 2 :c 3}]
    (println (map-count m))
    (map-free m))
  0)
```

```
SUMMARY: AddressSanitizer: 1080 byte(s) leaked in 11 allocation(s).
```

Plain Turmeric, no `any`, no Saffron, and `map-free` is called.

## Root cause

`#map{...}` lowers to `hamt-of`, which chains `map-assoc`. Each `map-assoc`
returns a NEW `{void *hamt}` carrier box (`stdlib/map.tur`'s `map-assoc-eq-o`
mallocs `r` and returns it), because a HAMT is persistent. So an N-entry literal
mints N boxes and the author holds only the last one. `map-free` releases that
one; the other N-1, and the trie generations they retain, have no owner.

This is the container analogue of the ownership shape
`docs/archive/any-struct-box-leak-per-widen.md` and
`docs/reported/byvalue-recursive-adt-boxes-are-never-freed.md` describe: a value
whose lifetime is an EXPRESSION's, with no scope to attach a drop to. Here the
intermediate is a whole map, and the expression that produced it is a macro
expansion the author never wrote.

The interpreter does not have this problem: `set_wrap_tracked` registers every
box on the env, so `turi_env_free` releases them all (that is what
`interp-collections-never-freed` installed).

## Fix directions

1. **Free the previous generation inside `hamt-of`'s chain.** The chain owns
   every intermediate by construction -- it just produced it and immediately
   shadows it -- so each step can `map-free` its input after the assoc. Needs
   care: the FIRST input is `(map-new)`, also owned, but a caller-supplied map
   passed to `map-assoc` directly must NOT be freed. That is why this belongs in
   the literal's own lowering rather than in `map-assoc`.
2. **Give the carrier box a scope owner**, the way the `any` widen boxes got one
   (`drop_localowned_<T>`). More general -- it would also cover a hand-written
   `(map-assoc (map-assoc m ...) ...)` -- and correspondingly larger. It shares
   the residue that report records: a value handed to a callee is MOVED, and
   nothing discharges ownership there.
3. **Refcount the carrier box.** Settles it wherever the box ends up, at the
   cost of a count on every map operation.

Direction 1 is small, confined to the literal, and covers the case that shows
up; measure it first.

## Measured, so the next person does not re-derive it

| literal | leaked |
|---|---|
| `#map{:a 1 :b 2 :c 3}` (plain Turmeric) | 1080 bytes / 11 allocations |
| `#map{:a 1 :b "two" :c 7.1}` (Saffron, `any` values) | 1144 bytes / 13 allocations |

The Saffron delta is 2 allocations -- the `any` value boxes -- so the `any`
widen is NOT the cause and fixing this does not depend on the `any` work.
`tests/fixtures/saffron-map-literal` therefore carries no
`requires.leak-check`, and says why in the fixture rather than leaving the
absent marker to look like an oversight.
