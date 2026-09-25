# `#lang r7rs`: `apply` and dynamic calls stop at four arguments

**RESOLVED 2026-09-25, archived.** The ceiling is eight now, on every path,
behind one constant (`TUR_FAT_SHIM_MAX_ARITY`, types.h): the fat-closure
shim family (`__tur_fatshim0..8`, `__tur_poly_to_fat0..8`), the `any`
widen of a bare function and the `^fat` auto-shim (elab_call.c), the
fat-normalization rule (types.c), the dynamic call's fixed slots and the
trampoline's descriptor (`__tur_dyn_call_var`, `__tur_tb_call`,
`__tur_tb_tail`, `__tur_tb_bounce_box`, `tur_tb_desc`), the two bounce
decisions (emit_expr.c, emit_cps_ir.c), and the prelude's
`r7rs-apply-list__`. A procedure of up to eight parameters is a first-class
value on the compiled back end -- through `apply`, a variable, a parameter,
`call-with-values`, a variadic callee and a 100,000-deep dynamic tail call
-- and the direct call to a named procedure has no limit. Past eight, a
dynamic call is refused at compile time and `apply` panics, each saying to
pass the rest as a list. Pinned by `tests/fixtures/r7rs-apply-many-args`
(both back ends). Original report follows.

**Severity:** low-medium. A Scheme procedure of five or more parameters can
be called directly, but not through `apply` (both back ends) and not through
a variable that holds it (compiled back end: a compile-time refusal; the
interpreter answers). R7RS puts no bound on either. Documented in
docs/guides/r7rs-guide.md ("Where it differs") and r7rs-lang-plan 9.3; this
report gives it a repro and a root cause. No chibi test reaches it.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (f a b c d e) (+ a b c d e))
(write (f 1 2 3 4 5))              ; 15 on both back ends
(write (apply f '(1 2 3 4 5)))     ; want 15
```

```
$ tur run apply5.tur
panic at ...: apply: more than 4 arguments is not supported yet (r7rs-lang-plan R6)
$ tur --interpret apply5.tur
panic: apply: more than 4 arguments is not supported yet (r7rs-lang-plan R6)
```

The same procedure through a variable:

```scheme
(define g f)
(write (g 1 2 3 4 5))              ; want 15
```

```
$ tur run dyn5.tur
dyn5.tur:5:8: error: calling a dynamic value with 5 arguments is not supported
by the compiled back end (the fat-closure apply helpers stop at 4);
`tur --interpret` has no such limit
$ tur --interpret dyn5.tur
15
```

Measured 2026-09-25 against `./build/tur` v0.51.0 (Debug).

## Root cause

Two ceilings, one per layer:

- **The prelude's `apply`.** `r7rs-apply-list__`
  (stdlib/r7rs/prelude.tur:1640-1647) spreads the list into a direct call by
  length, with arms for 0 to 4 and `r7rs-fail-any__` past that. It is the
  same procedure `call-with-values` uses (prelude.tur:1669-1672), so a
  producer of five values cannot reach its consumer either (measured: the
  same panic). The prelude is
  compiled on both back ends, which is why the interpreter refuses `apply`
  too.
- **The compiled dynamic call.** `emit_dyn_call`
  (src/compiler/emit_expr.c:6704-6713) refuses `n > 4` outright: the fat
  protocol's typed apply helpers are `TUR_APPLY0_T` to `TUR_APPLY4_T`
  (src/compiler/emit_module.c:9823), and the emitter does not want a sixth
  ceiling in a second place. A direct call to a named Scheme procedure is
  a static call and has no limit, which is why `(f 1 2 3 4 5)` works.

The interpreter's dynamic call has no arity table, so its only limit is the
prelude's `apply`.

## Fix directions

- **The dynamic call, in a Scheme-only path.** R6 already packs the SURPLUS
  arguments of a variadic callee into a `(Cons any)` rest chain
  (`__tur_dyn_call_var`). A dynamic call with more than four arguments could
  take the same route: pass the first four positionally and the rest as a
  chain, with the callee's fixed-arity entry unpacking them. Or extend the
  shim table to eight, which is what most Schemes' fast paths do, and keep
  the chain for the rest.
- **The prelude's `apply`.** Once the dynamic call takes a chain, `apply`
  can hand its spread list over as one, with no arm per arity.

Preserve: Turmeric's and Saffron's own dynamic-call ceiling is theirs to
decide (r7rs-lang-plan 9.1); a lift in `emit_dyn_call` that is right for
every dialect is fine, a Scheme-only unpacking is gated on `LANG_R7RS`.
