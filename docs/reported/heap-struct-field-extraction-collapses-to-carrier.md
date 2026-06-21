---
title: Parametric field extraction from a `:heap` struct collapses to the carrier in a constrained instance (Encode/Decode [list]) -- the `:heap` read-side gap #475 did not cover
category: Constrained instance dispatch / carrier ABI -- `:heap` struct field deref
severity: Medium. Blocks `Encode`/`Decode` for lists (`(Cons A)`) -- every element
  dispatches on / reads as its int64 carrier regardless of the element type. Depending
  on call-site specialization it surfaces either as a runtime miscompile (element read
  as the carrier bit pattern) or as a hard C compile error (`(xs)->head` deref of an
  int64 carrier). Concrete extraction works; only the parametric/constrained context
  breaks.
status: OPEN -- verified 2026-06-21 on `tur` at `a876773` (compiler identical to
  `8e8b34f`, post-#475/#476, v0.22.0).
---

# Parametric `:heap` field extraction collapses to the carrier in a constrained instance

## One-line summary

#475's commit notes its emit fix handled **value-struct** field deref (e.g. `Option`'s
`(.value x)`, which now works). A **`:heap`** struct -- `(defstruct Cons :heap [A]
(head A) (tail :int))` (stdlib `list.tur:15`) -- extracted via `(.head xs)` (pointer
deref `(xs)->head`) inside a constrained instance/generic is **not** covered: the
instance body bakes in `__inst_Enc_enc_int` and reads the raw int64 head for every `A`.
Concrete extraction (`(.head c)` / `(thead c)` at a known type) works; only the
parametric context breaks.

This is the `:heap` read-side companion of #475, and a sibling of
`nested-construct-byvalue-in-constrained-instance-body.md` (the *construct* side):
all three are the same carrier-ABI machinery not resolving a generic payload against
the current spec's concrete element type at a carrier boundary.

## Minimal repro

```turmeric
(defmodule headinst (export))
(defclass Enc [a] (enc [x] : cstr))
(definstance Enc [int]  (enc [x] : cstr
  ```c char*b=(char*)malloc(24);snprintf(b,24,"%lld",(long long)x);return b; ```))
(definstance Enc [cstr] (enc [x] : cstr
  ```c const char*s=(const char*)x;size_t n=s?strlen(s):0;char*b=(char*)malloc(n+3);
       b[0]='"';if(s)memcpy(b+1,s,n);b[n+1]='"';b[n+2]=0;return b; ```))

;; Cons is :heap. Extracting the parametric head dispatches to enc_int for EVERY A.
(definstance Enc [Cons]
  [(Enc A)]
  (enc [xs] : cstr (enc (.head xs))))

(defn main [] : int
  (do
    (println (enc @Cons (:: (list 42 0)     (Cons int))))    ;; "42"   (ok)
    (println (enc @Cons (:: (list 7.1 0.0)  (Cons float))))  ;; want "7.1"
    (println (enc @Cons (:: (list "hi" "x") (Cons cstr))))   ;; want "\"hi\""
    0))
```

## Emitted C (the bug)

The `Enc [Cons]` instance body lowers to (carrier base clone, `int64_t xs`):

```c
static const char * __inst_Enc_enc_Cons(int64_t xs) {
    return __inst_Enc_enc_int((xs)->head);   // for Enc[Cons] over EVERY A
}
```

Two defects, both the same carrier collapse:
- **dispatch** is hardcoded to `__inst_Enc_enc_int` regardless of `A` (the receiver
  element type was erased to the int64 carrier, so `emit_reresolve_method_call`'s
  `(.field container)` recovery -- the #475 fix -- never fires for a `:heap` deref); and
- **the read** `(xs)->head` treats the head as the int64 carrier.

No per-element `__inst_Enc_enc_Cons__spec__*` clone is generated.

### Two observed manifestations (call-site dependent)

1. **Runtime miscompile** (reported originally): when a spec clone is minted that casts
   `xs` to the concrete `Cons__int *`, the body compiles but still dispatches `enc_int`
   and reads `->head` at the carrier -- the float case prints `4619679907765970534`
   (the bit pattern of `7.1`), the cstr case prints a pointer value.
2. **Hard C compile error** (observed on `a876773` with the self-contained repro
   above, which routes through the base clone): `(xs)->head` is a deref of the `int64_t`
   carrier --
   ```
   error: invalid type argument of '->' (have 'int64_t')
       return __inst_Enc_enc_int((xs)->head);
   ```
   i.e. the same collapse, surfacing at the field deref instead of at runtime.

## Control -- concrete extraction works

```turmeric
(let [c (:: (list 7.1 2.5) (Cons float))]
  (pf (.head c))     ; => 7.1   (direct field access, concrete type)
  (pf (thead c)))    ; => 7.1
```

So the gap is specific to extracting a type-`A` field from a `:heap` `(Cons A)`
**inside a constrained generic** (instance body or constrained `defn`).

## Two secondary frictions (for whoever designs the list instance surface)

These are surface-design choices, not the compiler bug, but they shape how a list
typeclass instance can be written:

1. **Dispatch ambiguity.** A `(Cons A)` is a heap pointer carried as int64 -- the same
   carrier as `int` -- so `(enc xs)` is "ambiguous method dispatch: matches Enc[Cons],
   Enc[int]" with the receiver erased. Requires `@Cons` (or `@TypeName`) at the call
   site.
2. **Empty list is `int`.** `tnil` is `(defn tnil [] : int 0)` (stdlib `list.tur:66`),
   so the empty list is the bare `:int` carrier `0`, not a `(Cons A)`. One `Enc [Cons]`
   instance can't uniformly handle empty vs non-empty (empty would dispatch to
   `Enc[int]` -> `"0"`, not `"[]"`).

## Fix direction

Extend the #475 emit fix (recover the concrete element type from the spec's
`arg_types[]` for a `.field` receiver) to cover **`:heap`** struct field derefs
(`(xs)->head`), not only by-value struct field reads:

- In `emit_reresolve_method_call` (`src/compiler/emit_core.c`, the
  `(.field container)` recovery block added by #475), handle a `:heap` container
  receiver: cast the carrier to the concrete `Cons__<A> *` and dispatch on the
  recovered element type.
- In `emit_expr.c` (the field-deref lowering), read `(xs)->head` with the element
  ABI -- i.e. spec-resolve the `:heap` struct receiver and the field's `full_type`,
  the heap analogue of the value-struct deref the #475 `emit_value` change already
  fixed.

(The two secondary items are surface-design choices -- likely a `List`/`Cons` wrapper
that carries non-int, or an `encode-list` that takes `(Cons A)` plus an explicit empty
case.)

## Scope

Compiler-side (emit specialization), same machinery as #475 and
`nested-construct-byvalue-in-constrained-instance-body.md`. Unblocks list serialization
in `turmeric-spices/spices/json` (Track C / U2). `Encode [Option]` already works
post-#475.
</content>
