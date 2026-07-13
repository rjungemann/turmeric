# Vec element boxes leak on free (Map VALUE boxes: RESOLVED)

**Severity:** low (process-lifetime; no correctness impact; not test-gated --
compiled programs are not run under LSan in `tests/run.sh`).

## Status

- **Map VALUE boxes -- RESOLVED.** A multi-word by-value struct/ADT Map value is
  now boxed via `tur_hamt_box_key` (a refcount+size box) and the HAMT
  retains/releases it symmetrically with an owned key, gated by bit 1 of the
  `owned` flag that `map-assoc` threads via `(tur-wide-byval? v)` (an emit-time
  type query folded per monomorphization).  `map-free` releases each boxed value
  exactly once -- verified LSan-clean, refcount-safe under structural sharing and
  key-update.  See the "Lifecycle" section of the plan.
- **Vec element boxes -- still open** (below).

## Summary (Vec element boxes)

A multi-word by-value struct/ADT stored as a **Vec element** is heap-boxed (the
int64 carrier slot cannot hold >8 bytes -- see
`docs/upcoming/v2/collection-multiword-element-boxing-plan.md`). The box is a
plain `malloc` from the concrete->carrier escaping bridge
(`emit_carrier_bridge_escaping`, `src/compiler/emit_expr.c`), and `vec-free`
frees `data` + the header but not the per-element boxes -- it is a generic
inline-C body and cannot tell that a multi-word `A`'s `data[i]` is a box pointer
vs. a single-word inline value.

Map KEY boxes / Set element boxes are NOT affected: `mk-owned? = 1` +
`tur_hamt_free` release them exactly once (the compiled `map-free`/`set-free`
deep-free the HAMT).

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

## Root cause

The boxing is **caller-side and invisible to the collection**: the escaping
bridge boxes the aggregate at the `vec-push!` call site and stores the raw
pointer on the int64 carrier; `vec-push!` only sees an `int64`. So the generic
`vec-free` has no signal that a slot holds an owned box, and cannot free it
without knowing the element type is multi-word.

## Fix direction

Vec is mutable and NOT structurally shared, so `vec-free` can own+free the boxes.
Give `vec-free` element-type awareness for a multi-word `A` -- e.g. route it
(per-`A` at the spec site, keyed on the same `type_is_wide_byval_adt` predicate
the boxing uses, or via a `(tur-wide-byval? ...)`-style emit-time query like the
Map value fix uses) to a box-aware `tur_vec_free_boxed(v)` that `free`s each
`data[i]` before `data`. Also free the old box in `vec-set!` (overwrite) and
`vec-pop!`/element-removal, and retain/copy boxes in a deep `vec-clone`. Boxing
via `tur_hamt_box_key` (as the Map value path now does) would let `vec-free`
release with `tur_hamt_box_release` uniformly.

## Related

- `docs/upcoming/v2/collection-multiword-element-boxing-plan.md` -- the boxing
  design; the "Lifecycle" section tracks this.
- `src/runtime/hamt.c` -- `tur_hamt_box_key` / `tur_hamt_box_release` and the
  key/value ownership hooks (the machinery the Map value path reuses).
- `stdlib/vec.tur` (`vec-free`).
