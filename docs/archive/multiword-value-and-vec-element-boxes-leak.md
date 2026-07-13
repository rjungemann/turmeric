# Multi-word collection element/value boxes leak on free -- RESOLVED

**Severity:** low (process-lifetime; no correctness impact; not test-gated --
compiled programs are not run under LSan in `tests/run.sh`).

## Status -- RESOLVED

Both box classes tracked by this report now release exactly once when the
collection is freed:

- **Map VALUE boxes -- RESOLVED.** A multi-word by-value struct/ADT Map value is
  boxed via `tur_hamt_box_key` (a refcount+size box) and the HAMT
  retains/releases it symmetrically with an owned key, gated by bit 1 of the
  `owned` flag that `map-assoc` threads via `(tur-wide-byval? v)` (an emit-time
  type query folded per monomorphization).  `map-free` releases each boxed value
  exactly once -- verified LSan-clean, refcount-safe under structural sharing and
  key-update.
- **Vec element boxes -- RESOLVED.** `vec-free` is now a macro that forwards
  `(vec-free-o v (tur-vec-elem-wide? v))`, where `tur-vec-elem-wide?` is an
  emit-time query (twin of `tur-wide-byval?`, intercepted in
  `src/compiler/emit_expr.c`) that peels the element type out of the `(Vec A)`
  spine and folds to 1 for a wide by-value `A`.  A Vec is mutable and NOT
  structurally shared, so it owns its element boxes outright: `vec-free-o` `free`s
  each `data[i]` box (a plain `malloc` owned pointer from the escaping bridge)
  before the buffer.  Verified LSan-clean (the only residual on the repro is the
  unrelated process-lifetime `show`-string allocs).  The interpreter's
  `native_vec_free_o` frees the buffer + header (elements ride as TuriStruct
  pointers it owns separately).

## Verification

```turmeric
(load "stdlib/typeclass.tur")
(defstruct Point :copy [x : int y : int])
(defn sum-vec [v : (Vec Point) i : int acc : int] : int
  (if (= i (vec-len v)) acc
    (sum-vec v (+ i 1) (+ acc (.x (vec-get v i))))))
(defn main [] : int
  (let [v (vec-of (Point 1 2) (Point 3 4) (Point 5 6))]
    (println (show (sum-vec v 0 0)))
    (vec-free v)
    0))
```

`tur emit-c` -> compile with `-fsanitize=address,undefined` against
`build/src/libturi.a`, run with `ASAN_OPTIONS=detect_leaks=1`: the three
`malloc(sizeof(tur_adt_Point))` element boxes are freed; no Point-box leak
remains.  The `vec-multiword-struct-element` fixture now ends with `(vec-free v)`
to exercise the release path on both suites.

## Residual (tracked separately)

An in-place `vec-set!` overwrite and a dropped `vec-pop!` result orphan their old
element box -- see `docs/reported/vec-set-pop-element-box-leak.md`.

## Related

- `docs/upcoming/v2/collection-multiword-element-boxing-plan.md` -- the boxing
  design; the "Lifecycle" section tracks this.
- `src/runtime/hamt.c` -- `tur_hamt_box_key` / `tur_hamt_box_release` and the
  key/value ownership hooks (the machinery the Map value path reuses).
- `stdlib/vec.tur` (`vec-free` macro, `vec-free-o`, `tur-vec-elem-wide?`).
- `src/turi/collections_native.c` (`native_vec_free_o`).
