---
title: A defmacro cannot emit more than one top-level form from a single invocation
category: Reported
severity: Blocks per-component accessor generation and other "one macro -> N defns" patterns
discovered: 2026-06-11, executing ECS prereq plan step 3 (docs/upcoming/ecs-prereq-plan.md)
---

# A defmacro cannot emit more than one top-level form from a single invocation

## Summary

A macro that wants to mint several top-level definitions from one
invocation -- the natural shape for `(defworld Name [Pos Vel ...])`
that emits per-component `get-Pos` / `set-Pos!` / `has-Pos?` accessors
alongside the world struct -- has no clean way to do so. Three
plausible mechanisms all fail or have unusable trade-offs:

1. **`(do ...)` wrapping** at the top level: the emitter aborts with
   `tur: emit: EX_FN_DEF in stmt position`. `do` treats its body as
   statements, and `defn` cannot be a statement.

2. **Emitting a `defmodule`** with the new defns inside: the
   defmodule expands cleanly, but the names are scoped to the
   generated module's namespace and not visible at the call site
   without an explicit `import`. The macro can't emit the import
   alongside (same multi-emit problem).

3. **Top-level form list / splice**: there is no `~@` at top level
   because top level isn't a list. No precedent in `stdlib/` for
   any macro that produces multiple top-level forms in a single
   call.

So with `str->sym` in hand, a macro can compute the per-component
identifier names but has nowhere to emit them.

## Severity

Blocks the natural shape of two ECS spice macros:

- **`defworld Name [Pos Vel ...]`** as documented in
  `docs/upcoming/ecs-spice-plan.md` § "World":
  > Generates a `defstruct GameWorld` ... and per-component
  > `get-Pos`, `set-Pos`, `remove-Pos`, ... accessors.
  The defstruct emit is one form; each accessor is another. Without
  multi-emit, the macro can either stop at the defstruct (current
  state) or require the user to call a second per-component macro
  per accessor (`(defaccessors GameWorld Pos)`) -- which itself
  needs to emit three defns, so doesn't solve the problem.

- **`defsystem name params :reads ... :writes ... body`** as the
  ECS plan describes it. Wants to emit `(defn name-impl ...)` and
  `(def name (make-system reads writes name-impl))`. Two top-level
  forms.

For both, the workaround is "have the user write the boilerplate
by hand" -- not a blocker for the spice's correctness, but a real
regression in the planned developer ergonomics.

Severity: not a correctness bug; an expressiveness gap that pushes
us further from the plan's promised surface than it should be.

## Minimal repro

```turmeric
;; A macro that wants to emit two sibling defns from one invocation:
(defmacro pair-of-defns [name]
  (let [s1 (str->sym (str-append (symbol-name name) "-a"))
        s2 (str->sym (str-append (symbol-name name) "-b"))]
    `(do
       (defn ~s1 [] : int 1)
       (defn ~s2 [] : int 2))))

(pair-of-defns foo)

(defn main [] : int
  (println (foo-a))
  (println (foo-b))
  0)
```

Fails at codegen:

```
tur: emit: EX_FN_DEF in stmt position
```

Wrapping in `(defmodule ~mod (export ~s1 ~s2) (defn ~s1 ...) (defn ~s2 ...))`
expands fine, but the names live inside the generated module and
need an `import` at the call site to be visible. The macro can't
emit that import alongside.

## Observed vs. expected

Observed: `(do (defn ...) (defn ...))` at the top level treats the
inner defns as statements and aborts at codegen. `defmodule` emit
works but does not surface its names to the caller's namespace.

Expected: one of the following:

- **(a) A top-level `(begin ...)` / `(toplevel-do ...)` form** that a
  macro can return whose body is *not* a statement sequence but a
  top-level form sequence, each of which gets installed as if it
  had been written at the call site directly. Common Lisp's
  `progn`-at-top-level has this behaviour: top-level progn is
  spliced into the surrounding top-level context.

- **(b) A reader-level splice** at the macro return point: the
  expander, on seeing the macro returned a `(toplevel-forms ...)`
  wrapper, splices the body into the surrounding top-level
  context. Equivalent to (a), different surface.

- **(c) A `defmodule`-import bundle:** emit the module *plus* a
  silent import so the user gets the names automatically. This is
  the version that's closest to the current code path; it just
  needs the multi-emit at top level to work, which is (a) or (b)
  in disguise.

## Root-cause pointer

`src/compiler/emit_stmt.c` -- search for `EX_FN_DEF in stmt
position`. The emitter has explicit "shouldn't reach here in stmt
position" code with an `abort()`; `(do ...)` flows through the
statement emitter, which has no case for `EX_FN_DEF`. The elaborator
presumably accepts the form (no diagnostic at elaboration time);
the failure is at the C emit stage.

The fix likely lives upstream: when the elaborator sees a top-level
`(do ...)` whose body contains top-level forms (defn, def, defstruct,
defmodule), it should treat the `do` as a *top-level splice* rather
than a statement sequence. Each child gets registered as its own
top-level form. The "compile-time `do` at top level" path probably
needs a new case.

Alternative scope: rather than changing `do`'s semantics, add a new
`(toplevel-do ...)` form whose only purpose is to be a multi-emit
container at top level. Cleaner but requires a new keyword.

## Proposed fix directions

1. **Smallest: top-level `(do ...)` splices its body into the
   surrounding top-level context.** Matches Common Lisp's
   top-level-progn semantics. The "do at top level" path in the
   elaborator (currently treats body as statements) would split
   into "do in expression position -> statement sequence" and
   "do at top level -> top-level splice." Minimal new surface;
   matches what users already write.

2. **Add a `(toplevel ...)` form.** Same effect, no overloading of
   `do`. Slightly more explicit -- the user knows they're returning
   multiple top-level forms. Slightly more surface to document.

3. **Loosen `defmodule`'s export visibility.** If a macro-emitted
   `defmodule` is consumed at the same site that emitted it (i.e.,
   the macro returned it as a top-level form), implicitly import
   its exports into the surrounding namespace. Subtle; harder to
   document; not recommended.

(1) is the smallest change that unblocks the most plan; (2) is the
cleanest if there's any concern about overloading `do`.

## What this blocks downstream

- **ECS `defworld` per-component accessors** -- the E0/E1' README's
  "Known limitations" entry that just got re-marked "fixed thanks to
  str->sym" is actually still blocked, one step downstream. The
  string-to-symbol path works; we just can't emit the resulting
  defns.

- **ECS `defsystem`** -- wants to emit `(defn impl ...)` and `(def
  sys (make-system reads writes impl))` in one invocation. Same wall.

- **Any macro that derives a family of definitions from a single
  declaration.** `(deftypeclass Show [A] (show [x] : cstr))` could
  emit the class dictionary struct, a default `(definstance Show ...)`,
  and a smart constructor; today it can only emit one.

## Validation plan

A fix is validated when:

- The minimal repro above expands, codegens, and runs (prints
  `1\n2`).
- An ECS `(defworld GameWorld [Pos Vel])` invocation emits the world
  defstruct AND per-component `(get-Pos w)` / `(set-Pos! w e v)`
  defns, all callable at top level without any further `import`.
- An ECS `(defsystem physics [w :GameWorld] :reads [Pos Vel]
  :writes [Pos] body)` invocation emits `(defn physics-impl ...)`
  and `(def physics (make-system ...))`, both callable.
- Existing single-form-emitting macros continue to emit byte-
  identical C (the new top-level-splice case is purely additive).

## Interaction with str->sym

`str->sym` is shipped and lets macros *compute* the identifier
names they want to emit. This gap is the second half of the same
story: once the names are computed, they need to land somewhere
visible. Without multi-emit, `str->sym` gives us computed *single*
names but no good way to emit several at once.

The ECS prerequisite plan
([`../upcoming/ecs-prereq-plan.md`](../upcoming/ecs-prereq-plan.md))
should add this as gap E and resequence: per-component accessor
generation in `defworld` and the natural `defsystem` shape both
depend on E. Sequential `stage` does not -- the user can write
`(def physics (make-system reads writes (fn [w] body)))` manually
without `defsystem` boilerplate.
