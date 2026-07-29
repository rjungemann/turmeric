# No transparent type alias for composite types, and three docs advertise forms the compiler rejects

**Severity:** medium (expressiveness) for the alias gap itself -- there is no
route at all to name a composite type transparently. **High (docs)** for the
three snippets below, which are published as working code and are hard errors
if pasted.

Verified against `./build/tur` at **v0.32.2** (Debug, rebuilt from the tree at
`b54ab718e`).

## Summary

Turmeric has two alias forms, and between them they cover primitives and
refinements but nothing else:

| Form | Accepts | Result |
|---|---|---|
| `(defalias N :int)` | primitive `TypeKind` keywords only | transparent |
| `(deftype N #refine{...})` | contract bodies, no type params | transparent (special-cased) |
| `(deftype N <anything else>)` | any type expression | wrapped in `TY_REC`, **not** an alias |

So a composite body -- `(list int)`, a struct, an ADT, a function type -- has
no transparent-alias spelling. `deftype` accepts it syntactically and then
binds a recursive type, so every use site fails to unify.

The `TY_REC` wrapping is deliberate (`deftype` is the `Fix`/`Free` recursive
type constructor). The gap is that nothing else fills the composite-alias role.

## Repro

All four below are `tur check` on v0.32.2.

**1. Composite body via `deftype` -- use site fails to unify:**

```turmeric
(deftype IntList (list int))
(defn len [xs : IntList] : int (list-length xs))
```
```
error [TUR-E0001]: function 'list-length' arg 1: expected int, got IntList
```

**2. Struct body via `deftype` -- field access fails:**

```turmeric
(defstruct P [x : int y : int])
(deftype Point P)
(defn getx [p : Point] : int (.x p))
```
```
error: no typeclass method found for 'x'
```

The identical function typed `[p : P]` checks clean, so the alias is the only
difference.

**3. Composite body via `defalias` -- rejected outright:**

```turmeric
(defalias Backtrack (fn [int] int))
```
```
error: defalias: target type must be a keyword (e.g. :int, :float)
```

**4. Control (works, for contrast) -- the refinement carve-out:**

```turmeric
(deftype NonZeroX #refine{ x : int | (not= x 0) })
(defn safe-div [n : int d : NonZeroX] : int (/ n d))
```
Checks clean.

## Root cause

`elab_deftype` in `src/compiler/elab_types.c`:

- **2465** -- `if (body_type->kind == TY_CONTRACT)` binds the name *directly*
  to the contract type. The comment at **2458-2464** states the reason
  plainly: wrapping it "would make every use site fail to type (`expected
  <rec>, got int`), which is what stdlib/refine.tur needs."
- **2507-2514** -- every other body falls through to the `TY_REC` construction.

So the exact failure mode the refinement carve-out was written to avoid is
still live for all non-contract bodies. The carve-out fixed one body kind, not
the class of problem.

`elab_defalias` (**src/compiler/elab_types.c:248**) is the narrow path: it
requires `items[2]` to be an `F_KEYWORD` and runs it through
`typekind_from_symbol`, erroring at **265-277**.

## Why this looks untracked rather than decided

`docs/archive/defalias-plan.md:406-409` is the only place the behavior is
acknowledged, and it appears under *Open questions*:

> **`deftype` overlap.** `(deftype Sample [] :int)` already parses but
> produces a `TY_REC` binding rather than a `TY_INT` alias. `defalias`
> is intentionally narrower and simpler. The two forms are not
> interchangeable.

It notes the wrapping but does not observe that composite bodies therefore
have no alias route at all.

The primitive-only restriction has **no recorded technical rationale** -- only
scope minimization. `defalias-plan.md:44-58` lists "Aliases for ADTs, structs,
or parameterized types" as Not supported and says:

> Future phases can extend `defalias` to non-primitive targets once the
> need arises; Phase TA1 is strictly limited to the signal spice use case.

No such phase exists. And the one driver retired:
`route-b-typed-slots-plan.md:492` deleted the sole `defalias` use
(`defalias Sample :int`), keeping the form only "as a documentation /
readability tool" (`:59`).

Nothing claims composite aliases are hard or unsound. Nobody needed them.

## Doc defects -- three published snippets are hard errors

These are the more actionable half of this report. Each shows an alias form
the elaborator rejects, written as though it already works.

**All three are now FIXED** (see *Doc fixes applied* below); recorded here for
the paper trail.

| File:line | Snippet | Actual result |
|---|---|---|
| `docs/guides/logic-programming-guide.md:101` (and `:120` sweet-exp) | `(defalias Backtrack<a> (-> ((list (-> a)))))` | `defalias: target type must be a keyword` -- composite target |
| `docs/guides/serializable-continuations-guide.md:170` (and the sweet-exp twin below it) | `(defalias serial-continuation<T> (struct [...]))` | same |
| `docs/upcoming/v1/refinement-types-plan.md:2653` | `(deftype (Bounded lo hi) #refine{...})` | `error: deftype name must be a symbol` |

Note for anyone reading the diff: the `<T>` / `<a>` angle brackets in those
snippets were **never** the defect. `Name<T>` is valid type-application syntax
(`option<int>`, `SC<int>`), and `defstruct serial-continuation<T> [...]` is a
valid *declaration* -- both verified. Only the composite `defalias` target was
wrong. `deftype` is the exception: it takes its parameters as a bracket vector
(`(deftype Backtrack [a] ...)`), not `Backtrack<a>`.

The two guide snippets are exactly this report's gap written as if it shipped.

The plan line is worse in one respect: it sits inside a phase (RT5b) marked
**done**, and `stdlib/refine.tur` shipped only monomorphic aliases -- there is
no `Bounded`. Written in the accepted shape `(deftype Bounded [lo hi] #refine{...})`
it hits the deliberate guard at `elab_types.c:2466-2470`:

```
error: deftype 'Bounded': a refinement alias takes no type parameters in this
prototype (the predicate cannot mention them)
```

That guard is a **settled non-goal**, not a defect -- see
`docs/guides/refinement-types-guide.md:965` (tagged `[prototype]`, which the
legend at `:860` defines as "Not planned") and the revisit trigger in
`docs/upcoming/v1/ecs-refinement-typed-apis-plan.md:640-643`. The plan text is
what needs correcting, not the compiler.

## Doc fixes applied

Done on 2026-07-28; each replacement was verified with `tur check` on v0.32.2.

1. `serializable-continuations-guide.md:169-181` -- `defalias` -> `defstruct`,
   keeping the `serial-continuation<T>` spelling so the block stays consistent
   with the file's twelve other uses of that name. Checks clean, self-reference
   and all.
2. `logic-programming-guide.md:101,120` -- `defalias Backtrack<a> ...` ->
   `(deftype Backtrack [a] (-> (list (-> a))))`. The existing `(Backtrack a)`
   use sites were already correct for this spelling and were left alone. The
   body also lost a stray paren pair (`(-> ((list ...)))` was malformed).
   Added a note that this is a *partial* fix: the declaration, `mzero`, and
   `return` check, but `mplus`/`bind` still fail at the applied position with
   `'fs' is not a function or continuation` -- which is Gap B in this very
   report blocking its own doc fix.
3. `refinement-types-plan.md:2652` -- `Bounded` removed from the RT5b listing
   (the phase is marked done and `stdlib/refine.tur` never shipped it),
   replaced with a note recording both failure modes and pointing at the
   settled-non-goal classification.

## Fix directions

**The alias gap, roughly in increasing order of cost:**

3. **Narrowest:** generalize the `TY_CONTRACT` carve-out at `elab_types.c:2465`
   into "if the body does not mention its own name, bind it directly instead of
   wrapping in `TY_REC`." `type_is_guarded_recursive` (called at **2481**)
   already computes the needed occurs-check, so the test is available. This
   makes `(deftype IntList (list int))` transparent while leaving genuinely
   recursive `deftype` untouched. Needs a look at whether anything relies on a
   non-recursive `deftype` producing a `TY_REC`.
4. **Alternative:** extend `defalias` to accept a full type expression, as
   `defalias-plan.md:44-58` anticipated. Keeps `deftype` purely recursive and
   the two forms disjoint, which matches the stated design intent -- at the
   cost of a second type-expression path in `elab_defalias`.
5. Either way, decide whether composite aliases are transparent (structural,
   like `defalias` today) or nominal. The `IntList` error message printing
   `got IntList` rather than `got <rec>` suggests the alias name already
   survives into diagnostics, which is good for error quality under a
   transparent implementation.

Option 3 is the smaller change and reuses machinery already on the path.

## Doc follow-up -- do these when the fix lands

The 2026-07-28 doc pass worked *around* this gap. Once composite aliases exist,
revisit:

1. **`docs/guides/logic-programming-guide.md`** -- the Backtrack Monad section
   currently spells `(fn [] int)` inline in six signatures precisely because
   the alias does not work. Reintroduce the named alias
   (`(deftype Backtrack [a] ...)` or whatever spelling ships) across `mzero`,
   `mplus`, `pure`, `bind`, and `list-flat-map`, and **delete the first bullet**
   ("No composite type alias") from the constraints blockquote below the code.
   Re-run the block before publishing -- the original aspirational version of
   this section never compiled, which is how the gap survived so long.
2. **`docs/archive/defalias-plan.md:406-409`** -- the *Open questions* entry
   framing `deftype`-vs-`defalias` as "not interchangeable" becomes stale;
   whichever form wins should be stated as the answer, not an open question.
3. **`docs/guides/syntax-guide.md`** (or wherever the alias forms are taught)
   -- there is currently no user-facing doc that says "primitives use
   `defalias`, refinements use `deftype`, composites have no form." If the fix
   unifies them, document the single form; if it does not, document the split
   explicitly so the next person does not rediscover it by trial.
4. Re-check the two guide snippets fixed in *Doc fixes applied* above --
   `serializable-continuations-guide.md` moved to `defstruct` and
   `logic-programming-guide.md` to inline function types. Both are correct
   today, but a real alias may be the more natural spelling for the second one.
