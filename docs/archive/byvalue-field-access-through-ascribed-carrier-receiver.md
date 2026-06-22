# By-value struct field access through an ascription-wrapped carrier receiver emits an uncast `.field` (broken C)

> **RESOLVED 2026-06-22.** `EX_GET_FIELD` in `src/compiler/emit_expr.c` now
> strips leading `EX_ASCRIBE` wrappers from the receiver before the
> carrier-vs-by-value classification tests (the `EX_VAR`/binding checks at
> ~5182 and ~5202, the `expr_is_pbp_param` test, and the two
> `emit_var_spec_arg_type` recovery sites at ~5111/~5245 all key on the
> unwrapped receiver `recv_expr`). An ascribed receiver
> `(.snd (:: x (Duo cstr int)))` now casts the int64 carrier through the struct
> layout (`((Duo *)(intptr_t)(x))->snd`) like the bare-receiver path. Regression
> fixture: `tests/fixtures/byvalue-field-ascribed-carrier-receiver/` (pairs the
> ascribed body with the bare control, asserting the by-value field value).

Repo: rjungemann/turmeric
Found by: follow-up to docs/reported/kind-star-instance-two-param-type-cannot-bind-constraint-var.md
Verified on: turmeric 0.22.0, main built from source (build-release)
Severity: Low-Medium. A narrow codegen gap, not a correctness-in-disguise: it
  fails at the C compiler (a hard, visible error), never silently miscompiles.
  Reachable by a by-value parametric struct instance whose method body reads a
  field through an *ascribed* receiver. The common idiom (read the field off the
  bare receiver param) is unaffected.

## Context: where "complete struct by-value dispatch" stands

End-to-end by-value monomorphization is **landed and archived**
(`docs/archive/end-to-end-monomorphization-plan-2.md`, COMPLETE 2026-06-19):
values thread by value end-to-end and the remaining int64 carrier bridge is
intentional. So there is no open master plan for "complete struct by-value
dispatch"; the carrier bridge is a deliberate boundary, not a TODO. This report
captures one concrete sharp edge on that boundary, exposed when a kind-* class
gained fully-applied 2-parameter instance heads (the kind-star report above).

Two distinct residuals sit behind a fully-applied multi-param instance head:

1. **Heap-carried opaque containers (`Map`, heap `Cons`) -- intentional carrier
   bridge.** `(Map cstr cstr)` and `(Map cstr int)` share one C representation
   (an int64 handle), so the instance method is not element-monomorphized and an
   in-body `(encode (:: (map-get m k) V))` bakes the int-carrier representative.
   This is the #475-class boundary the spice's `Encode [Cons]` already flags;
   by-value containers (`Option`, `Vec`) escape it because their elements are
   reified into distinct C types. Not a defect -- the declared boundary.

2. **By-value parametric struct field access through an *ascribed* receiver --
   the actual gap reported here.**

## Repro -- broken C (by-value struct, ascribed field receiver)

    (defstruct Duo [a b] (fst :a) (snd :b))
    (defclass Tag [a] (tg [x] : int))
    (definstance Tag [int]  (tg [x] 10))
    (definstance Tag [cstr] (tg [x] 20))
    (definstance Tag [(Duo cstr int)]
      (tg [x] (tg (.snd (:: x (Duo cstr int))))))   ;; ascribed receiver
    (defn main [] : int
      (let [p (:: (make-struct Duo "k" 7) (Duo cstr int))]
        (println (tg p)) 0))

`tur run` emits, then the C compiler rejects:

    static int64_t __inst_Tag_tg_Duo_int(int64_t x) {
        return __inst_Tag_tg_int((x).snd);     // x is int64_t -> `(x).snd` is invalid
    }
    error: request for member 'snd' in something not a structure or union

## Control -- works (same instance, bare receiver)

    (definstance Tag [(Duo cstr int)]
      (tg [x] (tg (.snd x))))                  ;; bare param receiver

Runs; prints `10`. The bare-receiver path casts the int64 carrier correctly
(same as the working `Encode [(Option cstr)]` instance, whose body reads
`(.value x)` off the bare receiver). Only the *ascribed* receiver regresses.

## Root cause

`src/compiler/emit_expr.c`, `EX_GET_FIELD` (the `through_carrier`
determination, ~line 5182):

    if (!through_rc && !through_carrier && def->n_type_params > 0
        && e->as.get_field_.struct_expr->type.kind == TY_APP
        && e->as.get_field_.struct_expr->kind == EX_VAR            // <-- only EX_VAR
        && e->as.get_field_.struct_expr->as.var.binding
        && !e->as.get_field_.struct_expr->as.var.binding->emit_byvalue_carrier_abi) {
        through_carrier = true;
    }

The receiver of a `.field` access is recognized as carrier-represented (and so
gets the `((Duo *)(intptr_t)x)->snd` cast) only when it is a bare `EX_VAR` param.
An ascription `(:: x (Duo cstr int))` is an `EX_ASCRIBE` node wrapping the param,
so the guard does not fire, `through_carrier` stays false, and the `else` branch
emits a direct `(x).snd` against the int64 carrier.

The same `struct_expr->kind == EX_VAR` assumption recurs at the sibling spec-arg
recovery sites in this case (the `emit_var_spec_arg_type` calls around 5111,
5202, 5245), which also peer at the receiver expecting a bare param.

## Fix directions

Strip leading `EX_ASCRIBE` wrappers before the receiver-kind tests in
`EX_GET_FIELD`, then key the carrier-vs-by-value decision on the *unwrapped*
expression (the param binding + its `emit_byvalue_carrier_abi` flag) and the
ascribed type's struct head. Concretely:

- Add a local `const Expr *recv = struct_expr; while (recv->kind == EX_ASCRIBE)
  recv = recv->as.ascribe_.inner;` and use `recv` for the `EX_VAR`/binding
  checks at ~5182 (and consider the same unwrap at the `emit_var_spec_arg_type`
  sites).
- Walk the full `TY_APP` spine to the struct head when resolving `def` for the
  cast, so a nested fully-applied head `(Duo cstr int)` =
  app(app(Duo,cstr),int) resolves to `Duo` (single-applied `(Option cstr)`
  already resolves; verify the multi-arg spine does too).

A regression fixture should pair the ascribed-receiver body with the bare-
receiver control so both stay green, and assert the by-value field value
(`snd` = int here) rather than only that it compiles.

## Why filed, not fixed now

On the one track to v1 this is a rough edge, not a blocker: the idiomatic body
(bare receiver) works, and the failure mode is a loud C compiler error, not a
silent miscompile. Filed so the carrier-cast unwrap is not forgotten when the
ascribed-receiver path next matters.
