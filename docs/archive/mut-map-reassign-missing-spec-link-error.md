# `set!` on a `^mut` Map binding fails to link (missing map-assoc spec emission)

**Severity:** compiled-only build failure on a basic-looking shape; turi runs
it correctly.
**Status: FIXED, 2026-08-16 (both defects, filed and fixed the same day).**
**Found by:** the container-element collapse's performance probe (2026-08-16)
-- its map leg was the first code in the tree to grow a map by `set!`
reassignment rather than chained lets.

Both fixes landed exactly along the report's fix directions:

- **Defect 1 (missing spec emission):** `emit_abi_scan_expr` -- the
  spec-materialization walk -- had no `EX_SET` case, so a generic call in a
  `set!` RHS was the one statement position it never descended into; the
  call emitted the BASE generic name (`map_hyassoc_hyeq_hyo`, no `__spec__`
  suffix, no decl) and died at link.  One added case
  (`emit_module.c`) descending into `set_.value`.
- **Defect 2 (merge-temp spelling seam):** chokepoint 1's concrete-heap
  rule is now shared: `emit_repr_concrete_heap_ptr_c_name` (extracted from
  `emit_binding_repr_c_name`) is consulted by the let-bind decl AND by
  `emit_control_result_temp_decl` + its ctype mirror
  (`control_result_temp_ctype`), so the `^mut`/merge-temp path declares the
  typed pointer (`tur_adt_Map__int__int *`) the protocol wants.  The
  existing `bridge_control_result_int_ptr` reconciles a carrier-emitting
  tail into the pointer temp, so no new bridge was needed.

Verified: the repro prints `3` compiled and interpreted; suite 2606/0 with
the new fixture and **zero snapshot churn** -- the respelling fires only for
shapes that previously ICE'd, confirming the corpus never carried one.
Pinned by `tests/fixtures/mut-map-reassign/` (the report's int shape plus a
float-valued variant at `7.1`).

## Repro

```turmeric
(defn main [] : int
  (let [^mut m (:: (map-new) (Map int int))]
    (let [^mut i 0]
      (while (< i 3)
        (set! m (map-assoc m i i))
        (set! i (+ i 1))))
    (println (map-count m)))
  0)
```

- turi: prints `3`.
- compiled: `undefined reference to 'map_hyassoc_hyeq_hyo'` at link -- the
  `map-assoc-eq-o` specialization is *declared* and *called* but its body is
  never emitted.
- Debug builds fail earlier and differently: the R3 representation ICE fires
  on a merge-temp spelling seam first (below).

Pre-existing: reproduces identically on the pre-collapse compiler
(worktree at `e6e07e20`). Not caused by any of the 2026-08-16 changes; the
shape simply appears in no fixture -- every fixture grows maps by chained
`let`s (`(map-assoc (map-assoc ...) ...)`), never by `set!`.

## Two distinct defects

1. **Missing spec emission** (the link error). The `(set! m (map-assoc m i i))`
   call resolves to the `map-assoc-eq-o` clone `map_hyassoc_hyeq_hyo`, whose
   forward declaration is emitted but whose body generation is skipped.
   Presumably the spec-materialization walk does not reach call sites inside
   a `set!` RHS (or inside the `^mut` rebinding path); chained-let call sites
   materialize fine.

2. **Merge-temp spelling seam** (the Debug ICE). The control-form merge temp
   for the concrete heap app `(Map int int)` is declared `int64_t` where the
   consolidated protocol says the typed pointer
   (`want=heap-ptr got=carrier-i64`). Chokepoint 1 migrated the LET-BIND
   position for exactly this class; the `^mut`/merge-temp path was left
   behind, invisible because the corpus never exercised it. Benign bits
   (pointer-in-carrier is lossless), so this half is a consolidation gap,
   not a miscompile -- but with enforcement armed it is a hard error in
   Debug, and it fires before the link error, masking defect 1.

## Fix directions

- Defect 1: find where ABI specs are materialized from call sites and why a
  `set!` RHS (or a `^mut` binding's reassignment) is not walked; the
  chained-let path is the working reference.
- Defect 2: extend chokepoint 1's concrete-heap-binding rule to the
  merge-temp/`^mut` declaration path (`control_result_temp_ctype` /
  the mutable-binding decl), mirroring `emit_binding_repr_c_name`.

Until fixed, `TUR_REPR_NO_SHADOW_ICE=1` un-masks defect 1 in Debug builds
(you then hit the link error), and the workaround for users is the
chained-let / recursive-accumulator idiom every fixture already uses.

## Guide upkeep

`docs/guides/value-representations-guide.md` -- open cell: **`^mut`
rebinding of a concrete heap container (merge-temp position)**, plus the
spec-materialization hole, which is not a representation cell but travels
with it (same repro, same trigger).
