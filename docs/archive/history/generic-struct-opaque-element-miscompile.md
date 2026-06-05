---
title: Generic structs with an opaque (or struct) element miscompile
category: Reported
description: A parametric struct such as `Pair<A B>` instantiated with an *opaque* element type (or any non-primitive aggregate element) is not monomorphized -- the compiler emits the generic carrier template `(int64_t){.fst = a, .snd = b}`, which is invalid C, and never creates the `Pair__int__Foo` specialization. The same template is reached when a *generic function* tries to return a parametric aggregate whose element type is a phantom type variable that is only recoverable from an opaque argument.
---

# Generic structs with an opaque element miscompile

> **Severity:** silent-miscompile -> hard cc error (the emitted C does not
> compile; never reaches codegen-correct output). Blocks any use of stdlib
> `Pair` / parametric structs whose element is an opaque newtype.
> **Found:** 2026-06-04, executing
> [stdlib-session-typed-channels-plan](stdlib-session-typed-channels-plan.md)
> phase S1 (recv was specified to return `Pair<T SChan<R>>`). The plan has
> since shipped; the canonical user-facing reference is now
> [session-types-guide](../../guides/session-types-guide.md).
> **Status:** Variant 1 (concrete, no generics) **FIXED** 2026-06-05 and
> Variant 2 (phantom element through a generic-from-generic relay) **FIXED**
> 2026-06-05 -- see [Resolution](#resolution).

## Summary

Constructing or accessing a stdlib `Pair` (or any parametric struct) whose
element type is an *opaque* newtype emits the generic carrier template instead
of a monomorphized specialization. The template is invalid C.

## Minimal repro

```turmeric
(defopaque Foo :ptr<void>)
(defn nullp [] : ptr<void> ```c return (void*)0; ```)
(defn main [] : int
  (let [h (:: (unsafe (nullp)) :Foo)
        p (pair 5 h)]              ;; Pair<int Foo>
    (println (pair-fst p))
    0))
```

### Observed

`tur build` emits, then `cc` rejects:

```
error: field name not in record or union initializer      // (int64_t){.fst = a, .snd = b}
error: request for member 'fst' in something not a structure or union
```

The generic `pair` / `pair_fst` are emitted as:

```c
static int64_t pair(int64_t a, int64_t b) { return (int64_t){.fst = a, .snd = b}; }
static int64_t pair_fst(int64_t p)        { return (p).fst; }
```

i.e. the *carrier* int64_t is treated as if it were the struct. No
`pair__spec__Pair__int__Foo` clone is produced, so the broken generic template
is the one that gets called.

### Expected

`(Pair int Foo)` has the same C layout as `(Pair int int)` (an opaque lowers to
the int64_t carrier), so it should monomorphize to a `Pair__int__Foo` (or reuse
`Pair__int__int`) and print `5`. A plain `(pair 5 6)` *does* work -- only the
opaque element trips it up.

## Variants

1. **Concrete, no generics** -- the repro above.
2. **Through a generic function** -- a generic fn returning a parametric
   aggregate whose element is a phantom type variable that is only present
   inside an opaque argument:

   ```turmeric
   (defopaque SChan [P] :ptr<void>)
   (defn recv [T R] [c : (SChan (SRecv T R))] : (Pair T ptr<void>)
     (:: (make-struct Pair (val-of c) (raw-of c)) (Pair T ptr<void>)))
   ```

   Here `T` cannot be recovered from the opaque `SChan` argument, so the result
   `(Pair T ptr<void>)` never specializes and the generic carrier template
   (broken) is emitted for `recv`. `make-struct` *does* specialize correctly
   when the element types are bare type variables bound directly by an argument
   (e.g. `(defn wrap [A B] [a :A b :B] : (Pair A B) (make-struct Pair a b))`),
   which is why the failure is easy to miss.

## Root cause (analysis)

The ABI-specialization gate in `emit_abi_scan_call`
(`src/compiler/emit_module.c`, around the `abi_changes` computation, ~lines
617-678) decides to create a specialization only when
`type_c_name(generic) != type_c_name(instantiated)` for some arg or the result.
For a struct-app argument/result whose element is an opaque, the instantiated
`type_c_name` appears to collapse to the carrier (so `abi_changes` stays false),
and no spec is interned -- the call falls back to the generic template emitted by
the struct-constructor codegen, which uses the carrier-int64_t struct-literal
form `(int64_t){.fst = ...}` (see the generic `pair` body in the emitted C).
For variant 2, the type variable `T` is not recovered by
`emit_abi_instantiate_type` because it is buried inside the opaque arg, so the
result type is never made concrete.

The mangling itself is fine: `append_type_mangle`
(`src/compiler/types.c:433`) already prints an opaque `TY_STRUCT` as its name
(so `Pair__int__Foo` is a valid name) -- the gap is upstream, in deciding to
specialize and in recovering the element type.

## Impact / workaround

- `stdlib/schan.tur` recv was respecified to return the typed continuation
  `(SChan R)` directly and deliver the received value through a caller-provided
  cell, avoiding any parametric aggregate with an opaque/phantom element. This
  keeps the continuation protocol fully type-checked. See `schan-recv`.
- General workaround: keep parametric-struct elements to primitive types
  (`int`, `ptr<void>`, ...) or to bare type variables bound directly by an
  argument; do not place an opaque newtype (or another struct) in a `Pair` /
  parametric struct that flows through generic code.

## Proposed fix directions

1. In `emit_abi_scan_call`, treat a struct-app whose element resolves to a
   carrier-ABI opaque as a distinct instantiation (intern a spec keyed by the
   element's nominal name, reusing the carrier layout), so `Pair__int__Foo` is
   emitted and called.
2. Make the generic struct-constructor template valid for the carrier ABI
   (box/heap-allocate the aggregate and return a pointer-as-int64_t, with the
   accessors dereferencing) so that even the unspecialized fallback compiles.
3. Recover phantom element types in `emit_abi_instantiate_type` when they appear
   only inside an opaque argument (needed for the generic-function variant).

## Validation

`(pair 5 (:: (unsafe (nullp)) :Foo))` followed by `(pair-fst ...)` should build
and print `5`. A generic `recv` returning `(Pair T (SChan R))` should build and
round-trip a value plus a typed continuation.

## Resolution

**Variant 1 fixed** in `src/compiler/types.c`. The root cause was exactly as
analyzed: `type_has_concrete_codegen_layout` (the element-recursion predicate
that, via `type_c_name(TY_APP)`, gates `register_struct_app` and hence the
ABI-specialization decision in `emit_abi_scan_call`) rejected an *opaque*
struct element:

```c
case TY_STRUCT:
    return t->as.struct_.def && !t->as.struct_.def->is_opaque &&  /* <- bug */
           t->as.struct_.def->n_type_params == 0;
```

An opaque newtype lowers to the `int64_t` carrier -- a perfectly concrete,
monomorphizable field layout -- so as an *element* of a parametric struct it
should contribute a well-defined `int64_t` field. Dropping the `!is_opaque`
guard makes `Pair<int Foo>` concrete, so `type_c_name` mangles it to
`Pair__int__Foo` (distinct from the generic carrier `int64_t`), `abi_changes`
becomes true, and the `pair`/`pair-fst` specializations are interned and called
instead of the broken `(int64_t){.fst = ...}` template. An opaque struct never
reaches this gate as a by-value *head* (`type_c_name` lowers it to `int64_t`
directly, so it is never `register_struct_app`'d), so the fix is element-local.

Regression coverage: `tests/fixtures/typed/pair-opaque-element/` exercises
`Pair<int Tag>` and `Pair<Tag int>` (opaque element in each slot) through
`pair-fst`/`pair-snd`. Full suite: `1450 passed, 0 failed`.

**Variant 2 (phantom element via a generic function) FIXED 2026-06-05.** The
diagnosis above was incomplete. The element type variable is *not* unrecoverable
in general: when `recv [T R] [c : (SChan (SRecv T R))]` is called *directly* with
a concrete argument type (`(SChan (SRecv int ...))`), elab already attaches a
concrete `abi_bindings` substitution and `recv` monomorphizes correctly. The
real gap was a **generic-from-generic relay**: a forwarder

```turmeric
(defn fwd [T R] [c : (SChan (SRecv T R))] : (Pair T ptr<void>)
  (recv c))   ;; T,R here are *fwd's* tyvars, still abstract
```

When `fwd` is specialized at a concrete leaf, the inner `recv` call's
`abi_bindings` (captured at elab time) map `recv`'s tyvars to *fwd's* tyvars,
which are still abstract. The ABI scan never composed those through `fwd`'s
concrete bindings, so the inner call (a) was carrier-noted -- forcing the broken
generic `recv` carrier body `(int64_t){.fst = ...}` to be emitted -- and (b) was
never re-specialized, so `fwd__spec` called the broken generic `recv` instead of
a `recv__spec`. The C compiler then rejected the struct-vs-int64 mismatch (the
exact hard error in this report).

Fixed in `src/compiler/emit_module.c`:

1. **Binding composition.** When `emit_abi_register_call` runs inside an active
   specialization, it now instantiates each inner-call binding's type *and* the
   call's result type (`call->type`, still expressed in the enclosing generic's
   tyvars) through the active spec's concrete bindings. So the relayed `recv`
   call specializes to `recv__spec__Pair__int__ptr_void` and `fwd__spec` calls
   it directly.
2. **Relay carrier-note suppression.** A new `emit_abi_call_is_generic_relay`
   predicate suppresses the spurious carrier note for a relay call (abstract
   bindings, inside a *generic* body, to a generic-unsafe callee), so the broken
   generic carrier body is no longer emitted. It is scoped by `ctx->current_scan_fn`
   so a top-level call with genuinely-unresolvable phantom tyvars
   (`map_count(map_new())`, KB-022) keeps its carrier fallback.

Regression coverage: `tests/fixtures/generic-relay-aggregate-result/`
(a `[T R]` forwarder relaying to a `[T R]` `recv` that returns
`(Pair T ptr<void>)`, prints `7`). Full suite: `1476 passed, 0 failed`.
