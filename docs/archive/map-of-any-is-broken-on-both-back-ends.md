---
title: A `(Map K any)` gives a wrong answer under `--interpret` and emits uncompilable C
category: Reported
description: RESOLVED. Both halves fixed. The interpreter's Map dropped the value TAG at the store, so every read of a (Map K any) came back `int`; it now boxes the value and sets bit 1 of the HAMT's `owned` flag -- the same mechanism the compiled path already used -- with the boxing conditional so an int-valued map is unchanged. A (Set any) still refuses at elaboration, which is the good failure.
---

# `(Map K any)` is broken on both back ends; `(Set any)` refuses cleanly

**RESOLVED 2026-09-08. Both halves.**

**COMPILED HALF FIXED 2026-09-08.** A `(Map K any)` now builds and reads back
each value with its own tag; `tests/fixtures/map-any-value-roundtrip` pins it.
The fix was small because the STORE side was already right: the HAMT assoc
boxes the value (`tur_hamt_box_key(&v, sizeof(tur_tagged_t))`), since `repr_of`
answers `REPR_BOXED_AGG` for an `any` at a container-element position, so the
word in the trie IS a `tur_tagged_t *`. Only the READ was missing its deref --
a control-form result temp declared `tur_tagged_t` was assigned the raw slot
word. `bridge_control_result_int_ptr` exists to reconcile exactly that straddle
for the int64<->pointer pairs, and bailed before its branches rather than being
handled by them; a tagged temp taking a carrier value is the third pair.

**INTERPRETER HALF FIXED 2026-09-08**, by direction 1 -- and it turned out to be
much smaller than "it touches every map operation that reads a value" predicted,
because the HAMT already had the mechanism. See below.

**Severity was medium-high** for the interpreter half -- a wrong answer with no
diagnostic. The compiled half was a hard failure, which is the better of the
two.

Filed to **settle a question rather than react to a bug**: fixing
[the Vec element tag](../archive/vec-any-interp-keeps-one-element-tag.md) ended
with a note that Map and Set should be checked *before* S6's `#map{...}` /
`#set{...}` work rather than after, because the Vec case was a silent wrong
answer that only appeared once a heterogeneous container became constructible.
This is that check.

## Measured

| container | shape | `--interpret` | compiled |
|---|---|---|---|
| `Vec` | `(Vec any)` | correct | correct |
| `Map` | `(Map int any)` | **`int` for every value** | correct (fixed) |
| `Set` | `(Set any)` | clean diagnostic | clean diagnostic |

### Map -- the wrong answer

```turmeric
(defn dyn [x : any] : any x)
(defn main [] : int
  (let [m (:: (map-new) (Map int any))]
    (let [m2 (map-assoc m 1 (dyn 7.1))]
      (let [m3 (map-assoc m2 2 (dyn "hi"))]
        (println (type-of (map-get m3 1)))     ; interpreted: int  (want float)
        (println (type-of (map-get m3 2)))     ; interpreted: int  (want cstr)
        (map-free m3))))
  0)
```

Compiled, the same program used to fail in `cc` (`__t210 = __ps_216;`,
incompatible types) -- loud, and now fixed; it prints `float` / `cstr`.

A HOMOGENEOUS map is fine -- `(Map int float)` round-trips `7.1` on both back
ends -- because the tag is recovered from the STATIC element type through the
ascription/reinterpret path. `any` is the case that has no static element type
to recover from.

### Map -- root cause

Unlike Vec, the interpreter's Map has no tag side table at all.
`native_map_get_eq` (`src/turi/collections_native.c`) ends:

```c
void *v = tur_hamt_get_eq(set_hamt(a[0]), (uint64_t)a[1].as_int, ...);
return turi_int((int64_t)(intptr_t)v);
```

Every read is `turi_int` of the raw carrier word. So Map is not "Vec's problem
again with a different table" -- it is the same problem one step earlier, with
no mechanism to extend. `vec_tag_set` has no Map or Set counterpart (grep finds
none), which is what made the one-probe answer in the Vec report inconclusive.

### Set -- the good failure

```
error [TUR-E0001]: function 'set-add-eq-o' arg 2: expected int, got any
```

Elaboration refuses, on both back ends, before anything can go wrong at run
time. Nothing to fix here beyond eventually admitting `any`; the important part
is that it does not silently accept.

## Fix directions -- direction 1, and it was small

**Direction 1 taken (interpreter).** Store a boxed value as the HAMT value.

The cost estimate on it -- "it touches every map operation that reads a value
(get, merge, dissoc, count, iteration, the GC mark walk)" -- was right about the
list and wrong about the size, because the HAMT ALREADY HAS THIS MECHANISM and
the compiled path already uses it. `Hamt` carries a `val_owned` flag and
`val_ops`, selected by **bit 1 of the same `owned` flag** the `_eq_o` operations
already thread (bit 0 = key, bit 1 = value): the runtime then retains the box on
every structural copy, releases it when the entry dies, and stamps `val_owned`
on the resulting map. So the persistence problem that ruled out direction 2 --
"which map does this tag belong to, when assoc returns a new one" -- is solved
by the trie itself, and a reader asks the map rather than a table.

What the interpreter was doing wrong was not having a table; it was passing
`(void *)a[3].as_int` where the compiled path passes
`tur_hamt_box_key(&v, sizeof(tur_tagged_t))` and `owned | 2`.

Five sites, all in `src/turi/collections_native.c`:

- `native_map_assoc_eq` / `_eq_o` -- box the value, pass `owned | 2`.
- `native_map_get_eq` / `_eq_o` -- read back through `map_val_read`, which
  consults the map's `val_owned` and derefs only for a boxed map.
- `map_eq_iter` -- hand the comparator the VALUES, not the box addresses.
  Comparing addresses would report two structurally equal maps unequal.
- `set_buf_scan` (the eval-boundary sweep) -- mark the value a boxed map holds,
  not its box address, or a handle stored in one is never seen.
- `native_map_merge` -- `tur_hamt_merge` re-inserts b's value WORDS into a copy
  of a without retaining them, which is right for a raw carrier and a
  double-free for a map-owned box. A boxed merge is done by hand, minting a
  fresh box per entry so each map owns its own.

### The boxing is conditional, and that is the part with a trap

An int-valued map is byte-identical to before -- no box, no `val_owned`, the raw
carrier inline -- so the change is confined to the maps that were broken. A map
boxes from its **first non-int value**.

Which leaves the shape that has to be right: a map that has already stored raw
int carriers and is then handed a string. Boxing only the new value leaves the
map HALF-boxed, and the next read of the older key dereferences an integer as a
pointer. So the first non-int store REBUILDS the map with every value boxed --
at most once per lineage, since every later assoc sees `val_owned` -- into a NEW
map, leaving the persistent original raw and readable.
`tests/fixtures/map-any-value-upgrade` pins that, including reading the
pre-upgrade map afterwards. `#map{:a 1 :b "two"}` is exactly this shape.

The rebuild declines for an OWNED (boxed) key, because re-inserting an existing
key box into a second map would need a retain it does not do. Scalar keys are
the case that matters -- int, cstr and Sym all report `mk-owned? = 0` -- so
`(Map int any)` and every keyword-keyed literal are covered; a struct-keyed map
whose first value is an int and whose second is not stays as it was.

### What did NOT need to change

Returning a properly-tagged value does not disturb the homogeneous read that
used to depend on the re-tag. `map-get-eq-o` returns the tyvar `:V`, so the
read-back is an `EX_ASCRIBE` -- and that arm coerces **only on a tag mismatch**
(struct/closure/ADT ascriptions stay transparent). `(:: (map-get m k) :float)`
on a value that is already `TURI_FLOAT` is therefore transparent. Same reason
the Vec element-tag fix composed with its ascription. This was the one real risk
in the design and it was settled by reading that arm rather than by guessing.

### The two rejected directions, for the record

2. ~~**A per-entry tag side table**, the Vec analogue.~~ Ruled out on
   inspection: the Vec table keys on the header pointer plus an ELEMENT INDEX,
   and the trie exposes no stable index to key on. Still true, and direction 1
   turned out not to need one.
3. ~~**Refuse `(Map K any)` at elaboration** the way Set already refuses.~~
   **Withdrawn** while the report was open: it was the right interim while both
   back ends were broken, but once the compiled half worked an elaboration
   refusal would have undone it -- elaboration is shared, so there is no way to
   refuse for the interpreter alone.

## Residue

- **The turi-closure key-comparator branch is not boxed.** A `MapKey` instance
  written in Turmeric rather than inline-C routes through `tur_hamt_set_eq_ctx`,
  which takes no `owned` flag, and the thread-local value hooks `_vo` installs
  are not reachable from there -- so a structural copy would not retain the box.
  Left alone rather than boxed without a retain. A map's key type fixes which
  branch it takes, so such a map is uniformly unboxed and `val_owned` says so;
  it is no worse than before, and a `(Map K any)` with that key shape still
  reports `int`.
- ~~**`map-iter-cur-val-as` and `map-get-dynamic-as`**~~ -- **CLOSED 2026-09-08,
  and the residue was wrong on its facts.** It claimed these two "cannot consult
  `val_owned`". Both can: `HamtIter` carries the map it is walking (`iter->map`),
  and `map-get-dynamic-as` takes the bare `Hamt *` directly, so `val_owned` is
  one deref away in each. They now unbox through the same `map_val_read`.

  This was not an academic gap. It backs `map-show`, so a HOMOGENEOUS
  `(Map Sym cstr)` -- boxed, because the conditional rule boxes from the first
  non-int value -- printed `#map{:a }` with the value missing. The `ctest`
  target `tur_show_collection_elems` caught it; the three fixture suites did
  not, because none of them shows a cstr-valued map.

## Not this bug

The store side emits `-Wdiscarded-qualifiers` on `tur_hamt_box_key`'s first
argument. Measured pre-existing: `(Map int Pt)` with a by-value struct value, a
shape that works today, emits it too. Not a pointer/integer mix, so the emitted-C
ratchet does not catch it.

Also not this bug, found while measuring it: **`(:: any-value int)` does not
compile** -- narrowing an `any` back to a concrete type by ascription is
"aggregate value used where an integer was expected", with no Map involved.
Filed as
[any-narrowing-ascription-does-not-compile](any-narrowing-ascription-does-not-compile.md).
