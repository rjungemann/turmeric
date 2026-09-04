---
title: Container Element Form (CE) -- per-monomorph element words in Vec
category: Planning
description: Extend per-monomorph specialization from container RECEIVERS (done, the typed-pointer producer slice) to container ELEMENT STORAGE, so a niche `(Option String)` element lands in the Vec slot as its 8-byte word instead of a heap carrier box. Scoped to Vec; the form only diverges for niche elements (so no new flag), and it is the default since the niche graduated on 2026-09-03. The soundness invariant, the one chokepoint, a census phase before any codegen, and the never-silent rule for the erased residue.
---

# Container Element Form (CE)

**Status: CE0, CE1, CE2 and CE3 BUILT (CE3 closed 2026-09-04) and DEFAULT
since the option niche graduated 2026-09-03; CE4 deferred, CE5 done for
the container row.** The word convention and the TUR-E0714 / TUR-E0715
backstops are default-path facts now. The refinement of the "monomorphization
dependency" the [option-niche graduation hold](sr3-option-niche-plan.md)
points at, sketched 2026-08-28 in that plan's container-boxing section.

**The exit gate is met.** The container row that read parity before CE2
(`2e6 (Option String) vec elements: 0.080 s / 79.8 MB` both ways) now reads:

| representation | wall | peak RSS |
|---|---:|---:|
| default (16 B by-value, boxed per element) | 0.081-0.096 s | 79.7 MB |
| niche, CE_WORD slot | 0.018-0.019 s | 17.8 MB |

Niche allocations per element are zero -- 2e6 x 8 B of slots is the whole
RSS delta over the baseline. Same probe shape as the graduation measurement
(one shared String payload, -O2, three runs each).

**What CE2 found on the way, and fixed.** The class-2 (spec) path was
already WRONG under the experiment, on both halves: a generic
`(defn push-it [A] [v : (Vec A) x : A] (vec-push! v x))` specialized for
`(Option String)` heap-promoted the niche pointer into a `void **` cell
(the escaping bridge was asked at the still-unresolved tyvar, so its niche /
heap delegation never fired) and then the by-value-spec-param arm boxed the
cell pointer a second time, while the matching `get-it` spec did no
unwrapping at all -- so the first element read back blank. CE0's
"store/read agree" was measured only at class 1 and at `vec-eq-loop`'s read.
The same push spec failed to BUILD on the default path (a by-value
`(Option String)` element) for the same double-bridge reason; resolving the
bridge type inside the spec and gating the second arm fixed both. The read
half of the default-path wrapper is a separate pre-existing defect, filed as
[generic-vec-read-wrapper-spec-returns-carrier-word](../reported/generic-vec-read-wrapper-spec-returns-carrier-word.md).

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
**BUILT 2026-09-03.** `container_elem_form(Type)` (types.c) is defined AS
`repr_of(REPR_POS_CONTAINER_ELEM) == REPR_BOXED_AGG ? CE_BOX : CE_WORD`, so
the two cannot drift. The Vec element-store discriminator is the CE0
census's, kept as a decision (`emit_call_vec_elem_store`, emit_expr.c): the
callee declares `(Vec A)` and the slot as that `A`; class 1/2/3 as CE0
defines them. The class-3 diagnostic is **TUR-E0714**, raised at emit when
the STORED VALUE's resolved type is a niche option and the receiver's
element is still erased at the site (`tur explain TUR-E0714`). It is the
backstop the census said it would be: no in-tree program reaches it, a
concrete niche value unifies the receiver's element at any typed site, and a
raw-`:int` receiver is rejected by the checker before emit
(`(vec-push! v x)` with `v : int` is TUR-E0001) -- so no errors fixture can
be written for it from source today. It stays as the guard for a future
erased route (an existential element, a helper the checker admits), where
the silent alternative is two conventions in one vec.

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
**BUILT 2026-09-03, one commit, both halves through the ONE crossing
they already shared.** No `vec-push-word__` twin was needed: every niche
store and read already funnels through `emit_carrier_bridge`'s niche row
(emit_core.c), so the convention lives there.

- *Store.* The argument loop flags a Vec element-store argument whose
  resolved element is a CE_WORD niche option (`EmitCtx.ce_word_store_sink`,
  set after the argument's own emission and restored before the next); the
  niche row then hands the slot the payload word -- `(int64_t)(intptr_t)p`
  -- instead of `tur_box_some(p)`. An inline-C producer's box (recorded
  int64 spelling, `(vec-push! v (mk-c 1))`) is unboxed with
  `tur_opt_value_checked` and released, since the inline-C contract makes it
  the caller's. Every non-container crossing keeps the box, so an erased
  READER (`some?` base) still meets the tagged layout.
- *Read.* The call hoist marks the temp of a raw slot read (`vec-get`,
  `vec-pop!`, `vec-data-get-checked__` -- the latter is `vec-get-byval`'s
  core, which covers the spec path) in a side table
  (`emit_slot_word_mark`); the niche row's carrier->niche direction casts a
  marked temp instead of unwrapping it. `vec-of`, an unascribed read into a
  parameter, a match scrutinee, a struct field and `vec-pop!` all reach the
  same row.
- *Keys.* Both halves are keyed on the value (a side-table mark, a
  recorded spelling) or on the sink (the flag), never on a type alone, per
  the Risks section; a double-bridge is unrepresentable because the marked
  temp is consumed by exactly one bridge.

*Validation:* `tests/fixtures/option-niche-vec-word` (in the seam
population) asserts the word form at the language level -- a raw slot read
equals the unwrapped String's address, None is 0, the generic push/get
specs agree with the direct calls, the inline-C producer is unboxed into the
slot, and vec-of / pop / match / field / parameter reads all agree.
`option-niche-crossings` still passes with zero `tur_box_some` at its push
sites. Seam harness 10/10.

**CE3 -- higher-order and ownership.** `vec-eq?` (raw-comparator path),
Show/Eq instance loops, `vec-free` / `vec-pop!`: CE_WORD elements are
borrowed handles (nothing to free; ownership rules match today's heap
elements), CE_BOX unchanged. *Validation:* leak-check fixture
(`tests/run-leak-check.sh` marker) for a vec of niche options; vec-eq/show
fixtures under the flag.
**Mostly moot after CE2 (2026-09-03).** Ownership needed no change:
`vec-free` / `vec-pop!` / `vec-drop-last!` fold `tur-vec-elem-wide?` from
`repr_of`'s container answer, which already reports a niche element as
HEAP_PTR (not boxed), so they never freed a per-element box -- under the old
convention that was a LEAK of every carrier box the store minted, and under
CE_WORD there is nothing to free, which is exactly what the folds already
do. `vec-eq-loop` / `vec-show-loop` read through `vec-get` + ascription in a
class-2 clone and take the marked-temp path. What remains for CE3 is the
inline-C raw-comparator `vec-eq?` handing slot words to an ERASED closure
(one compiled against the int64 carrier, expecting a box). The graduation
(2026-09-03) split that residue in two. The SYNTHESIZED comparator -- what
`(eq? v w)` on a `(Vec (Option String))` lowers to -- is bridged: the
constrained-Eq synthesizer names its params `__cmp_slot_a`/`__cmp_slot_b`
and `emit_slot_word_is` recognises the prefix, so the value-keyed bridge
reinterprets the word (pinned by `option-niche-vec-closure-cmp`, seam
population). A USER comparator with untyped params that ascribes the word
itself, `(fn [a b] (eq? (:: a (Option String)) ...))`, was the residue --
filed as
[erased-closure-param-over-niche-vec-slot-reads-box](../archive/erased-closure-param-over-niche-vec-slot-reads-box.md).

**Closed 2026-09-04, and CE3 is done.** The residue split again along one
line: whether the comparator is written AT the call. A lambda there is
minted for that argument and reachable from nowhere else, so it can be given
the same `__cmp_slot_` mark the synthesized comparator carries -- the mark is
the emitted parameter name, which makes it self-scoping in a way the
program-wide value table is not. Both spellings reach it, a captureless
lambda through its lifted `__fn_N` binding (`is_lifted_lambda`) and a
capturing one as an `EX_CLOSURE` literal. A NAMED comparator cannot be
marked: it is elaborated once and another caller may hand it genuine boxes,
so marking its parameters would corrupt that caller instead. Erased
parameters there have no decidable convention and are refused with
TUR-E0715, the read-side twin of the store side's TUR-E0714. Every path is
now either correct or loud; typing the parameters remains the fix at the
site, and shape 4 of `option-niche-vec-closure-cmp` pins the inline form.

**CE4 -- decide Map/Set/HAMT by evidence, not momentum.** Keys need
hash/cmp over the word and the HAMT's own boxing story; the census from CE0
says whether any real code puts niche options in maps. Default disposition:
defer -- Vec was 100% of the measured parity cost.

**CE5 -- fold the result into the graduation calculus.** Re-run the
[measurement](sr3-option-niche-plan.md) container row; update the graduation
section. This plan does not itself argue for the flip.
**Container row re-measured 2026-09-03** (table at the top); the
graduation section in sr3-option-niche-plan.md records it. The flip is
still not argued here.

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
