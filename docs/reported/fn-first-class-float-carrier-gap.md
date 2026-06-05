# `:fn` first-class carrier does not round-trip float-class types

**Summary:** A first-class `:fn` value (the `tur_poly_fn_t {env, fn}` carrier) is
the int64 register class. Integer-register kinds (`int`, `cstr`, `ptr<T>`,
`bool`) round-trip through it, but a **float-class** argument or result
(`float`/`float32`/`float64`) lives in a different register class (xmm0), so
passing one through the carrier is a register-class miscompile.

**Severity:** Ergonomics/expressiveness gap, *not* a live miscompile any more.
As of the `fn-first-class-application` work the compiler **rejects** float-class
`:fn` application/coercion with a hard error, so no program silently miscompiles;
the gap is that the typed-signature round-trip (phase F5 of the plan) is not yet
implemented.

## Minimal repro (now a clean error, previously a silent miscompile)

```turmeric
(defn applyf [g : fn x : float] : float (g x))
(defn ident  [n : float] : float n)
(defn main [] : int
  (let [r (applyf ident 7.5)]
    (if (> r 7.4) (if (< r 7.6) 1 9) 0)))   ; want 1 (r == 7.5)
```

- **Observed before the guard:** the call site emitted `g.fn(g.env, (int64_t)(x))`,
  truncating `7.5` to `7`; `r` came back `7.0`, the program returned `0`.
- **Observed now:** a hard error -- "applying a `:fn` value to a floating-point
  argument is not supported ...".
- **Expected (with F5):** `r == 7.5`, program returns `1`.

## Root cause

The carrier's function-pointer field is fixed at `int64_t (*)(void *, int64_t)`
(see `tur_poly_fn_t` in `emit_module.c`). The generic poly-call dispatch
(`emit_expr.c`, the `is_poly_call` branch) widens every argument with
`(int64_t)(arg)`, which is a value *conversion* (truncating a `double`), not a
bit-preserving reinterpretation -- and even a bit-preserving reinterpretation
would land the value in a gp register while a concrete float thunk reads xmm0.

`make_poly_wrapper` (`elab_call.c`) already retypes float-class *arguments* of a
plain named inner fn to their concrete kind, and `kind_is_non_int_register_class`
(`elab_fns.c`) is the single extension point that flags float results. The pieces
for a typed round-trip exist but are not wired through the `:fn` call site.

## Guard (current behaviour)

`elab_poly_call` rejects a float-class argument when the callee is a bare `:fn`
(mono carrier, `poly_type == NULL`), and the `arg_poly_fn` coercion in
`elab_call_fn` rejects boxing a function whose signature has a float-class
argument or result into a `:fn` slot. Integer-register kinds are unaffected.

## Proposed fix (plan phase F5)

Extend the Phase-F concrete poly-call dispatch (`emit_expr.c`, currently gated on
`type_kind_is_poly_concrete` for sub-64-bit ints) to cover float-class result and
argument kinds: cast `g.fn` to the concrete `R (*)(void *, A...)` signature and
pass native-typed args, so the call matches the concrete-typed wrapper/thunk the
carrier stores (`make_poly_wrapper` already retypes float args; the closure
pass-through stores a natively concrete thunk). Allow `:fn` to carry an explicit
`(fn [A...] : R)` signature so the concrete kinds are known at the call site, and
relax the two guards above once the typed path lands.

## Validation

- `tests/fixtures/errors/fn-float-carrier-unsupported/` asserts the guard fires.
- A future `tests/fixtures/fn-first-class-application-typed/` should cover
  `:float`/`:cstr`/`:ptr<T>` round-trips once F5 lands (the plan's `*-typed`
  fixture).
