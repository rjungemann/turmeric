# A constrained generic relaying to another ran the wrong instance

**Severity: high.** A segfault in the shapes found. With a payload that does
not fault, it is the wrong instance's answer:

```turmeric
(defclass R [a] (rm [x : a] : a))
(definstance R [W] (rm [x : W] : W x))
(definstance R [float] (rm [x : float] : float {x + 1.25}))
(defn ri [a] [(R a)] [x : a] : a (rm x))
(defn ru [a] [(R a)] [v : a] : a (ri v))
(defn use [l (forall [a] [(R a)] (-> a a)) v : W] : W (l v))
(use ru (Wc 6))                                  ; segfault
```

Found 2026-10-01 by the type fuzzer's widened rank-2 class crossing
(`--crossing rank2_class`, seed 5150: 15 invalid C in 300 cases). Four
defects sat on the same path. **RESOLVED 2026-10-01.**

## Defects

1. **An int64 temp re-boxed as the aggregate.** The base carrier clone of
   `ri` calls a mixed spec (`__inst_R_rm_W__spec__int64_t_tur_adt_W`) or a
   word-returning dict slot. Either hands back the word under a
   concrete-looking type (`W`). Three return paths in `emit_fn_return_spelling`
   (`box_aggregate_result`, the inst-method carrier spill, and Phase 5's
   concrete→carrier box) read the elaborated tail type and boxed the int64 a
   second time, which assigned an int64 into a struct.
2. **The relay's dict clone dropped the dict.** `ru`'s dict clone called
   `ri`'s carrier base, whose `(rm x)` resolves statically to a representative
   instance. `dict_clone_forward_generic_calls` forwarded the caller's dict
   only for higher-kinded classes, by rewriting the call node in place. Every
   clone shares that node, and a kind-* generic also has concrete specs emitted
   from it. The specs, which have no dict params, then named an undeclared
   identifier.
3. **A spec borrowed a sibling spec's clone.** `(ei v)` inside `eu` was
   recorded as `ei__spec__W` under `eu__spec__W`, and nothing was recorded
   under `eu__spec__int64_t` (no ABI change at `int`). `emit_call_name`'s
   cross-spec fallback took the W record, so `(eu 40)` called the W instance's
   spec on an integer. Each line passed alone, because the sibling spec existed
   only when both were used.
4. **A construct in a spec adopted the spec's result.** `(some x)` at
   `A := (Option int)` builds an `(Option (Option int))` scrutinee. The ABI
   scan adopts the enclosing spec's result for a same-family construct
   whenever the call's own bindings have an `int` leaf, its guess for a
   carrier collapse. That peeled a layer and minted `some` at `A := int`,
   which was invalid C. `(Option float)` never showed it because its leaf is
   not `int`. The generic-spec matrix used float payloads only.

## Fix

1. At the head of the return ladder: an int64-returning function whose
   return value is a temp recorded `int64_t` returns it as-is.
2. A kind-* forward is recorded on the call node (`call_.dict_fwd_clone` /
   `dict_fwd_params`) instead of being rewritten. `emit_value_dispatch`
   applies it only when every named dict is a dict param of the function being
   emitted, via `ctx->dict_dispatch_params`. The HKT rewrite is unchanged.
3. The cross-spec fallback borrows a sibling's clone only when the clone's
   parameter C types are what this call's arguments resolve to here
   (`cross_spec_clone_fits_call`).
4. The adoption yields when it contradicts the construct's own arguments.
   An argument with more structure than the adopted parameter
   (`(Option int)` against `int`) cannot be the collapsed side
   (`abi_type_is_int_collapse_of`). The matrix gained `optint` and `resint`
   rows.

## Verified

- `tests/fixtures/constrained-generic-relay-word-temp-return` (defects 1–3)
  and `tests/fixtures/construct-in-spec-nested-option-int` (defect 4), compiled
  and `--interpret`.
- Suite 3464/0 and turi 2506/0. The 10 snapshots that moved lose a no-op
  `(int64_t)(intptr_t)` on an int64 temp.
- The new matrix rows: `optint` 214 cells and `resint` 214 cells, 0 failing.
