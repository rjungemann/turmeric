# Positional Nominal Type-Identity Fix -- Implementation Plan

> **Status:** Ready to implement (spike-validated)
> **Last Updated:** 2026-06-03
> **Type:** type-checker correctness fix
> **Blocks / unblocks:** [stdlib-opaque-handle-types-plan.md](stdlib-opaque-handle-types-plan.md)
> **Bug report:** [docs/reported/positional-nominal-type-identity-not-checked.md](../reported/positional-nominal-type-identity-not-checked.md)

---

## Why this plan is flat, not recursive

The bug report's "proposed fix directions" section is the kind of stub that
spawns a sub-plan ("first figure out where full types live", which spawns
"first figure out the encoding", which spawns "first measure the blast
radius", ...). This document collapses that recursion by **pre-resolving
every sub-decision with a measured answer**, so an implementer executes
phases top-to-bottom without stopping to investigate. Each phase below states
its decision, the evidence, the exact edit site, and how to know it worked.

All findings here were validated by a throwaway spike (both edits applied,
full build, full `tests/run.sh`) on 2026-06-03; the spike was reverted. The
headline measurement: **the fix passes the entire suite unchanged -- 1322
passed, 0 failed -- and the in-repo blast radius is zero.**

---

## Phase 0 -- Confirmed groundwork (no code; read once)

These were open questions in the report. They are now answered; do not
re-investigate.

| Question | Answer | Evidence |
|----------|--------|----------|
| How is `defopaque T` encoded? | A `StructDef` with `is_opaque=true`; the type is `TY_STRUCT` carrying that distinct `def` pointer. Renders as `<struct>`, lowers to `int64_t`. | `src/compiler/elab_structs.c:961-964`, `elab_toplevel.c:743-748` |
| Can the checker already tell two nominals apart? | Yes. `type_eq` compares structs by `def` pointer and ADTs by `def` pointer. The machinery exists; it is simply not invoked positionally. | `src/compiler/types.c:104` (struct), `types.c:108` (ADT) |
| Where does the positional check live, and what does it compare? | `bool arg_ok = (args[i]->type.kind == expected_arg_kind);` -- coarse `TypeKind` only. | `src/compiler/elab_call.c:2035` |
| Are full per-param types available at the call site? | Only when a param is polymorphic. `fn_type.as.fn.arg_full_types` is allocated under `if (any_poly)`; for an ordinary `:Chan`/`:P` param it is **NULL**. This is the real gap. | `src/compiler/elab_fns.c:2125-2129` |
| Do the variadic-rest and builtin paths already do this right? | Yes -- variadic rest keeps `rest_full_type` and compares by resolved type/name (`elab_call.c:~1850-1896`); builtins use full `type_eq` (`elab_call.c:1374`). The fix only needs to bring the positional `defn` path up to parity. | -- |
| Does `FnDef` carry full param types? | Yes (`Type *param_types`, `expr.h:311`), but the call site reaches the callee via a `Binding`/`fn_type`, not the `FnDef`, so `arg_full_types` is the practical channel. | `expr.h:307-311` |

---

## Phase 1 -- Make full param types available at every call site

**Decision:** Populate `arg_full_types` for **all** params, not just
polymorphic ones. Fall back to the param binding's own full type.

**Why not the alternatives (these are the sub-plans we are NOT spawning):**

- *Extend the `any_poly` condition to "any nominal param".* Functionally
  equivalent but adds a second predicate to maintain; the unconditional form
  is simpler and was measured safe.
- *Reach the `FnDef` from the call site and read `param_types`.* Requires a
  new `Binding -> FnDef` link (the `Binding` struct has none today, see
  `expr.h:43`) -- strictly more plumbing for no benefit.

**Edit site:** `src/compiler/elab_fns.c:2119-2130`, the
"attach full poly types for rank-2 params" block. Replace the `if (any_poly)`
gate with unconditional population:

```c
{
    Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
    for (uint8_t i = 0; i < n_params; i++)
        aFT[i] = param_poly_types[i] ? param_poly_types[i] : &params[i]->type;
    fn_type.as.fn.arg_full_types = aFT;
}
```

`&params[i]->type` is arena-stable (each `params[i]` is an arena-allocated
`Binding`), so the pointer outlives the call. Cost: one `Type*` array per
`defn`, in the arena -- negligible.

**Why this is safe (the interaction audit):** every existing consumer of
`arg_full_types` already gates on the *kind* of the stored type, never on
"is the array present", so always-populating cannot flip their behavior:

| Consumer | Guard it uses | Effect of always-populate |
|----------|---------------|---------------------------|
| rank-2 detection | `aft->kind == TY_FORALL` (`elab_call.c:2020-2031`) | none -- nominal param's full type is `TY_STRUCT`, not `TY_FORALL` |
| poly ADT field `(Some 1.5)` | `expected_arg_kind == TY_INT && aft2->kind == TY_TYVAR` (`elab_call.c:2074-2082`) | none -- different expected kind |
| GS2 applied-type tightening | `call_type_has_named_tyvar(expected_full)` else `TY_APP` (`elab_call.c:2123-2134`) | none for a plain struct full type |
| union subtyping | `union_t->kind == TY_UNION` (`elab_call.c:2149`) | none |
| emit param typing | uses it as the param type directly (`emit_fns.c:24,420,496,636`) | none -- it now equals what `fd->param_types` already gave |

**Verify:** build; `bash tests/run.sh` stays at 0 `FAIL` **with this phase
alone** (no call-site change yet). The spike confirmed this.

---

## Phase 2 -- Tighten the saturated positional check

**Edit site:** `src/compiler/elab_call.c`, immediately after line 2035
(`bool arg_ok = (args[i]->type.kind == expected_arg_kind);`), before the
existing escape-hatch chain. Demote `arg_ok` when the kinds match but the
nominal identities differ:

```c
/* Nominal identity: a same-kind struct/opaque/ADT argument must be the
 * *same* type, not merely the same TypeKind.  Full param types come from
 * arg_full_types (Phase 1).  Placed before the escape hatches: those only
 * ever set arg_ok from false->true for cross-kind coercions, so demoting a
 * spurious same-kind match here cannot resurrect a real coercion. */
if (arg_ok && (expected_arg_kind == TY_STRUCT || expected_arg_kind == TY_ADT) &&
        fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
    uint32_t nidx = fn_binding->closure_fn_binding ? i + 1 : i;
    if (nidx < fn_type.as.fn.arity) {
        Type *ef = fn_type.as.fn.arg_full_types[nidx];
        if (ef && (ef->kind == TY_STRUCT || ef->kind == TY_ADT) &&
                !type_eq(args[i]->type, *ef)) {
            arg_ok = false;
        }
    }
}
```

**Placement rationale:** the ~15 hatches at `elab_call.c:2038-2166` are all
`if (!arg_ok && <cross-kind condition>)`. None matches "expected struct, got
a different struct", so none will re-accept a value we demote. When `arg_ok`
remains false after the chain, control reaches the existing
`TUR_E0001_TYPE_MISMATCH` emitter -- no new diagnostic code path needed.

**Restrict to `TY_STRUCT`/`TY_ADT` only.** Do **not** extend to `TY_INT`
(opaques over int already lower to int but the HKT paths at
`elab_call.c:2093-2106` deliberately pass structs/ADTs/apps *at* int params);
do not touch `TY_PTR_VOID` (closure/callback coercions live there). The
nominal tightening fires *only* when expected and actual kinds are equal and
both are nominal.

**Verify (spike-confirmed):**
- `(take-a (mk-b))` with `A`,`B` distinct opaques -> `TUR-E0001` "got B".
- `(take-p (make-struct Q ...))` with distinct structs -> `TUR-E0001` "got Q".
- `(take-a (mk-a ...))` (same opaque) -> still type-checks.
- `(take-a 5)` (int at opaque) -> still rejected (pre-existing).
- Full `tests/run.sh` -> 1322 passed, 0 failed (unchanged).

---

## Phase 3 -- Secondary call paths (completeness sweep)

Phase 2 fixes the saturated direct-call path (`elab_call_fn`). Audit and, if
needed, apply the same demotion at the other places that read coarse
`arg_kinds`. Each is listed with its site; the spike showed none are exercised
by current fixtures, so treat these as belt-and-suspenders to prevent the hole
re-opening through a different door.

1. **Partial application** -- `elab_call.c:1436`
   (`TypeKind cap_kind = fn_type.as.fn.arg_kinds[...]`). Under-saturated calls
   that bind a nominal handle into a closure. Apply the same
   `arg_full_types`-based `type_eq` demotion for `cap_kind`'s captured args.
2. **Remaining-arg kinds for the returned closure** -- `elab_call.c:1451-1456`
   (`rem_kinds`). These describe the *not-yet-supplied* params; they feed the
   thunk type, so nominal identity is enforced when the closure is later
   saturated (which routes back through Phase 2). No edit expected -- confirm
   by test, see Phase 5.
3. **Typeclass method dispatch** -- bare-name and dotted method calls route
   through `elab_method_call` / `elab_call_fn` (`elab_call.c:1005-1112`,
   `1157-1293`). Because they ultimately call `elab_call_fn`, Phase 2 covers
   them; add a fixture (Phase 5) rather than code.

Deliverable of this phase: a one-paragraph note in the PR confirming each
path is either covered by Phase 2 or carries its own demotion, with the test
that proves it.

---

## Phase 4 -- Diagnostic quality

Today the demoted-arg message reads `expected <struct>, got B`: the *actual*
side names the type (`B`) but the *expected* side renders the generic
`<struct>` because it is built from a bare `TypeKind`. Improve the expected
side to name the type using `arg_full_types[nidx]`.

- Locate the `TUR_E0001` emission for the user-defn path (the one that
  prints `expected %s, got %s` after the hatch chain).
- When `arg_full_types[nidx]` is present, render the expected string from it
  (struct/opaque/ADT `def->name`) instead of `type_name(expected_kind)`.
- `type_render`/`type_name` already resolves struct/ADT names elsewhere
  (`types.c:1163`, `:499`, `:805`); reuse that path so the message becomes
  `expected A, got B`.

This is cosmetic but high-value for the opaque-handle plan, whose whole point
is legible "wrong handle" errors. Keep it a separate commit so the
behavioral fix (Phases 1-2) can land even if message wording bikesheds.

---

## Phase 5 -- Fixtures and validation

Mirror the existing variadic error fixtures
(`tests/fixtures/errors/variadic-rest-opaque-mismatch`, `...-int`,
`...-opaque-int`) on the positional axis.

**New error fixtures** (each expects `TUR-E0001`):

- `errors/positional-opaque-mismatch` -- distinct opaques, `B` at `A`.
- `errors/positional-struct-mismatch` -- distinct structs, `Q` at `P`.
- `errors/positional-adt-mismatch` -- distinct ADTs.
- Keep the existing `errors/...-opaque-int` behavior (int at opaque still
  errors) -- already passing; add a positional variant if not covered.

**New positive fixtures** (must compile + run):

- `positional-opaque-ok` -- correct handle flows through (`A` at `A`).
- A polymorphic-ADT regression guard (`(Some 1.5)`-style) -- proves the
  `TY_TYVAR`-field path at `elab_call.c:2074` is untouched. (The suite
  already covers this; make it explicit.)
- A typeclass-method call passing a nominal value (covers Phase 3 item 3).
- A partial-application case binding a handle then saturating (covers Phase 3
  items 1-2).

**Suite gates:**

1. `bash tests/run.sh` -> zero `FAIL` (compiled fixtures, ASan/LSan on).
2. Regenerate any `expected.c` only if codegen actually changed -- it does
   not for this fix (pure front-end type check), so expect **no** snapshot
   churn. If a snapshot diff appears, investigate; do not blanket-regenerate.
3. Spot-check the interpreter harnesses if they exercise opaque args
   (`run-turi.sh`); the spike touched only the elaborator shared by both.

---

## Phase 6 -- Hand-off to the stdlib opaque-handle sweep

With Phases 1-2 landed, the headline guarantee in
[stdlib-opaque-handle-types-plan.md](stdlib-opaque-handle-types-plan.md)
finally holds: `(async-chan-send sync-chan ...)`, `(future-get pool)`,
`(condvar-wait mutex condvar)` become compile errors. Resume that plan with
two carry-over notes discovered here:

- **`ptr<void>` sinks need an explicit cast.** A `defopaque H :ptr<void>`
  value will *not* implicitly satisfy a plain `:ptr<void>` parameter (the
  kinds differ: `TY_STRUCT` vs `TY_PTR_VOID`), so any handle passed to an
  `extern-c`/inline-C sink typed `:ptr<void>` (e.g. `reactor-add-chan` ->
  `tur_reactor_add_chan`) needs `(:: h :ptr<void>)` at the call site. This is
  the "intended source-level break" the plan already anticipates; it surfaced
  immediately in `tests/fixtures/reactor-fibers-park-chan` during the chan
  probe.
- **Constructor inline-C returns int64_t.** An opaque-over-`ptr<void>`
  function returns `int64_t` in the C ABI (like `Panic`), so an inline-C body
  ending `return (void *)p;` warns; use `return (int64_t)(intptr_t)p;`
  (matches `stdlib/panic.tur`).

---

## Risk ledger

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Existing program relied on loose same-kind passing | **Measured zero** in-repo (1322/0) | Each surfaced case is a latent bug to fix, not a reason to loosen the check; downstream `.tur` may differ -- call out in changelog |
| Always-populate perturbs a poly/HKT path | Low (interaction audit above) | Phase 1 verified standalone before Phase 2 |
| Snapshot churn | None expected (front-end only) | Phase 5 gate 2 |
| Diagnostic wording churn | Cosmetic | Phase 4 isolated in its own commit |
