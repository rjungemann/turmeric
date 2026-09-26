---
title: A float type parameter captured by a generic closure reaches a fn-typed callback as garbage
category: Reported
description: (defn setter [A] [v : A] : (fn [(fn [A] bool)] bool) (fn [k] (k v))) at A = float prints 4.7e-310. The shared thunk dispatches k through the int64 carrier ABI, while the fat box the caller built for k holds a typed shim that takes a double in xmm0. The per-spec inner-closure clone that fixes the returned-value shape declines this body because it "dispatches untyped".
---

# A float type parameter captured by a generic closure reaches a fn-typed callback as garbage

**RESOLVED 2026-09-26** -- fix direction 1.

- `binding_dispatch_is_untyped` (`src/compiler/elab_core.c`) no longer calls
  a fat dispatch through a `(fn [..] R)` binding untyped when `R` is a
  concrete scalar (`bool`, the int and float families, `cstr`, `nil`): the
  call's own type already is `R`, so Direction 3 has nothing to recover on
  the result side, and it already resolves the declared ARGUMENT types
  through the active spec. `(k v)` with `k : (fn [A] bool)` is therefore
  typed, and `closure_return_dispatches_untyped` stops vetoing the per-spec
  clone.
- `emit_inner_closure_needs_float_spec` (`src/compiler/emit_module.c`) also
  asks for the clone when one of the lifted closure's parameters is itself a
  fn type that takes or returns a float-bound type variable
  (`abi_fn_type_mentions_float_tyvar`). The clone dispatches `k` with the
  resolved signature -- `double` in `xmm0` -- which is what the typed shim in
  the fat box the caller built expects.

Pinned by `tests/fixtures/generic-closure-float-fn-callback`: the repro, an
`int` spec that still rides the carrier, two float arguments with an int
result, float in and out, a let-bound closure called with a named fn and a
lambda, the closure reaching its caller through a fn-typed parameter,
`float32`, and a by-value struct -- every row identical compiled and under
`--interpret`. No `expected.c` snapshot moved.

**Severity at filing: medium-high** (silent wrong answer, exit 0). Found 2026-09-25 while
fixing [generic-closure-capture-of-float-truncates](../archive/generic-closure-capture-of-float-truncates.md);
it was broken the same way before that fix (the value was then also
numerically converted on the way in), so it is pre-existing, not a regression.

## Repro

```turmeric
(defn setter [A] [c : int v : A] : (fn [(fn [A] bool)] bool)
  (fn [k : (fn [A] bool)] (k v)))
(defn show [x : float] : bool
  (println x)
  true)
(defn main [] : int
  (println ((setter 1 7.25) show))
  0)
```

Prints `4.68777e-310` then `true`; expected `7.25`. `--interpret` is right.

## Root cause

The env fill is correct now -- `setter__spec__void___int64_t_double` stores
the double's bits in the shared env's `int64_t v` slot. The READ side is not:
the one shared thunk `__fn_N(void *, int64_t k)` dispatches `k` as

```c
((bool (*)(void*, int64_t))(intptr_t)((int64_t *)k)[0])(k, env->v)
```

-- the int64 carrier ABI -- while the fat box `main` built for `show` has
`__tur_fatshim_bool_double` in slot 0, which takes the float in `xmm0`. The
bits go in `rsi`; the shim reads whatever is in `xmm0`.

The fix that landed for the returned-value shape is a per-spec clone of the
inner closure (`emit_inner_closure_needs_float_spec`, emit_module.c), which
dispatches with the resolved signature. Widening that predicate to "an inner
closure argument whose fn type mentions a float-bound tyvar" was tried and
does **not** fire: the outer defn's `closure_return_dispatches_untyped` is
set (elab_core.c `binding_dispatch_is_untyped` -- `k`'s `(fn [A] bool)` has a
non-tyvar result), and that flag vetoes the clone before the predicate runs.

## Fix directions

1. Teach Direction 3 (the typed-dispatch path the clone relies on) to type a
   dispatch through a binding whose ARGUMENT types mention a spec tyvar, not
   only its result, and relax `binding_dispatch_is_untyped` to match; then
   widen the clone predicate as above.
2. Or, at the dispatch site in the shared thunk, bridge through a carrier-ABI
   shim when the fat box's slot 0 is a typed shim -- the caller knows it built
   one, so it could store the carrier shim instead when the callee's param is
   a tyvar-typed fn.

A store of the captured value into a generic container (`vec-push!` inside the
closure -- the `dfs-set` shape) is fine: it rides the carrier both ways.
