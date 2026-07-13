# `Eq [Bound]` `.eq?` misdispatches to the wrong instance when any extra `Eq` instance is present

**Severity:** High (silent wrong-code -> segfault on a working typeclass method).

**Status:** RESOLVED (2026-07-13). Root cause was narrower than "any extra `Eq`
instance": the offender is any instance whose `type_args[0]` is a bare
`TY_TYVAR` -- i.e. registered for a type name that never resolves to a concrete
nominal type. `stdlib/str.tur`'s `Eq [str]` is exactly this: `str` has no
`defstruct`/`defdata`, so it stays an unbound tyvar. In `elab_method_call`'s
`KIND_ARROW` instance search (`src/compiler/elab_typeclasses.c`), a bare-tyvar
`type_args[0]` passed the `type_ok = !inst_is_primitive` gate and no
discrimination block rejected it, so it became an *exact* match and, being
first in registration order, shadowed the receiver's own concrete instance.
The `KIND_STAR` (primitive-receiver) path already treated a tyvar arg as a
non-match/fallback; the fix aligns `KIND_ARROW` with it: when the receiver is a
concrete nominal type (a `TY_ADT` with a def, bare or applied), a bare-tyvar
instance is demoted to a *fallback*, so the search continues to the concrete
instance and prefers it -- while a lone bare-tyvar instance still wins when it
is the only candidate (e.g. `.eq?` between two `str` views). Regression
coverage: `tests/fixtures/eq-adt-vs-tyvar-instance`. Full suite 2123/0, turi
1594/0.

## Summary

`(.eq? (Inclusive 4) (Inclusive 4))` -- an `Eq [Bound]` method call over the
`Bound` GADT (`stdlib/range.tur`) -- compiles to a call to the **wrong** Eq
instance (`__inst_Eq_eq_qu_str`) as soon as the program pulls in *any*
additional `Eq` instance beyond the auto-loaded set. The `str` instance then
reinterprets the `Bound*` argument as a `str` view and dereferences garbage,
segfaulting at runtime. Without the extra instance, the same call dispatches
correctly to `__inst_Eq_eq_qu_Bound` and runs fine.

This is compile-time instance selection producing wrong code, not a runtime
lookup miss -- there is no diagnostic.

## Minimal repro

```turmeric
(load "stdlib/range-bound.tur")
;; ANY extra Eq instance -- a fresh user type, or re-stating an existing one --
;; flips the dispatch below. Eq [bool] shown; Eq [cstr], Eq [Foo], etc. all do it.
(definstance Eq [bool] (eq? [a b] (if a b (if b 0 1))))
(defn main [] : int
  (println (if (.eq? (Inclusive 4) (Inclusive 4)) "ok" "ne"))   ; SIGSEGV
  0)
```

- Remove the `(definstance Eq [bool] ...)` line and it prints `ok`.
- `(load "stdlib/str.tur")` triggers it too (via its `Eq [str]` instance) --
  that is how it surfaced while un-carving `tests/fixtures/range-show`
  (interp-string-natives-and-range-show-plan): loading `str.tur` into
  `range-bound`'s graph to reach `str-concat`/`int->str` broke the fixture's
  `.eq?` assertions. The plan works around it by putting the string builders in
  the dependency-free leaf `stdlib/str-build.tur` (no `Eq` instance), so
  `range-bound` never pulls a second `Eq` instance.

## Root cause (observed in emitted C)

For the repro, `emit-c` lowers the `Bound` `.eq?` call to the `str` instance:

```c
__auto_type __ps_619 = (ctor_Inclusive(INT64_C(4)));
__auto_type __ps_620 = (ctor_Inclusive(INT64_C(4)));
__auto_type __ps_621 = (__inst_Eq_eq_qu_str(__ps_619, __ps_620));  // <-- should be __inst_Eq_eq_qu_Bound
```

`__inst_Eq_eq_qu_str` casts each arg to `struct { const char *p; size_t len; }*`
and reads `->p`, so it walks a `tur_adt_Bound*` as a string view -> segfault.

The receiver's static type is unambiguously `Bound` (it is `ctor_Inclusive`'s
result), so instance resolution has the information it needs; adding an
unrelated `Eq` instance perturbs the selection (a shifted index / "first match"
fallback in the `Eq` method-dispatch lowering is the likely mechanism). Look at
the `.eq?` -> instance mapping in `src/compiler/elab_typeclasses.c` (Eq method
lowering) and how the candidate instance list is ordered/indexed when more than
the auto-loaded instances are in scope.

## Impact

Any program that both (a) uses `Eq [Bound]` (`range->str` interval code, range
comparisons, `.eq?` on endpoints) and (b) declares or loads any other `Eq`
instance -- which is nearly every real program -- gets a silent miscompile and
crash. The stdlib currently side-steps it only because `range-bound`'s load
graph happens not to include a second `Eq` instance.

## Not fixed here

Out of scope for the string-natives / range-show plan, which only needed to
avoid *triggering* it. Filed for a follow-up focused on the `Eq` (and likely
`Ord`/`Show` -- the same GADT carries all three) method-dispatch instance
selection over GADT types.
