---
status: open
severity: medium
discovered: 2026-07-31
area: codegen/elab (residual carrier<->fat seams after fat-normalization stage 2)
---

# Residual carrier<->fat seams: let-aliased carrier params and boxed-result unification

## Summary

Found by `tests/type-fuzz-src.py` the first time thunk legs ran the full
crossing pool (seeds 89/97, 5 invalid-C + 2 rejects out of 500), immediately
after fat-normalization stage 2 landed. Not regressions of anything that
worked: these shapes were unreachable before stage 2 (their enclosing
family was the known-crash avoid list). Two cells:

**Cell 1 -- a let-ALIAS of a carrier fn param in tail position (invalid C).**

    (defn thru2 [v : (fn [] float)] : (fn [] float) (let [w v] w))
    ;; error: incompatible types when assigning to type 'void *'
    ;;        from type 'tur_poly_fn_t'

`v` is the by-value `tur_poly_fn_t` carrier; `w` copies it but its binding
carries only "non-boxed TY_FN", so the stage-2 tail normalizer cannot
classify it (param leaves convert via EX_POLY_TO_FAT; `w` is not a param).
Direct `... v)` works -- only the alias breaks. Same shape via `if`/`do`
wrapping of the alias, and cstr/int variants, all reproduce.

**Cell 2 -- if-branch unification of a carrier param against a boxed
recursive result (checker reject).**

    (defn f3 [n : int v : (fn [] bool)] : (fn [] bool)
      (if (= n 0) v (f3 (- n 1) v)))
    ;; error: if branches have mismatched types:
    ;;        then=ptr<void> else=(fn [] : bool)

`v` is the carrier (spelled `ptr<void>`); the recursive call's result is the
stage-2 boxed fn type. The unifier has no leniency for
carrier-vs-boxed-TY_FN of the same signature, though the runtime values are
convertible (the fat->carrier arg conversion exists and works -- verified:
a capturing closure result into a carrier param runs fine).

## Root cause (direction)

Both cells are the SAME seam: `(fn [] scalar)` is the tur_poly_fn_t CARRIER
at param position but a fat handle at (stage-2 normalized) result position,
and the classification/unification machinery only understands the
conversion at direct, statically-visible spots (param leaves, call args).
An alias or a control-flow join loses the provenance. Candidate fixes:

1. Tail normalizer: when a leaf is an unclassifiable local TY_FN var,
   normalize the let INIT it aliases (requires binding->init provenance) or
   conservatively bail out of normalizing that defn (restores pre-stage-2
   behavior for the ambiguous shape -- invalid C either way, but no new
   surface).
2. Unifier: treat carrier `ptr<void>`-spelled fn params and boxed TY_FN of
   the same signature as unifiable, inserting the (existing) conversion at
   the join.
3. Longer term: this seam is increment-4 material -- one decision function
   for fn-value representation would give the alias and the join the same
   answer by construction.

## Pinned

`tests/type-fuzz-src.py`: `known_bug_slug` maps `thunk` + `through`/`deep`
legs here; `--known-probes` pins Cell 1. Retire both when fixed.

## Guide upkeep

When this report is resolved -- or any representation/bridge it describes
changes shape on the way -- update
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
in the same PR: fix the representation inventory, move this report's row out
of the missing-cells table, and correct the link when the report moves to
`docs/archive/`.
