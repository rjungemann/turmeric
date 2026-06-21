---
title: Constrained instance body dispatching a class method on a NESTED parametric element bakes in the inner instance's carrier signature (silent miscompile for by-value/float elements)
category: Carrier <-> Concrete ABI -- method dispatch re-resolution, recursive (nested-container) case
severity: Medium-high. SILENT miscompile, not a hard error. A constrained
  instance whose element is itself a parametric container -- `Enc [Option]`
  applied to `(Option (Cons A))`, so the inner `(.value x)` is a `(Cons A)` --
  dispatches the inner `enc` through the GENERIC carrier shim
  `__inst_Enc_enc_Cons(int64_t)` instead of a per-instantiation spec. The `int`
  element prints correctly by luck (pointer width aligns); `float` prints a
  double's bit pattern reinterpreted as int64 (denormal-class garbage); a
  by-value-struct element would fail to compile. Blocks any nested-container
  Encode/Decode (the encode/read sibling of #480's nested-construct fix).
status: RESOLVED 2026-06-21 (gap G2). Fixed on
  branch claude/g2-carrier-concrete-abi-audit-3yzkhm; fixture
  tests/fixtures/constrained-instance-dispatch-nested-parametric-element. See
  "Resolution" below.
---

## Resolution (2026-06-21)

Fixed. The dispatch-type chokepoint `emit_reresolve_disp_type`
(`src/compiler/emit_core.c`) already recovered the concrete receiver
`(Cons B)`; what was missing was (1) the FnDef lookup matching a bare
type-constructor instance head (`Enc [Cons]`) against the applied `(Cons B)`,
and (2) minting + routing to the inner instance's by-value spec.

Changes:

- `emit_inst_head_matches` (`emit_core.c`): a bare type-constructor instance
  head (`Cons`, with type params) now matches any concrete application
  `(Cons X)`, mirroring the `__inst_<Class>_<method>__<Head>` name the
  suffix-reconstruction fallback already builds, so the FnDef lookup agrees with
  the name resolver.
- `emit_abi_try_nested_instance_dispatch_redirect` (`emit_module.c`): when an
  active constrained-instance spec dispatches a class method whose recovered
  receiver is a concrete parametric container, mint the inner instance method's
  per-instantiation by-value spec (`__inst_Enc_enc_Cons__spec__..._Cons__int`,
  taking `Cons__int *`), record the call->spec mapping, and recurse into the
  spec body (so its own `(.head xs)` dispatch is scanned). The single-level
  `@Cons` witness path mints the identical spec; `emit_abi_intern_spec` dedupes.
- `emit_call_name` (`emit_core.c`): prefer a recorded nested-dispatch spec over
  the carrier-base re-resolution when one exists for the call under the active
  outer.
- `emit_reresolve_disp_type` (`emit_core.c`): free the owned `(Cons int)` clone
  that the field-substitution branch produced (a pre-existing leak that only
  surfaced once the recovered element was a TY_APP, as in this nested case) so
  the leak-checked codegen path stays clean.

The float line now prints `7.1`; int/float/cstr all round-trip. Suite green
(1740/0).

**Distinct, still-open mirror:** the OTHER nesting `(Cons (Option A))` --
encoding a list whose elements are themselves a parametric container, dispatched
inside the `Enc [Cons]` body -- is a separate pre-existing gap (the single-level
`@Cons` witness collapses the `(Option int)` element to `int`, then dispatches
the inner `enc` on the carrier). It is verified pre-existing (fails identically
without this fix) and tracked as gap G9 in the audit
(`docs/reported/constrained-instance-dispatch-parametric-container-element-collapse.md`).

---

# Nested-parametric-element dispatch collapses to the inner instance's carrier signature

## One-line summary

In the body of `(definstance Enc [Option] [(Enc A)] (enc (.value x)))`, when
the concrete element `A` is itself a parametric container `(Cons B)`, the inner
`(enc (.value x))` resolves to the **generic carrier** shim
`__inst_Enc_enc_Cons(int64_t)` rather than a per-instantiation spec. The
extracted `(.value x)` is emitted as the concrete `Cons__B *`, so source
(pointer) and sink (carrier int64) disagree: the `int` case survives by luck,
the `float` case is a silent bit-reinterpret miscompile.

## Minimal repro (self-contained)

```turmeric
(defclass Enc [a] (enc [x] : cstr))
(definstance Enc [int]
  (enc [x] : cstr
    ```c
    char *buf=(char*)malloc(32); snprintf(buf,32,"%lld",(long long)x); return buf;
    ```))
(definstance Enc [float]
  (enc [x] : cstr
    ```c
    char *buf=(char*)malloc(32); snprintf(buf,32,"%g",x); return buf;
    ```))
(definstance Enc [Cons]
  [(Enc A)]
  (enc [xs] : cstr (enc (.head xs))))
(definstance Enc [Option]
  [(Enc A)]
  (enc [x] : cstr (if (.is-some x) (enc (.value x)) "null")))

(defn main [] : int
  (do
    ;; (Option (Cons int)) : outer Option (by-value) -> inner Cons (heap) -> int
    (println (enc @Option (:: (some (:: (list 42 7) (Cons int))) (Option (Cons int)))))
    ;; FLOAT is where the silent miscompile shows:
    (println (enc @Option (:: (some (:: (list 7.1 2.5) (Cons float))) (Option (Cons float)))))
    0))
```

Result (`tur run`):

```
warning: passing argument 1 of '__inst_Enc_enc_Cons' makes integer from pointer without a cast
note: expected 'int64_t' but argument is of type 'Cons__float *'
   __t46 = __inst_Enc_enc_Cons((x).value);

stdout:
42
4619679907765970534         <- want "7.1" (IEEE-754 bits of a double, read as int64)
```

## Root cause (direction)

Site 8 of the crossing audit -- the dispatch re-resolver
`emit_reresolve_disp_type` (`src/compiler/emit_core.c:1141`) -- handles a
method receiver whose recovered element type is a scalar, a value-struct, or a
single-level `:heap` container (that is what #475/#479/#480 closed). It does
**not** recurse: when the recovered receiver type is *itself* a parametric
container (`(Cons float)`), it must mint / select the per-instantiation inner
spec (`__inst_Enc_enc_Cons__spec__Cons__float`, which takes `Cons__float *`)
instead of leaving the call pinned to the generic carrier shim
`__inst_Enc_enc_Cons(int64_t)`.

This is the *dispatch-on-nested-element* (encode/read direction) companion of
#480, which fixed the *construct-of-nested-element* (decode/write direction).
Same carrier<->concrete machinery, recursive (nested) case.

## Related

`docs/reported/instance-method-byvalue-struct-field-receiver-abi-mismatch.md`
(filed independently on `main`, #482 era) is the **single-level** form of this
same defect: an instance whose head is a by-value applied struct
(`Enc [(Option cstr)]`) takes the carrier parameter while a by-value
struct-field receiver passes the aggregate. That report and this one are the
non-nested and nested faces of one dispatch-ABI fix; they should close together
(see gap G2/G3 and phase P2 in
`docs/carrier-concrete-abi-crossing-audit-plan.md`). Verified still-open against
a fresh `origin/main` build on 2026-06-21.

## Fix directions

In `emit_reresolve_disp_type` (and its scan-time twin
`emit_reresolve_method_fndef`, `emit_core.c:1308`): after recovering the
receiver's concrete type via `emit_var_spec_arg_type`, if that type is a
parametric container, recurse to select the inner instance's per-instantiation
spec rather than the carrier base. The value-struct payload path already routes
field reads through the spec arg type (site 6, #475); the dispatch path needs
the same recursion so the *callee* matches the by-value receiver the field read
now produces.

## Validation

When fixed, the float line prints `7.1` and the `(Option (Cons A))` cell of the
stress matrix (int/cstr/float) can be promoted to a fixture. Cross-reference:
gap G2 in `docs/carrier-concrete-abi-crossing-audit-plan.md`.
