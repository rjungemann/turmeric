# Generic HKT instance body mixes carrier and by-value `Option__opaque` specs

**Severity:** low (force-lower / `--enable=defstruct-as-defadt`; the hand-written
stdlib `Applicative`/`Monad [Option]` instance bodies). Default path unaffected.

## Summary

Under the lowering, the autoloaded stdlib `Option` becomes a record ADT and its
hand-written `Applicative`/`Monad` instance methods are emitted once with the
uniform carrier ABI (`int64_t` params). The GENERIC instance body, however,
operates on an element type that is the class's own type variable applied to a
function -- `ff : F (a -> b)` = `Option (fn a b)` -- so `type_c_name` mints the
by-value monomorph `Option__opaque` (its value field is the `void *` opaque
carrier). The body then mixes the two representations and fails at `cc`.

`tests/fixtures/hkt-stdlib-option-result-instances` (force-lower / flag-on), two
distinct crossings:

1. **Carrier param into a by-value spec (consuming direction).**
   ```c
   static int64_t __inst_Applicative_ap_Option(int64_t ff, int64_t fa) {
       if (some___spec__bool_tur_adt_Option__opaque(ff)) {   /* ff is int64 */
   ```
   `some?` on `ff` specialized to the by-value `Option__opaque` predicate spec
   (the generic element is a fn -> by-value-eligible), but `ff` arrives as the
   int64 carrier. The sibling `some?(fa)` correctly used the carrier base
   `some_qu` because `fa`'s element is a bare tyvar (carrier). The spec call
   needs a carrier->by-value unbox (`*(tur_adt_Option__opaque *)(intptr_t)ff`),
   OR the selection should fall back to the carrier base for an opaque/fn
   element (which is ABI-identical to the carrier -- the by-value spec buys
   nothing).
   `error: incompatible type for argument 1 of 'some___spec__bool_tur_adt_Option__opaque'`.

2. **By-value aggregate return treated as a carrier pointer (producing
   direction).**
   ```c
   tur_option_t *__t74 = (tur_option_t *)(intptr_t)(
       ((tur_adt_Option__int (*)(void*, int64_t))k.fn)(k.env, (int64_t)(ma).value));
   ```
   The continuation `k` is typed to return the by-value aggregate
   `tur_adt_Option__int`, but the `Monad bind` body casts that result through
   `(intptr_t)` to a `tur_option_t *` carrier pointer.
   `error: aggregate value used where an integer was expected`. The aggregate
   return must be spilled to obtain its address, or `k`'s return ABI kept on the
   carrier in this generic body.

## Root cause

The hand-written stdlib `Option` instance bodies assume the carrier
representation throughout (`tur_option_t *`, int64 handles), which the plan's
seam-2/seam-4 notes already flag as "should be reconciled or retired when the
gate comes off." Lowering makes `Option` a record ADT, so a generic element that
is a function specializes to the `Option__opaque` by-value monomorph, and the
carrier-assuming body straddles the two representations at the `some?` spec call
(consuming) and the `k` continuation result (producing).

The fn-element CONSTRUCT direction (minting `some__spec__...opaque`) is fixed:
`emit_abi_register_call` normalizes a `TY_FN` construct element to the opaque
`void *` carrier, and the `none` default emits `(void *){0}` for a `TY_FN`-typed
field (cleared `hkt-ap-fn-in-container`; canary
`conv-defstruct-option-fn-element`). The remaining two crossings are in the
INSTANCE-METHOD body emit (dispatch/spec hot path), independent of the construct
direction.

## Affected fixtures (force-lower / flag-on)

- `hkt-stdlib-option-result-instances`.

## Status

Open. The last force-lower build blocker. Reproduce with
`./build/tur build --enable=defstruct-as-defadt --allow-experimental
tests/fixtures/hkt-stdlib-option-result-instances/input.tur`, or restore the
temporary `getenv("TUR_FORCE_LOWER")` bypass at the top of
`defstruct_lowers_to_adt` (`elab_structs.c`). The fix belongs in the
instance-method body emit -- either a carrier<->by-value bridge at the spec-call
arg / continuation-return sites, or making an opaque/fn element resolve to the
carrier base in a carrier-ABI instance body (an opaque monomorph is
ABI-identical to the carrier, so the by-value spec is pure overhead there).
