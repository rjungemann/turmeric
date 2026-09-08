---
title: A `#map{...}` literal leaks its intermediate maps even when the result is freed
category: Reported
description: RESOLVED by direction 1. hamt-of chained map-assoc and each step minted a fresh `{void* hamt}` box, of which only the LAST reached map-free -- 1080 bytes in 11 allocations for `#map{:a 1 :b 2 :c 3}`, with no `any` involved. Each step of the chain now frees the map it assoc'd into, which is sound there and deliberately not in map-assoc itself.
---

# A map literal leaks its intermediate maps

**RESOLVED 2026-09-08** by direction 1. `tests/fixtures/map-literal-frees-intermediates`
and `tests/fixtures/map-literal-owned-key-no-double-free` pin it.

**Severity was medium.** Bounded per literal, unbounded in a loop that builds
one. Not a wrong answer -- the map was correct and freeing it freed the final
generation -- but a `#map{...}` inside a loop leaked proportionally to
iterations x entries, and there was nothing the author could free.

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

## Fix directions -- direction 1 taken

1. **Free the previous generation inside `hamt-of`'s chain.** Done, as a
   `map-assoc-consuming__` macro that `hamt-assoc-each__` threads instead of a
   bare `map-assoc`:

   ```turmeric
   (defmacro map-assoc-consuming__ [m k v]
     `(let [__tur_mprev__ ~m]
        (let [__tur_mnext__ (map-assoc __tur_mprev__ ~k ~v)]
          (do (map-free __tur_mprev__)
              __tur_mnext__))))
   ```

   The care the direction called for is where it lives. This is sound in the
   CHAIN and deliberately not in `map-assoc` itself: the caller of `map-assoc`
   generally holds its argument and must keep it, whereas `hamt-of` seeds the
   chain with a fresh `(map-empty-for ...)` and every later input is a previous
   step's result -- so every generation in the chain is the chain's own.
   `hamt-assoc-each__` has exactly one caller (`hamt-of`), checked, so no
   caller-owned map can reach it.

   Freeing the previous generation does not disturb the new one: `tur_hamt_free`
   is refcount-safe under structural sharing, which is the guarantee
   `map-free`'s own body already documents.

2. ~~Give the carrier box a scope owner.~~ Not needed for this. Still the more
   general answer -- it would also cover a hand-written
   `(map-assoc (map-assoc m ...) ...)`, which this does not -- and correspondingly
   larger, sharing the parameter-side residue that
   [byvalue-recursive-adt-boxes-are-never-freed](byvalue-recursive-adt-boxes-are-never-freed.md)
   records.
3. ~~Refcount the carrier box.~~ Not needed.

## What had to be checked, and was

The risk this fix carries is the opposite of the bug: freeing an intermediate
too eagerly, showing up as a double-free or a use-after-free rather than a leak.
Three shapes, all ASan-clean (address AND leak):

- **Owned (boxed) keys.** With `mk-owned? = 1` every key is a
  `tur_hamt_box_key` box the map owns, and `map-free` releases them via the
  map's stamped `key_ops`. If an intermediate's release freed key boxes the next
  generation still points at, this is where it would surface. It does not -- the
  boxes are refcounted and a structural node copy retains them.
  `tests/fixtures/map-literal-owned-key-no-double-free`.
- **Duplicate keys**, where a step's assoc can share more with its input than a
  fresh key would.
- **cstr keys**, which are content-compared.

The last two are in `tests/fixtures/map-literal-frees-intermediates`, which runs
on both back ends.

`tests/fixtures/saffron-map-literal` shipped for one commit WITHOUT a
`requires.leak-check` marker, and said in the fixture why. It carries one now.

## Codegen snapshot

One snapshot moved: `tests/fixtures/data-literal-nested/expected.c`, because
`map_free` is now emitted into a program that did not previously call it (plus
declaration reordering). Its stdout is byte-identical, and the snapshot was
regenerated in the same change.

## Measured, so the next person does not re-derive it

| literal | leaked BEFORE | after |
|---|---|---|
| `#map{:a 1 :b 2 :c 3}` (plain Turmeric) | 1080 bytes / 11 allocations | 0 |
| `#map{:a 1 :b "two" :c 7.1}` (Saffron, `any` values) | 1144 bytes / 13 allocations | 0 |

The Saffron delta was 2 allocations -- the `any` value boxes -- so the `any`
widen was never the cause, and fixing this did not depend on the `any` work.

## Not this bug

The interpreter never had this problem: `set_wrap_tracked` registers every box
on the env, so `turi_env_free` releases them all
(`interp-collections-never-freed`). The fix is a stdlib macro change, so the
interpreter now frees the intermediates eagerly instead of at teardown -- same
outcome, earlier.
