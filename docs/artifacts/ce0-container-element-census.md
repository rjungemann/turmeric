---
title: CE0 -- container element-form census results
category: Artifact
description: The corpus sweep CE0 gates on. 4699 element stores across 2111 fixtures; 5 class-3 stores into a Vec, all existential-element, none niche-eligible. The class-3-reachable-with-a-niche-element count is ZERO, which is the branch where CE1's diagnostic suffices and the per-vec runtime flag stays declined.
---

# CE0 -- container element-form census

Sweep date: 2026-08-28. Compiler: `claude/seam-harness-registration-zqmrit`
at the CE0 instrumentation commit. Plan:
[container-element-form-plan.md](../upcoming/container-element-form-plan.md).

## Method

`ce0_trace_elem_store()` (emit_expr.c) prints one line per container element
store under `--emit-abi-trace`:

```
repr-trace elem-store class=<1|2|3> form=<word|box> niche=<yes|no>
           cont=<head> elem=<type> fn=<callee>
```

Swept every `tests/fixtures/*/` input through `emit-c` under
`--enable=option-niche` (the flag is required: `adt_app_is_niche_option` is
false with the experiment off, so a census run without it reports `niche=no`
everywhere and means nothing).

**2111 fixtures swept, 2098 emitted cleanly.** The 13 that did not are
`errors/` fixtures that are supposed to fail elaboration; they emit no
element stores either way.

Which argument is the element is read from the callee's DECLARED signature
(the element slot is the tyvar the receiver is applied to), not from resolved
types -- see the "false positives" and "blind spot" sections for what that
does and does not buy.

## Results

4699 element stores total.

| class | meaning | count |
|---|---|---|
| 1 | concrete -- receiver names the monomorph here | 4686 |
| 2 | spec -- inside a spec clone, tyvar concrete here | 4 |
| 3 | erased -- still a tyvar after resolution | **9** |

Scoped to `Vec`, which is the whole of CE1-CE3 (CE4 defers Map/Set):

| class | Vec stores |
|---|---|
| 1 | 4470 |
| 2 | 1 |
| 3 | **5** |

Vec element forms: 4443 `word`, 33 `box`. The CE_BOX population is small and
entirely by-value aggregates -- `Point` (10), `(Option int)` (8), `(Opt2 int)`
(6), `FzB` (4), `Sm` (2), `S` (2), `FzF` (1) -- which is the restatement the
plan predicted: CE_BOX is today's behavior for by-value aggregates and the
niche row is the only change.

`Map` / `Set` / `MutableMap`: 185 stores, **all class 1**. Relevant to CE4:
there is no erased-key/value store in the corpus to be unsound over.

## The gate: class-3 stores reachable with a niche-eligible element

**Zero.**

All 5 class-3 Vec stores have an EXISTENTIAL element type, from two fixtures:

| count | fixture | element |
|---|---|---|
| 3 | `w3-letrec-open-capture` | `(exists [a] [(Sz a)] a)`, `(exists [a] [(Sz2 a)] a)` |
| 2 | `vec-get-exists-element` | `(exists [a] [(Rdr a)] a)` |

These are erased **by design** -- the vec-get-existential-element shape, where
the receiver is declared `(Vec (exists [a] [(Rdr a)] a))` and the element
stored is a `pack`. A niche `(Option P)` cannot reach these slots: the
element type of such a vec IS the existential package, so an Option would be
stored inside a pack, not as the slot's own form. It is also structurally
impossible for the niche predicate to fire on one --
`adt_app_is_niche_option` requires `adt_app_type_arg_is_concrete`, and an
existential is not concrete.

The 6 `niche=yes` stores in the corpus are all **class 1** (`(Option String)`
into a `Vec`, via `vec-push!`), which is exactly the shape CE2 targets and
exactly the shape that is decidable locally.

**Disposition:** this is the favorable branch of CE0's gate. The invariant is
enforceable with the CE1 diagnostic on the residue; the per-vec runtime form
flag stays declined, as priced. CE1 should still emit the diagnostic -- the
census bounds today's corpus, not tomorrow's user code -- but it is a
backstop, not load-bearing machinery, and it breaks nothing that exists.

## What this census does NOT cover -- read before relying on it

**1. A fully `:int`-erased store is invisible to it.** The census keys on a
declared `(Container A)` parameter. A helper that takes its receiver as a
raw `:int` has no container type to key on and is structurally uncountable
here. This is the shape the plan's Risks section worried about most, so the
gap matters.

The one instance in stdlib is `seq-out-vec-push!`
(`stdlib/seq/consume.tur:50`, `[v : int val : int]`). It is **not a `(Vec A)`
store**: `seq-out-vec-new` mallocs its own
`struct { int64_t *data; size_t len, cap; }` that merely copies Vec's layout,
and only `seq-out-vec-push!` / `seq-vec-get` touch it. CE is Vec-scoped, so
it is outside CE's blast radius entirely -- but it is an independent
`:int`-stand-in defect against CLAUDE.md's rule and is worth typing on its own
merits, separately from CE.

Beyond that one, `stdlib/` contains **zero** real container-insert call sites
(every `vec-push!` / `map-set!` / `set-add!` occurrence in `stdlib/` is inside
a docstring or comment). So the "CE1's diagnostic breaks stdlib" risk has no
remaining candidate in the tree.

**2. Reads are not instrumented.** CE0's line in the plan names
`elem-store/elem-read`; only the store half is built, because the gate is
stated over stores. The read side (`(:: (vec-get v i) T)` and the iter/pop
paths) still needs its own pass before CE2 lands both halves in one commit.

**3. 38 lines are non-container false positives.** The declared-signature test
admits any ADT app whose last type argument is a tyvar, which catches
non-containers whose signature has that shape: `Option` (28, from
`unwrap-or [o : (Option A) default : A]`), `Dense` (6), `Box` (2), `Lens` (1),
`BoxF` (1). They are excluded from every Vec-scoped number above. Two of the
nine raw class-3 lines are `unwrap-or` and two are `dense-set!`; neither is a
container store, which is why the Vec class-3 count is 5 and not 9.

## Validation

Trace-only: `ce0_trace_elem_store` returns before doing anything unless
`g_emit_abi_trace`, so codegen is unchanged without `--emit-abi-trace`. That
is a structural argument, not a measurement.

CE0's stated validation is "zero behavior change (full suite untouched)". At
the time of this commit that run is **still in progress** -- 0 FAIL lines so
far, no summary line yet. It is not yet evidence, and this artifact does not
claim it as such; the result belongs in a follow-up commit here once the run
lands. Nothing in the census numbers above depends on it (they come from the
`--emit-abi-trace` sweep, which is the instrumented path by construction).
