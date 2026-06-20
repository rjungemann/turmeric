---
title: MutableMap's zero-arg `[K V]` producer (`mutmap-new`) is not typed-pointer-monomorphized, so concrete `(MutableMap int int)` consumers get an int->pointer mismatch
category: Bug report -- ABI / Codegen (Track A, end-to-end monomorphization)
severity: Medium. Not a miscompile on the common (carrier) path -- the int64 and
  the `MutableMap__int__int *` pointer are bit-compatible via `intptr_t`, so the
  generated code runs correctly on platforms where that round-trip is well-defined.
  But it emits a `-Wint-conversion` warning for ordinary user code (any function
  that takes a concrete `(MutableMap K V)` and is fed by `mutmap-new`), and it
  blocks the MutableMap typed-pointer producer slice (the analog of the Vec slice
  in #377/#400): typing the *consumers* without typing the *producer* is exactly
  the inconsistency that surfaces.
status: RESOLVED. The "multi-param resolution gap (#364)" framing turned out to be
  wrong -- the bindings/result-type resolution for the zero-arg `[K V]` producer
  was already correct. The two real causes were (1) `mutmap-new`'s inline-C body
  not returning through `__TUR_RET__` (so it never minted a typed producer spec),
  and (2) a GENERAL call-site relabel bug (a typed `:heap` value spilled to the
  int64 carrier when passed to a user fn taking the concrete heap type) that hit
  Vec equally. Both fixed; see "Resolution" below.
---

# MutableMap multi-param producer typing is blocked

## Resolution (this session)

The "multi-param resolution gap" was a misdiagnosis. Instrumenting
`emit_abi_register_call` showed the zero-arg `[K V]` constructor `mutmap-new`
already gets correct `K->int, V->int` bindings and a concrete
`result_type = MutableMap__int__int *` (`abi_changes=1`). Two concrete,
non-multi-param causes were the real blockers:

1. **`mutmap-new`'s inline-C body returned `(int64_t)`, not `(__TUR_RET__)`.**
   That is the difference that routes `vec-new` and `mutmap-new` down different
   paths: a `__TUR_RET__`-bearing body sets `inline_c_has_ty_template`, which
   makes `body_qualifies_for_carrier_skip` false, so the call interns its typed
   producer spec on the `abi_changes` path. Without it, `mutmap-new` entered the
   carrier-skip block and fell to the producer-result gate at
   `emit_module.c` -- which keys on `type_is_heap_vec(fd->return_type)`, and
   `fd->return_type` for a `(MutableMap K V)` return is a degenerate `TY_APP`
   (`type_from_kind(TY_APP)` at `elab_fns.c:3120`, no spine/def), so the gate
   missed it. **Fix:** `stdlib/mutmap.tur` -- `mutmap-new` now returns
   `(__TUR_RET__)(intptr_t)m`, and `type_is_heap_vec` (emit_module.c) accepts
   `MutableMap`. `mutmap-new` now mints
   `mutmap_new__spec__MutableMap__int__int__()`.

2. **A typed `:heap` value was spilled to the int64 carrier when passed to a
   user fn taking the concrete heap type.** `emit_expr.c`'s call-arg emit wrapped
   a `Vec__int *` / `MutableMap__int__int *` argument in `(int64_t)(intptr_t)`
   whenever the callee's `arg_kinds[i] == TY_INT` (true for every carrier-ABI
   slot, including a concrete `:heap` param whose C type is actually a typed
   pointer). This was a GENERAL bug -- the Vec analog
   `(defn f [v : (Vec int)] ...)` fed by `vec-of` warned identically. **Fix:**
   `emit_expr.c` -- compute `callee_param_is_typed_heap_ptr` (the callee's
   DECLARED full param type is a concrete `:heap` struct) and skip the three
   `concrete->carrier` spill branches when set, so the typed pointer flows in
   unchanged. A genuine `:int`-sink consumer (`some?`/`unwrap-or`, declared
   `o : int`) has a non-heap full param type and is unaffected.

**Validation:**
- The minimal repro and the Vec analog both build with **zero**
  `-Wint-conversion` warnings.
- New fixture `tests/fixtures/mutmap-typed-consumer` exercises a typed
  `(MutableMap int int)` flowing through user fns (`total`/`sum-vals`) -- 0
  warnings, 0 bridge crossings, `mutmap_new__spec__` + `MutableMap__int__int *`
  locals, no `(int64_t)(intptr_t)` relabel.
- A 200k-entry ascribed `.eq?` drops from 2 bridge crossings to **0**.
- Compiled suite **1666 passed, 0 failed**; interpreter **1221 passed, 0
  failed**; suite-wide bridge audit crossing-neutral (60/11) with **zero**
  snapshot drift on the existing fixtures (the change activates only on the new
  typed paths).

The original finding is retained below for the paper trail.

---

## One-line summary

`mutmap-new` is a zero-arg `[K V]` inline-C constructor returning `(MutableMap
K V)`. Under the `:heap` ABI (#377) a concrete `(MutableMap int int)` parameter
lowers to a typed pointer `MutableMap__int__int *`, but `mutmap-new`'s result
does NOT get the matching typed producer spec the way the single-tyvar
`vec-of`/`vec-new` does. So a concrete consumer takes `MutableMap__int__int *`
while the producer hands it an `int64_t` carrier -- an int->pointer mismatch.

## Minimal repro

```turmeric
(defn touch [m : (MutableMap int int)] : int (mutmap-len m))
(defn main [] : int
  (let [a (:: (mutmap-new) (MutableMap int int))]
    (println (touch a))
    (mutmap-free a))
  0)
```

```
$ ./build/tur build repro.tur -o /tmp/repro
/tmp/tur-build/..._tur.c:NNNN: warning: passing argument 1 of 'touch' makes
  pointer from integer without a cast [-Wint-conversion]
$ /tmp/repro
0          # runs correctly -- int64 <-> pointer is bit-compatible here
```

## Observed vs. expected

- **Observed:** `touch` is emitted as `int64_t touch(MutableMap__int__int *m)`
  (the `:heap` concrete-param lowering), but `a` is bound from
  `a = mutmap_hynew()` whose C return type is `int64_t` (the carrier base). The
  call `touch(a)` passes `int64_t` to a `MutableMap__int__int *` parameter ->
  `-Wint-conversion`.
- **Expected (the Vec shape):** `vec-of` / `vec-new` are monomorphized to a
  typed producer spec at a concrete site, so `a` binds a typed-pointer local
  (`Vec__int * a = vec_new__spec__Vec__int__();`) and flows into the typed
  consumer with no cast and no warning. `mutmap-new` should do the same:
  `MutableMap__int__int * a = mutmap_new__spec__MutableMap__int__int__();`.

## Root cause (file:line)

The producer-slice gate that mints typed inline-C producer specs is
`type_is_heap_vec` in `src/compiler/emit_module.c` (used in
`emit_abi_register_call` at the arg gate, the result gate, and the
slot-forcing test). It is deliberately scoped to the single struct
constructor `Vec`. Extending it to `MutableMap` is necessary but **not
sufficient**: even with `MutableMap` added to the predicate, `mutmap-new`'s
result spec does not mint/match because the producer is a **zero-arg,
two-type-param** constructor. The two unconstrained tyvars `K`/`V` are
resolved from the ascription context, and the multi-param resolution records
a type-ctor param as `TY_STRUCT` rather than `TY_TYVAR` in some positions
(the documented `#364` multi-param gap -- see
`docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`,
"Roadblocks for the by-value direction", point 5, and the Map/Set producer
slice plan's "Multi-param caveat"). The single-tyvar `vec-of` does not hit
this because there is exactly one tyvar to resolve.

Empirically: prototyping `type_is_heap_vec` to also accept `MutableMap`
typed the *consumers* (`fill`/`touch`/`mutmap-eq-loop`) to
`MutableMap__int__int *` but left `mutmap-new` on the int64 carrier, turning
the latent warning into a hard `MutableMap__int__int *`-from-`int64_t` build
break in ascribed code. The gate extension was reverted; only the
pure-Turmeric `Eq [MutableMap]` instance (which dispatches through the
carrier base, exactly like `Eq [Vec]`'s abstract path) shipped.

## Why it does not break the suite today

No fixture binds a concrete `(MutableMap int int)` and threads it through a
typed consumer; the mutmap fixtures use abstract `(mutmap-new)` bindings that
stay on the int64 carrier end-to-end (0 bridge crossings, no typed consumer,
no warning). The gap is only reachable from user code that ascribes the
handle and passes it to a `(MutableMap K V)`-typed function.

## Proposed fix direction

1. Close the `#364` multi-param resolution so a zero-arg `[K V]` constructor's
   result type resolves to a concrete `(MutableMap int int)` (both type-ctor
   params recorded as `TY_TYVAR`, resolved from the ascription) at
   `emit_abi_register_call`. This is the shared prerequisite for typing ANY
   multi-param `:heap` producer.
2. Generalize `type_is_heap_vec` to a `type_is_heap_collection` keyed on the
   struct constructor name (`Vec`, `MutableMap`, ...), per the recipe in
   `docs/upcoming/v1/map-set-typed-pointer-producer-slice-plan.md`. The
   element-slot carrier-forcing already in place covers MutableMap's `K`/`V`
   slots; only the producer-result resolution (step 1) is missing.
3. After (1)+(2): `mutmap-new` mints `mutmap_new__spec__MutableMap__int__int__`,
   the concrete consumer/`Eq [MutableMap]` typed spec receives the typed
   pointer with no cast, and the ascribed-dispatch crossings (currently 2 in a
   synthetic ascribed probe) drop to 0 -- the same end state the Vec slice
   reached.

## How to validate a fix

- The minimal repro above builds with **zero** `-Wint-conversion` warnings.
- A synthetic ascribed probe
  (`(:: (mutmap-new) (MutableMap int int))` dispatched through `.eq?`) audits
  **0** `MutableMap` crossings under `TUR_M3_AUDIT=1` (currently 2).
- `bash tests/run.sh` stays green; the mutmap fixtures are unaffected
  (they stay on the carrier path).
- `mutmap-new`'s typed spec appears in the emitted C of the ascribed probe
  (`MutableMap__int__int * a = mutmap_new__spec__...();`).

## Related

- `docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
  -- the multi-param producer gap is roadblock 5 there.
- `docs/upcoming/v1/map-set-typed-pointer-producer-slice-plan.md`
  -- the producer-slice recipe + its "Multi-param caveat".
- `docs/upcoming/tco-in-abi-specs-for-stdlib-iteration.md`
  -- the plan whose MutableMap follow-up surfaced this.
- `src/compiler/emit_module.c` `type_is_heap_vec` -- the gate to generalize.
- `stdlib/mutmap.tur` `mutmap-new` -- the zero-arg `[K V]` producer.
