# M7 stdlib migration: Monad `bind` blocked on continuation/result representation reconciliation

**Summary.** `Monad` is receiver-dispatched (`bind` dispatches on `ma`), so it has
no return-directed inference problem like Applicative/Alternative. But migrating
its signature to the by-value shape `(bind [ma : (m a) k : (fn [a] (m b))] : (m
b))` is blocked because the continuation `k` RETURNS the monadic value `(m b)`,
and that result cannot be reconciled with the carrier ABIs `bind` relies on. Two
approaches were tried (2026-06-19); both fail:

## Approach A -- continuation as a by-value `tur_poly_fn_t` element

Mark `k : (fn [a] (m b))` `is_poly_fn` (remove the HKT-returning-result exclusion
from the M7 capturing-closure gate in `elab_typeclasses.c`). A CAPTURING
continuation then works for a custom `MyMonad [Option]` (probe-style, returns
121), BUT the reference bind probe fails to COMPILE:

```
error: incompatible types when returning type 'Option__int' but 'int64_t' was expected
```

Root cause (generated C): the poly wrapper `__poly_N` built by
`make_poly_wrapper` (`elab_call.c:4200`) has the fixed `tur_poly_fn_t` thunk ABI
`int64_t (*)(void*, int64_t)`, but the wrapped continuation `__fn_N` returns the
by-value `Option__int` struct:

```c
static Option__int __fn_1009(int64_t x) { ... return (Option__int){...}; }
static int64_t __poly_1011(void *env, int64_t x) { return __fn_1009(x); }  /* struct -> int64 */
```

The wrapper must SPILL the by-value `(m b)` struct to the int64 carrier (box it),
and then `bind`'s body must BRIDGE the carrier `(k x)` result back to the by-value
`(m b)` -- and the body's `if` (`(if (some? ma) (k (.value ma)) (none))`) must
unify a carrier `(k x)` branch with a by-value `(none)` branch. That is a
multi-bridge reconciliation, not a localized change.

## Approach B -- keep the continuation as the `:fn` carrier, type only `ma`/result

Change the sig to `(bind [ma : (m a) [k :fn]] : (m b))` (continuation stays the
`tur_poly_fn_t` poly carrier that already handles capturing under the OLD sig),
keep every instance body unchanged (inline-C `fn.fn(fn.env, ...)` returning the
int64 carrier). This COMPILES but SEGFAULTS at runtime on a capturing
continuation: the body returns the int64 carrier while the now-by-value result
`(m b)` consumer reads it as the `Option__int` struct, and the carrier-base
dispatch (whose dict template emits bare `some?`/`ok` calls -- the implicit-decl
warnings) is reached with a truncated pointer.

## Why Functor migrated cleanly but Monad does not

Functor's element fn returns a PLAIN element `b` (an int-register-class value the
`tur_poly_fn_t` thunk ABI carries losslessly), and its combinator instances stay
carrier with the result bridged. Monad's continuation returns the WRAPPED `(m b)`
-- a by-value aggregate -- which the fixed int64 thunk ABI cannot carry without an
explicit spill/bridge on BOTH the wrapper-return and bind-body sides.

## Fix directions

1. **Wrapper struct-return spill + bind-body result bridge.** Teach
   `make_poly_wrapper` to spill a by-value-aggregate inner result to the int64
   carrier (box), and have the by-value `bind` spec bridge the carrier `(k x)`
   result back to `(m b)` so the body `if` unifies. Mirrors the existing
   carrier<->by-value bridge used for the migrated Functor combinator instances,
   extended to the continuation-RESULT position.
2. **Typed thunks for by-value-struct returns** (generalize the
   poly-to-fat-typed-shim-plan to struct-returning thunks), so `__poly_N` can be
   `Option__int (*)(void*, int64_t)` and no spill is needed.

## Validation

- The bind probe (`v2/m7-hkt-probe-bind.tur`) exits 21 AND a capturing
  continuation exits 121, with `Monad [Option]`/`[Result]` rewritten by-value;
  `bash tests/run.sh` green; do-m notation over Option/Result unaffected.
