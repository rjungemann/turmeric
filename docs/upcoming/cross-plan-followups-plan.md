# Cross-Plan Followups

> **Status:** F1-1, F1-2, F1-3 (retire), F2-1, F2-2-2 shipped.
> Existential safety completeness phase done.  F1-1 =
> constrained-existential return/param SEGV fix.  F1-2 = move-at-pack
> + smart drop hook for RCEXP_RC payloads (fixture
> `exg5-rc-in-exists`).  F1-3 = formally retired EXG4-3.  F2-1 =
> defn-boundary diagnostic for escaping `:linear` existentials.
> Remaining open:
>   - `exg4-pack-into-struct` (blocked on `defstruct` compound-type
>     parser),
>   - F2-2-1 cycle-construction fixture (same parser blocker),
>   - F3 dictionary passing (largest item, independent),
>   - F4 ^deprecated attribute,
>   - F5 MutableMap[K V],
>   - F6 turi gaps,
>   - F7 doc hygiene (partially done as fix-and-flip in the affected
>     plans).
> **Last Updated:** 2026-05-23
> **Type:** Compiler / Runtime / Stdlib / Docs

---

## Overview

The recent existential-types and typed-collections work landed in
several waves, each of which explicitly deferred a small follow-up so
the main change could ship clean.  Those deferrals have now
accumulated, and several of them are blocking each other.  This plan
gathers them into one place so they can be planned and prioritised
together.

Source plans the work threads back to:

- `docs/upcoming/existential-gc-followup-plan.md` (EXG4/EXG5/EXG6)
- `docs/existential-types-plan.md` (EX1/EX2)
- `docs/existential-gc-plan.md` (EXG1/EXG2/EXG3 -- now superseded by
  the followup plan, but the doc itself still needs a status update)
- `docs/ptc-plan.md` (PTC4 forward-looking note)
- `docs/typed-collections-plan.md` (deprecation warnings, mutable
  variants, interpreter gaps, recursive `.eq?` dispatch)

The phases below are independent and can land in any order, with the
single sequencing constraint called out under F1.

---

## Dependency Graph

```
F1-1 (parser fix)
  |
  +--> F1-2 (smart drop + pack semantics)
  |       |
  |       +--> F2-2 (cycle-construction tests)
  |
  +--> F2-1 (linear escape rejection)
  |
  +--> reopens EXG4-pack-return / EXG4-pack-into-fn /
       EXG4-pack-into-struct fixtures from the followup plan

F3 (typeclass dictionary passing)
  +--> unblocks Vec[Vec[int]] / Map[K (Vec V)] etc.

F4 (^deprecated attribute)
  +--> retires the docstring-only deprecation in typed-collections

F5 (MutableMap[K V]) -- independent

F6 (turi gaps) -- independent

F7 (doc hygiene) -- independent, blocks nothing
```

---

## Phase F1 -- Parser + existential semantics gap-closers

### F1-1 -- Parser fix for constrained-existential type annotations

**Problem.**  Today the only way to spell a constrained existential as
a function return or parameter type is the compound annotation form,
e.g.

```turmeric
(defn make [] :(exists [a] [(Show a)] a)
  (pack 1 (exists [a] [(Show a)] a)))
```

The annotation parses, but the elaborator builds a `Type` whose
`as.forall_` payload is invalid, and the first downstream consumer
(`elab_open` at `src/compiler/elab_types.c:1793`) SEGVs with a
misaligned read trying to walk `packed->type.as.forall_.body`.

This blocks:

- The plan's `exg4-pack-return`, `exg4-pack-into-fn`, and
  `exg4-pack-into-struct` fixtures (EXG4-5 in
  `existential-gc-followup-plan.md`).
- A natural spelling for `errors/exg6-linear-escape` (F2-1).
- Any user-level API that returns or accepts a constrained existential
  by value.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F1-1-1 | Audit `type_expr_from_form` for the `:(exists ...)` compound case -- **done** (the compound case parses fine; the payload was populated correctly inside `type_expr_from_form`).  The bug was downstream in `elab_fns.c` losing the payload at return-type capture time. | `src/compiler/elab_types.c` |
| F1-1-2 | Fix any field the audit shows uninitialised -- **done** via F1-1-3 (no fix needed in `elab_types.c`). | `src/compiler/elab_types.c` |
| F1-1-3 | Mirror the fix in `elab_fns.c`'s F_TYPE_ANN handling so the function's `result_full_type` carries the full TY_EXISTS/TY_FORALL payload -- **shipped** (`elab_fns.c`: capture `return_exists_type`, attach to `result_full_type`; `elab_call.c`: patch call-expression type from `result_full_type` on TY_EXISTS/TY_FORALL).  Same change also gives constrained-existential parameters a value-typed path (was previously mis-routed through the rank-2 branch which rejected `pack` arguments). | `src/compiler/elab_fns.c`, `src/compiler/elab_call.c` |
| F1-1-4 | Add fixture `tests/fixtures/ex-exists-return-type` -- **shipped**. | `tests/fixtures/` |
| F1-1-5 | Add fixture `tests/fixtures/ex-exists-param-type` -- **shipped**. | `tests/fixtures/` |
| F1-1-6 | Ship the previously blocked EXG4-5 fixtures -- **partial:** `exg4-pack-return` and `exg4-pack-into-fn` shipped.  `exg4-pack-into-struct` blocked on `defstruct`'s field-type parser only accepting simple keyword/sym types, not compound `(exists ...)` annotations -- track that fix separately. | `tests/fixtures/` |

### F1-2 -- Smart drop hook for existential RC payloads + pack semantics

**Problem.**  EXG5 added the cycle-walker visibility for existentials
whose payload is itself an `rc<T>` (kind tag `RCK_EXISTENTIAL` +
payload `RCEXP_RC`).  What it deliberately skipped is the matching
*ownership* semantics:

- `pack` of an `rc<T>` value today stores the inner control-block
  pointer in the existential's `value` slot without incrementing it
  and without marking the source binding moved.
- The existential's drop hook (`tur_existential_drop`) is still a
  no-op, so freeing the outer existential does **not** decrement the
  inner rc.

Adding either piece alone would break things.  Adding both together is
the natural completion of EXG5 and unblocks the genuine cycle-collection
test (F2-2).

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F1-2-1 | Decision: **move-at-pack** (followup-plan recommendation).  Cheaper than clone-at-pack; matches the existing `rc/clone` ergonomic where users opt into shared ownership explicitly. | design decision |
| F1-2-2 | Clone-at-pack: rejected by F1-2-1. | -- |
| F1-2-3 | Move-at-pack in `elab_pack` -- **shipped**.  When the packed value is an `EX_VAR` of TY_RC, TY_WEAK, or TY_EXISTS, mark the source binding moved via `binding_mark_moved`.  The surrounding scope's auto-drop pass then skips it, leaving the existential as the sole owner of the inner rc reference. | `src/compiler/elab_types.c` |
| F1-2-4 | Smart drop hook -- **shipped**.  Runtime `rc_cb_free` and the matching inline runtime in `emit_module.c` (both `rc_free_queue_drain` and `rc_weak_decrement`'s zombie-free path) dispatch on `reserved[0] == RCK_EXISTENTIAL` + `reserved[1] == RCEXP_RC` and call `rc_strong_decrement` on the inner cb before the outer drop hook fires.  Layout-mirror the GC walker's existing read of `*(int64_t*)cb->value`. | `src/runtime/rc.c`, `src/compiler/emit_module.c` |
| F1-2-5 | Hook-vs-dispatch choice: **dispatch in `rc_cb_free`** (and the inline analogue) rather than swapping `tur_existential_drop` per-program.  The single teardown entry point keeps the per-program drop hook a layout-free no-op, and the runtime library never needs to know about the codegen-generated struct beyond reading the first 8 bytes of `cb->value`. | `src/compiler/emit_module.c` |
| F1-2-6 | Fixture `tests/fixtures/exg5-rc-in-exists` -- **shipped**.  Packs an `rc<int>` into a constrained existential, opens it, and exits cleanly (no leak, no double-free).  The would-be fixture `exg5-rc-in-exists-gc-roundtrip` (using `weak` + `upgrade` to assert post-collection state) was dropped because the compiler currently leaks 24 bytes from `fresh_tmp` on emitting `weak`/`upgrade` paths -- a pre-existing leak (also fails `weak-dangling` baseline) unrelated to F1-2 logic.  Track that compiler leak separately. | `tests/fixtures/` |

### F1-3 -- Storage-site auto-clone (EXG4-3)

**Problem.**  The followup plan deferred EXG4-3 (auto-clone of
constrained existentials when stored into a struct field, vec slot, or
async capture) on the grounds that current `rc<T>` baseline does not
auto-clone there either and the plan's premise was inaccurate.  If we
decide auto-clone *is* desired across the board, the same change should
apply to `rc<T>` first, then extend to constrained existentials.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F1-3-1 | Decision: **retire** EXG4-3 -- **shipped**.  The original EXG4-3 design proposed auto-cloning constrained existentials at storage sites on the premise that `rc<T>` already does so.  The premise is wrong: `rc<T>` requires explicit `rc/clone` at struct/vec/async storage sites.  Bringing the `rc<T>` policy to auto-clone is a much larger user-visible change and is out of scope for this plan.  Constrained existentials follow the established `rc<T>` discipline: explicit `rc/clone` at storage sites. | design decision |
| F1-3-2 | Implementation: not needed -- **n/a** (retire path chosen). | -- |
| F1-3-3 | Doc updates: backfill `existential-gc-followup-plan.md` to flip EXG4-3 from "open" to "retired" and update the Relation-to-Other-Plans section to drop the "EXG4-3 is what unblocks Vec[Showable]" framing in favour of explicit-clone -- **shipped**. | docs |

---

## Phase F2 -- Existential safety completeness

### F2-1 -- `errors/exg6-linear-escape`

**Problem.**  The EXG6 plan called for a diagnostic when a `:linear`
existential is returned from a `defn`.  Today the natural spelling
(declare the return type as the linear existential) hits F1-1's parser
crash.  Erasing the return to `:ptr<void>` silently strips the linear
discipline, leaking the record.

**Tasks (depends on F1-1).**

| ID | Task | File(s) |
|----|------|---------|
| F2-1-1 | Decision: **defn-boundary check** -- a single deterministic check at return-type annotation parsing, independent of body shape (direct pack, let-tail, conditional return).  Simpler than flow analysis at the pack site, and the linear-escape contract is naturally a property of the function's return type. | design decision |
| F2-1-2 | If at pack: rejected by F2-1-1 decision. | -- |
| F2-1-3 | At defn: in `elab_fns.c` return-type parsing, when the annotation resolves to a `TY_EXISTS` with `is_linear=true`, emit a diagnostic -- **shipped**. | `src/compiler/elab_fns.c` |
| F2-1-4 | Add fixture `tests/fixtures/errors/exg6-linear-escape` -- **shipped**. | `tests/fixtures/errors/` |

### F2-2 -- Cycle-construction tests for existentials

**Problem.**  EXG5 shipped three mechanical fixtures verifying the kind
tag is written and the walker doesn't crash.  The plan's real
cycle-collection tests (`exg5-exists-cycle`) are blocked on F1-2
(smart drop + correct pack semantics).

**Tasks (depends on F1-1 and F1-2).**

| ID | Task | File(s) |
|----|------|---------|
| F2-2-1 | Cycle-construction fixture -- **deferred**: building a cycle requires mutability through an `rc<Cell>` field that points back into the existential, which the current `defstruct` field-type parser cannot express cleanly (same compound-annotation gap that blocks `exg4-pack-into-struct`).  Re-attempt once that limitation lands. | `tests/fixtures/` |
| F2-2-2 | Roundtrip fixture -- **shipped via F1-2-6** as `exg5-rc-in-exists`.  The would-be `exg5-rc-in-exists-gc-roundtrip` companion (weak + upgrade) was dropped because it trips the same pre-existing compiler-side `fresh_tmp` leak that also fails `weak-dangling` -- not an F1-2 regression, track the compiler leak separately. | `tests/fixtures/` |

---

## Phase F3 -- Recursive typeclass dispatch through dictionaries

**Problem.**  PTC4 shipped constrained `definstance` for collections
(`Eq[Vec[A]]`, `Eq[Map[K V]]`, etc.), but element comparison inside
the instance bodies uses integer `=` instead of dispatching through
`.eq?` on the element type.  This is correct for primitive elements
(`int`, `bool`, `cstr`) but breaks for nested collections like
`Vec[Vec[int]]` and `Map[K (Vec V)]`.  Both PTC and typed-collections
plans flag this as deferred to a future phase with identical wording:

> Full recursive structural equality (e.g. `Vec[Vec[int]]`) requires
> dictionary passing to method bodies.

(See `docs/ptc-plan.md` "Remaining Work After PTC4" and
`docs/typed-collections-plan.md` "Blocking Work: Phase PTC4" -- both
end with this exact note.)

The blocker is that method bodies do not currently receive their
constraint dictionaries as runtime parameters, so `.eq?` inside a
`definstance Eq [Vec] [(Eq A)]` body has nothing to dispatch through
beyond the syntactic name.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F3-1 | Design dictionary-passing call convention: for a constrained instance method `f`, the generated function takes one extra hidden parameter per constraint -- a pointer to the resolved typeclass dictionary at the call site.  Document the convention. | design + `docs/` |
| F3-2 | Extend `elab_typeclasses.c` to elaborate constrained-instance methods with the hidden dictionary parameters in scope, accessible by the constrained typeclass name. | `src/compiler/elab_typeclasses.c` |
| F3-3 | Extend the method-dispatch loop to thread the resolved dictionaries from the call site into the hidden parameters. | `src/compiler/elab_typeclasses.c` |
| F3-4 | Update `emit_fns.c` to emit the extra parameters and `emit_expr.c` to pass the dictionaries at the call. | `src/compiler/emit_fns.c`, `emit_expr.c` |
| F3-5 | Replace the integer-`=` element comparison in each `t*.tur` constrained `Eq` instance with a dispatch through the element's `.eq?`. | `stdlib/t*.tur` |
| F3-6 | Add fixture `tests/fixtures/typed/tvec-of-tvec-eq` and `tests/fixtures/typed/tmap-of-tvec-eq` covering recursive structural equality. | `tests/fixtures/typed/` |

---

## Phase F4 -- `^deprecated` attribute system

**Problem.**  `typed-collections-plan.md` shipped per-function
deprecation notices in docstrings (`Deprecated:` section) for the
untyped collection modules.  The compile-time warning that DEP-1/DEP-2
originally called for was deferred because it needs a generic
`^deprecated` attribute on definitions.  Without compiler-level
warnings, users do not see the deprecation until they read the API
docs by hand.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F4-1 | Add `^deprecated` to the recognised binding/def annotations alongside `^mut`, `^persistent`, `^linear`, etc.  Accepts an optional message: `(defn ^deprecated "use tvec-push! instead" old-push ...)`. | `src/compiler/reader.c`, `elab_*.c` |
| F4-2 | Store the deprecation message on the Binding / function record. | `src/compiler/expr.h` |
| F4-3 | At every EX_VAR / EX_CALL of a deprecated binding, emit a `DIAG_WARNING` with the stored message; suppressible via `(suppress-warnings deprecated ...)` or a CLI flag. | `src/compiler/elab_toplevel.c`, `elab_call.c` |
| F4-4 | Apply `^deprecated` to every function in `stdlib/{map,vec,list,slice,option,result,pair}.tur` (the seven modules from DEP-1/DEP-2). | `stdlib/*.tur` |
| F4-5 | Add fixture `tests/fixtures/deprecated-warning` to verify the warning fires; add `tests/fixtures/errors/deprecated-as-error` for `-Werror=deprecated` behaviour. | `tests/fixtures/` |

---

## Phase F5 -- `MutableMap[K V]`

**Problem.**  `typed-collections-plan.md` open-question section flags a
mutable map variant as a future TC2+ item.  `map.tur`'s header
references a planned mutable map; the HAMT-backed `Map[K V]` from TM0
covers only the persistent variant.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F5-1 | Decide backing data structure: open-addressed hash table (no persistence overhead) vs. a mutated HAMT (uniform code path with `Map[K V]` but loses RC-friendly sharing).  Open-addressed is the conventional choice. | design decision |
| F5-2 | Implement `stdlib/tmutmap.tur` with `(defstruct MutableMap [K V] ...)` and the standard operations (`tmutmap-new`, `tmutmap-set!`, `tmutmap-get`, `tmutmap-delete!`, `tmutmap-len`, `tmutmap-eq?`).  Mirror the `Hash[K]`/`Eq[K]` typeclass constraints from TM0. | `stdlib/tmutmap.tur` |
| F5-3 | Add docstrings per the project doc-comment standard. | `stdlib/tmutmap.tur` |
| F5-4 | Add fixtures: insert/lookup, deletion, collision, eq?, resize. | `tests/fixtures/typed/` |

---

## Phase F6 -- Interpreter (turi) fixture gaps

**Problem.**  Six fixtures pass under the compiler (`tur`) but fail
under the tree-walker (`tur run`); they were removed from
`TURI_FIXTURES_DEFAULT` in `tests/run-turi.sh` as a known-bad list
(see `typed-collections-plan.md`).  Each has a small, documented root
cause.

**Tasks.**

| ID | Task | File(s) | Root cause |
|----|------|---------|------------|
| F6-1 | Fix `defdata`-typed `let` bindings in turi -- the tree-walker drops the ADT type tag, so `match` on the binding fails with `scrutinee must be an ADT type, got adt`.  Re-enables `adt-param`, `adt-nested`, `gadt-syntax-multi`. | `src/turi/eval.c` | tag lost on `let` |
| F6-2 | Disambiguate user-defined `defstruct Cons`/`Option`/`Pair` from the built-in forward declarations the runtime emits.  Either rename the built-in symbols (less invasive) or detect the user redef and skip the builtin emit.  Re-enables `clone-list`, `clone-option`, `clone-pair`. | `src/compiler/emit_module.c` or `src/turi/eval.c` | symbol collision |
| F6-3 | Add the missing `flags` file to `tests/fixtures/gadt-syntax-multi` (it needs `-Xgadt`).  Independent of F6-1 but the fixture also depends on F6-1 being resolved before it'll pass under turi. | `tests/fixtures/gadt-syntax-multi/flags` | missing flag |
| F6-4 | Re-add the six fixtures to `TURI_FIXTURES_DEFAULT` in `tests/run-turi.sh`. | `tests/run-turi.sh` | -- |

---

## Phase F7 -- Doc hygiene

Cleanup-only; flips status markers and adds cross-references so
nobody re-investigates already-shipped work.

| ID | Task | File(s) |
|----|------|---------|
| F7-1 | In `docs/existential-types-plan.md`, backfill `[x]` on EX1a-1..EX1f-5 (all shipped during the EX1 wave; the EX2 entries were updated when EX2 landed and EX1 entries were missed).  Mark EX0 (struct-encoded helper macros) as **superseded** with a note that EX1 ships native `pack`/`open` instead. | `docs/existential-types-plan.md` |
| F7-2 | In `docs/existential-gc-plan.md`, flip EXG2-2, EXG2-4, EXG3-1, EXG3-2, EXG3-3, EXG3-4 from `defer` to `done` with cross-references to the EXG5/EXG6 commits in the followup plan.  Add a banner at the top: "Superseded by `docs/upcoming/existential-gc-followup-plan.md` for the cross-scope ownership, cycle-walker, and `:linear` work that originally lived in EXG2/EXG3." | `docs/existential-gc-plan.md` |
| F7-3 | Once F1-1 + F1-2 + F2 + F3 land, retire the corresponding sections from `docs/upcoming/existential-gc-followup-plan.md` and update its status banner to reflect the closed gaps. | `docs/upcoming/existential-gc-followup-plan.md` |

---

## Sequencing Notes

- **F1-1 is the only true blocker** in this plan; it unblocks F2-1
  and three EXG4-5 fixtures.  It also closes the most user-visible
  bug (a compiler SEGV).  Recommend landing first.
- **F1-2** is independent of F1-1 but is a prerequisite for the two
  cycle-collection tests in F2-2.  Recommend landing second.
- **F3** (typeclass dispatch) is the largest item and is independent
  of all the existential work.  Can land in parallel with F1/F2.
- **F4** (deprecation attribute), **F5** (MutableMap), and **F6**
  (turi gaps) are all independent and can be picked up by whoever has
  spare cycles.
- **F7** is doc cleanup and can be done any time, ideally as the
  matching feature lands.

---

## Relation to Other Plans

- `docs/upcoming/existential-gc-followup-plan.md` -- F1/F2 close the
  items that plan explicitly deferred ("blocked on a separate parser
  limitation", "blocked on a smart drop hook").  F7-3 retires the
  deferred entries once they ship.
- `docs/existential-types-plan.md` -- F1-1 is the EX1 follow-up that
  was never tracked but is needed for full ergonomic use of the form.
  F7-1 corrects the stale status.
- `docs/existential-gc-plan.md` -- F7-2 marks the plan superseded;
  the work itself landed via EXG5/EXG6.
- `docs/ptc-plan.md` -- F3 implements the "deferred to a future
  phase" note about dictionary passing.
- `docs/typed-collections-plan.md` -- F3 enables the recursive `.eq?`
  dispatch; F4 implements the deferred deprecation-warning system; F5
  delivers the `MutableMap[K V]` mentioned in the open-questions
  section; F6 closes the interpreter-gap table.
- `docs/upcoming/refinement-types-plan.md` and
  `docs/upcoming/currying-plan.md` -- unrelated; this plan does not
  touch them.
