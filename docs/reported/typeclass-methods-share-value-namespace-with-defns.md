# Typeclass methods share the value namespace with `defn`s (and lose silently)

> **Status (2026-06-05): partially resolved -- fix (2) landed.** The *silence*
> is gone: a free top-level `defn` that collides with a **user-defined**
> typeclass method now raises `TUR-W0039` (warning, not error, so the
> intentional stdlib-method-override pattern stays expressible). Stdlib classes
> are exempt (`TypeClass.from_stdlib`). The deeper namespace separation
> (fix (1)) that would let the bare + typeclass Arrow layers reunite in one
> module is **still open** -- Repro B continues to fail to type-check (it now
> warns first). See the "Fix directions" / "Validation" notes below for the
> remaining work.
>
> Implemented in: `src/compiler/typeclass.{h,c}` (`from_stdlib` flag),
> `src/compiler/elab_typeclasses.c` (set the flag at `defclass`),
> `src/compiler/diag.{h,c}` (`TUR_W0039_METHOD_DEFN_CLASH`),
> `src/compiler/elab_toplevel.c` (post-pass-2 clash scan). Fixture:
> `tests/fixtures/typeclass-method-defn-clash/`.

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

- Repro A: with fix (1) or (3), `(render 5)` reaches the method (or is a
  diagnosable ambiguity); with fix (2), compilation fails with a clear
  "name clashes with typeclass method" error rather than silently printing
  `from-free-defn`.
- Repro B: `(arr add1)` dispatches to the `Arrow [(->)]` instance and prints
  `6`.
- Regression: the existing "a user `defn` of a stdlib method name (e.g. a local
  `show`) intentionally overrides dispatch" behavior must remain expressible --
  fix (1)/(3) should keep a way to bind a plain function that wins, so the
  current intentional-override use case in `elab_call.c` is not lost.

## Relationship to other work

- This is the namespace root-cause behind the "separate module" decision in the
  Arrow typeclass reintroduction. If fix (1) lands, `stdlib/arrow-class.tur`
  could merge back into `stdlib/arrow.tur` and expose one surface.
- Independent of the
  [reversible-name-mangling-plan](../upcoming/reversible-name-mangling-plan.md):
  that is about *C-identifier* spelling/injectivity; this is about *source-level*
  name resolution. They do not overlap.
