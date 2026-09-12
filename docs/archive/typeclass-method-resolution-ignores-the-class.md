# Typeclass method resolution goes through the instance table, not the class

**Severity:** medium. Two user-visible symptoms, one root cause. Neither is a
wrong answer at runtime; both are a diagnostic/ordering defect that sends the
author looking in the wrong place.

**RESOLVED 2026-09-11 -- all four items closed.** Symptom A's fix is recorded
under "Symptom A: fixed by deferral" at the end of this file. Earlier status
text in this report describes A as open; it is kept for the reasoning, not as
current state.

**Superseded status (kept for the record):** See "Execution" at the end. Symptom **B is FIXED** and
verified (a clean check-time `TUR-E0015` naming the class, the tyvar and the
constraint to add -- no cc divergence). The **"Trap" below is FIXED** (a borrow
caret in a `definstance` method impl no longer consumes a param slot). The
**docs correction owed is DONE**. Symptom **A still reproduces**: instance
registration is still source-order dependent, and making it order-independent
remains the scoped project described under "The structural fix for A".

**Status:** open. Found while auditing the diagnostics residual left by the
archived `ecs-component-set-bounds-plan.md` (ECB). Reproduced against
`build/tur` at v0.46.0 (Debug, macOS arm64).

## Summary

`elab_typeclasses.c` resolves a method call by walking the elaborated
**instance** table for a matching method name (`inst->method_impls[i]`, the loop
above `elab_typeclasses.c:6675`). It never consults the `defclass` declaration,
and it never consults the caller's own `[(Foo W)]` constraint. Two consequences
follow.

### Symptom A -- a `definstance` after the use site does not resolve

```turmeric
(defstruct Bar [v : int])
(defclass Foo [W]
  (foo-of [^borrow w] : int))

(defn use-foo [W] [(Foo W)] [^borrow w : W] : int
  (foo-of w))                      ;; <-- error here

(definstance Foo [Bar]             ;; instance is BELOW the use
  (foo-of [w] (.v w)))
```

```
r-con-after.tur:5:3: error: no typeclass method found for 'foo-of'
3 |   (foo-of [^borrow w] : int))
4 | (defn use-foo [W] [(Foo W)] [^borrow w : W] : int
5 |   (foo-of w))
  |   ^^^^^^^^^^
```

Moving the `definstance` **above** the `defn` compiles, builds, and runs. The
message is wrong twice: the method is declared two lines above the arrow, and
what is actually missing is a *preceding instance*, which the text never hints
at. Neither `docs/guides/typeclass-guide.md` nor
`docs/guides/typeclass-internals-guide.md` documents any ordering requirement
on top-level forms, so this is not a stated rule being enforced.

Note the resolution is not even constraint-directed: one instance for an
**unrelated** type (`Bar`) appearing before the use is enough to make a body
generic in `W` resolve.

### Symptom B -- an unconstrained generic passes `tur check`, then fails in `cc`

Drop the `[(Foo W)]` constraint and keep the instance above:

```turmeric
(defn use-foo [W] [^borrow w : W] : int    ;; no constraint at all
  (foo-of w))
```

`tur check` exits **0**. `tur build` then fails inside the C compiler:

```
..._r-unc-before_tur.c:7741:53: error: passing 'int64_t' (aka 'long long') to
  parameter of incompatible type 'tur_adt_Bar' (aka 'struct tur_adt_Bar')
 7741 |         int64_t __ps_264 = (__inst_Foo_foo_hyof_Bar(w));
```

The constraint is what tells the specializer which instance a call site needs;
without it the emitter hard-wires `__inst_Foo_foo_hyof_Bar` and hands it the
int64 carrier. The right behaviour is a Turmeric diagnostic at the definition --
"`foo-of` is a method of `Foo`; add a `(Foo W)` constraint on `W`" -- not a C
type error naming a mangled internal symbol. A `tur check` that passes where
`tur build` fails is the part that stings: `check` is what editors and the CI
type-check step run.

## Root cause

One bug: **method identity lives in the instance table.** The class declaration
records the method's signature but is not what the call site is resolved
against, so

- with zero preceding instances there is nothing to find (A), and
- with some preceding instance there is always something to find, whether or not
  the caller is entitled to it (B).

`src/compiler/elab_typeclasses.c:6675` is where the failure surfaces; the search
loop immediately above it is the mechanism.

## Fix direction

Resolve a method name against the **class** declaration first, and use the
caller's constraint set to decide which instance (or dictionary) the call needs:

1. If the name is a method of some class in scope, the call elaborates -- with
   no dependence on whether an instance has been elaborated yet. That fixes A
   outright and makes top-level order irrelevant, matching every other
   top-level form.
2. If the receiver is a type variable, require a constraint naming that class on
   the enclosing binder, and emit a diagnostic naming the class and the variable
   when it is absent. That converts B from a `cc` error into a Turmeric one.
3. Instance *selection* stays where it is; only method *identity* moves.

Both symptoms want fixtures: a positive with the `definstance` below the use,
and a negative for the unconstrained generic pinned to whatever new code (2)
introduces.

## The structural fix for A: measured, not cheap

Direction (1) above -- make instance registration order-independent -- was
attempted and reverted. What it actually costs, measured rather than guessed:

**The one piece of good news.** Pass 2 stores results as `items[i]`, indexed by
the form's ORIGINAL position (`elab_toplevel.c`, "Pass 2: Elaborate all
forms"). So elaboration order can be changed without changing the order
definitions are EMITTED in -- a correct implementation churns no `expected.c`
snapshot. That was the expensive-looking part, and it is free.

**Obstacle 1: instances are usually not top-level forms.** stdlib writes
`definstance` at column 0, but every defmodule-wrapped file -- which is every
spice and most user code -- makes them CHILDREN of the `(defmodule ...)` form.
A pre-pass over `forms[]` iterates the single defmodule form and never sees
them. `load_expand_forms` descends into a defmodule body only to expand
`(load ...)`; it does not splice the body out. Confirmed: the defmodule-wrapped
repro fails identically to the top-level one. So the pre-pass has to be a
recursive walk with its own scoping story, not a scan.

**Obstacle 2: Pass 2 carries sequential state that a reorder invalidates.** A
straight "elaborate every non-defn form first" reorder breaks `has_defmodule`,
whose file-boundary reset is driven by comparing `forms[i]` and `forms[i+1]`
span file_ids as the loop advances. Hoisting the defmodules out from under it
made stdlib fail to load at all:

```
stdlib/safe.tur:10:1: error: only one defmodule is allowed per file
```

`in_stdlib_load` is the same shape of hazard -- it is a running toggle flipped
at `stdlib_prefix`, and it feeds `TypeClass.from_stdlib` and the
stdlib-vs-user fallback preference in dispatch, so getting it wrong
misattributes instances rather than failing loudly.

**Obstacle 3: instances must still follow type elaboration.** An instance body
that reads a struct field (`(foo-of [w] (.v w))`) needs the full defstruct, not
the RF0 forward stub. So the phase order is types -> classes -> instances ->
defn bodies, and a pre-pass has to establish that rather than simply hoisting.

None of this is unbuildable; it is a scoped project (a recursive
registration walk plus making Pass 2's sequential state position-derived
rather than loop-carried), not a patch. Filed here so the next attempt starts
from the three obstacles rather than rediscovering them.

> **Correction 2026-09-11 -- it is not an ordering problem.** The three
> obstacles above are real but they are not the blocker, and obstacle 1 is
> overstated. See "Symptom A, re-measured" at the end of this file: a two-sweep
> Pass 2 was built and run, and it fails for a reason no phase order can fix.
> **Do not start the next attempt from the three obstacles.** Start from the
> cycle.

Meanwhile the diagnostic is accurate and names the fix, so the failure mode is
a papercut with a one-line workaround (move the `definstance` above the use)
rather than a mystery.

## Docs correction owed -- DONE 2026-09-11

`docs/archive/ecs-component-set-bounds-plan.md` says the failure mode for the
typeclass encoding is "an instance-not-found error, which is noticeably worse"
than a structural `has` bound would give. That claim is **stale**. The case it
names -- calling a bounded system with a world lacking the component -- now
produces a good diagnostic:

```
error [TUR-E0001]: no 'HasVel' instance for 'GameWorld' in constrained call to
'count-vel': the type bound to 'W' has no HasVel instance, but 'count-vel'
requires one
```

That names the class, the world, the function and the type variable, and points
at the call. The genuine diagnostic debt is A and B above, which that plan does
not mention. If the ECB ergonomics argument is ever revisited, it should be
argued against this text, not the archived one.

## Trap for anyone writing a repro -- FIXED 2026-09-11

The method impl in a `definstance` takes the bare parameter list and no return
type -- `(foo-of [w] (.v w))`. Repeating the class's `^borrow` and `: int`
(`(foo-of [^borrow w] : int (.v w))`) is accepted by the elaborator and emits a
**two-parameter** C function (`_crborrow` plus `w`), which then fails in `cc`
with "too few arguments to function call, expected 2, have 1" at every call
site -- including correct ones. That is a separate papercut and not this bug;
do not mistake it for one. `spices/ecs/src/ecs/world.tur:114` has the correct
spelling.

**Fixed 2026-09-11.** The cause was a missing guard, not a design: the
`defclass` parser has skipped substructural/borrow carets since ECS E2d-P6
(`^borrow`, `^mut`, `^unique`, `^linear`, `^affine`, `^relevant`, `^fat` each
annotate the NEXT parameter and are not parameters themselves), and the
instance-impl parser carried no such skip. The identical guard now sits in
both loops. All four spellings -- `[w]`, `[w] : int`, `[^borrow w]`,
`[^borrow w] : int` -- agree; pinned by
`tests/fixtures/definstance-borrow-caret-in-impl`. Worth fixing here rather
than deferring: like symptom B, it was a **check/build divergence**, `tur
check` exiting 0 on a program cc then rejected.


## Execution 2026-09-11

Verified against `build/tur` at v0.46.1 (Debug, macOS arm64).

| Item | State |
| --- | --- |
| Symptom A -- instance below the use | **open** (diagnostic good, ordering stands) |
| Symptom B -- unconstrained generic | **FIXED**, verified |
| Trap -- borrow caret in an impl | **FIXED**, fixture added |
| Docs correction owed | **DONE** |

### Symptom B: verified fixed, not merely believed

`39c0dac53` claimed it; this is the confirmation. An unconstrained generic
body calling a class method now fails at **check** time, exit 1:

```
error [TUR-E0015]: 'foo-of' is a method of typeclass 'Foo', but 'use-foo' does
not constrain 'W' to it -- so there is no instance to dispatch to. Add the
constraint: (defn use-foo [W] [(Foo W)] ...).
```

That is fix direction 2 in full: it names the class, the type variable, and the
constraint to add, and there is no cc divergence left to find.

### Symptom A: still open, and why no patch landed here

A now reports accurately -- it names the class, says no instance is visible,
explains that instances register in source order, and tells you to move the
`definstance` above the use. What it does **not** do is stop requiring that.

The reason no fix landed in this pass is worth recording, because it is not the
three obstacles above. It is that the constrained-tyvar path needs a concrete
`FnDef` to reach `found_method`: the representative search binds the receiver
to an arbitrary carrier-compatible instance precisely so the polymorphic base
clone stays valid C, and monomorphization re-resolves later. A class
declaration carries a *signature*, not an implementation, so "resolve against
the class" cannot by itself produce the `best_method` the downstream path
consumes. Making that path signature-only is a second scoped change on top of
the registration pre-pass -- so the estimate in the section above is if
anything low, not high.

Nothing here is a reason to defer indefinitely; it is a reason not to attempt
it as a patch inside an unrelated pass.

## Symptom A, re-measured 2026-09-11

Four findings, all measured against v0.46.1. The last one changes the shape of
the problem.

### 1. No base clone is emitted, so no symbol is needed

`tur emit-c` on a working constrained generic
(`(defn use-foo [W] [(Foo W)] [^borrow w : W] : int (foo-of w))`) emits **only**
`use_foo__spec__int64_t_tur_adt_Bar` -- there is no generic `use_foo` in the
output at all, and adding a second instance adds a second specialization rather
than a base clone. Every call site monomorphizes.

So for a CONSTRAINED body the representative instance is purely an
elaboration-time typing device; nothing about it survives into the C. That is
the good news, and it is what makes a "resolve against the class" path
conceivable at all: the class declaration need only supply a *type*, never a
callable symbol. (Symptom B is the contrast -- an UNCONSTRAINED body has no
specialization to be re-resolved into, which is why the representative leaked
into the C there, and why rejecting it was right.)

### 2. But result typing reads the instance BODY, not just its signature

A signature-only path is still not a drop-in, because `elab_typeclasses.c`
refines the call's result type from the selected implementation in at least
four places:

- `result_type = best_method->body->type;` (the non-`TY_FN` arm)
- the transparent-int-newtype propagation (`type_is_transparent_int_newtype`)
- the zero-arity passthrough case that recovers a boxed `fn` result
- the M7 by-value checks (`m7_body_constructs_byvalue`,
  `m7_body_returns_byvalue_element`)

A `defclass` declares a signature and has no body, so routing the constrained
case through the class would silently change result typing for every
constrained generic. Not fatal, but it is a second scoped change stacked on the
first, and it needs the full corpus to validate.

### 3. Obstacle 1 is overstated: the recursive walk already exists

The obstacle says a pre-pass "iterates the single defmodule form and never sees
them", implying the walk has to be built. It largely does not:
`elab_forward_declare_defns` (`elab_module.c:11`) is already the shared
forward-declaration walk over a form range, called from **both**
`elab_defmodule` (the module body) and `elab_load_module` (a spliced module's
top-level forms), with `elab_pre_declare_toplevel_defn` as the entry-unit
twin. Forward *defn* references consequently work inside a defmodule today --
verified. What is missing is an instance-registration counterpart at those same
three sites, not the traversal machinery.

### 4. The actual blocker: instance bodies and defn bodies are CYCLIC

A two-sweep Pass 2 was implemented and run -- every non-`defn` form in original
order, then every `defn`, with `items[i]` still indexed by original position so
emission order is unchanged. This satisfies obstacle 3 by construction
(`defstruct` / `defclass` / `definstance` keep their relative order) and
sidesteps obstacle 2 (`in_stdlib_load` is position-derived; every `defmodule`
stays in sweep 0 in order, so the `has_defmodule` file-boundary reset is
undisturbed).

It fails immediately, on stdlib, before any user program:

```
stdlib/map.tur:924:10: error [TUR-E0006]: operator lookup failed for '=':
  got 2 arg(s), first arg type (fn [int] : int)
921 | (definstance Eq [Map]
922 |   [(Eq K) (Eq V)]
923 |   (eq? [x y]
```

`Eq [Map]`'s method body calls `map-count`, an ordinary **defn** declared
earlier in the same file. With defn bodies deferred to sweep 1, that call sees
only the Pass-1 forward declaration, which is not precise enough to type it.

That is the whole problem, and it is not an ordering problem:

- a **defn body** needs its class's **instances registered** (symptom A), and
- an **instance body** needs the **defn bodies** it calls to have elaborated.

No linear phase order satisfies both, because the dependency is a cycle.
Obstacles 1-3 are all about arranging a better order, so none of them can be
the fix, and any attempt framed as reordering will rediscover this the same way.

### What this leaves

Three directions remain, in rough order of plausibility:

1. **Deferred re-elaboration of the failing defns only.** Let Pass 2 run
   unchanged; when a defn body fails *solely* because no instance of a named
   class is registered, record it and retry after all forms are processed. The
   cycle is broken by time rather than by order. The cost is that elaboration
   has side effects (bindings, diagnostics), so the first attempt must be made
   undoable or suppressible -- which is the real work, and is unexplored.
2. **Register instance heads early, bodies late.** Breaks the cycle only if
   finding 2 is dealt with first, since the representative's body is what the
   result typing reads.
3. **Iterate to a fixpoint.** Honest, and the most invasive.

**Direction 1 is what landed, same day. See below.**

## Symptom A: fixed by deferral, 2026-09-11

The cycle in finding 4 is real, so the fix does not try to order around it --
it breaks it by **time**, and only for the forms that need it.

### Mechanism

In both elaboration drivers -- `elaborate_program`'s Pass 2 (a flat file) and
`elab_defmodule`'s body loop (every spice) -- a `defn` is elaborated inside a
`diag_push_capture()` frame when the unit contains a later `definstance`. If
that attempt raises any error, the file-scope defs it registered are rolled
back (`e->n_file_scope_defs = mark`) and the form is queued. After every other
form in the unit has been processed -- so every instance is registered -- the
queued defns are elaborated again, this time with **no capture frame**, so a
body that fails for some other reason reports its real diagnostic.

The capture/rollback pair is not new machinery: `elab_defn` already uses
exactly it for the bare-`^fat` lazy probe (`elab_fns.c`, `needs_lazy_probe` /
`fsd_mark`), and `diag.h` documents the contract -- "a caller that wants the
real diagnostics simply re-runs the elaboration with no capture frame active".

### Why this succeeds where reordering failed

It never moves a form that is not blocked. The two-sweep experiment deferred
**every** defn, which is why stdlib's `Eq [Map]` body could no longer see
`map-count`. Here `map-count` is not blocked on an instance, so it is not
deferred, and the instance body still sees it fully elaborated. Only the defn
that genuinely cannot resolve moves, and it moves to a point where the thing it
was waiting for exists.

### Order is preserved

`elaborate_program` already indexes results by original position (`items[i]`),
so nothing there moves. `elab_defmodule` did **not** -- it appended to `body[]`
in fill order -- so a `slot[]` array indexed by original body position was
added and compacted afterwards. `tests/fixtures/typeclass-instance-after-use-in-defmodule`
pins this with a `defn` declared between the `definstance` and `main`: if a
deferred form were appended rather than slotted, module body order would change.

### Cost

The gate is `has a later definstance` -- purely syntactic, depth-bounded. A unit
with no later `definstance` (the overwhelming majority, including every program
that does not use typeclasses) takes exactly the previous path: no capture
frame, no retry, no behavioural change.

### Diagnostic follow-through

The zero-instances message used to end "Instances are registered in source
order: declare a (definstance ...), and place it ABOVE this use." That advice
is now wrong -- order no longer matters -- and would send a reader to move a
form that is already fine. It reads "this program declares no 'Foo' instance at
all ... it may appear anywhere in the file, above or below this use."

### Fixtures

- `tests/fixtures/typeclass-instance-declared-after-use` -- was
  `errors/typeclass-instance-declared-after-use`, which pinned the old
  diagnostic. Now a positive fixture asserting `111`.
- `tests/fixtures/typeclass-instance-after-use-in-defmodule` -- the spice
  shape, and the order guard.

### Verified

`tests/run.sh`: **2947 passed, 0 failed**. Auxiliary ctest: **150/150**,
including `turi_fixture_tests` (the interpreter agrees: the defmodule repro
answers 7 under `--interpret` too) and all four source-level fuzzers. Negative
cases re-checked by hand: a class with no instance anywhere still errors, and a
defn broken for an unrelated reason still reports that reason rather than
having it swallowed by the capture frame.