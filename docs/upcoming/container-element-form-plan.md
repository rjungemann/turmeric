---
title: Container Element Form (CE) -- per-monomorph element words in Vec
category: Planning
description: Extend per-monomorph specialization from container RECEIVERS (done, the typed-pointer producer slice) to container ELEMENT STORAGE, so a niche `(Option String)` element lands in the Vec slot as its 8-byte word instead of a heap carrier box. Scoped to Vec, inside `--enable=option-niche` (the form only diverges for niche elements, so no new flag). The soundness invariant, the one chokepoint, a census phase before any codegen, and the never-silent rule for the erased residue.
---

# Container Element Form (CE)

**Status: proposal.** The refinement of the "monomorphization dependency" the
[option-niche graduation hold](sr3-option-niche-plan.md) points at, sketched
2026-08-28 in that plan's container-boxing section. Nothing here is built.

## The problem, in one sentence

A Vec slot is one `int64_t` word, and every element type stores its **erased
carrier form** there; scalars and `:heap` pointers survive every context
because their carrier form IS their natural word, but a niche `(Option P)` is
the first eligible element type whose carrier form (a heap-allocated tagged
box) differs from its natural word (the payload pointer) -- so it pays a
malloc per element that its whole representation exists to remove, and the
[graduation measurement](sr3-option-niche-plan.md) shows exact container
parity with the by-value default.

## Why the box is where it is

Not in Vec -- `vec-push!` (stdlib/vec.tur:120) is element-agnostic inline-C
over `int64_t *data` that never interprets the word. The box is minted at the
**erased call boundary**: the element enters through `val : A`, and the
concrete->carrier crossing (`emit_carrier_bridge`, emit_core.c) materializes
the one form every erased consumer agrees on. The read side undoes it at the
`(:: (vec-get v i) T)` ascription. Store and read agree by both using the
universal convention; that agreement is the thing this plan must preserve
while changing the convention per monomorph.

## The soundness invariant (from the container-boxing sketch)

> The slot convention must be decidable at EVERY site that touches the slot.

Three site classes:

1. **Concrete sites** -- the receiver type names the monomorph
   (`(Vec (Option String))`). Decidable locally.
2. **Spec sites** -- a generic body under a Path A / SR2a spec clone
   (`push-it__spec__..._Option__String`): `A` is concrete inside the spec.
   Decidable.
3. **Truly erased sites** -- a body that never specializes (a dict-dispatched
   base, a `v : int`-typed helper like seq's `seq-out-vec-push!`), storing
   into a vec whose element monomorph it cannot name. **Not decidable.** A
   word-store here and a box-store at a concrete site would put two
   conventions in ONE vec, indistinguishable to every reader -- the
   silent-wrong-answer class the option-niche phase spent a week
   exterminating.

Class 3 is why the naive design is unsound and why the per-vec runtime flag
was priced and declined (new libturi ABI, a branch in every native, a second
source of truth). This plan's resolution is different: **measure class 3
first, and make it loud, never silent.** If the census (CE0) finds the
niche-element-through-truly-erased-store shape does not occur in the corpus,
the invariant is enforceable with a diagnostic on the residue -- an
elaboration-time TUR-E telling the author to ascribe the receiver -- instead
of runtime machinery. The polarity mirrors the niche eligibility allowlist:
an undecidable site refuses loudly; it never guesses.

## What already exists (do not rebuild)

- **Receiver typing.** `vec-new` / `vec-empty-like` mint typed-pointer specs
  (`tur_adt_Vec__Option__String *`) -- the producer-typing follow-through of
  the archived end-to-end-monomorphization plan.
- **Element-type specs against the niche form.** SR2a mints
  `some___spec__bool_void__(void *)` etc.; callbacks compiled per element
  monomorph already speak the word.
- **The decision function and its instrument.** `repr_of(t,
  REPR_POS_CONTAINER_ELEM)` (types.c) is the chokepoint this extends, and the
  repr-shadow machinery (emit_expr.c, `--emit-abi-trace`, Debug ICE) is the
  drift detector -- every representation campaign this year leaned on it.
- **The dual-ABI twin pattern.** `vec-get-byval [A]` (stdlib/vec.tur) reads
  `.data`/`.len` directly inside a spec over an inline-C word core
  (`vec-data-get-checked__`). The store side has no twin yet; CE2 writes it
  in the same shape. (The Option C twin *redirect*,
  `emit_abi_try_byval_twin_redirect` in emit_module.c, is a dormant stub
  since structdef-retirement DS-D -- reviving it is NOT required here: the
  redirect was about receiver ABI inside specs; element form keys on the
  receiver's element monomorph, which both the emitter's store/read sites and
  the twins can ask directly.)
- **Ownership folds.** The per-monomorph elem-wide/boxed constants that
  `vec-free` / `vec-pop!` read (see vec-pop!'s box-ownership docstring) --
  CE3 adds the form axis to the same folds rather than a parallel mechanism.

## Design

One new answer at one chokepoint, consumed by every element site:

```
container_elem_form(elem_ty) -> CE_WORD | CE_BOX
  CE_WORD: scalars, cstr, :heap pointers, pointer opaques, niche Options
           -- the value's one-word form is stored directly
  CE_BOX:  by-value aggregates (16B Option monomorphs, wide products)
           -- the slot holds a heap box pointer, as today
```

Everything except the niche row is a RESTATEMENT of current behavior --
scalars and heap pointers already store their word. The niche row is the only
change, which is why the whole plan lives inside `--enable=option-niche` and
needs no flag of its own: with the experiment off, `adt_app_is_niche_option`
is false everywhere and CE_WORD/CE_BOX reproduces today exactly. That also
means the existing option-niche seam harness (`tests/run-option-niche-seam.sh`)
is this plan's regression instrument for free.

Store sites consult it via the receiver's element monomorph and skip the
carrier materialization for CE_WORD niche elements (pass the word); read
sites skip the unbox. Callbacks (vec-eq? comparators, Show) are compiled per
element monomorph and already receive whatever the slot holds -- their
convention follows the slot's by construction once the slot is per-monomorph.

## Phases

**CE0 -- census before codegen** (the SR0 discipline). Instrument element
crossings under `--emit-abi-trace` (a `repr-trace elem-store/elem-read` line
naming site class 1/2/3 and the element form) and sweep the corpus plus
stdlib. Deliverable: the count of class-3 stores reachable with a
niche-eligible element type. Gate: if nonzero, enumerate the shapes before
CE2 decides between the diagnostic and a narrower scope.
*Files:* emit_expr.c / emit_core.c (trace lines only). *Validation:* sweep
log checked into `docs/artifacts/`; zero behavior change (full suite 2718/0
untouched).

**CE1 -- the chokepoint.** `container_elem_form()` in types.c beside
`repr_of`, plus the class-3 diagnostic (TUR-E, "cannot store a
niche-represented element through a fully erased container access -- ascribe
the receiver"), emitted only where CE0 says the shape is reachable.
*Validation:* errors fixture; suite green with the experiment off AND on
(the function restates current behavior until CE2 consumes it).

**CE2 -- Vec store/read under the experiment.** Store: the arg-loop /
ctor-loop / escaping-bridge sites that currently materialize the carrier for
a niche element into a container-store base skip to the word when the
receiver's element form is CE_WORD; a `vec-push-word__` twin in the
vec-get-byval shape covers the spec path. Read: `vec-get` + ascription and
the iter/pop paths return the word. Store and read land in ONE commit --
they are the two halves of one convention (the lesson of the vec-of
first-element bug, where two store sites disagreed within one expression).
*Files:* emit_expr.c, emit_core.c, stdlib/vec.tur. *Validation:*
`option-niche-crossings` extended with word-form assertions; the seam
harness; the container benchmark re-run -- the exit gate is the parity row
BREAKING (niche allocations per element drop to zero).

**CE3 -- higher-order and ownership.** `vec-eq?` (raw-comparator path),
Show/Eq instance loops, `vec-free` / `vec-pop!`: CE_WORD elements are
borrowed handles (nothing to free; ownership rules match today's heap
elements), CE_BOX unchanged. *Validation:* leak-check fixture
(`tests/run-leak-check.sh` marker) for a vec of niche options; vec-eq/show
fixtures under the flag.

**CE4 -- decide Map/Set/HAMT by evidence, not momentum.** Keys need
hash/cmp over the word and the HAMT's own boxing story; the census from CE0
says whether any real code puts niche options in maps. Default disposition:
defer -- Vec was 100% of the measured parity cost.

**CE5 -- fold the result into the graduation calculus.** Re-run the
[measurement](sr3-option-niche-plan.md) container row; update the graduation
section. This plan does not itself argue for the flip.

## Non-goals

- **Stride widening** (Rust-style `T data[]` at sizeof(T)) -- that is true
  storage monomorphization for BY-VALUE aggregates and a different, much
  larger program; CE keeps the one-word slot and changes only which one-word
  form the niche stores.
- **A per-vec runtime form flag** -- priced and declined in the
  container-boxing sketch.
- **Changing anything with the experiment off.** Every phase is inert
  without `--enable=option-niche`; the default path's element convention is
  untouched.

## Risks, named

- **The recorded-spelling keys.** The option-niche crossings key on the
  localvar side table; CE2's "skip the box" must use the same keys or the
  double-bridge class returns. Every CE2 site cites the crossing table in
  sr3-option-niche-plan.md.
- **`vec-pop!` ownership transfer.** Its box-ownership contract (docstring)
  becomes form-dependent; CE3 owns updating both the glue and the docstring
  in the same commit.
- **The class-3 diagnostic's blast radius.** If CE0 finds real class-3
  shapes in stdlib (seq's raw-int vec plumbing is the candidate), the
  diagnostic would break them -- the answer is typing those helpers, not
  weakening the rule; CE0's job is to find out how many there are before
  anything is promised.
