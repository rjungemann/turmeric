# Fix: by-value lowering for `option`/`result` over a nested monomorph

Resolves
[byvalue-adt-app-rejects-nested-monomorphs](../byvalue-adt-app-rejects-nested-monomorphs.md).
Filed and fixed 2026-08-22, in the session that scoped
[multi-variant-adts-always-heap-allocate](../../reported/multi-variant-adts-always-heap-allocate.md).

## Result

`option<vec<int>>`, `result<vec<int>, cstr>`, `option<(Q int)>` and
`option<option<int>>` all lower by value. One `malloc` per construction gone on
shapes that appear throughout ordinary code.

```c
/* before */ static int64_t                       ctor_Option__Q__int(bool, tur_adt_Q__int);
/* after  */ static tur_adt_Option__Q__int        ctor_Option__Q__int(bool, tur_adt_Q__int);
```

Suite: **2696 passed, 0 failed** -- the 2695 baseline plus the new fixture.
Snapshot drift: **zero**. No existing `expected.c` contained one of these
shapes, which is also why nothing caught the bug.

## The change, and why it is four edits and not one

Four predicates had to move together. Each one alone leaves the compiler
internally inconsistent, and the failure it produces names a *different*
subsystem than the one at fault -- which is what made this worth writing down.

1. **`adt_app_is_byvalue_product`** (`types.c`) -- representation. Admit a
   nested by-value ADT-app argument for any outer, not only a `:heap` one.
   *Alone: 8 fixtures fail to compile.*

2. **`type_app_is_concrete_adt`** (`types.c`) -- ctor-name selection. It
   carried the identical `def->is_heap` guard, deliberately, and its own
   comment said so. Widening (1) without (2) makes a spec return the by-value
   aggregate while its body still calls the carrier `ctor_Option`:
   `error: incompatible types when returning type 'int64_t' but
   'tur_adt_Option__Vec__int' was expected`. *8 -> 1 failure.*

3. **`recovered_byvalue`** in `emit_abi_register_call` (`emit_module.c`) --
   the return-only-poly accessor's result recovery. It gated on
   `type_has_concrete_codegen_layout`, whose `TY_APP` arm is an unconditional
   `return false`, so the ADT-app half of its own comment had never fired. It
   was masked: a nested-monomorph app was not by-value either, so accessor and
   field agreed on the int64 carrier. Added `type_is_byvalue_adt_product` as
   the app-aware alternative.

4. **The heap-container-insert bridge** (`emit_expr.c`) -- `vec-push!`'s
   inline-C `val : A` slot. With (3) in place `(ok-val r)` returns
   `tur_adt_Option__int` into an `int64_t` parameter, and no bridge fires
   because both reporters (`expr_emits_byvalue_carrier_abi`'s `EX_CALL` arm,
   `fn_body_tail_byvalue_carrier_type_inner`'s) gate on
   `type_uses_carrier_abi`, which answers **false** for a by-value app -- it is
   a concrete aggregate, not a carrier. Added `call_spec_result_byvalue_app`,
   used for the guard and the bridge type.

## The wrong turn in step 4, kept because it is the instructive one

The first attempt at (4) taught `expr_emits_byvalue_carrier_abi`'s `EX_CALL`
arm to report by-value apps directly. That is the tidier-looking fix and it is
wrong: it fires the escaping bridge at *every* carrier sink, and **broke 7
fixtures that had been passing** (`list-length-byvalue-aggregate-element`,
`vec-push-byvalue-aggregate-escapes-frame`, ...) by heap-promoting arguments
that already reached their slot correctly through the CONV-S1 boxing block
further down. The emitted tell was a temp declared with one type and
initialised with another:

```c
tur_adt_Option__int __t164 = (int64_t)(intptr_t)(__t163);
```

What separates the two cases is not the value's representation but **whose
type is collapsed**. A direct `(some 42)` carries its real type
(`e->type.kind == TY_APP`); a return-only-poly accessor's was collapsed to the
int64 scalar at elab (`TY_INT`). Only the second needs this bridge, so
`call_spec_result_byvalue_app` requires `e->type.kind == TY_INT` and the helper
stays local to the one sink.

Going 8 -> 1 -> 7 -> 0 is the shape of the lesson: in this ABI machine a fix
that generalises a shared predicate is usually a fix in the wrong place.

## Scope note

`ctor_Option__Zipper__struct` still mallocs and is correct to. Its argument's
own element erased to a non-concrete type, so it is the same
genuinely-unresolved category as the un-monomorphised base `ctor_Option`, and
it is dead in any program that does not instantiate it.

## Regression coverage

`tests/fixtures/byvalue-option-over-parametric-monomorph` -- four shapes
(non-parametric struct argument, parametric monomorph argument, `:heap`
container argument, doubly nested), with an `expected.c` snapshot pinning the
representation. A runtime-only fixture would not have caught this: on the
carrier the program prints the same numbers.

**Mutation-verified.** Reverting only the `types.c` widening makes this fixture
-- and no other, out of 2696 -- fail.
