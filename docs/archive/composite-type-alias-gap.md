# No transparent type alias for composite types, and three docs advertise forms the compiler rejects

> **RESOLVED 2026-07-30 -- Fix direction 4 (extend `defalias`).** `defalias`
> now accepts any type expression the elaborator can resolve; `deftype` is
> untouched and stays the recursive binder, so the two forms are disjoint
> exactly as the design intent stated. Repros 1, 2 and 3 below all check
> clean. Details in *Resolution* at the end of this document.

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

---

## Resolution -- 2026-07-30

Took **fix direction 4**: `defalias` accepts a full type expression. Direction
3 (relaxing `deftype`'s `TY_REC` wrap when the body is non-recursive) was the
smaller diff, but it makes one form mean two things depending on its body, and
the report's own note at *Why this looks untracked* records the design intent
that the two forms stay disjoint. Extending `defalias` keeps `deftype` purely
recursive and gives the transparent alias one unambiguous spelling.

### What ships

```turmeric
(defalias Sample    :int)                            ; TA1, unchanged
(defalias IntList   (Cons int))                      ; type application
(defalias Point     P)                               ; struct / ADT name
(defalias Backtrack (fn [] int))                     ; function type
(defalias Logger    (fn [cstr] #fx{Log} nil))        ; effect-annotated fn type
(defalias NonZero   #refine{ q : int | (not= q 0) }) ; refinement
(defalias Coord     Point)                           ; alias of an alias
```

The alias is **transparent** (fix direction 5's open question, answered):
`Point` and `P` are the same type at every use site, unification never mentions
the alias, and the codegen is byte-identical to the un-aliased spelling. That
matches what `defalias` already did for primitives, and it is what makes
`(defn f [b : Backtrack] : int (b))` apply `b` at all -- the nominal reading
would have reproduced the exact `deftype` failure this report is about.

Not supported: **alias type parameters**. `(defalias Name [a] body)` is a hard
error naming the restriction. A parameterised alias needs type-level
substitution at every use site and nothing in the resolver does that today, so
`Backtrack<a>` from the doc-defect table still has no spelling; the
monomorphic `(defalias Backtrack (fn [] int))` is what the guide now uses.

### Implementation

- `src/compiler/elab_internal.h` -- `Elab` gains `type_alias_types`
  (`Type **`, the full resolved target) beside the existing
  `type_alias_kinds`, which stays as a fast kind-only view.
- `src/compiler/elab_types.c` `elab_defalias` -- primitive keywords keep the
  TA1 fast path; every other target goes through `type_expr_from_form`.
  Four guards, all of them about not letting a bad target into the table
  where it would resurface as a confusing use-site failure:
  - a `deftype`-shaped parameter vector (`(defalias N [a] ...)`) gets a
    diagnostic naming the restriction;
  - a self-referential alias (`(defalias Loop Loop)`) is rejected with a
    pointer at `deftype`;
  - an unresolved target name is rejected, because `type_expr_from_form`'s
    last resort is a `TY_TYVAR` carrying that very name -- without the guard
    `(defalias Bad :not-a-type)` would have silently become a tyvar alias
    instead of the error the existing fixture pins;
  - the same fallback one level in: an applied name that is not a type
    constructor (`(defalias L (list int))` -- the head of the `TY_APP` chain
    is an unresolved tyvar) is rejected at the declaration, matching the
    kind-mismatch the direct annotation `[xs : (list int)]` reports.

  That last one is worth a note for anyone re-reading this report's **Repro
  1**: `(list int)` was never a valid type. The cons-list constructor is
  `Cons`, and `(defn len [xs : (list int)] ...)` fails on its own with
  `TUR-E0012` even with no alias in the file. `(defalias IntList (Cons int))`
  is the working form, and is what the fixture uses.
- Five lookup sites now copy the full target type instead of rebuilding one
  from a bare `TypeKind`: two in `type_expr_from_form` (bare-symbol and
  keyword annotation positions) and three in the `elab_fns.c` ladders
  (`defn` parameter, `defn` return, lambda return). The `defn` return site
  also threads the target into the same capture variables the
  `: (type-expr)` path uses (`return_adt_def`, `return_fn_type`,
  `return_app_type`, ...), without which a composite return reached call
  sites as an empty shell of the right kind.

### Tests

- `tests/fixtures/defalias-composite/` -- struct, type application, function
  type, refinement and alias-of-alias targets, each exercised in parameter,
  return and lambda position, in both the spaced (`: Point`) and keyword
  (`:Point`) annotation spellings.
- `tests/fixtures/errors/defalias-type-params/` -- the parameter-vector guard.
- `tests/fixtures/errors/defalias-self-reference/` -- the self-reference guard.
- `tests/fixtures/errors/defalias-not-a-type-constructor/` -- the applied
  non-constructor guard.

The pre-existing `tests/fixtures/errors/defalias-bad-target/` still passes:
`(defalias Bad :not-a-type)` is an error under the new resolver too.

`bash tests/run.sh`: **2440 passed, 0 failed**. No fixture snapshot moved --
the alias is transparent, so codegen is unchanged. That transparency was
checked directly rather than assumed: for each composite target the emitted C
was diffed against the same program with the type spelled inline, and is
identical.

### Doc follow-up -- done

1. `docs/guides/logic-programming-guide.md` -- the Backtrack Monad section now
   names `Backtrack` and `Goal` via `defalias` across `mzero`, `mplus`,
   `pure`, `bind` and `list-flat-map`, and the "No composite type alias"
   bullet is gone. Both code blocks were extracted and run verbatim: they
   print the documented `1 / 2 / 10 / 20`. The partial-fix note is removed --
   Gap B (`'fs' is not a function or continuation`) was a *consequence* of the
   `TY_REC` wrap, so a transparent alias resolves it rather than working
   around it.
2. `docs/archive/defalias-plan.md` -- the *Open questions* `deftype`-overlap
   entry now records the answer, and a Phase TA2 section documents the
   extension. The Phase TA1 scope table carries a pointer to it.
3. `docs/guides/syntax-guide.md` -- new *Naming a type -- `defalias` vs
   `deftype`* subsection under Type-annotation syntax. This is the
   user-facing doc the report noted did not exist.
4. `serializable-continuations-guide.md` needs no further change. Its
   `serial-continuation<T>` is a parameterised **record** -- `defstruct` is
   the right form for it, and an alias could not express it anyway while
   alias type parameters are unsupported.

### Noted in passing, not fixed here

Two pre-existing defects were hit while validating the guide code and
confirmed **not** to be alias-related -- each reproduces identically with the
type spelled inline:

- Calling a fn-typed parameter whose type carries a **non-empty effect row**
  (`(fn [int] #fx{Log} nil)`) segfaults -- no handler or `perform` needed; the
  emitted call goes through `__tur_cps_lookup`, misses, and calls NULL
  unguarded. Filed as
  [effectful-fn-typed-param-call-segfaults.md](../reported/effectful-fn-typed-param-call-segfaults.md).
  This is why `tests/fixtures/defalias-composite/` aliases a pure
  `(fn [int] int)` rather than the effect-annotated shape.
- Sweet-exp `$` double-applies when the rest-of-line is already one complete
  call, so `println $ g(7)` becomes `(println ((g 7)))`. `CLAUDE.md`'s own
  chained example (`println $ normalize $ vec3(...)`) does not compile. Filed
  as
  [sweet-dollar-double-applies-single-call.md](../reported/sweet-dollar-double-applies-single-call.md).
