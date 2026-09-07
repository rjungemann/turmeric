---
title: A `(Map K any)` gives a wrong answer under `--interpret` and emits uncompilable C
category: Reported
description: Reading a value out of a (Map K any) reports `int` for every value under --interpret (the HAMT get native returns turi_int of the raw carrier word, with no tag recovery at all), and the compiled back end emits C that does not compile. A (Set any) is different and better -- it refuses at elaboration with a real diagnostic. Measured while settling the question S6 flagged before starting the #map/#set work.
---

# `(Map K any)` is broken on both back ends; `(Set any)` refuses cleanly

**Severity: medium-high** for the interpreter half -- a wrong answer with no
diagnostic. The compiled half is a hard failure, which is the better of the two.

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
| `Map` | `(Map int any)` | **`int` for every value** | **cc error** |
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

Compiled, the same program fails in `cc` (`__t210 = __ps_216;`, incompatible
types), so it is loud there.

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

## Fix directions

1. **Give the HAMT value read a per-entry tag**, as Vec now has per-element.
   The Vec fix was a byte-per-element side table keyed on the header pointer;
   the Map analogue is keyed on `(hamt, entry)`, which the trie does not expose
   as a stable index -- so this is not a transcription of the Vec fix.
2. **Store a boxed `TuriValue` as the HAMT value** for a `(Map K any)`, so the
   tag rides the value rather than a side table. This is what the compiled path
   already does for a `(Vec any)` element (`repr_of` answers `REPR_BOXED_AGG` at
   `REPR_POS_CONTAINER_ELEM`), so it makes the two back ends agree by
   construction rather than by two mechanisms kept in lockstep. Likely the right
   answer, and it subsumes Set.
3. **Refuse `(Map K any)` at elaboration** the way Set already refuses, as an
   interim. Turns a silent wrong answer into Set's diagnostic, cheaply, and is
   strictly better than the status quo if the real fix is not imminent.

Direction 3 is worth doing immediately if 2 is not; a wrong answer is the one
outcome that should not survive contact with S6, where `#map{...}` makes this
shape ordinary.
