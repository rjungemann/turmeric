---
status: RESOLVED (2026-07-29, Route B -- dictionary passing)
severity: high
discovered: 2026-07-29
area: compiler (ABI specialization / lambda lifting)
---

# A lifted continuation keeps the representative instance

> **RESOLVED.** Route B landed on the second attempt. The missed mechanism from
> the first prototype was `mapper_scan_dispatch` -- the scanner that decides
> which dicts a nested mapper captures -- which shared the same bare-tyvar
> receiver gate as the two sites the prototype relaxed. With a single shared
> predicate (`call_dispatched_constraint_class`: receiver is a bare tyvar, OR
> the result is tyvar-headed) applied at the scanner, the E0311 guard, and the
> emit-side env-dict index, the EXISTING nested-mapper lowering converts the
> lifted continuation into a dict-capturing closure -- including the
> non-capturing shape, via `convert_mapper_to_dict_closure`, so the TUR-E0311
> wall never materialized.  Direct calls at concrete constructors then route
> through the callee's dict clone with the concrete instance singletons, and a
> by-value `(Option int)` result is bridged back through an EX_ASCRIBE to the
> existing carrier->concrete deref.
>
> The continuation now compiles to a dict-slot load off its closure env:
>
>     struct __env { int64_t __fn; int64_t __dict; };
>     static int64_t __fn_N(void *env, int64_t v) {
>         ... (((int64_t (*)(int64_t))((void **)env->__dict)[0])((v) + 1))
>
> Fixtures: `hkt-constrained-continuation-dict` (two differently-tagged
> instances, 107/207 -- cannot pass by coincidence) and
> `hkt-constrained-byvalue-bind-pure` (stdlib Option end to end, upgraded back
> to the bind-then-pure shape).  Suite: 2405 passed, 0 failed.
>
> The analysis below is the pre-fix state, kept as the paper trail.

## Summary

Inside a constrained kind-polymorphic fn, a class-method call in a **lifted
lambda** -- the continuation of `(bind x (fn [v] (pure ...)))` -- is emitted once,
outside any specialization, and keeps whichever instance elaboration picked as
the representative. With the autoloaded stdlib that is `Applicative [Schema]`,
whose `pure` is `schema/always`.

The enclosing fn's own body is fine: receiver-directed dispatch re-resolves per
spec (fixed 2026-07-29, see
[../archive/history/constrained-hkt-spec-reresolve-hkt-dispatch.md](constrained-hkt-spec-reresolve-hkt-dispatch.md)),
and return-directed `pure` written directly in the body resolves correctly too.
Only the lifted lambda misses.

## Scope

| Shape | Dispatch | Correct? |
|---|---|---|
| Method call in a rank-2 `forall` body (dict-passed) | dict slot load | yes |
| Return-directed `pure` in the poly fn's OWN body | re-resolved per spec | yes |
| Receiver-directed `bind` in a monomorphized spec | re-resolved per spec | yes (fixed) |
| `pure` inside a LIFTED continuation | representative | **no** |

## Repro

    $ cat > /tmp/r.tur <<'EOF'
    (defn bind-then-pure [^m] [^Monad m ^Applicative m x : (m int)] : (m int)
      (bind x (fn [v] (pure (+ v 1)))))
    (defn main [] : int
      (println (unwrap-or (bind-then-pure (some 41)) -1))
      0)
    EOF
    $ ./build/tur run /tmp/r.tur
    42
    $ ./build/tur emit-c /tmp/r.tur | grep -A2 "^static int64_t __fn_"
    static int64_t __fn_1304(int64_t v) {
        __auto_type __ps_53 = (__inst_Applicative_pure_Schema((v) + (INT64_C(1))));

`Applicative [Schema]`'s `pure` is `schema/always`, which mallocs
`{12, value, 0, 0}`. The caller reads that pointer as an `Option`: `is_some`
lands on the tag word `12` (nonzero, so "some") and `value` on the payload. The
`42` is numerically correct **entirely by coincidence**.

Any functor whose layout does not happen to agree with Option's would return
garbage. This is a silent wrong-instance call, not a crash.

## Why it is dangerous for fixtures

A stdout-based fixture cannot see this. `tests/fixtures/hkt-constrained-byvalue-
bind-pure` originally used exactly this shape and passed while dispatching to
Schema; it was rewritten to put `pure` in the fn's own body, which genuinely
emits `__inst_Applicative_pure_Option`. **Do not add a `bind`-then-`pure` fixture
until this is fixed** -- it will pass while being wrong.

## Root cause -- a chain of four links

The lambda is lifted to a file-scope `__fn_<N>` and emitted once, so
`ctx->current_abi_specialization` is NULL in its body and
`emit_reresolve_disp_type` early-returns. Getting it cloned per specialization
means clearing four separate obstacles. Links 1-3 were implemented and verified
individually; **link 4 is the blocker** and the work was reverted because 1-3
without it only mint a clone nothing calls.

1. **The predicate is kind-`*` only.** `emit_call_dispatches_on_spec_tyvar`
   (`emit_module.c`) rejects anything but a bare `TY_TYVAR`. A higher-kinded call
   hands back the `(m int)` spine, so walk to its head first. (One-line change;
   verified.)

2. **The finder cannot see the lambda.** `emit_find_dispatch_spec_closure` only
   matches `EX_CLOSURE`. A NON-capturing continuation is lambda-lifted and packed
   by `EX_POLY_WRAP` around an `EX_VAR` reference, so it must also descend
   `EX_POLY_WRAP` / `EX_FN_TO_FAT` / `EX_POLY_TO_FAT` and match a lifted fn via
   `binding->source_fn_def`. (Verified.)

3. **The gate excludes constrained defns.** The caller runs the finder only when
   `fd->owner_instance` is set -- instance-method bodies. A constrained poly
   `defn` has the identical problem and never reaches it. Relaxing to
   `fd->owner_instance || fd->constraints.n_constraints > 0` admits it. (Verified;
   full suite stayed green at 2404.)

4. **The call site still references the ORIGINAL lambda.** With 1-3 in place a
   per-spec clone IS minted -- `__fn_1304__spec__int64_t_int64_t` appears -- but
   the enclosing spec still emits

       __inst_Monad_bind_Option(..., (tur_poly_fn_t){ NULL,
           (int64_t(*)(void*,int64_t))__poly_1306 })

   where `__poly_1306` is the `make_poly_wrapper` thunk built at ELABORATION time
   around the original `__fn_1304`. The wrapper binding is baked into the
   `EX_POLY_WRAP` node, so cloning the inner fn does not reroute anything; the
   wrapper needs a per-spec twin too.

## Fix directions

1. Land links 1-3 together with a fix for link 4, not before -- on their own they
   emit a dead spec.
2. For link 4, the natural place is the `EX_POLY_WRAP` emit (`emit_expr.c`, where
   `wn = raw_name_for_binding(wrapper_binding)`): inside an active specialization,
   if the wrapped inner fn has a registered per-spec clone, emit a twin wrapper
   forwarding to it. `ensure_aggregate_spill_shim` in `emit_module.c` is the
   existing pattern for minting a wrapper at emit time.
3. **Route B -- dictionary-pass instead of monomorphize.** Prototyped and
   reverted; see [Route B prototype](#route-b-prototype) below for what works and
   what blocks it.
4. Independently: prefer a representative whose layout is widest, or refuse to
   pick one when candidate layouts differ, so a miss degrades to a compile error
   rather than a silent wrong-instance call.

## Route B prototype

Route B is "stop depending on specialization: give the constrained poly fn its
dicts as parameters and let every class-method call be a dict-slot load". It was
prototyped end to end and reverted. Three of its four parts work; the fourth is
the same wall as link 4 above, and the compiler already has a diagnostic for it.

**Confirmed working.**

- *`pure` already lowers to a dict-slot load* inside a dict clone. The rank-2
  path emits, for a body of `(pure 7)`:

      static int64_t make__dict_1331(int64_t __dict_1332, int64_t x) {
          ... (((int64_t (*)(int64_t))((void **)(intptr_t)__dict_1332)[0])(7))

  So return-directed dispatch through a dictionary needs no new mechanism.

- *A direct call can be routed through the dict clone.* Adding a branch in
  `elab_call.c` that, for a call to a constrained HKT poly fn whose constraints
  all ground to concrete instances, calls `make_dict_clone` and prepends bare
  `EX_DICT` nodes for the instance singletons. The clone then receives one dict
  per constraint (`bump__dict(int64_t __dict_A, int64_t __dict_B, ...)`).

- *The carrier/by-value return bridge.* A dict clone returns the int64 carrier
  for every result (`emit_fns.c` forces that off `n_dict_clone`), so a call whose
  result is `(Option int)` needs bridging. Typing the call as the carrier and
  wrapping it in an `EX_ASCRIBE` to the aggregate reuses the existing
  carrier->concrete ascription bridge and works.

**The blocker: the continuation never gets the dict.**

With all of the above, the clone dispatches `bind` through `__dict_A[0]`
correctly, but still builds its continuation closure around the ORIGINAL lifted
lambda, which calls `__inst_Applicative_pure_Schema` directly. The second dict
parameter is passed and never read.

There is machinery for a nested lambda to read a captured dict from its closure
env -- `emit_call_dict_env_dispatch_index` / `ctx->cur_dict_env_*`, from
forall-dict-pass-nested-lambda-dispatch-plan Phase 2, driven on the elab side by
`dict_clone_nested_dispatch_rec`. Both its elab and emit gates require a
**receiver** that is a bare tyvar, so a return-directed `pure` (no receiver; the
constraint var is the head of the result spine) is invisible to them. Relaxing
both gates symmetrically to accept "result head is a tyvar" was implemented --
and the conversion still did not engage.

For a NON-capturing continuation the compiler refuses outright, and already says
so:

    TUR-E0311: 'bind-then-pure' dispatches a typeclass method on its constrained
    type variable from inside a directly-applied nested lambda that cannot be
    lowered (a lifted lambda called by name, with no closure env to carry the
    dict).

That diagnostic is the honest statement of the wall: a lifted non-capturing
lambda has no env, so there is nowhere to put the dict. Under Route B this fires
where the code previously compiled -- `tests/fixtures/hkt-constrained-pure-return-
dispatch` turns into a hard error -- which is why the prototype was reverted:
it converts working programs into errors without yet fixing the dispatch.

**What Route B needs to be landable**

1. Make `dict_clone_nested_dispatch_rec` / the dict-env conversion actually
   engage for return-directed methods (gate relaxation alone is not sufficient;
   find what else keys on the receiver).
2. Give a non-capturing continuation a closure env when its enclosing fn is
   dict-cloned, so TUR-E0311 stops being reachable -- otherwise Route B is a
   usability regression regardless of how correct it is.
3. Only then flip direct calls onto the dict path, with the by-value ascription
   bridge, and re-baseline the constrained-HKT fixtures.

## Related

- [constrained-hkt-byvalue-carriers.md](constrained-hkt-byvalue-carriers.md)
- `docs/archive/history/constrained-hkt-pure-return-dispatch.md` -- the gap-1
  fix, whose representative selection this inherits.
