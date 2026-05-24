# defstruct Compound Field-Type Annotations

> **Status:** Core extension shipped (cross-plan-followups F8 in
> `c6742ff`); residual hardening tasks tracked here.
> **Last Updated:** 2026-05-24
> **Type:** Compiler / Type system

---

## Overview

Before F8, `defstruct`'s field-type parser
(`parse_struct_field_type` in `src/compiler/elab_structs.c`) only
accepted a fixed list of keyword/sym tokens:

```
:int :int8..:int64 :uint8..:uint64 :float :float32 :bool :cstr
:nil :void :ptr<void> :fn
rc<T> ref<T> lref<T> weak<T>
<user-defined struct/ADT name>
```

Compound annotations -- the kind that `type_expr_from_form` already
parses for `defn` return types, `let` bindings, `pack`, and `::`
ascriptions -- were silently rejected with `defstruct field 'X' has
unrecognized type :Y`.  The four shapes that mattered in practice:

| Field annotation                          | Type kind  |
|-------------------------------------------|------------|
| `(exists [a] [(C a)] a)`                  | TY_EXISTS  |
| `(forall [a] (-> a a))`                   | TY_FORALL  |
| `(Vec int)`                               | TY_APP     |
| `(Map int (Vec int))` (nested TY_APP)     | TY_APP     |

Every user who wanted any of these as a struct field had to erase
to `:ptr<void>` and lose all type-level information -- including the
witness vtable needed for typeclass method dispatch through the
field, and the element-type info needed for the `.eq?` recursive
dispatch (F3-5).

## What F8 shipped (2026-05-24)

Cross-plan-followups F8 (commit `c6742ff`) closed the core gap.
All four compound shapes above now parse and round-trip correctly:

1. **`StructField` gains `Type *full_type`** (`src/compiler/types.h`)
   -- NULL for simple keyword/sym annotations (the existing
   `kind`/`inner_kind` summary captures them); set for compound
   TY_APP / TY_EXISTS / TY_FORALL annotations.  Both `StructField`
   arena allocations in `elab_structs.c` memset to zero so the
   field defaults to NULL.

2. **Pre-scan tag check** in `elab_structs.c` extended to accept
   `F_LIST` alongside `F_KEYWORD` / `F_SYM`.

3. **Both parse loops** (new-style at the per-field F_LIST path,
   ~line 343 in `elab_structs.c`; old-style at the flat-vector
   path, ~line 425) detect `F_LIST` field-type forms and route
   them through `type_expr_from_form`.  For TY_APP / TY_EXISTS /
   TY_FORALL the C-level kind is `TY_INT` (opaque heap pointer);
   storage layout unchanged.

4. **Field-access emit at `(.field s)`** in
   `src/compiler/elab_typeclasses.c` (`elab_method_call`) uses
   `def->fields[i].full_type` when present.  Without this,
   `(open (.field s) ...)` falls into the TY_PTR_VOID branch of
   `EX_EXISTS_OPEN`'s emit and reads the rc-block pointer as the
   bound value instead of dereferencing through to the
   existential record.  With it, `.eq?` via F3-5/F3-7 dispatch
   also sees the right element type for instance resolution.

5. **Fixture `tests/fixtures/exg4-pack-into-struct`** (revives
   the F1-1-6 / EXG4-5 fixture previously blocked on F8): stores
   a packed constrained existential in a struct field, opens it
   back out through `(.payload b)`, and dispatches `.show` on
   the bound value.

Working today (confirmed by `tur build` smoke tests):

- `(defstruct B :move [p (exists [a] [(Show a)] a)])` ✓
- `(defstruct B :move [v (Vec int)])` ✓
- `(defstruct B :move [m (Map int (Vec int))])` ✓
- `(defstruct B :move [f (forall [a] (-> a a))])` ✓
- `(defstruct Wrapper [T] (inner (Vec T)))` ✓ -- parameterized
  struct with compound field referencing its own type param
- `(defstruct Node :move [next (rc Node)])` ✓ -- self-referential
  via RF0 forward-stub registration

---

## Residual gaps (this plan)

### Phase DS1 -- make-struct strict type-check for compound fields

**Problem.**  Today the make-struct call-site type-check at
`elab_structs.c` is kind-level: each argument's TypeKind is
compared against `def->fields[i].kind`.  For compound fields the
kind is `TY_INT` (opaque pointer), and so is the kind of any
ascribed-but-mismatched value at the call site.  Result: the
type-checker silently accepts mismatches that should fire a
diagnostic.

Reproducer:

```turmeric
(defstruct B :move [p (exists [a] [(Show a)] a)])
(let [b (make-struct B 42)])     ; (!) silently accepted -- int
                                  ;     where constrained
                                  ;     existential expected
```

The kind-level check passes (both lower to `TY_INT`), but the
generated C reads the int as a `tur_existential_t *` and SEGVs on
the next field access or `open`.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| DS1-1 | At the make-struct elaboration (in `elab_structs.c`, search for the constructor-call path where `def->fields[i].kind` is checked), prefer `def->fields[i].full_type` over the kind summary when present.  Match the value's `Type` (kind + def + as.forall_ / as.app_ payload as appropriate) against the field's `full_type`. | `src/compiler/elab_structs.c` |
| DS1-2 | For TY_EXISTS field types specifically, mirror the constraint-class check that `elab_pack` already runs against its annotation: the source's `n_constraints` and constraint classes must match the field's annotation. | `src/compiler/elab_structs.c` |
| DS1-3 | Add error fixture `tests/fixtures/errors/defstruct-compound-field-mismatch` exercising the above with `expected.diag` substring. | `tests/fixtures/errors/` |

### Phase DS2 -- move-at-pack for struct field initialisers

**Problem.**  F1-2-3 added move-at-pack semantics inside
`elab_pack`: when the packed value is an `EX_VAR` over a TY_RC /
TY_WEAK / TY_EXISTS binding, the source binding is marked moved so
its scope-exit drop is skipped (ownership transfers into the new
existential record).

F8 did not extend this scan to struct field initialisers.  A
pattern like:

```turmeric
(let [p (pack 42 (exists [a] [(Show a)] a))
      b (make-struct Box p)]
  ...)
```

leaves `p` with a stale strong reference: the let-scope auto-drop
releases it, racing the struct's eventual smart-drop dispatch on
the same control block.

Today's shipped fixture (`exg4-pack-into-struct`) sidesteps the
issue by inlining the pack into the make-struct call:

```turmeric
(let [b (make-struct Box (pack 42 (exists [a] [(Show a)] a)))]
  ...)
```

Here the pack value is consumed in the make-struct call expression
directly, no let binding to leak.  But the patterned form (separate
let, then make-struct) is rare-but-natural in user code.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| DS2-1 | At the make-struct constructor elaboration in `elab_structs.c`, scan each argument's expression for an `EX_VAR` whose binding has type TY_RC / TY_WEAK / TY_EXISTS, and call `binding_mark_moved` on the binding -- analogous to the `elab_pack` scan that ships F1-2-3 in `elab_types.c`. | `src/compiler/elab_structs.c` |
| DS2-2 | Add fixture `tests/fixtures/exg4-pack-into-struct-via-let` exercising the let-then-make-struct pattern and asserting the rc strong count remains correct (no double-free, no leak). | `tests/fixtures/` |

### Phase DS3 -- cycle-construction fixture (F2-2-1 / F8-7)

**Problem.**  Cross-plan-followups F2-2-1 / F8-7 called for a
fixture that builds a back-edge:

```
Struct S
  has field f : rc<Cell>
Cell
  has field rev : rc<S>           ; <-- back-edge to S
```

Construct an `S` and a `Cell`, then `set!` the back-edge field so
the cycle exists.  Drop the only strong root and force `gc!`; the
Bacon-Rajan walker (which already follows
RCK_EXISTENTIAL + RCEXP_RC payloads per EXG5-2) should reclaim the
cycle.

The blocker isn't the compound field-type parser itself -- F8 makes
the `rc<Cell>` field annotation work.  The blocker is `set!` on
struct fields of `rc<T>` type:

- struct field writes via `(set! (.field s) new-rc)` may or may not
  exist as syntax;
- if they do, do they correctly install the back-edge with proper
  ownership semantics (rc/clone vs move)?
- the cycle requires *mutability* (the back-edge field can't be
  fixed at construction because the other half isn't allocated yet).

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| DS3-1 | Survey current support for `(set! (.field s) new-value)` on rc-typed fields in `elab_structs.c` / `emit_expr.c`.  Document what works and what doesn't. | survey |
| DS3-2 | Implement whatever's missing for the back-edge install (likely an emit path for rc-field assignment that increments the new rc's strong count and decrements the old). | `src/compiler/emit_expr.c` |
| DS3-3 | Add fixture `tests/fixtures/exg5-exists-cycle` that builds the back-edge, drops the strong root, runs `gc!`, and asserts the cycle is reclaimed.  Uses a weak<S> witness to observe post-collection state. | `tests/fixtures/` |

### Phase DS4 -- diagnostic hygiene

**Problem.**  Two small diagnostic regressions from F8 deserve
follow-up:

1. The `:copy struct with non-copy field` diagnostic in the
   new-style path now says `non-copy compound type` (no type name)
   when the field annotation is an F_LIST; the simple-token path
   still includes the type name.  Not a regression in semantics,
   but the message is slightly less helpful for compound fields.

2. F6-1's parallel investigation found that the user-side
   `(defstruct Cons ...)` collides with auto-loaded
   `tlist.tur`'s `(defstruct Cons [A] ...)` and silently produces
   a duplicate C-level `typedef struct Cons` -- a `cc` compile
   error rather than an elaborator diagnostic.  The fix is to
   make `elab_structs.c`'s `(defstruct X ...)` check at line 227
   fire when X is already in scope from the auto-loaded stdlib.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| DS4-1 | Improve the `:copy non-copy compound type` diagnostic by formatting the parsed `Type` (via `type_name(*full_type)` or similar) so users see the actual type they wrote. | `src/compiler/elab_structs.c` |
| DS4-2 | Reject `(defstruct X ...)` where X is already bound by an auto-loaded stdlib struct.  Today the elaborator accepts the redefinition silently and codegen produces a duplicate C-level typedef.  This unblocks removing the user-side fixture renames added in F6-2 (`Cell` / `Opt` / `Pr` -> back to original names).  Alternative: continue requiring users to pick distinct names (current state). | `src/compiler/elab_structs.c` |

### Phase DS5 -- UBSan-flagged uninitialised binding field

**Problem.**  While running the F8 smoke tests under UBSan, the
following trip appears:

```
src/compiler/elab_core.c:1310:14: runtime error: load of value 190,
which is not a valid value for type '_Bool'
```

Line 1310 reads `m->is_referred` -- a Binding field.  Some code
path creates a Binding (or alias) without going through
`binding_new`'s `memset`, leaving the bool field uninitialised.
The program still functions (the values happen to compare as truthy
or falsy in a way that doesn't break semantics), but the UBSan trip
is real undefined behaviour.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| DS5-1 | Audit Binding allocation sites for direct `arena_alloc(... sizeof(Binding))` that skip the `binding_new` helper.  Either route them through `binding_new` or add an explicit `memset`. | `src/compiler/` (grep `arena_alloc.*Binding`) |
| DS5-2 | Run the F8 smoke tests under UBSan after DS5-1 lands to confirm the trip is gone. | -- |

### Phase DS6 -- struct drop glue for free-floating bindings & compound rc fields

**Problem.**  Surfaced while implementing DS2: when a struct holds an
rc-managed payload in a compound field annotation
(`(exists [a] [(C a)] a)`, `(rc Node)`, etc.) and the struct binding is
not itself rc-wrapped, the payload silently leaks at scope exit.  Two
distinct gaps compose to produce the leak:

1. **`needs_drop_glue` doesn't see compound rc fields.**  The F8
   lowering in `elab_structs.c` (new-style ~line 422, old-style
   ~line 532) sets `fkind = TY_INT` for TY_APP / TY_EXISTS / TY_FORALL
   field annotations -- so the `fkind == TY_RC || fkind == TY_REF ||
   fkind == TY_WEAK` check that flips `def->needs_drop_glue` on
   silently misses every compound rc payload.  The real type lives on
   `StructField.full_type` but the drop-glue trigger ignores it.

2. **Free-floating struct bindings get no scope-exit defer.**  The
   auto-drop pass in `elab_forms.c` (~line 682) only injects defers
   for bindings whose type is TY_RC or a constrained TY_EXISTS.
   Structs with rc-managed fields don't qualify -- so even when their
   drop glue exists (DS6-1 above), it is never invoked unless the
   struct is wrapped in `rc/of` (which switches the codegen to attach
   `drop_glue_<Name>` via `emit_expr.c` ~line 1520).

Together: `(defstruct Box :move [payload (exists [a] [(Show a)] a)])`
followed by `(let [b (make-struct Box ...)] ...)` leaks the
existential record on every invocation.  The same is true for any
`(defstruct Wrapper :move [v :rc<int>])` whose binding is not
rc-wrapped.

The DS2 fix (move-at-make-struct) is still correct -- it prevents the
*double-decrement* that would otherwise happen if the struct's drop
glue ever did fire.  DS6 closes the other direction (the leak that
exists today) and makes the DS2 fixture's "no leak" assertion
actually verifiable.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| DS6-1 | At both `needs_drop_glue` trigger sites in `elab_structs.c`, also flip the flag when `full_type` is set and the parsed Type is rc-managed (TY_RC / TY_WEAK / TY_REF / constrained TY_EXISTS).  Helper: a small `type_is_rc_managed(const Type *)` predicate that the auto-drop pass in `elab_forms.c` can reuse. | `src/compiler/elab_structs.c`, `src/compiler/types.{h,c}` |
| DS6-2 | Extend the drop-glue emitter in `emit_module.c` (~line 124) to handle compound rc fields.  For TY_EXISTS payloads the C-level slot is `RcControlBlock *` (opaque `void *` in the typedef), released via `rc_strong_decrement` + `rc_free_queue_drain` -- analogous to the existing TY_RC branch.  For nested TY_RC inside TY_APP (e.g. `(Vec (rc T))`), fall back to a conservative no-op until container types provide their own drop hook. | `src/compiler/emit_module.c` |
| DS6-3 | In the let auto-drop pass (`elab_forms.c` ~line 669), inject a scope-exit drop for TY_STRUCT bindings whose `StructDef.needs_drop_glue` is true and that haven't been moved/consumed.  Emit calls `drop_glue_<Name>(&binding)` (so the same drop glue serves both rc-wrapped and free-floating struct uses).  The struct itself is stack-allocated for free-floating bindings, so the drop must not `free()` the storage -- split the existing `drop_glue_<Name>` into a field-releasing helper and the current `free(ptr)`-suffixed wrapper, or pass a "don't free" flag. | `src/compiler/elab_forms.c`, `src/compiler/emit_module.c` |
| DS6-4 | Add fixture `tests/fixtures/exg4-pack-into-struct-leak-free` that uses a weak ref to observe the existential's lifecycle across the make-struct scope: `(let [w (let [b (make-struct Box (pack ...))] (rc/downgrade (.payload b)))] (assert (none? (weak/upgrade w))))`.  Without DS6, the weak upgrade succeeds (the existential outlives its struct).  With DS6, it returns `none` (the struct's smart-drop ran and decremented to 0). | `tests/fixtures/` |
| DS6-5 | Re-check the existing `exg4-pack-into-struct` and `exg4-pack-into-struct-via-let` fixtures under ASAN with `detect_leaks=1` (the CMake configuration currently disables leak detection via `ASAN_OPTIONS=detect_leaks=0`); confirm zero leaks post-DS6. | `tests/fixtures/`, `CMakeLists.txt` |

---

## Sequencing notes

- **DS1** is the most user-impactful (silently-accepted type
  mismatch is a footgun); recommend landing first.
- **DS2** is small and unblocks the natural let-then-make-struct
  pattern; can land any time.
- **DS3** is the biggest item (needs `set!` on rc-typed struct
  fields, plus the cycle test); can land independently.
- **DS4** is diagnostic polish; nice-to-have.
- **DS5** is an unrelated hygiene fix but surfaced during F8 work;
  worth closing so future struct-field changes don't get blamed.
- **DS6** depends on DS2 being in place (the move-at-make-struct
  marking is what keeps DS6's new drop-glue path from
  double-decrementing).  Once DS6 lands, the DS2 fixture's "no leak"
  assertion can be tightened to actually observe collection.

---

## Relation to other plans

- `docs/upcoming/cross-plan-followups-plan.md` -- F8 shipped the
  core extension.  This plan is the natural follow-up tracking
  what F8 deliberately deferred (F8-3 strict type-check,
  F8-5 move-at-pack for struct fields, F8-7 cycle fixture)
  plus the DS4 / DS5 items that surfaced post-F8.
- `docs/upcoming/existential-gc-followup-plan.md` -- DS3 (cycle
  fixture) closes the last open EXG5-5 item.
- `docs/existential-gc-plan.md` -- DS3 also closes the
  superseded EXG2-4 task (already marked done via EXG5 walker;
  this plan delivers the end-to-end runtime test).
