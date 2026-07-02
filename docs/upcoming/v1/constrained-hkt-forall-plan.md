---
title: Constrained quantifiers + higher-kinded HRT vars (van Laarhoven lenses)
category: Planning
description: Lift the two restrictions that currently prevent encoding `forall f. Functor f => (b -> f b) -> (a -> f a)` -- (a) typeclass constraints on a `forall` bound variable, and (b) a `forall` bound variable whose kind is `* -> *`. LARGE; speculative; roadmap-only. Sliced.
---

# Constrained quantifiers + higher-kinded HRT vars

## Context

Today (`docs/guides/hrt-guide.md`) Turmeric supports rank-2/3 `forall` only
over kind-`*` type variables, with no class context. That makes the
van Laarhoven lens type

```
type Lens' a b = forall f . Functor f => (b -> f b) -> (a -> f a)
```

inexpressible -- both because `f` is `* -> *` and because the body needs
`Functor f` in scope. The HKT machinery (`docs/guides/advanced-type-system-rationale.md`)
and the typeclass dictionary infrastructure already exist; what is missing
is the surface syntax + elaborator wiring that connects them inside a
`forall`. This plan is **speculative roadmap material** -- not part of the
v1 ship gate -- but written so it can be cost-estimated against other v1+
work.

### What already exists (load-bearing)

- `Type.as.forall_` already carries `var_kinds[]` and the constraint pair
  `constraint_classes[] / constraint_var_idx[] / n_constraints`
  (`src/compiler/types.h:786-802`).  EX1b populated these for `exists`;
  `forall` reuses the same struct.
- `elab_types.c:980-1056` already **parses** the `(forall [vars] [(C v) ...]
  body)` shape for both quantifiers and rejects unknown classes / bad
  var-refs -- but the kind for each bound var is hard-wired
  (lowercase-letter heuristic: KIND_STAR or KIND_ROW, never
  KIND_ARROW{N}), and the constraint vector is only enforced for `exists`.
- `Kind` already encodes `KIND_ARROW{1..5}` (`types.h:60-83`), used by
  HKT-P1's `TY_APP`.
- Typeclass dictionary passing exists for top-level instances.

### What doesn't

1. **Surface syntax** for a kind annotation on a `forall` bound var.
   The parser currently infers kind from the first letter of the name.
2. **Constraint enforcement on `forall`** -- the `constraint_*` fields are
   read by exists' rc_cb/open paths, not by HRT call sites.
3. **Dictionary passing for rank-N constrained args** -- when a caller
   passes `inc : forall a. Functor f => ...` we need to either (i) emit a
   dictionary-augmented poly-fn carrier or (ii) monomorphise the call site.
   Neither path exists today.
4. **HKT instantiation under a quantifier** -- the HRT skolemiser only
   substitutes kind-* vars; substituting `f` with a kind-* -> * constructor
   (e.g. `Identity` for `view`, `Const r` for `over`) needs new substitution
   and unification code.
5. **Storage of poly-fn values that capture a dictionary** -- the current
   `tur_poly_fn_t = { void *env; int64_t (*)(void*, int64_t); }` has one
   payload slot. A constrained poly fn needs an additional dict pointer (or
   a fatter carrier).

The rest of this plan is the slice plan to close those gaps.

## Slices

Each slice ships behind a fresh experimental flag and adds fixtures before
landing. The flags are independent so we can stop after any slice without
regressing the shipped HRT path.

### Slice 1 -- Kind-annotated `forall` bound variables (`--enable=forall-kinds`)

**Status: landed.** Ships behind the `forall-kinds` experiment
(`--enable=forall-kinds`, registered in `src/runtime/experiments.c`; the
`-X` surface is retired). The quantifier parser in `elab_types.c` accepts a
`(name :: <kind>)` binder alongside the bare-symbol form, lowers the
arrow-kind grammar via `parse_kind_binder`/`parse_kind_seq`/`parse_kind_atom`,
and plumbs the resulting `Kind` into `var_kinds[]`/`ext_kinds[]` so the body
resolver sees `f` as higher-kinded in `(f a)`. Diagnostics `TUR-E0292`
(malformed kind form) and `TUR-E0293` (arity exceeds `KIND_ARROW5`) are wired.
Fixtures: `hrt-forall-kind-annotation/`,
`errors/hrt-forall-kind-arrow-malformed/`.

**Goal.** Replace the lowercase-letter kind heuristic with an explicit
kind annotation on each bound variable, so `forall [(f :: * -> *)]` parses
as `var_kinds[0] = KIND_ARROW`.

**Surface.** Sweet/plain agree:

```
(forall [(f :: * -> *)] (-> a (f a)))
forall [(f :: * -> *)] (-> a (f a))
```

The kind grammar is the existing `KIND_STAR..KIND_ARROW5` set spelled as
`*`, `(* -> *)`, `(* -> * -> *)`, ..., recursive. Unannotated vars keep the
current heuristic (back-compat).

**Work.**
- Parser: extend `elab_types.c:980-985` to recognise the
  `(name :: kind)` shape; lower the kind to a `Kind` value via a new
  `parse_kind_form` helper. ~40 LOC.
- Add `TUR-E0292` ("unknown / malformed kind form") and `TUR-E0293`
  ("kind arity exceeds KIND_ARROW5 -- raise the ceiling first").
- Plumb `var_kinds[i]` into the existing `ext_kinds[]` extension at
  `elab_types.c:983-984` so the body type-resolver sees `f` as a
  higher-kinded slot when used in `TY_APP`.
- Fixtures: `hrt-forall-kind-annotation/`,
  `errors/hrt-forall-kind-arrow-malformed/`.

**Not done by this slice.** The body can mention `(f a)`, but the call
site still rejects rank-2 args whose poly fn type quantifies a non-star
var (slice 3 lifts that).

### Slice 2 -- Constraint enforcement on `forall` (`--enable=forall-constraints`)

**Status: landed (mode A, static enforcement).** Ships behind the
`forall-constraints` experiment. The parser now accepts a constraint vector on
`forall` (gated; `elab_types.c`), reusing the existing exists constraint-parse
path, and `elab_poly_call` (`elab_call.c`) re-discharges each constraint at the
rank-2 instantiation site: it pins the constrained bound variable to the
concrete type from the matching argument and requires an in-scope instance,
raising `TUR-E0294` if none exists. Fixtures: `hrt-forall-constraint-show/`,
`hrt-forall-constraint-multi/`, `errors/hrt-forall-constraint-missing-instance/`.

**Mode decision -- (A), not (B).** Turmeric's HRT is *type-erased*: a rank-2
argument is a monomorphic function passed through the int64 `tur_poly_fn_t`
carrier, so for the `Show`-shaped constraints this slice targets, the passed
function already carries the right behavior and no runtime dictionary needs to
be threaded. The constraint's teeth are therefore a **static** obligation
checked at each instantiation site (mode A) -- which fully satisfies this
slice's goal and fixtures with **no carrier change and no poly-fn fixture
regen** (the plan's "largest blast radius" is avoided). Mode (B) -- widening
`tur_poly_fn_t` with a `void *dict` slot and threading dictionaries -- is only
actually required when the *callee* picks the type at runtime (the van
Laarhoven lens: `Identity` vs `Const r`). That work is deferred to the slice
that needs it (slice 3/4), where the existing `EX_EXISTS_DISPATCH` witness-table
machinery (runtime dict -> method pointer -> indirect call) is the template to
reuse. If a future slice requires the callee to choose a *constrained* `f`,
revisit (B) then.

**Goal.** Have `forall [a] [(Show a)] (-> a cstr)` actually require the
caller to supply a `Show` dictionary for the chosen `a` at each instantiation
site inside the callee.

**Work.**
- `elab_call.c` rank-2 path (~2425-5400) already pattern-matches
  `TY_FORALL` on arg-full-types; teach it to read
  `forall_.constraint_classes` and require an in-scope instance for the
  *instantiated* concrete type at each internal call to the poly fn.
- Reuse the typeclass-dispatch resolver (`typeclass_env_*`) -- this is the
  same lookup defns already do; the only new thing is "the type that fills
  `a` is decided per call inside the callee."
- Two enforcement modes (pick one in the plan; we recommend (B)):
  - **(A) Monomorphise per call site.** When the callee body calls
    `(f x)` with `x : int`, require `Show int` at *that* site, then emit
    a direct call to the monomorphic instance method. No carrier change.
    Cheaper but loses polymorphism inside the callee body.
  - **(B) Dictionary-passing.** Extend `tur_poly_fn_t` to
    `{ void *env; void *dict; int64_t (*)(void*, void*, int64_t); }` and
    have the call-site wrapper close over the resolved dict. The callee
    body invokes `f` once and gets the right method via vtable. Costs one
    extra pointer per poly fn and a wrapper-shape change.
  - The lens use-case **needs (B)** because the callee picks `f` (Identity
    vs `Const r`) and must dispatch `fmap` at runtime. (A) cannot encode
    that.
- Diagnostics: `TUR-E0294` ("no `Show` instance for `int` at this rank-2
  instantiation site -- required by `forall [a] [(Show a)] ...`").
- Fixtures: `hrt-forall-constraint-show/`,
  `hrt-forall-constraint-multi/`,
  `errors/hrt-forall-constraint-missing-instance/`.

**Wrapper-shape change risk.** The carrier widening is the largest blast
radius in this plan -- every poly-fn fixture's expected.c regenerates.
Schedule alongside a coordinated fixture regen window (see CLAUDE.md
"Fixture churn" policy).

### Slice 3 -- Rank-N over higher-kinded vars at call sites (`--enable=hkt-hrt`)

**Status: landed (type-level; carrier-compatible containers).** Ships behind
the `hkt-hrt` experiment. A rank-2 `forall` parameter may now quantify a
higher-kinded `f :: * -> *` used as `(f a)` in its body; both the pass site
(`elab_call.c`, the `EX_POLY_WRAP` block) and the invocation site
(`elab_poly_call`) gate on the flag and validate the instantiation: the type
filling `f` must be a type application whose base constructor kind matches f's
kind. Diagnostics: `TUR-E0295` (non-application argument), `TUR-E0296` (kind
mismatch). A prerequisite parser fix now preserves `arg_full_types` for a
`(F A)` argument in a `(-> ...)` type (previously dropped unless an arg was a
bare tyvar/quantifier), without which the `(f a)` body param was invisible
downstream. Fixtures: `hrt-hkt-option-instantiation/`,
`hrt-hkt-list-instantiation/`, `errors/hrt-hkt-non-application-arg/`,
`errors/hrt-hkt-kind-mismatch/`.

**Scope note -- carrier ABI.** Turmeric's HRT is type-erased, so a
*carrier-compatible* container (a parametric opaque / heap constructor, whose
value is the int64 carrier) flows through `tur_poly_fn_t` unchanged and works
end-to-end today. A **by-value aggregate product** container (a `defstruct` /
flat `defadt`, e.g. the stdlib `Option`) does not fit the erased carrier; it is
rejected cleanly with `TUR-E0297` rather than miscompiled. Lifting that -- via
the existing B4 box-at-store / deref-at-thunk bridge -- is tracked in
`docs/reported/hrt-hkt-aggregate-container-carrier.md` and is a prerequisite for
a lens over ordinary by-value containers. This matches the plan's original
framing: this slice delivers the *unifier/kind-tracking* novelty at the type
level; the aggregate-carrier codegen is deferred to the slice that needs it.

**Goal.** Allow a call site to pass a value whose type instantiates an
`f : * -> *` bound variable to a concrete `* -> *` constructor
(`option`, `list`, a user `defrec` of arrow kind, etc.).

**Work.**
- Skolemiser: today the HRT skolemiser allocates fresh `TY_TYVAR` nodes
  of kind `*`. Add `var_kinds[i]` propagation so the fresh skolem gets
  the bound var's kind, and teach the unifier to unify a
  KIND_ARROW{n} skolem with a `TY_APP`'s function head.
- `elab_call.c` arg-coercion: when the formal is `(f a)` and the actual is
  `(option int)`, decompose the actual into `f := option`, `a := int` and
  record both substitutions; reject if the actual is not a type
  application of the right arity.
- Dictionary resolution at call site (depends on slice 2): when the caller
  is the one choosing `f`, the caller supplies the `Functor f` dictionary;
  when the callee picks (rank-2 with both `f` and `a` quantified inside),
  the caller cannot supply anything and the callee must use whatever
  global instance is in scope at the instantiation point. (The lens case
  is the second.)
- Fixtures: `hrt-hkt-option-instantiation/`, `hrt-hkt-list-instantiation/`,
  `errors/hrt-hkt-non-application-arg/`,
  `errors/hrt-hkt-kind-mismatch/`.

### Slice 4 -- Lens-shaped end-to-end fixture + stdlib helper

**Goal.** Ship a single stdlib module `stdlib/lens.tur` that defines
`type Lens a b = forall [(f :: * -> *)] [(Functor f)] (-> (-> b (f b))
(-> a (f a)))` plus `view`, `set`, `over`, and one worked example
(record field lens). This is the acceptance gate.

**Work.**
- Lens combinator definitions (~80 LOC including `Identity`/`Const r`
  newtype Functor instances).
- End-to-end fixture `stdlib-lens-record-field/` that exercises
  view/set/over on a `defadt` record.
- Doc: `docs/guides/lens-guide.md` showing the encoding.

**No-go signal.** If slice 3 lands but the encoding still requires the
caller to thread `Functor f` manually because slice 2's dictionary path
can't see the callee-chosen `f`, lenses do not actually work as a
first-class abstraction and we should consider the alternate
"profunctor-by-record" encoding instead, which needs none of slices 1-3
but loses optic composition by ordinary function composition.

## Cost estimate (rough)

| Slice | Surface | Type system | Codegen | Tests/docs | Risk |
| --- | --- | --- | --- | --- | --- |
| 1 -- kind ann. | small | small | none | small | low |
| 2 -- constraints | small | medium | **carrier change** | medium | high (fixture regen) |
| 3 -- HKT HRT | small | **largest** (unifier, skolemiser, kind-tracking subst) | medium | medium | medium |
| 4 -- lens slice | medium | none | none | medium | low |

Slice 2's carrier change is the dominant risk. Slice 3's unifier work is
the dominant *novelty* -- it's the first place kind tracking actually has
to drive unification decisions rather than just being a tag.

## Open questions

1. **Should rank-2 dictionary passing also apply to `defclass` method
   parameters?** Today method dispatch resolves at the instance head, not
   inside a poly-fn arg. Slice 2 might want to subsume that.
2. **Effect-row interaction.** A constrained rank-2 fn whose body has an
   effect row -- does the constraint dictionary cross the effect boundary?
   Probably yes (dicts are pure), but worth a fixture.
3. **`exists` symmetry.** The slice 2 carrier change for `forall` would
   make `exists` and `forall` use the same constraint mechanism end-to-end;
   today `exists` enforces constraints via the rc_cb path. Worth
   consolidating in slice 2 or noting as a follow-up.
4. **Inference vs annotation.** This plan deliberately requires explicit
   `forall`/kind/constraint annotations everywhere. Bidirectional
   inference for any of these is out of scope and would be its own plan.

## Out of scope

- Rank-N inference (Turmeric does not infer `forall` today and this plan
  does not change that).
- Impredicative polymorphism (storing a `forall` inside a container).
  HRT's "container storage not supported" limitation stands.
- Profunctor / Van Laarhoven generalisations (Iso, Prism, Traversal); the
  plan's acceptance gate is `Lens'` only.
- Anything outside the v1 ship gate -- this plan is roadmap material only.

## Related

- `docs/guides/hrt-guide.md` -- shipped rank-2 mechanism
- `docs/guides/advanced-type-system-rationale.md` -- HKT + forall rationale
- `docs/guides/existential-types-guide.md` -- EX1b constraint storage that
  this plan reuses for `forall`
- `src/compiler/types.h:74-83` -- kind encoding
- `src/compiler/elab_types.c:980-1075` -- existing quantifier parser
