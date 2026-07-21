# Escaping fat closure's captured-env struct is never freed

**Severity:** low (bounded per-closure-construction leak; not a crash or
miscompile). General codegen -- **not** CPS/effect-specific (reproduces with no
effects at all). Keeps `requires.no-leak-check` on `cps-backend-fn-param`.

> **Progress (2026-07-20), report STILL OPEN.** Two adjacent slices landed:
>
> 1. A *different* env leak in the same machinery: a NON-escaping closure's env
>    leaked when the enclosing `let` also bound an owning `rc`/`ref` (an unrelated
>    `default: escape` false-positive on `EX_DEFER` / `EX_RC_OF` in
>    `binding_escapes_impl`). See
>    `docs/archive/history/fat-closure-env-free-owning-sibling.md`.
> 2. **S1c inferred non-retention (INLINE args):** a capturing closure passed
>    inline to a `^fat`/fn param the callee only CALLS is now freed at scope exit
>    (`Binding.nonretain_param_mask`, inline-C-gated for soundness). See the
>    closure-drop-glue-plan progress note (2026-07-20b).
>
> 3. **S1c fresh-closure-returning CALL arg:** this report's exact repro
>    `(use-it (make-scaler 2.0))` is now ASan/LSan-clean. `make-scaler`'s call
>    result is a fresh, uniquely-owned, scalar-capture env (`returns_fresh_closure`)
>    passed to a non-retaining `^fat` param, so it is hoisted and freed at scope
>    exit. See the closure-drop-glue-plan progress note (2026-07-20c).
>
> The MINIMAL REPRO above is fixed. This report stays OPEN for the remaining
> **S2** surface it also names -- closures that are STORED (returned into a
> struct/ADT and threaded onward: parser combinators, httpd middleware) rather
> than consumed once. Those need the move/uniqueness ownership model, not the
> consumed-once free. `cps-backend-fn-param`, `free-lift-bind`,
> `unsafe-closure-capture` keep `requires.no-leak-check` (different shapes;
> `free-lift-bind`'s residual is a non-closure free-monad ADT leak).

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

## Verified: why the same-scope free does not reach it (2026-07-20)

Reproduced under valgrind (16 bytes, 1 block; alloc trace `malloc <- main`).
Emitted flow for `(use-it (make-scaler 2.0))`:

```c
__auto_type __ps_157 = (make_hyscaler(2.0));                    /* returns the malloc'd env */
__auto_type __ps_158 = (use_hyit((int64_t)(intptr_t)(__ps_157))); /* consumes it, never frees */
```

Two reasons the existing conservative `let_binding_env_freeable`
(emit_expr.c:1349) cannot cover this:

1. **The env escapes its constructor.** `make-scaler`'s `(fn [x] (* x k))` is a
   direct `EX_CLOSURE`, but it is RETURNED, so `closure_binding_escapes` correctly
   forbids freeing it inside `make-scaler`. The owner is now `main` (holding it as
   `__ps_157`), which received it as a CALL RESULT, not a same-scope `EX_CLOSURE`
   literal -- so no local emit site knows it is a fresh, uniquely-owned env.

2. **`^fat` is a dispatch marker, not a linearity guarantee.** `use-it`'s
   `^fat h` selects the fat-call `{thunk,env}` protocol (elab_fns.c:1876,
   `is_fat`); it does NOT prove `h` is used exactly once and unaliased. A blanket
   "free the `^fat` param's env at scope exit" would double-free whenever the
   closure is shared (the same failure mode proven for `cps-reopen`: an
   ownership-blind free crashes).

So the fix genuinely needs the closure to carry cross-function ownership -- an
RC/drop or uniqueness bit that travels with the fat-closure value from
constructor through holder to consumer -- so exactly one owner frees the env.
That is the drop-glue subsystem the report already names; a same-scope or
per-site free cannot be made sound here. Confirmed low-severity (bounded, one
env per escaping construction; `cps-backend-fn-param` keeps `requires.no-leak-check`).
