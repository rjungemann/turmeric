# Uniqueness Types -- UT2 + UT3 Plan

## Context

UT0 + UT1 shipped: `CK_UNIQUE` capability kind, `^unique` annotation,
the elaborator check that two live bindings cannot refer to the same
unique value, and `ref<T>` modelled as an explicit unique type. The
`-Xunique-types` flag flips this surface on (and becomes always-on once
`drop-x-flags-plan.md` lands).

What's missing -- documented in `docs/guides/compiler-flags-guide.md:125`
("UT0–UT1 are complete. UT2–UT3 (inference, stdlib patterns) are
deferred.") and `docs/guides/uniqueness-types-guide.md`:

- **UT2 -- uniqueness inference.** Today every `^unique` binding must be
  hand-annotated. The elaborator already tracks uniqueness for `ref<T>`
  through the borrow checker, but does not promote that information into
  the type-system view that UT1 consumes. Users hit this most often in
  let-bindings of `ref<T>` factory results, where the binding lacks
  `^unique` and the next use fails a uniqueness check.
- **UT3 -- stdlib uniqueness patterns.** Standard "linear-resource"
  shapes (`with-unique`, builder pipelines that thread a unique handle,
  consume-and-return helpers) are written ad-hoc per call site, not as
  reusable stdlib forms.

**No prior plan exists for UT2 or UT3.** The original linear-types plan
([archive/history/linear-types-plan.md](../../archive/history/linear-types-plan.md))
covers `^linear` but explicitly defers uniqueness inference. This is the
first plan that names it.

## Goals

1. The elaborator infers `CK_UNIQUE` on bindings whose RHS is unique
   *without* a user-written `^unique` annotation, in the same places the
   borrow checker already infers uniqueness for `ref<T>`.
2. A small set of stdlib forms (`with-unique`, `consume`, `replace`)
   captures the recurring "linear-resource" pattern so user code stops
   open-coding it.
3. The status note in `compiler-flags-guide.md` drops from "Partial
   (UT0–UT1)" to "Complete (UT0–UT3)."

## Non-goals

- A general type-inference rewrite. UT2 only promotes the inference
  signal that already exists in the borrow checker; it does not add a
  new constraint solver.
- New capability kinds. `CK_UNIQUE` stays as defined in UT0.
- Cross-module uniqueness inference through opaque types. If an opaque
  hides a unique field, the user still annotates the constructor.

## Design

### UT2 -- inference

Source of truth: the borrow checker already classifies each value as
**owned-unique**, **owned-shared**, or **borrowed** when it walks
`ref<T>`. The elaborator just doesn't expose that classification to the
UT1 check, which runs later and re-derives uniqueness from explicit
annotations only.

Wire the existing signal through:

1. The borrow checker stamps each `Binding` with a
   `borrow_kind : BK_UNIQUE | BK_SHARED | BK_BORROWED` field (already
   computed, currently discarded after the pass).
2. UT1's "every live use must agree on capability kind" check consults
   `borrow_kind` when no explicit `CK_*` is present, treating
   `BK_UNIQUE` as `CK_UNIQUE`.
3. The "two live bindings to the same unique" diagnostic
   (`TUR-E0151`) now fires on inferred-unique bindings, not only
   annotated ones.

Edge cases:

- **Phi at if/match join.** If both arms of a branch produce a unique
  binding from distinct sources, the join is unique. If one arm
  produces unique and the other produces shared, the join is shared
  (downgrade is safe; the user can re-`^unique`-annotate if they want
  the error).
- **Closure capture.** A closure that captures a unique binding by
  value becomes itself unique; by reference, the closure stays shared
  and the captured binding stays unique (already enforced by the
  borrow checker for `ref<T>` -- UT2 just makes this consistent for
  arbitrary `CK_UNIQUE`).

### UT3 -- stdlib patterns

Add three forms to `stdlib/unique.tur` (new file):

- `(with-unique [name init] body...)` -- macro. Binds `name : ^unique`,
  runs body, asserts at elaboration time that `name` is consumed
  exactly once on every body exit (same machinery as `^linear`).
- `(consume x f)` -- `(forall [a b] (fn [^unique a (fn [^unique a] b)] b))`.
  Threads a unique through a single transforming function call. The
  point is documentation, not codegen -- it makes the consume-and-pass
  pattern visible at the call site.
- `(replace target new)` -- `(forall [a] (fn [^unique a a] a))`. Returns
  the old value and installs `new`. The stdlib stand-in for `mem::replace`.

These are the patterns the guide already shows users open-coding around
`ref<T>`. The macro hides the manual annotations without adding a new
capability kind.

## Work items

| # | Item | File(s) |
|---|------|---------|
| U1 | Add `borrow_kind` field to the per-binding record in the borrow checker; populate during the existing pass; do not consume yet. | `src/compiler/borrow.{c,h}` |
| U2 | Teach UT1's capability-kind check to read `borrow_kind` as the default when no explicit annotation is present. | `src/compiler/elaborate.c` (UT1 entry point) |
| U3 | Phi rule at if/match joins: unique ∧ unique → unique; unique ∧ shared → shared. | same |
| U4 | Closure-capture rule: by-value capture of unique → closure unique; by-ref capture → both shared. | same |
| U5 | Fixtures: a `unique-inferred-let.tur` (no annotation, still errors on second use) and a `unique-phi-downgrade.tur` (one unique arm, one shared, join is shared, second use OK). | `tests/fixtures/unique-types/` |
| U6 | Create `stdlib/unique.tur` with `with-unique` / `consume` / `replace`. | `stdlib/unique.tur` |
| U7 | Docstrings on all three following the project standard (summary, params, returns, example, since). | `stdlib/unique.tur` |
| U8 | Fixtures exercising each form. | `tests/fixtures/unique-types/` |
| U9 | Update `docs/guides/uniqueness-types-guide.md`: drop the "UT2/UT3 deferred" disclaimer; add a "Common patterns" section that shows `with-unique` etc. instead of hand-rolled equivalents. | `docs/guides/uniqueness-types-guide.md` |
| U10 | Flip the status row in `docs/guides/compiler-flags-guide.md` from "Partial (UT0–UT1)" to "Complete (UT0–UT3)." Coordinate with `drop-x-flags-plan.md` -- if that plan lands first, edit the "Removed flags" section instead. | `docs/guides/compiler-flags-guide.md` |

U1–U5 are the inference half; U6–U8 are the stdlib half; the two halves
are independent and can land in either order.

## Verification

Standard fixture sweep:

```sh
bash tests/run.sh 2>&1 | grep -E '^(FAIL|summary)'
```

Plus a hand check that the new diagnostic surface is right:

```sh
./build/tur check tests/fixtures/unique-types/unique-inferred-let.tur 2>&1 \
  | grep 'TUR-E0151'
```

## Risk

UT2 widens the surface where `TUR-E0151` fires -- any code that was
relying on "no annotation → no check" will now see the error. This is
the correct outcome (the borrow checker was already unhappy; UT1 just
didn't report it), but flag it in the 0.24.0 changelog if this plan
ships in the same release as `drop-x-flags-plan.md`. Existing
`^unique`-annotated code is unaffected.

## Out of scope

- Cross-module uniqueness inference through opaques (the user still
  annotates the constructor).
- A `Drop`-like typeclass for unique destructors -- a separate plan
  if and when it's wanted.
- Uniqueness for non-`ref<T>` heap types (struct-by-value with unique
  fields, etc.) -- the borrow checker doesn't track this today and
  UT2 doesn't change that.

## See also

- [archive/history/linear-types-plan.md](../../archive/history/linear-types-plan.md) -- LT0–LT4, the parent framework.
- [drop-x-flags-plan.md](drop-x-flags-plan.md) -- coordinates the flag
  graduation; this plan ends with `-Xunique-types`'s last gap closed.
- [docs/guides/uniqueness-types-guide.md](../../guides/uniqueness-types-guide.md) -- user-facing reference.
