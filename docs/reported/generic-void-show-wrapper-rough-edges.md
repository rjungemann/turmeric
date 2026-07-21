# Generic `show-line` / `print-show` wrappers: two rough edges (void return + unresolved element)

**Severity:** low (both are narrow; each has an easy call-site workaround, used in
the stage-4 fixtures)

**Status: Edge 2 RESOLVED (2026-07-21), Edge 1 still OPEN.** Report retained here
for Edge 1. See "Edge 2 resolution" below.

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

### Edge 2 resolution (2026-07-21)

Fixed in the CPS emitter (`src/compiler/emit_cps_ir.c`, the `CT_TAILCALL`
`cps->direct` arm). A void call reaches this arm as a tailcall delivering to a
continuation (KK_VAR inline join, in the repro), and the arm unconditionally
bound the result into an `__auto_type` temp. The arm now queries the callee's
return type via its `FnDef` (`fd_for_binding` + `fn_ret_type`) and, when it is
`TY_NIL`/`TY_NEVER` (a `:void`/`:nil` callee, which emits as C `void`), emits the
call as a bare statement and delivers the unit placeholder `0` to the
continuation. This mirrors the existing `CT_LETCALL` nil arm (which already
checked `x.ty == TY_NIL`) and `emit_value`'s `TY_NIL`/`TY_NEVER` skip in the
direct emitter. The tailcall node carries no result type of its own, hence the
`FnDef` lookup.

Regression fixture: `tests/fixtures/cps-void-show-wrapper-midbody/` -- a
`(show-line 42)` mid-`do` followed by `eq?` dispatches (the CPS boundary); prints
`42 / T / F`. (`show-line` on an int renders deterministically via `Show[int]`;
the report's original String repro renders a raw pointer for a separate reason --
there is no `Show[String]` instance, so `show` on a `String` falls back to the
int carrier representative and prints the pointer. That carrier-fallback misrender
is the dispatch gap tracked in
`docs/reported/method-dispatch-missing-instance-falls-back-to-carrier-representative.md`,
NOT this codegen bug; it reproduces identically in the direct emitter with no CPS
boundary involved.)

## Root cause / fix directions

Both are generic-wrapper monomorphization/lowering gaps, not `Show`-specific:

- Edge 1 (OPEN): `emit_reresolve_disp_type` should resolve (or cleanly diagnose)
  an unresolved element tyvar reached via a `^Class a` wrapper whose argument is
  an element-less parametric container -- rather than ICE.  The default element
  carrier (int) that `(show (vec-new))` uses directly should also apply when the
  container flows through one generic hop. This lives in the ABI carrier-crossing
  recovery machinery (the deep-side `emit_abi_assert_routed_concrete` invariant at
  `emit_core.c:1820`), which is why it is left open rather than point-fixed.
- Edge 2 (RESOLVED): the CPS->direct converter must not bind a `:void`-returning
  call's result to an `__auto_type` temp. A void call in statement position now
  emits as a bare statement even when a later `do` element forces CPS lowering.
  See "Edge 2 resolution" above.

## Notes

Filed while landing stage 4 of docs/archive/show-owned-result-plan.md.  The
stage-4 fixtures use the two workarounds above (type ascription for the empty
collection in show-collections; the explicit release form in string-basic /
string-slice), so the wrappers are otherwise in wide use and work for every
element-typed value and every non-CPS-boundary `do` position.
