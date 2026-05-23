# Cross-Plan Followups

> **Status:** F1-1, F1-2, F1-3 (retire), F2-1, F2-2-2, F7 (doc
> hygiene) all shipped.  Existential safety completeness phase done.
> Remaining open phases (each ~1 session of work; independent):
>   - F3 -- dictionary passing for recursive typeclass dispatch
>     (the plan's "largest item"),
>   - F4 -- `^deprecated` attribute system,
>   - F5 -- `MutableMap[K V]`,
>   - F6 -- interpreter (turi) fixture gaps,
>   - F8 -- `defstruct` compound field type annotations (unblocks
>     `exg4-pack-into-struct` and the F2-2-1 cycle-construction
>     fixture).
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

F8 (defstruct compound field annotations)
  +--> unblocks exg4-pack-into-struct fixture (originally EXG4-5)
  +--> unblocks exg5-exists-cycle fixture (F2-2-1)
  +--> unblocks Vec[Showable] / Map[K (exists ...)] storage idioms
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
| F1-1-6 | Ship the previously blocked EXG4-5 fixtures -- **partial:** `exg4-pack-return` and `exg4-pack-into-fn` shipped.  `exg4-pack-into-struct` deferred to **F8-6** (blocked on the `defstruct` compound-annotation parser extension; F8 is the dedicated phase that closes this). | `tests/fixtures/` |

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
| F2-2-1 | Cycle-construction fixture -- deferred to **F8-7** (same `defstruct` compound-annotation gap as `exg4-pack-into-struct`; F8 is the dedicated phase that closes both at once). | `tests/fixtures/` |
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
| F7-1 | Backfill `[x]` on EX1a-1..EX1f-5 in `existential-types-plan.md`; mark EX0 superseded -- **shipped** (27 EX1 entries flipped, EX0 banner added). | `docs/existential-types-plan.md` |
| F7-2 | Flip EXG2-2..EXG3-4 from `defer` to `done` with cross-refs; add superseded banner -- **shipped** (banner added, 6 entries flipped, task-summary cross-refs added). | `docs/existential-gc-plan.md` |
| F7-3 | Update `existential-gc-followup-plan.md` status to reflect cross-plan-followups closures -- **shipped** (status banner rewritten; EXG4-5, EXG5-5, EXG6-5 task-summary rows flipped from `partial` to `shipped (mostly)` / `shipped` with cross-refs to F1-1, F1-2, F2-1; EXG4-3 already marked retired via F1-3). | `docs/upcoming/existential-gc-followup-plan.md` |

---

## Phase F8 -- `defstruct` compound field type annotations

**Problem.**  `defstruct`'s field-type parser (`parse_struct_field_type`
in `src/compiler/elab_structs.c`) only accepts a fixed list of
keyword/sym tokens (`:int`, `:cstr`, `rc<T>`, `ref<T>`, `weak<T>`,
`lref<T>`, `ptr<void>`, `:fn`, plus user-defined struct/ADT names
looked up by symbol).  Compound annotations -- the kind that
`type_expr_from_form` parses for `defn` return types, `let` bindings,
and `pack` -- are silently rejected:

- `[payload :(exists [a] [(Show a)] a)]`  -- constrained existential field
- `[handler (forall [a] (-> a a))]`        -- rank-2 function field
- `[items (vec int)]`                      -- TY_APP field (also fails)
- `[lst (Cons int)]`                       -- TY_APP field (also fails)

Because the parser fails closed (`fkind == TY_UNKNOWN` triggers the
"unrecognized type" diagnostic), every user who wants any of these
storage idioms has to erase to `:ptr<void>` and lose all type-level
information, including the constraint witnesses needed for method
dispatch through the field.

The gap blocks at least:

- `tests/fixtures/exg4-pack-into-struct` (EXG4-5 wave; F1-1-6 in this
  plan) -- store a packed constrained existential in a struct field
  and reopen it through `(.field s)`.
- `tests/fixtures/exg5-exists-cycle` (F2-2-1 in this plan) -- build
  a cycle by mutating a struct field of type `rc<Cell>` to point back
  at the existential containing it; force `gc!`; observe collection.
- The natural spelling of `Vec[Showable]` / `Map[K (exists ...)]`
  storage in user code today -- without F8, every collection of
  constrained existentials needs `:ptr<void>` fields plus an
  out-of-band tag to remember the real type.

### Design sketch

Mirror the path that `defn` return types already use:
`type_expr_from_form` produces a full `Type *` that captures
`as.forall_`, `as.app_`, etc., and the call site stores it in
`fn_type.as.fn.result_full_type`.  For struct fields, extend
`StructField` to optionally carry a full `Type *` alongside the
existing `kind` / `inner_kind` summary; emit and field-access paths
read through the full type when present and fall back to the summary
kind otherwise so callers that only need `kind` keep working.

### Tasks

| ID | Task | File(s) |
|----|------|---------|
| F8-1 | Extend `StructField` in `src/compiler/types.h` with `Type *full_type` (NULL when the field's type is a simple keyword/sym that the current `kind`/`inner_kind` summary already captures fully).  Initialise to NULL at all existing call sites in `elab_structs.c`. | `src/compiler/types.h`, `src/compiler/elab_structs.c` |
| F8-2 | In `elab_structs.c`'s two field-type parsers (new-style at ~line 343 and old-style at ~line 391), detect when the field-type form is an `F_LIST` or `F_TYPE_ANN` wrapping a compound type (not a single F_SYM/F_KEYWORD).  Route those through `type_expr_from_form` and stash the result in `full_type`.  Derive `kind` from the parsed `Type`'s kind; for TY_EXISTS / TY_FORALL / TY_APP, the C-level representation is `int64_t` (opaque pointer) so the storage layout doesn't change.  Keep the simple-token fast path for the existing keyword set. | `src/compiler/elab_structs.c` |
| F8-3 | Update the type-check at the `make-struct` / `Box.` constructor call site (currently `elab_structs.c` ~line 1500-1700, where each arg's type is matched against `def->fields[i].kind`) to compare against `full_type` when present.  For TY_EXISTS, require the source's `Type` and the field's `Type` to agree on `n_constraints` and constraint classes (same check `elab_pack` does against the annotation). | `src/compiler/elab_structs.c` |
| F8-4 | Update field-access emit at `(.field s)` so a TY_EXISTS field returns an expression with the field's full `Type` (not bare TY_PTR_VOID).  Without this, `(open (.field s) ...)` falls into the TY_PTR_VOID branch of `EX_EXISTS_OPEN`'s emit and reads the rc-block pointer as the bound value instead of dereferencing through to the existential record (the symptom we hit while writing the original fixture: output of an int64 bit-pattern instead of `42`). | `src/compiler/emit_expr.c`, `src/compiler/elab_structs.c` (field-access elaboration) |
| F8-5 | Extend F1-2-3's move-at-pack scan to also fire when an EX_VAR over a TY_RC / TY_WEAK / TY_EXISTS binding is the source of a struct field initialiser (analogous to the elab_pack check that landed in F1-2).  Without this, storing a packed existential into a struct field leaves the originating binding with a stale strong reference that the let-scope auto-drop will release, racing the struct's eventual smart-drop dispatch on the same control block.  An explicit `(rc/clone ...)` at the storage site continues to opt into shared ownership exactly as it does for `rc<T>` today (per F1-3's retire decision). | `src/compiler/elab_structs.c` (constructor elaboration) |
| F8-6 | Re-add fixture `tests/fixtures/exg4-pack-into-struct` (the third F1-1-6 fixture that we dropped during the F1-1 wave).  Field declared `:(exists [a] [(Show a)] a)`; assert that `(.show v)` dispatches through the witness vtable when opened from the field. | `tests/fixtures/` |
| F8-7 | Add fixture `tests/fixtures/exg5-exists-cycle` (F2-2-1).  Build the back-edge: a mutable struct holding an `rc<...>` field that points to an existential whose payload is the same struct's rc-block.  Force `gc!` after dropping the only strong root; assert the cycle is reclaimed via the Bacon-Rajan walker (which already follows RCK_EXISTENTIAL + RCEXP_RC payloads per EXG5-2). | `tests/fixtures/` |
| F8-8 | Doc updates: flip the blocked rows in `docs/upcoming/cross-plan-followups-plan.md` (F1-1-6's `exg4-pack-into-struct` row, F2-2-1) and in `docs/upcoming/existential-gc-followup-plan.md` (EXG4-5 + EXG5-5 task-summary rows) once F8 lands.  Remove the "blocked on defstruct" notes from the status banners. | `docs/upcoming/cross-plan-followups-plan.md`, `docs/upcoming/existential-gc-followup-plan.md` |

### Caveats

- **TY_APP fields are bigger scope.**  Once `type_expr_from_form`
  is wired in, struct fields can in principle hold `(vec int)`,
  `(Cons int)`, etc.  TY_APP requires the field-type to track the
  PTC4 dispatch metadata the function-return path already wires up;
  if F8-2 hits issues there, scope F8 down to just TY_EXISTS /
  TY_FORALL and leave TY_APP for a follow-up.
- **Recursive struct types.**  A struct field whose type mentions
  the same struct (`[next :(rc Self)]`) needs the forward-stub
  registration path that `RF0` (existing) already handles for the
  symbol-name case; F8-2 must use the same `e->scope` so the
  forward stub is visible.
- **Borrow-check of field reads through `(.field s)`** for
  CK_LINEAR / CK_MOVE existentials needs to follow the same rules
  as let-binding reads.  If F8-4's elaboration produces a normal
  EX_VAR-style expression that the borrow checker already handles,
  no new work; otherwise add a follow-up.
- **emit_module.c struct codegen** assumes 8-byte storage per
  field for the existing kinds; TY_EXISTS / TY_FORALL also fit
  (opaque rc-block pointer cast to int64), so no struct layout
  change is needed.  Document this assumption in F8-1.

### Estimated scope

Medium: ~1 session.  The mechanical pieces (parser route-through,
StructField extension) are straightforward; the field-access emit
in F8-4 is the most subtle piece and may need its own debugging
pass.  F8-5 mirrors an already-shipped pattern (F1-2-3) and should
land in a handful of lines.  F8-7 (cycle fixture) is the only
piece that exercises new code paths end-to-end; budget extra time
for that one.

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
- **F8** (`defstruct` compound field annotations) is independent
  of F3/F4/F5/F6 and can land in parallel.  It unblocks the two
  remaining existential-storage fixtures that F1-1 / F1-2 could
  not ship (`exg4-pack-into-struct`, `exg5-exists-cycle`), so
  landing F8 lets F7 retire those last "blocked on `defstruct`"
  notes from the status banners.

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
  section; F6 closes the interpreter-gap table; F8 makes typed
  collections of constrained existentials (`Vec[Showable]` etc.)
  expressible without `:ptr<void>` erasure.
- `docs/upcoming/refinement-types-plan.md` and
  `docs/upcoming/currying-plan.md` -- unrelated; this plan does not
  touch them.
