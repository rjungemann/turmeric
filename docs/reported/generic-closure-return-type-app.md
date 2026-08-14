# Generic functions returning closures over a type application: type-app erased, and struct ctor never emitted

**Severity:** medium-high -- two independent defects that together make
"generic function returns a closure over `(F A)`" unusable. The first is a
checker error with a workaround; the second passes the checker and fails at
**link time**, which is the worse failure mode. Together they are why
`docs/guides/logic-programming-guide.md` ships a monomorphic `int` backtracking
monad instead of a parametric one.

Verified against `./build/tur` at **v0.32.2** (Debug, tree at `b54ab718e`).

## Defect A -- type application erased through a generic closure return type

A generic function whose declared return type is `(fn [] (Cons A))` produces a
value whose type-app has lost both its constructor and its argument.

```turmeric
(defn pure [A] [x : A] : (fn [] (Cons A))
  (fn [] (tcons x (tnil))))
(defn use [A] [xs : (Cons A)] : int 0)
(defn main [] : int (use ((pure 1))))
```
```
k1.tur:4:26: error [TUR-E0001]: function 'use' arg 1:
  expected (type-app Cons tyvar 'A'), got (type-app ? ?)
```

Note `(type-app ? ?)` -- not a wrong type, an *unset* one. Both the ctor slot
and the argument slot come back empty.

### It is narrowly scoped -- these all check clean

Each of these differs from the repro in exactly one way, and each passes:

| Variant | Result |
|---|---|
| `pure` returns `(Cons A)` directly, no closure | **clean** |
| Closure returns a bare tyvar: `(fn [] A)` | **clean** |
| Monomorphic: `(fn [] (Cons int))` | **clean** |
| Monomorphic `option<int>` through a closure | **clean** |
| Generic, closure in **parameter** position: `[^fat f : (fn [] (Cons A))]` | **clean** |
| Generic, type-app in closure **parameter** position: `(fn [(Cons A)] int)` | **clean** |

So it takes all three at once: **generic** + **type application** + closure in
**return** position. Bare type variables survive the round trip; type
applications do not. Parameter position is fine; only return position loses it.

### Workaround

An explicit carrier cast at the use site restores it:

```turmeric
(use (:: ((pure 1)) (Cons int)))   ;; checks clean
```

That is enough to build a real parametric `mplus`/`pure`, which I confirmed
type-checks -- and which then hits Defect B below.

## Defect B -- struct constructor referenced but never emitted (link failure)

Using a struct constructor inside a **closure body inside a generic function**
emits a call to `ctor_Cons` without emitting its definition.

```turmeric
(defn pure [A] [x : A] : (fn [] (Cons A)) (fn [] (tcons x (tnil))))
(defn main [] : int (thead (:: ((pure 5)) (Cons int))))
```
```
warning: call to undeclared function 'ctor_Cons'; ISO C99 and later do not
         support implicit function declarations
Undefined symbols for architecture arm64:
  "_ctor_Cons", referenced from:
      _main in d3_tur-2302bb.o
ld: symbol(s) not found for architecture arm64
tur: cc invocation failed (status 256)
```

This **passes `tur check`** and dies in the linker.

### Minimization

| Variant | Result |
|---|---|
| `(tcons 1 (tnil))` at top level | **runs** |
| `tcons` inside a generic fn, no closure: `(defn pure [A] [x : A] : (Cons A) ...)` | **runs** |
| `tcons` inside a closure inside a generic fn | **link error** |

The closure-inside-generic combination is what drops the constructor from the
emitted C. The implicit-declaration warning says the codegen emitted the
call site without ever registering the ctor for emission.

## Combined repro (both defects, one file)

This is the parametric backtracking monad the logic guide wanted. It needs the
Defect A casts to type-check, then fails to link on Defect B:

```turmeric
(defn pure [A] [x : A] : (fn [] (Cons A))
  (fn [] (tcons x (tnil))))

(defn mplus [A] [^fat fs : (fn [] (Cons A)) ^fat gs : (fn [] (Cons A))] : (fn [] (Cons A))
  (fn [] (:: (list-concat (:: (fs) :int) (:: (gs) :int)) (Cons A))))

(defn print-all [A] [xs : (Cons A) ^fat p : (fn [A] void)] : void
  (if (= (:: xs :int) 0)
    nil
    (do (p (thead xs))
        (print-all (:: (.tail xs) (Cons A)) p))))

(defn main [] : int
  (print-all (:: ((mplus (pure 1) (pure 2))) (Cons int))
             (fn [x : int] (println x)))
  0)
```

## Root cause -- Defect A (pinned)

**Substitution is not broken.** My first guess was that generic instantiation
failed to descend into a `TY_FN` return type's nested type-app. That is wrong,
and worth stating plainly so nobody re-investigates it: `call_instantiate_type`
(`src/compiler/elab_call.c:608`, with `TY_APP` at `:620-624` and the
`TY_FN`/`result_full_type` recursion at `:625-673`) correctly produces
`(fn [] (Cons int))` for the inner `(pure 1)`. The good type exists and is then
thrown away.

Two cooperating causes:

**1. The lambda's inferred result type is deliberately dropped**
(`src/compiler/elab_fns.c:6635-6637`):

```c
} else if (!return_full_type &&
           (body->type.kind == TY_APP || body->type.kind == TY_ADT) &&
           !fn_type_has_named_tyvar(&body->type)) {
```

The lambda `(fn [] (tcons x (tnil)))` has no return annotation, so `elab_fn`
infers `return_kind = TY_APP` (`:6630`), but this guard refuses to record
`return_full_type` because the body's type `(Cons A)` mentions `pure`'s tyvar
and so is "not fully ground". The gate is intentional -- see the comment at
`:6638-6653` (closure-result-monomorphization Phase 2 grounding). The lifted
thunk's `TY_FN` therefore has `result_kind == TY_APP` but
`result_full_type == NULL`: precisely a `(type-app ? ?)` shell.
(`fn_type_has_named_tyvar` is at `elab_fns.c:2342`; the assignment at `:6714`.)

**2. The call site then overwrites the correct type with that shell**
(`src/compiler/elab_call.c:3706-3708`):

```c
if (fn_binding->closure_fn_binding) {
    fn_type = fn_binding->closure_fn_binding->type;   /* the (type-app ? ?) shell */
```

`elab_call_head_expr` (`:1198-1208`) attaches the lifted lambda's binding as
`closure_fn_binding`, and this line replaces the precise instantiated
`(fn [] (Cons int))` with the shared thunk signature. The existing recovery for
exactly this case (`:3721-3733`) is guarded on `result_full_type` being
non-NULL, so it never fires. Execution falls through to
`result_type = type_from_kind(result_kind)` (`:5579-5581`), and `type_from_kind`
memsets the whole `Type` (`types.c:326-333`), leaving `as.app.fn` and
`as.app.arg` NULL. The printer renders both as `?` (`types.c:2004-2011`) --
confirming a zeroed shell rather than a partial substitution.

**Why the clean variants pass:** with no closure indirection the result comes
from `pure`'s own `result_full_type` and no `closure_fn_binding` swap happens.
With `(fn [] A)` the body is `TY_TYVAR`, which never reaches the `:6637` branch
at all -- only `TY_APP`/`TY_ADT` payloads get erased.

## Root cause -- Defect B (not pinned)

Whatever pass registers struct constructors for emission is not reached for a
closure body inside a generic function. The monomorphic and non-closure cases
both emit `ctor_Cons` correctly, so the registration exists; this path misses
it. Plausibly the same closure-lifting-under-a-generic seam as Defect A, but I
have not confirmed that.

## Fix directions

1. **Defect B first.** A link-time undefined symbol on code that passed
   `tur check` is the more dangerous of the two -- it escapes every type-level
   gate.
2. **Defect A** -- two candidate surfaces, per the trace above:
   - Relax the `!fn_type_has_named_tyvar(...)` gate at `elab_fns.c:6637`,
     preserving the ground-only restriction that the
     `emit_abi_fn_is_generic_unsafe` concern needs by some other means; or
   - Stop `elab_call.c:3706` from clobbering a *more precise* `tmp_b->type`:
     extend the recovery guard at `:3721-3725` to the
     `result_full_type == NULL && result_kind` matches case, preferring the
     binding's own fully-instantiated `TY_FN` over the thunk's.

   The second is the narrower change and does not touch the deliberate
   grounding invariant.
3. Add fixtures for all six passing variants in the Defect A table alongside the
   failing one -- the boundary is unusually sharp and easy to regress.

## Doc follow-up -- do these when the fix lands

**`docs/guides/logic-programming-guide.md`** is the doc this gap shaped. Its
Backtrack Monad section ships a **monomorphic `int`** implementation only
because the parametric one cannot compile. When A and B are fixed:

1. Promote the monad to parametric over `(Cons A)`: `pure`, `mplus`, `bind`,
   `list-flat-map`, and `print-all` all take a `[A]` type parameter. The working
   shape is already written out in *Combined repro* above -- lift it from there
   rather than redesigning it.
2. **Drop the `::` carrier casts** once Defect A is fixed. They exist purely as
   the workaround: `(:: ((mplus ...)) (Cons int))` becomes `((mplus ...))`, and
   the `(:: (fs) :int)` pairs inside `mplus` collapse if the typed-list
   concat story improves alongside.
3. **Delete the third bullet** ("Cons cells are int-carried ...") from the
   constraints blockquote under the code, and drop the link to this report.
   Keep the `^fat` annotations -- those are about closure representation, not
   this bug.
4. **Re-run the block before publishing.** The original version of this section
   never compiled; that is exactly how these two defects stayed invisible. The
   monomorphic version currently in the guide prints `1 2 10 20` -- the
   parametric replacement should print the same for the int case, plus a
   float case (use a value with a real fractional part, e.g. `7.1`, so an
   int/float coercion bug cannot hide).

If only **Defect A** lands, steps 1-2 are still blocked -- B is the link-time
failure and gates any parametric version actually running. If only **B** lands,
the parametric version compiles but still needs the casts from step 2.

## Related

- [poly-result-hof-capturing-closure-sigbus.md](poly-result-hof-capturing-closure-sigbus.md)
  -- **same family, different bug.** There: a capturing closure passed *into* a
  HOF whose result is a bare tyvar, compiling clean and crashing with SIGBUS.
  Here: a closure *returned from* a generic function over a type application,
  failing in the checker (A) and the linker (B). Distinct symptoms and
  positions, but all three are "polymorphic instantiation meets closure" -- a
  fix in the substitution/emission walk may well touch all of them, so worth
  reading together.
- [composite-type-alias-gap.md](../archive/history/composite-type-alias-gap.md) -- the
  missing transparent alias, which is why these signatures had to spell
  `(fn [] (Cons A))` inline everywhere instead of naming it once. **Resolved**
  (`defalias` takes composite targets as of Phase TA2), but only for a
  *monomorphic* target -- `(defalias Backtrack (fn [] int))` names the
  int-carried shape, while the parametric `(fn [] (Cons A))` still cannot be
  named, since alias type parameters remain unsupported. This defect is what
  keeps the parametric version out of reach.
- [defn-shadows-return-special-form.md](../archive/history/defn-shadows-return-special-form.md) --
  the third defect found in the same guide (resolved as TUR-W0042; archived).

## Guide upkeep

When this report is resolved -- or any representation/bridge it describes
changes shape on the way -- update
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
in the same PR: fix the representation inventory, move this report's row out
of the missing-cells table, and correct the link when the report moves to
`docs/archive/`.

## Re-verification note (2026-08-13) -- the pinned line references have rotted

Defect A still reproduces exactly as filed on `main` at v0.33.2:

```
gca.tur:4:26: error [TUR-E0001]: function 'use' arg 1:
  expected (type-app Cons tyvar 'A'), got (type-app ? ?)
```

But the "Root cause -- Defect A (pinned)" section's second cause does **not**
describe the current tree, and anyone starting from it will lose time. The
report says the correct instantiated type is clobbered at
`elab_call.c:3706-3708` by

```c
if (fn_binding->closure_fn_binding) { fn_type = fn_binding->closure_fn_binding->type;
```

That code still exists (now around `:4026`), but a probe placed on it prints
**nothing** for the repro above -- the swap never executes. `((mk 1))` has an
*expression* head, so it routes through `elab_call_head_expr` (`:1355`), which
builds a temp binding from `head_expr->type` and only then attaches
`closure_fn_binding` (`:1380`/`:1391`). Whether the shell arrives already in
`head_expr->type` or is substituted later was not determined.

So cause 1 (the deliberate `!fn_type_has_named_tyvar` drop at what is now
`elab_fns.c:6635-6637`, which the report pins convincingly and which is easy to
re-confirm) stands, and cause 2 needs re-pinning against the current tree before
fix direction 2's second candidate ("stop `elab_call.c:3706` from clobbering")
can be attempted -- there is presently nothing there to stop.

The verified-at line `v0.32.2 (tree b54ab718e)` at the top of this report is
doing real work; treat every `file:line` below it as needing a re-check first.
Defect B was not re-verified in this pass.
