# Generic `show-line` / `print-show` wrappers: two rough edges (void return + unresolved element)

**Severity:** low (both are narrow; each has an easy call-site workaround, used in
the stage-4 fixtures)

`show-line` and `print-show` (stdlib/typeclass-show.tur) are generic `^Show a`
wrappers that `(show x)` an owned String, print it, release it, and return
`:void`.  Both are the idiomatic replacement for the pre-stage-4
`(println (show x))`.  Two shapes miscompile.

## Edge 1 -- untyped empty collection through the wrapper ICEs

```turmeric
(load "stdlib/typeclass.tur")
(defn main [] : int (do (show-line (vec-new)) 0))
```

```
tur: internal error (ICE): carrier<->concrete crossing reached code emission
with an unresolved parametric param at emit_reresolve_disp_type.
```

`(vec-new)` is a `Vec` with an unresolved element type (no elements to infer `A`
from).  Passing it through the generic `^Show a` wrapper reaches emit with the
element tyvar still unresolved.  `(show (vec-new))` **directly** (not through the
wrapper) is fine -- it renders `[]`.  Workaround: ascribe the element type,
`(show-line (:: (vec-new) (Vec int)))`.

## Edge 2 -- void wrapper result captured under CPS lowering

```turmeric
(load "stdlib/typeclass.tur")
(defn yn [b : bool] : cstr (if b "T" "F"))
(defn demo [] : int
  (let [a (string/from-cstr "Hello")
        b (string/from-cstr "World")
        c (string/concat a b)]
    (do
      (println (string/to-cstr c))
      (println (string/len c))
      (show-line c)                                    ;; void, mid-do
      (println (yn (eq? a (string/from-cstr "Hello")))) ;; typeclass dispatch after
      (println (yn (string/eq? a b)))
      0)))
(defn main [] : int (demo))
```

```
error: variable or field '__t238' declared void
error: void value not ignored as it ought to be
```

The emitted C is `__auto_type __t238 = show_hyline(...); __t62 = __t238;` -- the
`:void` result of the generic `show-line` is captured into a temp and assigned,
which is only reachable through the CPS->direct lowering when a later `do`
statement introduces a CPS boundary (here the `eq?` typeclass dispatch).  A
`show-line` mid-`do` with only plain statements after it compiles fine; it is
the interaction with the following dispatch that trips it.  Workaround: use the
explicit form `(let [s (show c)] (do (println (string/to-cstr s)) (string/release s)))`.

## Root cause / fix directions

Both are generic-wrapper monomorphization/lowering gaps, not `Show`-specific:

- Edge 1: `emit_reresolve_disp_type` should resolve (or cleanly diagnose) an
  unresolved element tyvar reached via a `^Class a` wrapper whose argument is an
  element-less parametric container -- rather than ICE.  The default element
  carrier (int) that `(show (vec-new))` uses directly should also apply when the
  container flows through one generic hop.
- Edge 2: the CPS->direct converter must not bind a `:void`-returning call's
  result to an `__auto_type` temp.  A void call in statement position should emit
  as a bare statement even when a later `do` element forces CPS lowering.

## Notes

Filed while landing stage 4 of docs/archive/show-owned-result-plan.md.  The
stage-4 fixtures use the two workarounds above (type ascription for the empty
collection in show-collections; the explicit release form in string-basic /
string-slice), so the wrappers are otherwise in wide use and work for every
element-typed value and every non-CPS-boundary `do` position.
