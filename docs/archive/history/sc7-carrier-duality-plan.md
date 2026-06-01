# SC7 final blocker: the parametric-wrapper carrier/concrete representation duality

> **Status: RESOLVED (2026-06-01).** The duality below was collapsed by the
> *transparent int-newtype* approach (the "Alternative" at the end of this note),
> not the four-site carrier pinning originally sketched. A parametric struct with
> a single `:int` field (`(defstruct Schema [A] (raw :int))`) is now a transparent
> newtype over int64 -- one C representation everywhere -- so the by-value
> `Schema__int` layout is never emitted and HKT results chain. The chainable HKT
> return is handled by a tiny, gated dispatch override (propagate the instance
> body's transparent-newtype type as the call result). SC7's
> `Functor`/`Applicative`/`Alternative` instances and applicative struct building
> now ship. See the implementation notes at the end and
> `docs/return-type-dispatch-and-schema-sc5-sc7-plan.md`.
>
> The original diagnosis is retained below for historical context.

## What "chainable HKT return" needs

For `(<$> f s)` / `(<*> sf sa)` / `(<|> a b)` to compose, each step must return a
value that still flows with its `(Schema a)` (container) type, so the *next*
operator can dispatch on it. Today a Functor-style method declared `:int`
(the carrier) loses the wrapper on return, so the result is a bare `int` and the
next `.fmap`/`<*>` has nothing to dispatch on.

Two dispatch-level pieces make the *type* chain (both were prototyped and shown
to work, then reverted pending the codegen fix below):

1. **Chainable result type.** At the method-dispatch site
   (`elab_typeclasses.c`, where `result_type` is computed ~line 2933), when the
   *class* declares the method's return as the dispatched HKT type parameter
   (e.g. `(defclass Mappable [^f] (fmapc [container [fn :fn]] :f))`), set the
   call's result type to the *receiver's* container type. `f a -> f b` preserves
   `f`, so the result dispatches by the same `(C a)` head. Opt-in: methods that
   declare a concrete `:int` return are unchanged (zero churn for existing
   instances/fixtures).

2. **HKT methods are argument-dispatched, not return-dispatched.**
   `method_is_return_dispatch` (`elab_typeclasses.c:17`) currently classifies any
   method whose type param appears *only* in the return as return-dispatch (the
   HasSchema `(decode! [node] :a)` case). But an HKT method leaves its receiver
   `(f a)` unannotated, so the HKT param also appears only in the return -- yet it
   is dispatched on the *receiver*. Fix: a return-only param that is HKT-kinded
   (`type_param_kinds[ti] != KIND_STAR`, i.e. declared `^f`) **and** has a
   receiver param is argument-dispatched, not return-dispatched. This keeps the
   receiver-param-as-container typing (so `.raw container` resolves in the body).
   Inert for existing classes (Functor/Applicative declare `:int` returns, so the
   HKT param never appears in their returns).

Both are correct and inert for existing code. They are **not** committed because
they cannot be exercised end-to-end without the codegen fix below, and shipping
un-testable forward code is a liability.

## The blocker: `(Schema a)` has two coexisting C representations

A parametric wrapper `(defstruct SBox [A] (raw :int))` instantiated at a concrete
element -- `(SBox int)` -- has **both**:

- a concrete by-value layout `SBox__int { int64_t raw; }`
  (`type_has_concrete_codegen_layout` is true: TY_APP with all-concrete args), and
- the int64 carrier ABI (`type_uses_carrier_abi` is true: any parametric struct).

Codegen picks *different* representations at different sites, so a chained
`.fmapc` mixes them. From the actual emitted C of the experiment
(`Mappable [SBox]`, body `(:: (make-struct SBox (schema/transform (.raw container) fn)) (SBox int))`):

| Site | Emitted as | Should be |
|---|---|---|
| receiver `s` let-binding | `SBox__int s = (SBox__int){.raw = schema_int()};` | (either, but must agree) |
| instance fn definition | `int64_t __inst_..._fmapc_SBox(...)` | one rep |
| instance fn body return | `return (SBox__int){.raw = ...};` (struct!) | matches fn signature |
| dict slot | `SBox__int (*fmapc)(...)` (struct!) | matches fn signature |
| call-site result `s2` | `int64_t s2 = __inst_..._fmapc_SBox(...)` | matches result type |
| field access `(.raw s2)` | `(s2).raw` (struct member on an int64!) | matches `s2`'s rep |

Three distinct disagreements: (a) the function returns `int64_t` but its body
yields a `SBox__int` struct; (b) the dict slot says `SBox__int` while the function
says `int64_t`; (c) `s2` is bound as `int64_t` but `.raw` accesses it as a struct.

`emit_carrier_return_override` (`emit_core.c:151`) fixes the *inverse* case (a
non-carrier struct body under a carrier-declared return) but bails for a
*parametric* struct body because `type_uses_carrier_abi` is true -- so the
function keeps the int64 carrier while the body, dict slot, and field access
diverge.

## Proposed fix (one representation, consistently)

Pin a parametric wrapper instantiated through HKT dispatch to **one**
representation end to end. The cleanest is the **int64 carrier** everywhere
(box `{raw}` once; `.raw` reads `box[0]`), because the typeclass dictionary ABI
and `schema-decode` already speak int64:

1. **Function definition + forward decl + dict slot** must all use the carrier
   (int64) for a parametric-struct method return -- they already mostly do;
   the dict-slot path (`emit_stmt.c` ~449 / `emit_module.c`) must stop emitting
   the concrete `SBox__int` for a carrier-ABI return.
2. **Instance body** returning `(make-struct SBox x)` must box to the carrier
   (the *inverse* of `emit_carrier_return_override`): when the declared return is
   the carrier and the body is a by-value parametric struct, spill-and-box via
   `emit_carrier_bridge` (CK_CONCRETE -> CK_CARRIER) before the return.
3. **`.raw` field access** on a carrier value (int64) typed as a parametric
   struct must read through the carrier (`((int64_t*)v)[idx]`) rather than emit a
   C struct member access. (`EX_GET_FIELD` emit needs a carrier-aware path keyed
   on `type_uses_carrier_abi(struct_expr->type)`.)
4. **Receiver let-binding** (`s`) must also be the carrier, so the chained
   receiver and the produced result share one rep.

Alternative (smaller surface, narrower): treat a *single-int-field phantom
wrapper* as a transparent newtype -- the int64 *is* the payload, `make-struct`
and `.raw` are identities, and no `SBox__int` layout is emitted. This sidesteps
all four sites but needs a "transparent parametric newtype" notion the codegen
does not have today.

## Risk

Touching the parametric-struct representation policy is high-regression: `Pair`,
`Vec`, and every existing by-value parametric struct ride
`type_has_concrete_codegen_layout`. Gate any change so it only affects structs
reached as a typeclass *method return/receiver* (HKT dispatch), or behind the new
chainable-return path from the dispatch section above, and regenerate the
`expected.c` snapshots. Land the dispatch-level pieces (1) and (2) *together*
with the codegen fix, guarded by a fixture that chains `(.fmapc (.fmapc s g) h)`
and reads back through `.raw`.

## How it was actually resolved (2026-06-01)

The "Alternative" (transparent int-newtype) was the winning path, and it turned
out to need **no** gating on "reached through HKT dispatch": a parametric struct
with a *single concrete `:int` field* is structurally distinct from every
value-carrying parametric struct (`Box [A] (x A)`, `Pair`, `Vec`, ...), whose
field is the type parameter or a non-int type. So the gate is just the shape.

- **`type_is_transparent_int_newtype(Type)`** (`src/compiler/types.c`): parametric
  struct, one field, field declared `:int`. Uniquely matches the phantom
  wrappers (`Schema`/`Sbox`/`SchemaW`); no other struct in the tree qualifies.
- **One representation, int64, everywhere:** `type_c_name` returns `int64_t` for
  it (TY_STRUCT and TY_APP), and `type_uses_carrier_abi` returns *false* (it is a
  scalar, not a carrier aggregate -- so no spill/box/deref bridging). The
  `Schema__int` by-value layout is never registered or emitted.
- **`make-struct` and field access are identities** (`emit_expr.c`):
  `(make-struct Schema x)` emits `(int64_t)(x)`; `(.raw s)` emits `s`.
- **Chainable HKT return** (`elab_typeclasses.c`): after computing a dispatched
  method's result type, if the resolved instance *body* has a transparent
  int-newtype type, that type becomes the call result -- so `(fmap c f)` keeps its
  `(Schema b)` head and `ap`/`alt-or` can dispatch on it. Inert for instances
  whose bodies are bare `:int` carriers (every existing Functor instance).
- **Applicative struct building** uses `schema/ap-fat` (decoder kind 16), which
  applies the function arm via `TUR_APPLY1`, so a curried constructor closure
  composes; the assembled value is boxed by the constructor and read back.

Net result: zero `expected.c` churn (the three existing canary fixtures assert
stdout only), and the full suite stays green. Proof fixtures:
`schema-hkt-functor`, `schema-hkt-alternative`, `schema-applicative-user`,
`schema-applicative-user-errors`.
