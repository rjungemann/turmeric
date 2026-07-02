---
title: Mode B -- runtime dictionary passing for constrained rank-2 forall (van Laarhoven lenses)
category: Planning
description: Follow-up to constrained-hkt-forall-plan. Thread a typeclass dictionary into a constrained rank-2 `forall f. C f => ...` poly fn so its body can dispatch a class method (`fmap`) on the caller-chosen instance at runtime -- the piece slices 1-4 deferred, and the only thing still blocking a first-class van Laarhoven `Lens'`. LARGE; speculative; roadmap-only. Sliced.
---

# Mode B -- runtime dictionary passing for constrained rank-2 `forall`

## Context

The parent plan
([`constrained-hkt-forall-plan.md`](constrained-hkt-forall-plan.md)) landed
slices 1-4:

- **Slice 1** -- kind-annotated `forall` binders (`--enable=forall-kinds`).
- **Slice 2** -- constraint enforcement, **mode A** (static): a constrained
  `forall [a] [(Show a)] ...` re-discharges its constraints at each rank-2
  instantiation site as a *compile-time* instance check (`TUR-E0305` if
  missing). No dictionary is threaded -- Turmeric's HRT is type-erased, so for
  `Show`-shaped constraints the passed monomorphic function already carries the
  behavior.
- **Slice 3** -- higher-kinded rank-2 (`--enable=hkt-hrt`) plus the by-value
  aggregate-container carrier codegen (so `(Option int)`-shaped functors flow
  through the poly carrier).
- **Slice 4** -- the acceptance gate fired its own **No-go signal**: the van
  Laarhoven `type Lens s a = forall f. Functor f => (a -> f a) -> (s -> f s)`
  is inexpressible today, so `stdlib/lens.tur` ships the profunctor-by-record
  encoding instead.

This plan closes the one gap that keeps the van Laarhoven form out of reach:
**mode B**, threading a runtime `Functor f` dictionary into the lens body so its
`fmap` call dispatches on the functor the *caller* chose (`Const` for `view`,
`Identity` for `set`/`over`).

### The two blockers slice 4 confirmed empirically

1. **Method dispatch on an abstract constrained var.** The lens body is
   `\g s -> fmap (setter s) (g (getter s))`; `fmap` runs over `(f a)` where `f`
   is chosen by whoever invokes the lens. The lens is compiled **once**,
   polymorphically, and invoked through the erased int64 carrier, so `fmap`
   cannot be resolved statically inside the body, and mode A threads no runtime
   dictionary for it to resolve dynamically. (`TUR-E0001`: `fmap` on
   `(f a)` with `f` abstract.)
2. **Curried rank-2 whose result is a function.** `(l g)` returns `(s -> f s)`,
   which is then applied to `s` -- a poly-carrier call whose *result* is itself
   a closure. The current rank-2 machinery does not thread that. (`TUR-E0002`:
   "returns ?, which is not callable".)

Mode B is (1). Blocker (2) is orthogonal machinery, sliced separately below
(MB3) because the curried shape is what buys optic composition by ordinary
function composition -- the record encoding's one lost affordance.

### What already exists (load-bearing)

Turmeric already does **runtime dictionary dispatch** in exactly one place --
constraint-carrying existentials (`open`) -- and that is the template to reuse:

- **Dict singletons.** `(definstance C [T] ...)` emits
  `typedef struct dict_C_T { <ret> (*method)(...); } dict_C_T;` and a
  `static dict_C_T dict_C_T_singleton = { .method = <impl> };`
  (`emit_stmt.c:427-656`). `&dict_C_T_singleton` is a first-class,
  address-taken value.
- **Witness tables + runtime dispatch.** An existential pack carries a
  `witnesses[]` array of `TypeClassInstance*` (`expr.h:921-948`), lowered to a
  table of `&dict_C_T_singleton` pointers (`emit_expr.c:6668-6680`);
  `EX_EXISTS_DISPATCH` reads that table and calls the method pointer through the
  carrier ABI (`emit_expr.c:6900-6942`):
  `((<ret> (*)(<args>))(((void **)witnesses[ci])[method_idx]))(...)`. This is
  precisely "runtime dict pointer -> method pointer -> indirect call."
- **`EX_DICT`** already materializes either a bare dict value
  (`(int64_t)(intptr_t)(&dict_C_T_singleton)`) or a method field
  (`dict_C_T_singleton.method`) (`emit_expr.c:2332-2344`).
- **The constraint fields** on a `forall` -- `constraint_classes[]` /
  `constraint_var_idx[]` / `n_constraints` (`types.h:735-753`) -- are already
  populated by slice 2's parser and read by mode A in `elab_poly_call`
  (`elab_call.c`, the re-discharge loop).
- **The resolver** `typeclass_env_lookup_instance(env, class, &concrete, 1)`
  (`typeclass.c:151`) turns a `(class, concrete-type)` pair into the
  `TypeClassInstance*` whose dict singleton we pass.

So mode B is not "build dictionaries" -- they exist. It is "route an existing
dict singleton *into a rank-2 poly-fn body* and dispatch the constrained
method through it," using the existential witness-dispatch path as the model.

## The mechanism -- dictionary-passing desugar

Standard dictionary-passing: a constraint `C f =>` becomes an explicit **dict
parameter**, and a method call on the constrained var becomes an **indirect
call through that dict**.

- **Caller side (chooses `f`).** At an invocation of a constrained rank-2 poly
  fn where the constrained var is pinned to a concrete type, resolve the
  instance (`typeclass_env_lookup_instance`) and pass its dict singleton
  address. In the lens, `view` invokes the lens at `f := Const`, statically
  resolves `Functor (Const a)`, and passes `&dict_Functor_Const_singleton`.
- **Callee side (the poly-fn body).** The lens receives the dict (as a hidden
  leading parameter, see B1 below) and its `fmap` call lowers to an indirect
  vtable call through that dict -- the `EX_EXISTS_DISPATCH` shape, but keyed on
  a dict *parameter* rather than an `open`-bound witness table.

### Carrier options -- B1 (recommended) vs B2

The parent plan sketched mode B as **widening `tur_poly_fn_t`** with a `void
*dict` slot:
`{ void *env; void *dict; int64_t (*)(void*, void*, int64_t); }`. That is the
"largest blast radius" item -- it regenerates every poly-fn fixture's
`expected.c` and touches both preamble literals (`emit_module.c:~5669`,
`~9296`), every carrier construction site (`emit_expr.c` `EX_POLY_WRAP`), every
invocation (`emit_expr.c:~2744/2756/2762`), the fat-closure rebox, and
`make_poly_wrapper`.

**B1 -- dict as an explicit leading argument (recommended).** A dictionary is
just a pointer; it rides the *existing* int64 carrier as an ordinary argument.
Desugar `l : forall f. Functor f => A -> R` to an internal
`l : ptr<DictFunctor> -> A -> R`, insert the dict param on the poly-fn body, and
pass the resolved dict singleton as a leading arg at each invocation. **No
carrier change, no fixture regen** -- the same reason mode A avoided the blast
radius. The dict flows through `tur_poly_fn_t.fn(env, dict_ptr_as_int64, arg)`
with the wrapper forwarding it to the body.

**B2 -- widen the carrier.** Reserve for the case B1 cannot express: when the
dict must travel *attached to the poly-fn value itself* rather than supplied at
the call (e.g. a constrained poly fn stored in a container and later invoked
where the choosing site is not in scope). The van Laarhoven lens does **not**
need this -- `view`/`set`/`over` each choose `f` at the invocation, so B1
suffices. Keep B2 as a documented fallback, not the default.

Recommendation: **implement B1.** Note the divergence from the parent plan's
carrier-widening sketch -- the explicit-arg desugar is the same idea with a far
smaller blast radius, and the lens use-case does not exercise the case that
would force B2.

## Slices

Each slice ships behind a fresh experiment flag (per the CLAUDE.md
experimental-features rule) and adds fixtures before landing. Flags are
independent so we can stop after any slice.

### Slice MB1 -- dict parameter + invocation-site dict resolution (`--enable=forall-dict-pass`)

**Goal.** Thread a resolved dictionary into a constrained rank-2 poly fn via
B1's explicit leading dict argument, for the **caller-chooses-`f`** case.

**Status: LANDED (single-constraint, non-HKT).** Under
`--enable=forall-dict-pass`, a genuinely polymorphic single-constraint
constrained function passed as a rank-2 argument now dispatches its class method
through a runtime dictionary, so the *correct* instance runs at each invocation:
`poly-show` invoked at `int` and `bool` inside a callee prints `int:7` /
`bool:true` through one compiled body (fixture `forall-dict-show/`). The pieces,
as spec'd below: a **dict-clone** FnDef (`make_dict_clone`, `elab_call.c`) shares
the original's body + params and prepends one int64 dict param, marked
(`FnDef.dict_clone_*`) so `emit_call_name` (`emit_core.c`) emits the class-method
call as `((<ret>(*)(int64_t...))((void **)(intptr_t)<dict>)[<slot>])(...)` (the
existential-witness shape, dict from a param); `make_poly_wrapper_ex` wraps the
clone (its dict param is a forwarded arg) -- or, for a *monomorphic* inner,
accepts and ignores `n_lead_ignore` dict slots so mode B cleanly subsumes mode A;
`elab_poly_call` resolves each constraint's instance (reusing the mode-A
re-discharge) and prepends its `&dict_C_T_singleton` as a leading carrier arg
(B1, no carrier widening). A `Binding.source_fn_def` link reaches the original
FnDef from the passed binding. The soundness gate below still fires when the flag
is off. **Not yet:** multiple constraints and HKT `(f a)` method receivers
(MB2) -- both currently error clearly; the `poly_arg_mask` is shifted by the
dict count but the HKT arg-coercion is untouched.

**Step 0 -- soundness gate (LANDED, flag off).** Before dict passing exists, passing a
*genuinely polymorphic* constrained function (one whose own body dispatches a
class method on its constrained type variable -- `fn_constraints` non-empty) as
a rank-2 value **silently miscompiles**: `make_poly_wrapper` wraps the
function's single carrier-representative monomorph (the `int`/int64 instance),
so every invocation through the erased carrier runs that one instance regardless
of the caller-chosen type -- e.g. `(f true)` runs `Show int` on a bool and
prints `i1` instead of `T`. This is now rejected with **`TUR-E0308`** at the
rank-2 pass site (`elab_call.c`, the `EX_POLY_WRAP` wrap block) rather than
miscompiled. A *monomorphic* function passed as a constrained rank-2 arg (the
shipped mode-A `Show`-passthrough case, `hrt-forall-constraint-show/`) has no
`fn_constraints` and is unaffected. Fixture:
`errors/forall-poly-constrained-rank2-needs-dict/`. Implementing dict passing
below **replaces** this gate with the real thing.

**Why the rest is large (recon summary).** The remaining work is not "add a
param" -- it requires compiling the passed constrained function to **dispatch
its class method through a runtime dictionary** instead of the baked
representative. Today (`elab_typeclasses.c:5290-5347`) a method call on an
abstract constrained var is deliberately bound to the `int` carrier
representative (`__inst_Show_show_int`) plus an `EX_DICT` *annotation*, and the
concrete instance is recovered **only at emit inside an active ABI
specialization** (`emit_core.c:1347` gates `emit_reresolve_method_call` on
`ctx->current_abi_specialization`). A rank-2 value is never ABI-specialized --
it is reached only through the erased carrier -- so the representative is what
runs. `make_poly_wrapper` (`elab_call.c:5152`) hardcodes `dict_arg = NULL` and
an empty constraint set (`:5258`, `:5297`), and there is **no** existing
non-existential path that dispatches a method through a dict *parameter*
(`EX_DICT` always emits `_singleton`, `emit_expr.c:2360-2372`).

**Work (real dict passing).**
- **A dict-taking clone of the passed constrained function.** The core new
  piece: emit a variant of the function whose body dispatches its constrained
  method(s) through a leading `ptr<Dict>` parameter instead of the baked
  `__inst_..._int`. The cleanest route is a new **"dict specialization"** in the
  ABI-spec machinery (`emit_module.c:2191` mints specs; `emit_core.c:1567`
  re-resolves method calls): when the active spec is a dict-spec, have
  `emit_reresolve_method_call` return a *dict-param dispatch* expression rather
  than a concrete `__inst_..._T` symbol. This reuses the spec-clone emission
  and the `dict_arg` annotation the call already carries.
- **The dispatch emit.** Mirror the existential witness path
  (`emit_expr.c:6978-7013`):
  `((<ret> (*)(<sig>))(((void **)<dict_param>)[<method_idx>]))(args...)`, with
  the class-var args erased to `int64_t` and the slice-3 by-value-aggregate
  carrier bridge (`emit_expr.c:2559-2598`) applied for a `(f a)` method
  arg/return. `<method_idx>` is the method's slot in the class dict layout (all
  instances of a class share the field order, so the dict is a flat
  `void*`-array of method pointers, exactly as the witness table is indexed).
- **`make_poly_wrapper`** (`elab_call.c:5152`): when `inner_fn_b->fn_constraints`
  is non-empty and `forall-dict-pass` is enabled, prepend one `ptr<Dict>` param
  per constraint, wrap the *dict-spec clone* (not the representative monomorph),
  and forward the dict param(s) into the inner call.
- **Invocation** (`elab_poly_call`, the mode-A re-discharge loop that already
  pins the constrained var to a concrete type): instead of only *checking* the
  instance, resolve it (`typeclass_env_lookup_instance`, `typeclass.c:151`),
  materialize its dict singleton via the bare-value `EX_DICT` form
  (`emit_expr.c:2371`, `(int64_t)(intptr_t)(&dict_C_T_singleton)`), and prepend
  it as a leading argument. The dict rides the existing int64 carrier -- **B1,
  no carrier widening**. The N-ary carrier call form (`emit_expr.c:2789-2799`,
  already used for arity > 1) carries the extra leading arg.
- Keep mode A's `TUR-E0305` as the "no instance" error (now a hard blocker,
  since we need the dict, not just the check), and delete the `TUR-E0308` gate.
- Fixtures: `forall-dict-show/` -- `poly-show` (the step-0 repro) passed as a
  rank-2 arg and invoked at `int` and `bool`, printing `i7` then `T` (the
  observable proof the right instance runs per call); `errors/forall-dict-missing/`.

**Risk.** Medium-high for the real work -- the dict-spec is a new mode in the
delicate ABI-specialization core (`emit_module.c` / `emit_core.c`). Contained by
the `forall-dict-pass` flag (existing fixtures never take the path) and by
gating on `fn_constraints != empty`, so unconstrained and monomorphic rank-2
passing are untouched. The soundness gate (step 0) is low-risk and shipped.

### Slice MB2 -- HKT method dispatch through the passed dict (folded into `--enable=forall-dict-pass`)

**Status: LANDED.** MB1's dict dispatch now extends to a *higher-kinded*
constraint (`Functor f`, `f :: * -> *`): the same compiled body dispatches
`fmap` through the runtime dict at each caller-chosen functor. Fixture
`forall-dict-fmap/`: one `poly-fmap` invoked at `Functor Box` (maps: 5->105) and
`Functor Kt` (const: ->5) -- observably distinct instances through one body.
This is the dispatch the van Laarhoven lens needs. The pieces added on top of
MB1 (all under the same `forall-dict-pass` flag, so no separate flag was
needed):
- **Defn higher-kinded type params (`^f`)** -- a prerequisite: a `^f` defn/`fn`
  type parameter is kind `* -> *` (the defn analog of `(defclass Functor [^f]
  ...)`), so a function can be written polymorphic over a functor and use it
  as `(f a)`. `elab_fns.c`, mirroring the `^&` row-kind marker. Fixture
  `hk-defn-type-param/`.
- **Dispatch signature mirrors the dict field layout** -- `emit_call_name`
  derives each dispatched param's C type from the representative instance's
  method impl (a poly-fn method arg -- `fmap`'s `g : (fn [a] b)` -- is a
  `tur_poly_fn_t`, not the int64 carrier), instead of MB1's all-int64.
- **Higher-kinded constraint pinning** -- the invocation's re-discharge loop
  (`elab_poly_call`) pins `f` to the *container constructor* of a `(f a)`-typed
  argument (decompose `(Box int)` -> `Box`, resolve `Functor Box`), then
  prepends its dict; MB1 pinned only bare-tyvar `a` args.
- **Emit fix** -- the dict-clone's result type must not mention the tyvar
  (`(f a)` -> the int64 carrier), or the boxing wrapper is flagged
  generic-unsafe and skipped (`make_dict_clone`, `type_mentions_tyvar`).

**Scope.** Carrier-compatible functors (parametric opaque / heap). A by-value
*aggregate* functor (`Option`) as `(f a)` still hits the incomplete M7
higher-kinded-typeclass by-value monomorphization (a separate, large area; see
`g_m7_hkt_enabled`) -- out of MB2's scope.

**Original goal.** Inside a constrained rank-2 poly-fn body, lower a class-method
call on the constrained var (`fmap` on `(f a)`) to an indirect vtable call
through the dict parameter -- the `EX_EXISTS_DISPATCH` shape keyed on a dict
*param*.

**Work.**
- Elaboration: in a constrained poly-fn body, a call to a method of the
  constraint class whose receiver's type mentions the constrained var resolves
  to a new `EX_DICT_DISPATCH` node (or reuse `EX_EXISTS_DISPATCH` with a
  param-sourced witness) carrying `(dict_param_binding, method_idx)`.
- Emit: mirror `emit_expr.c:6900-6942` --
  `((<ret> (*)(<args>))(((void **)dict_param)[method_idx]))(args...)`.
- The method's `(f a)` argument/return is the by-value aggregate carrier from
  slice 3, so the box/deref + concrete-cast paths already apply.
- Fixtures: `forall-dict-fmap/` -- a constrained `forall f. Functor f =>
  (f int) -> (f int)` body that calls `fmap`, invoked at two different functor
  instances (observably distinct results) through the same compiled body.

**Risk.** Medium. This is the genuine novelty -- the first non-existential
runtime dict dispatch. The dispatch emit is a close analogue of the existing
witness path; the new part is sourcing the witness from a parameter and
resolving *which* method slot at elaboration.

### Slice MB2.5 -- aggregate `(f a)` functors: the M7 by-value boundary

**Status.** LANDED (Path A -- the carrier-boundary variant; see "What shipped"
below). A constrained rank-2 body `forall f. Functor f => (f int) -> (f int)`
that calls `fmap` on its constrained var now compiles and runs at a by-value
aggregate functor (`Maybe`/`Option`-shaped), not just carrier-compatible ones.
The rest of this section is kept as the original analysis; "What shipped" records
the actual (smaller-than-expected) fix. The van Laarhoven note below still holds
-- MB4 lenses can go through carrier-compatible `Const`/`Identity` regardless.

**What works today (MB2).** A constrained rank-2 body that calls `fmap` on its
constrained var works *as long as the functor is carrier-compatible* -- a
parametric opaque (`defopaque Box<a> :int` and friends) or any heap-backed type
whose runtime value is one int64 pointer. The dict-dispatch path (MB2) lowers
`fmap` to an indirect vtable call, and because the functor's `(f a)` value is a
single carrier word, the dispatched method's argument and return both flow
through the uniform int64 carrier with no ABI mismatch.

**What is blocked.** A *by-value aggregate* functor -- `Option`, a `list`, a
by-value product/sum whose `adt_is_byvalue_product` layout is a multi-word
struct (`tur_adt_Option__int`, not one int64) -- fails codegen when used as the
`(f a)` in a constrained HKT body, even under `TUR_M7_HKT=1`. The concrete
symptom is:

```
incompatible types when returning type 'int64_t' but 'tur_adt_Option__int' was expected
```

**Root cause.** The M6/M7 carve-out deliberately keeps constrained higher-kinded
poly-fns on the **uniform carrier ABI**: inside the rank-2 body every value is an
int64 carrier word, so the body compiles once regardless of the instantiating
functor. That is exactly what makes rank-2 type-erasure cheap. But a by-value
aggregate functor's `(f b)` result is a real multi-word struct, not a carrier
word. The dict-dispatched method returns the struct by value, the body expects
the carrier int64, and the two C types collide at the `return` site. Carrier-
compatible functors dodge this only because their `(f a)` *is* already one word.
Completing this means giving the constrained HKT body a by-value spelling for the
functor -- i.e. finishing M7's by-value higher-kinded-typeclass monomorphization,
where the body is re-emitted per concrete functor with specs named
`Functor_fmap__spec__Option__int` (see `g_m7_hkt_enabled` and
[docs/guides/monomorphization-abi-guide.md](../../guides/monomorphization-abi-guide.md)).

**Two fix paths.**

- **Path A -- reuse the slice-3 aggregate-carrier bridge (smaller).** Slice 3
  already built `emit_agg_box` / `emit_agg_unbox` (heap-box an aggregate into one
  carrier word, deref back out) plus the carrier-spill shim gated on
  `boxes_aggregate`. Extend that bridge across the *dict-dispatch return*: box the
  method's aggregate `(f b)` result into a carrier word at the call boundary and
  unbox it back at the body's consuming site. This keeps the uniform-carrier body
  and pays one heap box/unbox per dispatched aggregate result -- the same tax
  slice 3 already accepts for aggregate rank-2 *arguments*. Lower risk, bounded
  blast radius (touches only the MB2 dispatch emit + the slice-3 bridge), but
  leaves an allocation on the hot path.

- **Path B -- complete M7 by-value HKT monomorphization (larger).** Re-emit the
  constrained HKT body per concrete functor so the functor is spelled by value
  end-to-end, no carrier, no box. This is the residual Phase 5 carrier-bridge
  work tracked in
  [docs/archive/m7-phase5-carrier-bridge-audit.md](../../archive/m7-phase5-carrier-bridge-audit.md)
  and [docs/archive/m7-stdlib-migration-execution.md](../../archive/m7-stdlib-migration-execution.md).
  It is the *right* long-term answer (zero-overhead aggregate functors) but is a
  large, cross-cutting monomorphization effort touching elaboration spec-keying,
  emit spec naming, and the residual carrier bridges -- out of proportion to
  what MB4 (lenses) needs on its own.

**Do lenses actually need this? -- almost certainly not.** The van Laarhoven
encoding only ever instantiates the functor at **`Const r`** (for `view`) and
**`Identity`** (for `set`/`over`). Both can be defined as **carrier-compatible
opaques** -- a `Const r` whose runtime value is just the boxed `r`, and an
`Identity a` whose runtime value is just the boxed `a`, each one int64 carrier
word. Under that definition MB4's lenses go entirely through the MB2 carrier-
compatible path and **never touch the aggregate-functor boundary**. So MB2.5 is
*not* on the MB4 critical path: build MB4 against carrier-compatible `Const` /
`Identity`, and defer MB2.5 until a user genuinely needs `fmap`-over-`Option`
inside a constrained rank-2 body. When that day comes, prefer **Path A** first
(cheap, contained) and only reach for **Path B** if the box/unbox overhead shows
up in a profile.

**What shipped.** The fix was Path A, but *smaller* than "extend the box/unbox
bridge across the dispatch return" -- the insight is that a runtime dict ALWAYS
speaks the carrier ABI (its slot stores the carrier instance method), so the
aggregate box/unbox already happens at the poly-carrier boundary (the caller
boxes the arg and unboxes the return, exactly as for carrier-compatible
functors). Nothing new needs to box at the dispatch site; the dispatch just has
to stay on the carrier and stop fighting the M7 by-value spec machinery. Four
edits, all keeping the carrier-compatible (Box) path byte-identical:

- `emit_core.c` -- factor the dict-param-dispatch test into the shared predicate
  `emit_call_is_dict_param_dispatch`; derive the dispatched fn-ptr cast's RETURN
  type from the representative instance method's declared result (mirroring the
  dict struct field, `emit_stmt.c:581-603`) -- the carrier int64, not
  `call->type`'s by-value aggregate.
- `emit_expr.c` -- `find_matched_abi_spec` returns NULL for a dict-param-dispatch
  call, so the carrier args are no longer deref'd to the aggregate against the
  carrier dict slot.
- `emit_fns.c` -- a dict-clone wrapper (`fd->dict_clone_class`) returns the int64
  carrier in all three return-type sites (signature, `ret_ctype`, return
  statement) and returns the dispatch result directly instead of malloc-boxing.
- `emit_module.c` -- mirror the carrier return in the forward decl, and skip
  minting a by-value instance spec for a class-method call on a higher-kinded
  constrained var inside a constrained poly-fn / its dict-clone (both share the
  body `Expr`) -- such a spec is dead (nothing calls it) and ill-typed for a
  by-value aggregate. Restricted to higher-kinded classes so ground-kind
  constrained-defn monomorphization (M4c) is untouched.

Path B (full M7 by-value HKT monomorphization) is therefore NOT needed for
correctness here; it remains the *zero-overhead* answer (no carrier round-trip)
if a profile ever shows the boundary box/unbox mattering.

**Fixture.** `hrt-hkt-aggregate-functor-dispatch/` -- runs the SAME compiled
`poly-fmap` body at a by-value `Maybe` (Some -> 105, None -> -1) and a
carrier-compatible `Box` (-> 105), on the default by-value gate. It previously
died with `int64_t` vs `tur_adt_Maybe__int`; it now passes.

### Slice MB3 -- curried rank-2 whose result is a function (`--enable=hrt-curried-result`)

**Status.** LANDED. A rank-2 poly fn whose forall body result is itself a
function now instantiates that result to a concrete callable closure type, so
`(l x)` yields a closure and `((l x) y)` applies it -- the shape van Laarhoven
optics compose through.

**Goal.** Support a rank-2 poly fn whose result is itself a function
(`(a -> f a) -> (s -> f s)`), i.e. `(l g)` returns a closure that is then
applied. This is blocker (2) from slice 4 and is what enables optic composition
by ordinary function composition.

**What shipped.** Two edits behind `--enable=hrt-curried-result` (default off; the
gate is `g_opt_hrt_curried_result`, warned via TUR-W0060 at the elaboration
entry):

- `elab_call.c` (`elab_poly_call` result determination) -- when the forall body's
  result is a `TY_FN`, instantiate it through the argument bindings
  (`call_collect_type_bindings` / `call_instantiate_type`, mirroring the `TY_APP`
  result branch) and set `result_full` to the concrete fn type. Without this the
  result collapsed to a bare `type_from_kind(TY_FN)` -- no arity/result -- so the
  chained application errored with TUR-E0002 "returns ?".
- `elab_call.c` (`elab_call_head_expr`) -- when the call head is itself a poly
  call (`is_poly_call`) whose static type is a function, mark the head temp's fn
  type `boxed` so emit fat-dispatches through slot 0 of the returned closure box
  (thunk in slot 0, box as env). The poly carrier erases the result to the int64
  carrier and the concrete callee hands back a uniform fat box; calling it as a
  thin function pointer jumps into the env struct (SIGSEGV). Mirrors the existing
  cata-carrier `boxed` marking.

- Fixture: `hrt-curried-fn-result/` -- `((l x) y)` at two shapes: `l : forall a.
  a -> (a -> a)` (konst, returns a constant closure -> 3) and `l : forall a.
  (a -> a) -> (a -> a)` (returns a closure that captures and applies the fn
  argument, so a wrong dispatch would crash rather than pass -> 12).

**Alternative that sidesteps MB3:** ship the lens **uncurried**
(`forall f. Functor f => (a -> f a) -> s -> f s`, a 2-arg body) so no
intermediate closure is produced. This makes `view`/`set`/`over` work without
MB3, at the cost of losing `.`-composition of optics (compose via an explicit
combinator, as the record encoding does). Decide in MB4 whether the composition
affordance is worth MB3; MB1+MB2 alone (uncurried) already deliver first-class
van Laarhoven `view`/`set`/`over`.

### Slice MB4 -- van Laarhoven `Lens'` in stdlib (`--enable=van-laarhoven-lens`)

**Status.** PARTIAL. The encoding is proven expressible and the No-go signal is
NOT triggered -- `Const`/`Identity` (carrier-compatible opaques) flow as `(f a)`
through the carrier for both the argument and the result positions, so
`view`/`set`/`over` work end to end at concrete focus types (fixture
`van-laarhoven-lens-concrete/` returns 3/30/4/99, exercising the full mode-B
stack: MB1/MB2 dict passing + dispatch, the parametric `(Const r)` dict, MB3-era
closure crossing, and the fat-box crossing of the functor-wrapping function).
What is NOT yet available is an *ergonomic generic* `(view lens s)` that infers
the focus type from the lens argument -- that needs generic-type-param inference
through a rank-2 poly argument, a separate inference feature that regresses the
existing rank-2 fixtures when attempted naively. So the shipped lens stays the
profunctor-by-record encoding for now; the van Laarhoven form ships as a
demonstrated-at-concrete-types fixture, and the guide rewrite is deferred until
the generic form lands. See
[docs/reported/van-laarhoven-generic-inference-gap.md](../../reported/van-laarhoven-generic-inference-gap.md).

**Compiler advances that landed for MB4** (suite-green, independently useful):

- **Parametric-instance dict naming** (`emit_core.c` `emit_dict_name`): the
  pass-site dict name for a partially-applied instance head (`(Const r)`) now
  reads the source symbol (`type_arg_syms`), matching the definition site --
  previously `dict_Functor_T_...` vs `dict_Functor_Const_...`, an
  undeclared-symbol link error when an MB1 dict for a parametric functor was
  passed.
- **HK constraint pinning through a nested function parameter** (`elab_call.c`
  `elab_poly_call`): the `Functor f` dict resolves when `f` appears inside a
  function-typed parameter `g : (-> A (f A))` (a lens), not just as a top-level
  `(f a)` argument (MB2's shape).  Uses `call_collect_type_bindings` to bind `f`
  from the concrete argument, decomposed to the base constructor.
- **Fat-box crossing of the functor-wrapping function** (`elab_call.c`
  `make_dict_clone` + the poly-call pass site): a function value crossing the
  poly carrier is a uniform fat box.  The dict-clone marks its function-typed
  params `boxed` (fat-dispatch through slot 0), and the pass site boxes a thin
  (non-capturing) fn argument via `EX_FN_TO_FAT` -- done *after* constraint
  pinning and result-type determination so the arg's real function type stays
  visible to `f`-inference.  Without this a capturing closure (`set`/`over`'s
  `\a -> Identity (h a)`) was called as a thin function pointer -> SIGSEGV.

**Goal.** Re-express `stdlib/lens.tur`'s optic over the van Laarhoven encoding
(or add it alongside the record encoding), with `Identity` / `Const r` functor
instances and `view`/`set`/`over` derived by instantiating `f`. Acceptance gate.

**Work.**
- `Identity a` and `Const r a` newtypes + their `Functor` instances (`fmap`
  over `Identity` maps; over `Const` ignores the function).
- `view l s = get-const (l (\x -> Const x) s)`;
  `set l b s = run-identity (l (\_ -> Identity b) s)`;
  `over l f s = run-identity (l (\b -> Identity (f b)) s)`.
- End-to-end fixture `stdlib-van-laarhoven-lens/` exercising view/set/over on a
  record field, and -- if MB3 landed -- a composed lens via ordinary function
  composition.
- **Rewrite `docs/guides/lens-guide.md` to teach the shipped encoding cleanly,
  with no historical scaffolding.** The current guide documents the record
  encoding *and* an "why not van Laarhoven" section explaining the deferral, the
  mode-A/mode-B blocker, the two compiler errors, and the composition tradeoff.
  Once the van Laarhoven form ships, **delete all of that** -- a learner reading
  the guide should see only the encoding they will use and how to use it, not a
  changelog of what used to be impossible. Specifically:
    - Remove the "The encoding, and why not van Laarhoven" section and the
      "one tradeoff: composition" section in their entirety.
    - Present van Laarhoven `Lens'` as *the* lens type, with `view`/`set`/`over`
      and composition (`.`/`compose` if MB3 landed, else the uncurried form)
      shown as plain, working idioms.
    - If the record encoding is retained at all, keep it to a short "lightweight
      alternative" note stated on its own merits (no getter/setter allocation,
      no functor instances) -- **not** framed as a workaround for a former
      restriction, and with no reference to mode A/B, erasure, or the old
      No-go.
    - Move any genuinely useful design rationale that must be preserved into
      this plan or an archived report, not the user-facing guide.
  The parent plan and this plan remain the place for the historical decision
  trail; the guide is for learning the language as it is.

**No-go signal.** If MB1/MB2 land but `Const`/`Identity` cannot be made to flow
as `(f a)` through the carrier for *both* the arg and result positions of the
lens (a slice-3 carrier edge), fall back to keeping the record encoding as the
shipped lens and mark the van Laarhoven form as reference-only.

## Cost estimate (rough)

| Slice | Surface | Type system | Codegen | Tests/docs | Risk |
| --- | --- | --- | --- | --- | --- |
| MB1 -- dict param/arg | small | medium | small (B1: no carrier change) | small | low-medium |
| MB2 -- dict dispatch | small | medium | medium (new dispatch node) | medium | medium (core novelty) |
| MB3 -- curried result | small | medium | medium | small | medium |
| MB4 -- van Laarhoven lens | medium | none | none | medium | low |

MB2 is the dominant novelty (first parameter-sourced runtime dict dispatch).
Choosing B1 over the parent plan's carrier-widening removes the "largest blast
radius" from the estimate entirely; B2 is only costed if a future non-lens
use-case forces it.

## Open questions

1. **Multiple constraints / superclasses.** `forall f. (Functor f, Foldable f)
   => ...` passes two dicts. B1 prepends one param per constraint; confirm the
   ordering is stable across the pass site and the body, and that a superclass
   (`Traversable f => Functor f`) resolves the parent dict from the child.
2. **Dict for a caller-chosen *higher-kinded* instance.** `Functor (Const r)` is
   a partially-applied instance head. Confirm `typeclass_env_lookup_instance`
   resolves the parametric `(Const r)` instance (PTC3 parametric-constraint
   machinery) and that its dict singleton name is distinct per `r` where needed
   (`typeclass.h:96-97` already notes `dict_Functor_option` vs
   `dict_Functor_vec` naming for HKT instances).
3. **Curried vs uncurried lens (MB3).** Is ordinary-function optic composition
   worth MB3, or is an explicit `lens-compose` acceptable? If uncurried is the
   shipped form, MB3 can be dropped and MB4 depends only on MB1+MB2.
4. **Interaction with mode A.** Should mode A (static check, no dict) and mode B
   (dict pass) coexist behind separate flags, or does mode B subsume mode A?
   Recommendation: mode B *is* mode A plus the dict; when a constrained rank-2
   value is only ever monomorphic-passthrough (the `Show` case), the passed dict
   is unused but harmless -- so mode B can subsume mode A at graduation, and the
   `forall-constraints` flag folds into `forall-dict-pass`.
5. **`exists` symmetry.** `exists` already dispatches via witness tables
   (`EX_EXISTS_DISPATCH`); MB2's `forall` dispatch should share that emit path.
   Worth unifying the two into one "dict/witness dispatch" node.

## Out of scope

- Impredicative polymorphism / storing a constrained `forall` in a container
  (would force carrier option B2).
- Profunctor / full-optic generalisations (Iso, Prism, Traversal) -- the gate is
  `Lens'` only, as in the parent plan.
- Inference of constraints or `f` -- explicit annotations remain required.

## Related

- [`constrained-hkt-forall-plan.md`](constrained-hkt-forall-plan.md) -- the
  parent plan (slices 1-4) and the mode-A/mode-B decision
- [`../../guides/lens-guide.md`](../../guides/lens-guide.md) -- the shipped
  record encoding and why van Laarhoven is deferred
- `src/compiler/emit_expr.c:6900-6942` -- `EX_EXISTS_DISPATCH` runtime witness
  dispatch (the template for MB2)
- `src/compiler/emit_stmt.c:427-656` -- dict-singleton emission
- `src/compiler/typeclass.c:151` -- `typeclass_env_lookup_instance` (the
  instance -> dict resolver)
- `docs/archive/history/existential-open-witness-dispatch.md` -- the one
  existing runtime-dictionary-passing path, worth reading before MB2
