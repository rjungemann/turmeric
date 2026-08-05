---
status: resolved
severity: medium
discovered: 2026-07-31
resolved: 2026-07-31
area: codegen/elab (residual carrier<->fat seams after fat-normalization stage 2)
---

# Residual carrier<->fat seams: let-aliased carrier params and boxed-result unification

> **RESOLVED** (2026-07-31). Three fixes, all giving an ALIAS or a JOIN the
> same answer the direct spelling already got:
>
> - **Cell 1 (let-aliased carrier param in tail position).** The stage-2
>   tail walkers (`fn_tail_fn_leaf_kinds`, `elab_box_thin_fn_tail_leaves`,
>   `elab_normalize_fn_tail_leaves` in `src/compiler/elab_fns.c`) now carry
>   an alias environment of the let bindings they descend past and resolve a
>   leaf var transitively to its origin (`fn_tail_resolve_alias`, peeking
>   through ascriptions).  An alias of a carrier param converts via
>   EX_POLY_TO_FAT exactly like the direct param leaf (the conversion's
>   emission reads the alias's by-value tur_poly_fn_t copy); an alias of a
>   fat/normalized value passes through instead of being double-boxed; an
>   alias of a genuinely thin producer still gets the thin shim.
> - **Cell 2 (if-join of a carrier param against the boxed recursive
>   result).** The if unifier (`src/compiler/elab_forms.c`) admits a
>   carrier-param VAR arm (spelled ptr<void>) against a boxed or
>   fat-normalizable concrete fn type by inserting EX_POLY_TO_FAT on the
>   carrier arm AT THE JOIN -- context-independent, so it holds whether or
>   not the if sits in a normalized tail -- and adopts the boxed fn type.
>   Tyvar and effect-row'd signatures keep their conventions and still
>   mismatch loudly.
> - **Ascribed-alias variant** (found while fixing): `(:: v (fn [] int))`
>   on a carrier param is a pure type assertion; `elab_ascribe`
>   (`src/compiler/elab_types.c`) returns the var unchanged instead of
>   wrapping it in a node the let-binding path would bridge with a pointer
>   cast (invalid C on the by-value aggregate).
>
> Regression fixture: `tests/fixtures/fn-value-carrier-fat-seams/` (plain /
> do-wrapped / nested / if-arm / ascribed aliases; joins with the carrier
> arm on either side; float/int/cstr/bool results).  The fuzzer's
> `thunk` + `through`/`deep` legs are back in the full crossing pool and its
> known-probe row is retired.

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

## Root cause

Both cells are the SAME seam: `(fn [] scalar)` is the tur_poly_fn_t CARRIER
at param position but a fat handle at (stage-2 normalized) result position,
and the classification/unification machinery only understood the conversion
at direct, statically-visible spots (param leaves, call args). An alias or
a control-flow join lost the provenance. The fix followed the report's
directions 1 (provenance through the alias) and 2 (conversion at the join);
direction 3 -- one decision function giving the alias and the join the same
answer by construction -- is increment 4 of the consolidation meta-plan,
which these point fixes feed.

## Guide upkeep

Done with the resolution: the fn-value inventory in
[docs/guides/value-representations-guide.md](../../guides/value-representations-guide.md)
notes the alias/join provenance rules, and this report's row moved out of
the missing-cells table.
