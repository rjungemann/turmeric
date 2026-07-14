# Escaping fat closure's captured-env struct is never freed

**Severity:** low (bounded per-closure-construction leak; not a crash or
miscompile). General codegen -- **not** CPS/effect-specific (reproduces with no
effects at all). Keeps `requires.no-leak-check` on `cps-backend-fn-param`.

## Summary

Constructing a closure that captures locals allocates a heap "fat" env struct
(`struct __env_N { <thunk fn ptr>; <captures...> }`) via `malloc`. When the
closure escapes (returned / stored / passed as `^fat`), that env struct is never
freed -- one leak per closure construction that escapes.

## Minimal repro (no effects)

```turmeric
(defn make-scaler [k : float] : ptr<void>
  (fn [x : float] : float (* x k)))          ; captures k -> heap env
(defn use-it [^fat h : (fn [float] #fx{} float)] : float (h 3.0))
(defn main [] : int (println (use-it (make-scaler 2.0))) 0)
```

LSan: `16 byte(s) leaked` from `make_scaler` (the `struct __env_N` malloc). The
in-tree fixture `cps-backend-fn-param` hits the same site via a `handle`, but the
effect is incidental -- the leak is the closure env.

## Root cause

`src/compiler/emit_expr.c` (~`:5511`-`:5537`): a captured closure lowers to
`struct __env_N *tmp = malloc(sizeof(struct __env_N)); tmp->__fn = ...;
tmp->cap = ...;` and the fat closure value carries `tmp`. No corresponding
`free` is emitted when the closure's lifetime ends, so an escaping (or even a
locally-dropped) fat closure leaks its env. This is the closure analogue of the
by-value owning-field leak in `docs/reported/byvalue-struct-local-owning-field-leak.md`
-- part of the deferred owning-pointer / drop-glue lifecycle work, but tracked
separately here because the allocation site and fix differ.

## Fix directions

1. Give the fat-closure env the same drop treatment as other owning heap values:
   inject a `free`/`drop!` of the env when the closure value is dropped, and a
   retain/copy when it is duplicated (an escaping closure that outlives its
   constructor transfers ownership to the callee/holder).
2. This needs the closure to participate in the RC/drop / uniqueness analysis so
   a shared closure is not double-freed. Until closures carry drop glue, the
   conservative interim is the current leak.
3. Interim: `cps-backend-fn-param`, `free-lift-bind`, `unsafe-closure-capture`
   keep `requires.no-leak-check`.

## Scoped fix

A concrete, phased design lives in
[docs/upcoming/closure-drop-glue-plan.md](../upcoming/closure-drop-glue-plan.md):
an env drop-glue function (`drop_glue_env_N` -- walk owning captures, then free),
S1 a scoped free for NON-escaping closures (partial-app + non-retaining HOF arg,
no ownership tracking), and S2 move-based drop glue for ESCAPING (stored)
closures. S1 clears this leak for the non-escaping fixtures; S2 covers stored
closures (parser combinators, httpd middleware).
