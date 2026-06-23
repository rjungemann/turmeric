---
title: Constrained generic `defn` returning a parametric container is not monomorphized (blocked json list decode)
category: Constrained generic monomorphization / carrier ABI -- emit-time specialization
severity: Medium. A length-1 / garbage list or segfault (the int case), plus
  `Cons__int`-vs-`Cons__A` codegen warnings and a wrong head ABI for cstr/float
  elements. Blocks `Decode [Cons]` (JSON array -> `(Cons A)`) in turmeric-spices'
  json spice -- the mirror of the shipped `Encode [Cons]`.
status: RESOLVED 2026-06-21 -- carrier double-box fixed + spec-body element
  recovery; round-trip fixture `tests/fixtures/constrained-defn-cons-return-
  monomorphize` covers int / cstr / float, direct and through a constrained
  wrapper; `bash tests/run.sh` is green (1735 passed, 0 failed).
---

# Constrained, self-recursive `defn` returning `(Cons A)` miscompiles

## One-line summary

A constrained generic `defn` whose **return type is a parametric heap
container** `(Cons A)` -- decode each element via a `(Decode A)`-shaped class,
`tcons-of` it onto the recursive tail -- came back as a length-1 list with a
garbage head (or segfaulted), and for `cstr`/`float` elements the per-`A` spec
body kept building `Cons__int` internally. Same family as #475/#479/#480
(carrier vs concrete ABI in constrained generics), but on the *return /
recursion* side.

## Minimal repro (self-contained, no yyjson)

```turmeric
(defmodule xgen (export))
(defclass C [a] (one [i : int] : (Result a cstr)))
(definstance C [int] (one [i] (ok i)))

;; self-recursive constrained walk that builds a (Cons A)
(defn rec [A] [(C A)] [i : int n : int] : (Cons A)
  (if (>= i n) (:: (tnil) (Cons A))
    (tcons-of (ok-val (:: (one i) (Result A cstr)))
              (:: (rec (+ i 1) n) :int))))

;; a constrained generic that WRAPS the walk (e.g. a public decode-list)
(defn wrap [A] [(C A)] [n : int] : (Cons A) (rec 0 n))

(defn main [] : int
  (println (list-length (:: (rec 0 3) :int)))   ;; expected 3, got 1
  (println (list-length (:: (wrap 3) :int)))    ;; expected 3, got 1
  0)
```

## Root cause

Three distinct emit-time gaps, all in how a `:heap` parametric container flows
through the carrier ABI of a constrained generic body:

1. **Carrier double-box of a `:heap` return (`emit_fns.c`).** A `(Cons A)` is a
   `:heap` struct -- represented as a `Cons__A *` *pointer* that already fits the
   int64 carrier. The carrier-base / carrier-return path heap-spilled it
   (`{ Cons__A * *p = malloc(sizeof(Cons__A *)); *p = v; return (int64_t)p; }`),
   double-boxing the pointer into a `Cons__A **`. The consumer (`list-length`,
   `tcons-of`'s `tail`) then read a pointer-to-pointer as the head cell -> a
   length-1 / garbage list. This is the heap-struct mirror of the existing M7
   "there is nothing to box" double-box guard.

2. **Spec-body inner calls keep the carrier element (`emit_module.c`).** The
   `(Cons cstr)` / `(Cons float)` spec of `rec` was emitted with the right
   signature, but its body's inner `tcons-of`, self-`rec`, and the wrapper's
   forwarding `rec` call all stayed on `Cons__int`: elab left those calls either
   with NO abi_bindings (return-only-polymorphic self-call / wrapper forward) or
   with the element tyvar's NAME intact but its TYPE collapsed to the int64
   carrier (`tcons-of`'s head came through the bare-tyvar accessor `ok-val`).
   The enclosing spec already knows the element (it dispatches `one` to
   `__inst_C_one_cstr`), so the fix recovers it from the active spec: (a) family
   recovery -- when the callee's declared result is the SAME parametric family
   the spec returns (`(Cons A)` vs `(Cons cstr)`) and the call has no bindings,
   synthesize `{A -> cstr}` from the spec's result element; (b) by-name
   re-hydration -- when a binding's type collapsed to the carrier but its name
   survives, restore the concrete type from the active spec's binding of that
   name. The recovered result is then re-derived from the callee's declared
   result through those bindings (so the spec returns `Cons__cstr *`).

3. **cstr/pointer head arg uncast at the construct seam (`emit_expr.c`).** With
   the `tcons-of` head param recovered to `const char *`, the head arg
   `ok_hyval(__inst_C_one_cstr(i))` is still the int64 carrier. The float seam
   already reinterpreted via a union bridge; the pointer-carried (`cstr`) seam
   had no cast (`-Wint-conversion`). Added a sibling branch that emits a plain
   `(const char *)(intptr_t)` reinterpret (NOT the heap-struct deref
   `emit_carrier_bridge` would emit for an aggregate, which would deref the
   string as a pointer-to-pointer).

## Why `int` "limped"

When `A = int` the carrier IS the value, so the mis-resolution to
`Cons__int`/`int` is accidentally what the caller wants -- except for gap 1's
double-box, which corrupts the list regardless of element type (hence the
reported `1` instead of `3`). cstr happens to share the carrier's pointer width
(so it ran but warned); float needs the bit reinterpret.

## Resolution (2026-06-21)

- **Gap 1** -- `emit_fns.c`: both carrier-return spill sites now cast a
  `type_is_heap_struct` tail (`Cons__A *`) to the carrier instead of
  malloc-boxing it.
- **Gap 2** -- `emit_module.c` `emit_abi_register_call`: recover the element of
  an inner generic call from the enclosing spec's parametric result (family
  recovery for no-binding calls; by-name re-hydration for carrier-collapsed
  bindings), and re-derive the concrete result from the callee's declared result
  through the recovered bindings.
- **Gap 3** -- `emit_expr.c`: a concrete pointer-carried (`cstr`) spec param fed
  a carrier accessor result gets a plain `(T)(intptr_t)` reinterpret at the seam.

Fixture: `tests/fixtures/constrained-defn-cons-return-monomorphize` -- a
constrained `defn` returning `(Cons A)` built with `tcons-of`, asserting
`list-length` / `thead` over int / cstr / float elements, both called directly
and through a one-level constrained wrapper.

## Scope / impact

Compiler-side (emit monomorphization). Unblocks `decode-list` / `Decode [Cons]`
in `turmeric-spices/spices/json` (the `Decode [a] -> (Result a cstr)` class with
the element only in the return is the exact shape modeled by the fixture's `C`
class). Verified on `tur` built from this branch; closely related to
#475/#479/#480, same machinery, the return/recursion direction.
