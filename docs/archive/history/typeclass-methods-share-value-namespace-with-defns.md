# Typeclass methods share the value namespace with `defn`s (and lose silently)

> **Status (2026-06-05): RESOLVED -- fix (1) landed on top of fix (2).** A free
> `defn` and a *user-defined* typeclass method of the same name now **coexist**
> in one module. A bare `(m x ...)` dispatches to the matching instance when the
> receiver's static type selects one, and falls back to the free `defn`
> otherwise. Both repros below now behave as the validation requires: Repro A
> reaches the method (`from-method`), Repro B dispatches and prints `6`. The
> intentional stdlib-method-override pattern is preserved -- the matcher excludes
> stdlib classes (`from_stdlib`), so a user `defn` of a stdlib method name (e.g.
> a local `show`) still wins. `TUR-W0039` still fires for the user-class clash,
> now reworded to describe the dispatch-vs-fallback rule rather than silent
> shadowing.
>
> **Fix (1) implemented in:** `src/compiler/elab_call.c`
> (`elab_user_method_instance_matches` + the `prefer_method_dispatch` gate in
> the bare-call dispatch block). **Fix (2), still in place:**
> `src/compiler/typeclass.{h,c}` (`from_stdlib` flag),
> `src/compiler/elab_typeclasses.c` (set the flag at `defclass`),
> `src/compiler/diag.{h,c}` (`TUR_W0039_METHOD_DEFN_CLASH`),
> `src/compiler/elab_toplevel.c` (post-pass-2 clash scan, reworded message).
> **Fixtures:** `tests/fixtures/typeclass-method-defn-clash/` (Repro A: dispatch
> wins) and `tests/fixtures/typeclass-method-defn-coexist/` (Repro B: a free
> combinator and a function-receiver method share the name `arr`).
>
> With this, `stdlib/arrow-class.tur` *could* now merge back into
> `stdlib/arrow.tur` (one surface); that consolidation is left to the Arrow
> reintroduction plan's follow-up since it is a snapshot-touching refactor, not
> part of this fix.

**Summary.** A typeclass method name and a top-level `defn` of the same name
occupy the *same* value namespace. When both exist, the free `defn`
unconditionally wins at every bare call site, the method becomes unreachable by
its bare name, and **no diagnostic is emitted**. This is surprising because
types already have their own namespace (MF4: structs/GADTs resolve in
type-annotation position, distinct from values), so one reasonably expects
methods to be similarly namespaced rather than colliding with ordinary
functions.

**Severity.** Expressiveness hole + silent shadowing. Not a miscompile, but the
silence is the dangerous part: a method can be quietly shadowed into oblivion,
and two conceptually-separate surfaces (a class method `arr` and a free helper
`arr`) cannot coexist in one module at all. This directly forced the Arrow
typeclass layer into a *separate file* (`stdlib/arrow-class.tur`) instead of
living alongside the bare combinators in `stdlib/arrow.tur`; see
[stdlib-arrow-typeclass-reintroduction-plan](../upcoming/stdlib-arrow-typeclass-reintroduction-plan.md).

## Repro A -- silent shadow (method unreachable)

```turmeric
(defclass Showy [a] (render [x] : cstr))
(definstance Showy [int] (render [x] "from-method"))

;; A free defn with the SAME name as the method:
(defn render [x : int] : cstr "from-free-defn")

(defn main [] : int
  (println (render 5))   ;; prints "from-free-defn"; the method is dead, no warning
  0)
```

Observed: prints `from-free-defn`. Expected: at minimum a diagnostic that
`render` is declared both as a `Showy` method and as a free `defn`; ideally the
two live in separate namespaces (or the bare call is rejected as ambiguous and
must be disambiguated).

## Repro B -- the method-side breaks instead (the Arrow case)

When the free `defn` is the *less* useful one, the collision surfaces as a
downstream type error rather than a wrong value:

```turmeric
(defn arr [f] f)                                  ;; bare identity helper
(defclass Arrow [a] (arr [f] : a))                ;; method named arr
(definstance Arrow [(->)] (arr [f] (fn [x] (f x))))
(defn add1 [x : int] : int (+ x 1))
(defn main [] : int
  (let [a (arr add1)] (println (a 5)))            ;; TUR-E0002: 'a' not callable
  0)
```

The free `(arr [f] f)` wins, so `(arr add1)` returns `add1` typed as the
untyped echo of `f` (`int`), and `(a 5)` fails to type-check -- the method that
would have produced a callable arrow is never consulted.

## Root cause

`src/compiler/elab_call.c` resolves a bare call head by first looking up a
value binding (`fn_binding`). Typeclass-method dispatch is attempted **only as a
fallback when no such binding exists**:

```c
/* elab_call.c ~1104 */
/* Gated on `!fn_binding` so a user defn or local binding of the same name
 * always wins, and on class membership ... */
if (!fn_binding && call->as.list.len >= 2 &&
    elab_name_is_typeclass_method(e, name)) {
    ... rewrite (m a b ...) to (.m a b ...) and dispatch ...
}
```

And `elab_name_is_typeclass_method` (elab_call.c:616) walks the class env by
*name* only. So:

- Method names are registered/searched in the same value scope as `defn`s.
- The `!fn_binding` gate makes a free `defn` shadow the method silently.
- There is no check at `defclass` / `definstance` / `defn` time that flags a
  method-vs-defn name clash.

By contrast, struct/GADT *type* names live in a separate namespace
(`elab_types.c`: "MF4: separate struct / GADT namespaces"), which is exactly the
kind of separation methods lack.

## Fix directions

> **Landed approach (2026-06-05):** a hybrid of (1) and (3). A bare `(m args)`
> where `m` names a *user-defined* typeclass method prefers argument-type
> dispatch over a same-named free `defn` **when an instance matches the
> receiver's static type**; otherwise the free `defn` wins. This gives the
> coexistence (1) promised -- the bare and typeclass Arrow layers can live in one
> module -- using the dispatch-on-match mechanism of (3), while the conservative
> "only redirect on a real instance match, else keep the defn" rule and the
> stdlib-class exclusion together preserve the intentional-override use case
> (the regression guard below). The dotted `(.m ...)` form continues to force
> dispatch unconditionally.

1. **Separate method resolution namespace (preferred).** Treat a class method
   name as resolvable distinctly from value bindings, the way the dotted
   `(.method ...)` form already is. A bare `(m args)` where `m` is a method
   would dispatch on argument type regardless of a same-named `defn`; an actual
   free function and a method could coexist, with qualification (`(.m ...)` vs a
   module-qualified defn) to disambiguate when both are in scope. This is what
   would let the Arrow bare + typeclass layers reunite in one module.

2. **Hard-error (or warn) on a genuine clash (minimum viable fix).** At the
   point a `defn` and a class method (or two) claim the same name in the same
   module, emit a diagnostic instead of silently letting `defn` win. This makes
   the hole visible even if full namespacing is deferred. Cheap, and removes the
   "silent" from "silent shadow".

3. **Prefer dispatch when an instance matches.** When both a free `defn` and a
   class method named `m` are in scope and the call's argument types select an
   instance, dispatch (or warn-and-dispatch). More magic than (1)/(2); listed
   for completeness.

## Validation for a fix

All three criteria are now met (see the status block):

- Repro A: `(render 5)` reaches the method and prints `from-method` (the int
  receiver selects `Showy [int]`). Locked by
  `tests/fixtures/typeclass-method-defn-clash/`.
- Repro B: `(arr add1)` dispatches to the `Arrow [(->)]` instance and prints
  `6`. Locked by `tests/fixtures/typeclass-method-defn-coexist/`.
- Regression: a user `defn` of a **stdlib** method name (e.g. a local `show`)
  still wins -- the matcher excludes `from_stdlib` classes. Confirmed manually
  (`(defn show [x : int] : cstr "my-custom-show")` prints `my-custom-show`).
  A user-class fallback case -- a free `defn` that handles a receiver type with
  no instance -- also still resolves to the `defn`.

## Relationship to other work

- This is the namespace root-cause behind the "separate module" decision in the
  Arrow typeclass reintroduction. If fix (1) lands, `stdlib/arrow-class.tur`
  could merge back into `stdlib/arrow.tur` and expose one surface.
- Independent of the
  [reversible-name-mangling-plan](../upcoming/reversible-name-mangling-plan.md):
  that is about *C-identifier* spelling/injectivity; this is about *source-level*
  name resolution. They do not overlap.
