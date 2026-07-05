# CM2 consumer-clone emit needs per-clone identity for the passed `g` closure

**RESOLVED (CM2 landed).** Implemented in `src/compiler/emit_module.c` (the CM2
emit loop after the VBM2b mono block), `emit_expr.c` (the consumer-clone redirect
override), and `mono_specs.c` (the abstract-spec accessors).  The realization was
simpler than the fix-direction feared: because the consumer fixes the functor,
the inner `g` is IDENTICAL across a consumer's lens clones, so ONE shared
by-value `g` twin (`<g>__byval`, emitted with a distinct env via
`env_name_override`) backs every clone -- no clone-vs-clone collision, and the
boxed Path A carrier `g` stays live for un-rewritten sites.  Each clone
(`<consumer>__lens_<hash>`) links the twin via `inner_closure_spec_idx` so its
`(l g s)` builds `g` by value (`thunk_sym_override`) and redirects to the matching
`<lens>__mono`.  The by-value `(f A)` twin result is built explicitly as
`(<functor> <focus>)` (g's lifted `return_type` is left abstract).  Fixture:
`van-laarhoven-lens-wide-consumer-clone` (run + `expected.c` snapshot).  Call-site
rewrite to the clones is CM3; this slice emits them (dead until then).

---

**Severity:** medium (blocks Slice CM2 of
[`van-laarhoven-consumer-mono-plan`](../upcoming/van-laarhoven-consumer-mono-plan.md);
CM1 landed and is unaffected). Not a miscompile in shipping code -- the
ambiguous-consumer case correctly stays on Path A today.

## One-line

Emitting box-free consumer clones for an ambiguous lens param (`|set| >= 2`)
requires each clone's inner `g` closure to be emitted as its own by-value thunk
body; the naive "reuse the consumer AST + redirect + toggle `g`'s box flag"
approach cannot produce that, because the lifted `g` thunk (`__fn_N`) is a single
top-level function shared by every clone AND the still-live Path A carrier.

## Minimal repro (the CM1 fixture already exercises the resolve)

`tests/fixtures/van-laarhoven-lens-wide-consumer-resolve/input.tur`: `set-px`
(and `over-px`) are each called with two distinct wide lenses `point-x` and
`point-y`, so CM1 resolves the lens param `l` to `{point-x, point-y}`
(`mono-spec-set fn=set-px lens-param=l -> {point-x, point-y}`). CM2's goal is to
emit `set_px__lens_<point-x-hash>` and `set_px__lens_<point-y-hash>`, each
box-free, each calling the matching `<lens>__mono`.

## Root cause

1. The `(l g s)` redirect (`src/compiler/emit_expr.c`, the `is_poly_call` block
   ~`:2767`) turns `(l g s)` into `<lens>__mono((int64_t)g, (int64_t)s)`. For the
   mono body to consume `g` by value, `g`'s `(f A)` result must NOT be boxed
   (`g->box_aggregate_result == false`).
2. For the unique case (`|set| == 1`) `mono_specs.c`'s `clear_g_box_walk`
   (`:356/488`) permanently clears that flag before emit -- safe because the
   single consumer body IS the redirected body; there is no boxed-`g` consumer.
3. For the ambiguous case the SAME `g` closure FnDef is shared by (a) every
   by-value clone and (b) the boxed Path A carrier body that un-rewritten call
   sites still invoke (CM3 has not rewritten them yet). `g` is lambda-lifted to a
   single top-level `__fn_N` whose box behaviour is fixed when that one function
   is emitted -- so a construction-time toggle around the clone's `emit_value(g)`
   (which only emits the *reference*, not the `__fn_N` body) cannot rebox it, and
   one `__fn_N` cannot be both boxed and by-value. Even with the carrier deleted,
   the two clones alone still collide on the shared `__fn_N`.

Net: any multi-clone emit needs **per-clone identity for the passed `g`**, not a
shared lifted thunk.

## Fix direction (reuse existing machinery)

The codebase already gives a *passed* closure a per-spec clone with its own
suffixed thunk/env symbol -- the hooks are in place:

- `EmitAbiSpecialization.is_passed_closure_clone` +
  `inner_closure_spec_idx` + `env_name_override`
  (`src/compiler/emit_internal.h:119-153`).
- The `thunk_sym_override` path in the `EX_CLOSURE` emit
  (`src/compiler/emit_expr.c:5094-5103, 5191`): when the active spec's
  `inner_closure_spec_idx` links a spec whose `binding == closure->fn->binding`,
  the constructed closure references the *clone's* thunk/env instead of the base
  `__fn_N`.
- `emit_find_passed_spec_closure` / `emit_find_dispatch_spec_closure`
  (`src/compiler/emit_module.c:1258/1264`) already locate a passed/captured
  closure so it gets a per-spec clone.

CM2 should, per (ambiguous consumer, concrete lens):

1. Intern an `EmitAbiSpecialization` for the consumer FnDef (no tyvar binding;
   the consumer is already type-monomorphic -- only the lens *value* differs).
   Bypass `emit_abi_intern_spec`'s dedup (it keys on `(binding, arg_types,
   result, bindings)`, which is identical across lenses) -- append directly or
   add a lens discriminator, else the two lenses collapse to one clone.
2. Intern a linked passed-closure spec for `g` with its box flag cleared and its
   `(f A)` result resolved by value; wire the consumer clone's
   `inner_closure_spec_idx` to it so the `EX_CLOSURE` emit picks up
   `thunk_sym_override` (a `__fn_N__lens_<hash>` twin).
3. Drive `(l g s)` to the right `<lens>__mono` per clone. The scaffolding for
   this (a `is_consumer_mono` + `consumer_lens_binding/name/hash` triple on
   `EmitAbiSpecialization`, honoured in the emit_expr redirect ahead of
   `mono_spec_redirect_for_binding`) was prototyped and reverted with CM1; it is
   the small part. The passed-`g` twin (step 2) is the real work.

CM1 already exposes everything the loop needs:
`mono_spec_count()` + `mono_spec_abstract_binding(i)` /
`mono_spec_abstract_enclosing(i)` (the latter two were prototyped alongside the
scaffolding; re-add as trivial accessors) and
`mono_spec_lens_set_count/get/sites`.

## Interactions to check when CM2 lands

- OQ #3 (self-recursive consumer): the content/lens-hash clone key must make a
  consumer that forwards `l` to itself call its OWN clone (a fixed point), not
  spin a fresh clone per depth.
- OQ #4 (drop glue): a clone that builds/discards intermediate `(f a)` by value
  must not double-drop -- same discipline as the VBM2 mono body.
- Keep everything behind `g_opt_vl_wide_mono` until CM4 graduates it.
