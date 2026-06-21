---
title: Nested generic carrier construction in a constrained instance body miscompiles for pointer-carried elements -- Decode [Option] (the decode-side companion of #475)
category: Constrained instance dispatch / carrier ABI -- emit-time specialization
severity: Medium. Not a silent miscompile in the common case -- the direct form
  fails to *compile* (carrier int vs. struct). A helper-delegated form compiles
  but then silently returns `none` for a pointer-carried element. Either way it
  blocks `Decode [Option]` (and `Decode` for any container) in turmeric-spices'
  json spice: optional/contained fields can be encoded (Encode [Option] shipped,
  unblocked by #475) but not decoded.
status: OPEN -- root-caused 2026-06-21 (see "Root cause, confirmed" below). The
  fix is structural (four distinct carrier boundaries inside one spec body,
  M4/M7-class) and was NOT attempted in the root-cause session to avoid a
  half-landed change against the 1442-fixture gate.
---

# Constrained instance body can't build a nested `(Result (Option A) cstr)` for a pointer-carried `A`

## One-line summary

`#475` fixed dispatching a class method on an element *read out of* a container
(`(enc (.value x))`) inside a constrained instance body. The mirror direction --
*constructing* a nested generic value (`(ok (some <decoded A>))` :
`(Result (Option A) cstr)`) inside a constrained instance body -- still lowers
the inner `(Result A cstr)` / `(Option A)` as the int64 carrier instead of the
concrete struct, so the build (and the matching `ok-val` destructure) gets a
carrier/struct ABI mismatch. Works when `A`'s carrier *is* its value (`int`);
miscompiles for `cstr` / `float` / value-struct `A`.

## Minimal repro (self-contained -- no yyjson)

```turmeric
(defmodule decopt (export))

;; A Decode-shaped class: result-type ascription picks the instance.
(defclass Dec [a] (dec [tag : int] : (Result a cstr)))
(definstance Dec [int]  (dec [tag] ```c (void)tag; return tur_box_ok((int64_t)42); ```))
(definstance Dec [cstr] (dec [tag]
  ```c (void)tag; char *s=(char*)malloc(3); s[0]='h'; s[1]='i'; s[2]=0;
       return tur_box_ok((int64_t)(intptr_t)s); ```))

;; Constrained instance: build a (Result (Option A) cstr) from the inner decode.
(definstance Dec [Option]
  [(Dec A)]
  (dec [tag]
    (ok (some (ok-val (:: (dec tag) (Result A cstr)))))))

(defn main [] : int
  (let [oi (ok-val (:: (dec 0) (Result (Option int)  cstr)))
        oc (ok-val (:: (dec 0) (Result (Option cstr) cstr)))]
    0))
```

`./build/tur build decode.tur` -> the C compile fails (see below).

## Root cause, confirmed (2026-06-21)

The bug is **emit-time ABI specialization**, confirmed by instrumenting
`emit_abi_register_call` (`src/compiler/emit_module.c`). Inside the
`cstr` specialization
`__inst_Dec_dec_Option__spec__Result__Option__cstr__cstr_int64_t`, the active
spec's *own* bindings are correct -- the instance element `A` IS resolved:

```
spec_result = Result__Option__cstr__cstr
spec bindings: a -> Option__cstr   A -> const char *
```

But the four inner calls in the body `(ok (some (ok-val (:: (dec tag) (Result A cstr)))))`
each resolve `A` independently, and three of them collapse it to the int64
carrier representative:

| inner call | recorded abi_bindings | result chosen | should be |
| --- | --- | --- | --- |
| `(dec tag)` (ascribed `(Result A cstr)`) | -- | `__inst_Dec_dec_int` | `__inst_Dec_dec_cstr` |
| `ok-val` (destructure) | `A -> const char *`, `B -> const char *` (CORRECT) | result `int64_t` (carrier) | `cstr` |
| `some` (construct) | `A -> int64_t` (collapsed) | `Option__int` | `Option__cstr` |
| `ok` (construct) | `A -> Option__int` (collapsed) | `Result__Option__cstr__cstr` (CORRECT via `construct_recovered_byvalue`) but payload arg `Option__int` (WRONG) | payload `Option__cstr` |

The chain of collapse, with `file:line`:

1. **`ok-val` result collapses to carrier.** A bare-tyvar-result accessor has
   its `call->type` collapsed to the int64 carrier at elab
   (`call_result_type = TY_INT` for a non-composite tyvar result --
   `src/compiler/elab_call.c` `call_wrap_reinterpret` path). The emit-side
   recovery at `emit_module.c:1810-1820` re-derives the by-value result from
   the call's bindings -- **but only when `recovered.kind == TY_STRUCT`**
   (line 1816). `cstr`/`float` are pointer-carried scalars, not structs, so the
   guard rejects them and `ok-val`'s result stays `int64_t`. **Gap #1.**

2. **`some` argument is then `int64_t`.** Because `ok-val` (its argument)
   elaborated to the carrier `int64`, `some`'s recorded binding is
   `A -> int64_t` (a genuine `int64_t`, no named tyvar left to substitute), so
   the Phase-5 construct recovery at `emit_module.c:1914-1928` instantiates
   `(Option A)` through `{A -> int64_t}` and mints `some__spec__Option__int`.
   `spec_bindings` knows `A -> const char *`, but `some`'s own binding has no
   named `A` for the composition at `emit_module.c:1650-1671` to substitute.
   **Gap #2.**

3. **`ok` payload is `Option__int`.** `construct_recovered_byvalue`
   (`emit_module.c:1887-1910`) correctly recovers `ok`'s *result*
   (`Result__Option__cstr__cstr`) from the enclosing spec's return type, because
   `ok` builds the same struct family the spec returns
   (`rh.as.struct_.def == ch.as.struct_.def`, line 1904). But its *argument*
   type is still the collapsed `Option__int` (`some`'s wrong result), and there
   is no propagation of the recovered result's field type (`Option__cstr`) back
   down onto the inner `some`. **Gap #3.**

4. **Inner `(dec tag)` is not re-dispatched.** `emit_reresolve_method_call`
   (`emit_core.c:1100-1257`) re-dispatches a return-dispatched method only when
   `call->type.kind == TY_TYVAR` (line 1124). Here the ascribed call type is
   `(Result A cstr)` -- a `TY_APP` with an embedded tyvar, not a bare tyvar --
   so the re-resolver never fires and the call keeps the carrier base
   `__inst_Dec_dec_int`. This is the #475 gap in the ascribed-return-dispatch
   shape (#475 handled `(.field container)` receivers; this is
   `(:: (method ...) (F A))`). **Gap #4.**

### Emitted C (cstr spec) -- the mismatch

```c
static Result__Option__cstr__cstr
__inst_Dec_dec_Option__spec__Result__Option__cstr__cstr_int64_t(int64_t tag) {
    return ok__spec__Result__Option__cstr__cstr_Option__int(    /* takes Option__int */
             some__spec__Option__int_int64_t(                   /* builds Option__int, want Option__cstr */
               ok_val__spec__int64_t_Result__cstr__cstr(        /* wants Result__cstr__cstr by value */
                 __inst_Dec_dec_int(tag))));                    /* returns int64 carrier; want dec_cstr */
}
```

clang then rejects:

```
error: incompatible type for argument 1 of 'ok_val__spec__int64_t_Result__cstr__cstr'
  note: expected 'Result__cstr__cstr' but argument is of type 'int64_t'
error: incompatible types when initializing type '_Bool' using type 'Option__int'
```

### Base clone (`__inst_Dec_dec_Option`, no spec context) also miscompiles

```c
static int64_t __inst_Dec_dec_Option(int64_t tag) {
    return (int64_t)(intptr_t)ok(some__spec__Option__int_int64_t(ok_hyval(__inst_Dec_dec_int(tag))));
}
```

Here the carrier `ok(int64_t)` is handed a by-value `Option__int`: in a *carrier*
context the nested `some` was still promoted to by-value (Phase-5 recovery at
`emit_module.c:1914` fires unconditionally for a concrete non-heap TY_APP
result, regardless of whether the consumer -- the carrier `ok` -- wants the
carrier int64). The promotion of a nested construct is not gated on the
consumer's ABI. **Gap #5** (base-clone variant of #3).

## Why `int` "works"

When `A = int`, the carrier *is* the value (`Option int` carries as `int64`,
and the by-value `Option__int` happens to be the desired representation), so the
mis-resolution to `int`/`Option__int` is accidentally what the caller wants.
The divergence only surfaces for pointer-carried (`cstr`, `float`) or
value-struct elements.

## Second manifestation (helper-delegated form -- compiles, wrong at runtime)

Routing the inner decode through a plain constrained `defn` makes it compile
(the helper `defn` re-dispatches correctly, like the #475 encode fix) but the
pointer case is then wrong at runtime -- the `some` payload for a `cstr` element
is dropped (`is_some` reads false / value lost), with a codegen warning around
`some__spec__Option__cstr_const_char__`. Same Gap #2/#3 root: the `some`
construct does not lower the pointer-carried payload into the by-value
`Option__cstr` slot consistently.

```
ok     - decode (Option int)  present  -> some 42      (round-trips)
ok     - decode (Option int)  null     -> none
not ok - decode (Option cstr) present -> some "hi"     ;; reads back as none
```

## Fix direction

A coherent target lowering for the `cstr` spec (carrier-consistent inner chain,
by-value only at the construct seams the enclosing spec demands):

```c
return ok__spec__Result__Option__cstr__cstr_Option__cstr(
         some__spec__Option__cstr_const_char_(
           (const char *)(intptr_t) ok_hyval(__inst_Dec_dec_cstr(tag))));
```

That requires, at the emit-time ABI-specialization layer, resolving a generic
payload's type against the current spec's concrete args at **every** carrier
boundary, not just struct field reads:

- **Gap #4** -- extend `emit_reresolve_method_call` (`emit_core.c`) so an
  ascribed return-dispatched method call whose `call->type` is a `TY_APP`
  *containing* a tyvar (e.g. `(Result A cstr)`) is re-dispatched by resolving
  that tyvar through the spec's bindings (it already does this for a bare
  `TY_TYVAR` call type).
- **Gap #1** -- extend the by-value result recovery at `emit_module.c:1810` to
  also recover a pointer-carried scalar (`cstr`/`float`) result, not only
  `TY_STRUCT`.
- **Gaps #2/#3/#5** -- thread the enclosing construct's recovered by-value
  *field type* top-down onto a nested construct argument (so `some` inside
  `ok` builds `Option__cstr` from `ok`'s recovered `Result__Option__cstr__cstr`
  payload slot), in **both** the abi-scan (`emit_module.c emit_abi_scan_expr` /
  `emit_abi_register_call`) and emission (`emit_expr.c`), and gate the nested
  construct's by-value promotion on the consumer's ABI (so the carrier base
  clone keeps the nested `some` on the carrier).

A fixture mirroring `constrained-instance-element-dispatch`, but on the
construct side: a `Dec [Option]` (or `Encode`-shaped builder) over int / cstr /
float / value-struct, asserting the round-trip value rather than just that it
compiles.

## Relationship to #475 and the M4/M7 deferral

#475 keyed its fix on recovering the concrete element type from the current
spec's `arg_types[]` when the receiver is `(.field container)`. This report is
the **constructing/return** direction. The same `arg_types[]`/`spec_bindings`
substitution needs to apply to (a) constructor calls whose argument is
`A`/`(F A)` and (b) `ok-val`/match-style destructures whose scrutinee type
mentions `A`, not only `.field` receivers -- AND it needs top-down propagation
through *nested* constructs, which #475 did not have to deal with (a single
field read has no nesting).

Per `docs/archive/instance-method-return-carrier-bridge.md`, the structural end
state for this whole class is M4 (per-method instance ABI) / M7. The interim
carrier-bridge machinery (`emit_carrier_bridge`,
`type_uses_carrier_in_dispatch`, `expr_emits_byvalue_carrier_abi`) is the right
toolbox; the gap is that it is not yet applied at the nested-construct seam.

## Generalizations -- the combinations this one root cause subsumes

This is filed as a single report because the "various combinations of nested
generics" all reduce to the SAME emit-time machinery gap (a generic payload not
resolved against the spec's concrete args at a carrier boundary). They are NOT
separate bugs; do not file duplicates. Each becomes a fixture row once the fix
lands:

- **Element kind** -- `cstr` and `float` (pointer-/double-carried scalars) and
  value-struct elements all diverge; only `int` accidentally works (carrier ==
  value). The value-struct *read* side is already handled by #475; the
  *construct* side here is not.
- **Container constructor** -- `Option` (`some`), `Result` (`ok`/`err`), and by
  extension `Pair`/`Either`/`Slice` and any `#{Construct}` parametric struct
  built inside a constrained instance body.
- **Nesting depth** -- `(Option A)`, `(Result (Option A) E)`,
  `(Option (Option A))`, `(Result (Result A E1) E2)`: each additional construct
  layer adds another seam needing the top-down by-value field-type propagation
  (Gaps #2/#3), since the recovery currently only reaches the outermost
  construct via the enclosing spec's return type.
- **Destructure side** -- `ok-val` / `err-val` / `unwrap` / match scrutinees
  whose result type mentions the instance element (Gap #1) collapse to the
  carrier for non-struct payloads.
- **Container being decoded** -- `Decode [Vec]`, `Decode [Map]`,
  `Decode [Pair]`, etc. in the json spice are the same shape as `Decode [Option]`
  (build a nested generic from a decoded element inside a constrained instance
  body) and are blocked by the same gaps.

Closing this one report (fixing the five gaps + the round-trip fixture matrix)
closes the whole class.

## Scope / impact

Compiler-side (emit specialization). Unblocks `Decode [Option]` and `Decode`
for any container in `turmeric-spices/spices/json` -- the encode direction
already works post-#475 (`Encode [Option]` shipped). Verified on `tur` built
from `main` at `8e8b34f` (post-#475/#476, v0.22.0). Closely related to #475;
same machinery, opposite (construct/destructure vs. read) direction.
</content>
</invoke>
