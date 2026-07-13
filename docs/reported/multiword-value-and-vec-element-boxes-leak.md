# Multi-word Map VALUE boxes and Vec element boxes leak on free

**Severity:** low (process-lifetime; no correctness impact; not test-gated --
compiled programs are not run under LSan in `tests/run.sh`).

## Summary

A multi-word by-value struct/ADT stored as a **Vec element** or a **Map value**
is heap-boxed (the int64 carrier slot cannot hold >8 bytes -- see
`docs/upcoming/v2/collection-multiword-element-boxing-plan.md`). The box is a
plain `malloc` from the concrete->carrier escaping bridge
(`emit_carrier_bridge_escaping`, `src/compiler/emit_expr.c`), and nothing frees
it:

- **Vec element boxes** -- `vec-free` frees `data` + the header but not the
  per-element boxes (it is a generic inline-C body and cannot tell that a
  multi-word `A`'s `data[i]` is a box pointer vs. a single-word inline value).
- **Map VALUE boxes** -- `tur_hamt_free` (now called by the deep `map-free`, see
  below) releases KEYS via the map's stamped `key_ops`, but there is no symmetric
  release for VALUES, so a `Map[int Point]` value box outlives the map.

Map KEY boxes / Set element boxes are NOT affected: `mk-owned? = 1` +
`tur_hamt_free` release them exactly once (fixed alongside this note by making
the compiled `map-free`/`set-free` deep-free the HAMT).

## Minimal repro

```turmeric
(load "stdlib/typeclass.tur")
(defstruct Point :copy [x : int y : int])
(defn main [] : int
  (let [v (vec-of (Point 1 2) (Point 3 4))]
    (println (show (.x (vec-get v 0))))
    (vec-free v)                      ; frees data+header, NOT the 2 element boxes
    0))
```

Compile the emitted C with `-fsanitize=address,undefined` linked against
`build/src/libturi.a` and run with `ASAN_OPTIONS=detect_leaks=1`:

```
Indirect leak of 16 byte(s) ... in main ...   ; each malloc(sizeof(tur_adt_Point)) element box
```

The `Map[int Point]` value case is analogous (the box is allocated in
`map_assoc_eq_o__spec__...`).

## Root cause

The boxing is **caller-side and invisible to the collection**: the escaping
bridge boxes the aggregate at the `vec-push!` / `map-assoc` call site and stores
the raw pointer on the int64 carrier; `vec-push!` / the HAMT inserter only see an
`int64`. So the generic collection destructors have no signal that a slot holds
an owned box, and cannot free it without knowing the element/value type is
multi-word.

## Fix directions

Both are bounded but need per-type ownership signal the current generic
destructors lack:

1. **Vec element boxes.** Vec is mutable and NOT structurally shared, so
   `vec-free` can own+free the boxes. Give `vec-free` element-type awareness for a
   multi-word `A` -- e.g. route it (per-`A` at the spec site, keyed on the same
   `type_is_wide_byval_adt` predicate the boxing uses) to a box-aware
   `tur_vec_free_boxed(v)` that `free`s each `data[i]` before `data`. Also free
   the old box in `vec-set!` (overwrite) and `vec-pop!`/element-removal, and
   retain/copy boxes in a deep `vec-clone`.

2. **Map VALUE boxes.** Extend the HAMT ownership ops symmetrically to values:
   thread a `val_owned` flag through `map-assoc` (mirroring the existing key
   `owned`), box the value via `tur_hamt_box_key` (refcount+size header) instead
   of a plain `malloc`, and have `tur_hamt_free` / entry-drop release owned values
   the way it already releases owned keys. This is the plan's "extend the
   ownership ops to retain/release the value box symmetrically with the key box."

## Related

- `docs/upcoming/v2/collection-multiword-element-boxing-plan.md` -- the boxing
  design; the "Lifecycle" section tracks this.
- `src/runtime/hamt.c` -- `tur_hamt_box_key` / `tur_hamt_box_release` / the
  `key_ops` release hook (the machinery to generalize to values).
- `stdlib/vec.tur` (`vec-free`), `stdlib/map.tur` (`map-free`), `stdlib/set.tur`
  (`set-free`).
