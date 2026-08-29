---
title: CE0 -- container element-form census results
category: Artifact
description: The corpus sweep CE0 gates on, both halves. 4699 element stores and 9021 element reads across 2111 fixtures. Zero class-3 sites reachable with a niche element on either half -- but the read half gets there differently from the store half, and the store-only census had a misleading shape. CE1's diagnostic suffices; the per-vec runtime flag stays declined.
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

## The gate: class-3 STORES reachable with a niche-eligible element

**Zero.** (The read half is below; both halves must be zero, and both are.)

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

## The read half -- and why the store number alone was misleading

9021 element reads. Scoped to Vec:

| class | Vec reads | Vec stores (for contrast) |
|---|---|---|
| 1 concrete | 82 | 4470 |
| 2 spec | 63 | 1 |
| 3 erased | **4321** | 5 |

The read side is almost entirely class 3, which inverts the store side's
shape. **This does not mean 4321 undecidable read sites**, and the raw number
is the trap: 1989 of the 2111 fixtures contribute exactly 2 of them, and all
4321 are `fn=vec-get`. They are ONE generic body -- `vec-eq-loop`
(stdlib/vec.tur:592, `(if (eq? (:: (vec-get x i) A) (:: (vec-get y i) A))` --
two reads on one line) -- re-emitted once per compilation unit because every
fixture links stdlib. The census counts emission sites, so a stdlib generic
body is counted once per fixture that links it.

The question that matters is whether a niche element can REACH such a site,
and it is a different question from the store side's. Answering it needed a
direct probe, because no corpus fixture puts a niche vec through a generic
comparator (`option-niche-crossings` only does `vec-push!`/`vec-get` at
concrete sites). Probe: build two `(Vec (Option String))` under the flag and
run them through `vec-eq-loop`. Result:

```
2 elem-read  class=2 form=word niche=yes cont=Vec elem=(type-app Option String) fn=vec-get
2 elem-store class=1 form=word niche=yes cont=Vec elem=(type-app Option String) fn=vec-push!
3 elem-read  class=3 form=word niche=no  cont=Vec elem=tyvar                    fn=vec-get
```

The niche element does not reach a class-3 read: the spec machinery mints a
`vec-eq-loop` clone in which `A` is concrete, so the read lands at **class 2**
-- decidable. The class-3 reads that remain are the un-specialized generic
BASE bodies, which carry `niche=no elem=tyvar` and never see a niche value on
this path.

So the gate answer is zero on both halves, and store/read AGREE per element
monomorph (6 niche stores / 6 niche reads in the corpus, both class 1; 2/2 in
the probe, class 1 store and class 2 read, same `form=word`). That agreement
is the property CE2 must preserve when it lands the two halves in one commit.

**Recorded because it nearly went the other way:** the store-only census
reported 5 class-3 stores and read as a clean result, and it would have been
easy to close CE0 on it. The read half's raw 4321 looked at first like a
refutation of that, and was not -- it is one stdlib body times the fixture
count. Neither number means anything without the other and without the
reachability probe. A future phase re-running this should resist reading
either column on its own.

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

**2. It counts EMISSION sites, not call reachability.** This is the limit the
read half exposes. The generic `vec-eq-loop` base body is emitted in every
compilation unit and its reads are class 3. The probe shows a niche element
routes to a class-2 SPECIALIZED clone instead -- but "specialization fires for
a concrete element type" is a claim about one measured path, not a proof that
nothing ever calls the generic base with a niche vec (a dictionary-dispatched
route that never specializes would). CE1 should treat that as the residue its
diagnostic exists for, rather than assuming specialization always wins.

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

CE0's stated validation is "zero behavior change (full suite untouched)".

- **Store half: DONE.** `bash tests/run.sh` against the store-tracer build:
  `summary: 2718 passed, 0 failed`, no FAIL lines, no binary-changed warning.
  That is the number CE0's validation line names.
- **Read half: DONE.** `bash tests/run.sh` against the store+read tracer
  build: `summary: 2718 passed, 0 failed`, exit 0, no FAIL lines. Identical to
  the store-half run, which is the point -- both tracers are inert without
  `--emit-abi-trace`.

Nothing in the census numbers above depends on either suite run: they come
from the `--emit-abi-trace` sweep, which is the instrumented path by
construction.
