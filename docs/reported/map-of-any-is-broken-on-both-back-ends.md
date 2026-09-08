---
title: A `(Map K any)` gives a wrong answer under `--interpret` and emits uncompilable C
category: Reported
description: Reading a value out of a (Map K any) reports `int` for every value under --interpret -- the HAMT get native returns turi_int of the raw carrier word, with no tag recovery at all. The compiled half is fixed. A (Set any) is different and better -- it refuses at elaboration with a real diagnostic. Measured while settling the question S6 flagged before starting the #map/#set work.
---

# `(Map K any)` is broken on both back ends; `(Set any)` refuses cleanly

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

**The interpreter half is still open, and it is the wrong-answer one.**

**Severity: medium-high** for the interpreter half -- a wrong answer with no
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

## Fix directions

Only the INTERPRETER half remains.

1. **Store a boxed `TuriValue` as the HAMT value.** The compiled fix confirms
   this is the shape that works -- boxing at the store is what made the read a
   plain deref there -- and it makes the two back ends agree by construction
   rather than by two mechanisms kept in lockstep. It touches every map
   operation that reads a value (get, merge, dissoc, count, iteration, the GC
   mark walk), which is the cost.
2. **A per-entry tag side table**, the Vec analogue. Ruled out on inspection:
   the Vec table is keyed on the header pointer plus an ELEMENT INDEX, and the
   trie exposes no stable index to key on. Recorded so the next person does not
   re-derive it.
3. ~~**Refuse `(Map K any)` at elaboration** the way Set already refuses.~~
   **Withdrawn.** It was the right interim while both back ends were broken; now
   that the compiled half works, an elaboration refusal would undo it --
   elaboration is shared, so there is no way to refuse for the interpreter
   alone. The interim is gone and direction 1 is the only route.

`tests/fixtures/map-any-value-roundtrip` carries `requires.compiled` so the
remaining divergence is recorded rather than hidden by a skip nobody reads --
the same posture the Vec case used for the one commit it needed.

## Not this bug

The store side emits `-Wdiscarded-qualifiers` on `tur_hamt_box_key`'s first
argument. Measured pre-existing: `(Map int Pt)` with a by-value struct value, a
shape that works today, emits it too. Not a pointer/integer mix, so the emitted-C
ratchet does not catch it.
