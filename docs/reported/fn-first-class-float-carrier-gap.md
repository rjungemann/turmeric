# `:fn` first-class carrier does not round-trip float-class types

> **Status: RESOLVED (F5 landed).** A *typed* `:fn` carrier -- a `(fn [A...] :
> R)` parameter -- now threads its concrete signature through the
> `tur_poly_fn_t` carrier, so `float`/`cstr`/`ptr<T>` arguments and results
> round-trip. The two guards below remain in force for the *bare* `:fn` carrier
> (which has no result-type information and so still defaults to the int64
> signature). See the Resolution section at the bottom.

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

- `tests/fixtures/errors/fn-float-carrier-unsupported/` asserts the guard fires
  for the *bare* `:fn` carrier.
- `tests/fixtures/fn-first-class-application-typed/` covers `:float`/`:cstr`
  round-trips through the *typed* carrier -- for a named fn, a non-capturing
  lambda, a capturing closure, a multi-arg signature, and the `:fn` -> `:fn`
  pass-through.

## Resolution (F5 -- typed carrier)

Landed as the "full carrier unification" variant of plan phase F5
([fn-type-first-class-application-plan.md](../upcoming/fn-type-first-class-application-plan.md)).
A plain `(fn [A...] : R)` *parameter* whose every argument and result is a
single-register scalar kind (int/bool/float in a GP/xmm register, or a pointer:
`cstr` / `ptr<T>` / `ptr<void>`) is now routed through the `tur_poly_fn_t`
carrier with its concrete signature attached, rather than the old
bare-function-pointer path. Consequences:

- **Float/cstr round-trip.** The carrier stores a *natively typed* thunk
  (`make_poly_wrapper` retypes every argument to its native kind for a typed
  carrier; a capturing closure stores its own concrete thunk), and the call site
  casts `g.fn` to the concrete `R(*)(void*, A...)` -- so a `float` arg/result
  survives the xmm register class and a `cstr`/`ptr` no longer truncates through
  int64 (the prior `-Wint-conversion` "works by luck" path is gone).
- **Capturing closures now work** through a typed `(fn ...)` parameter. The old
  bare-pointer representation had no env slot, so passing a capturing closure to
  a `(fn [int] : int)` / `(fn [float] : float)` parameter *segfaulted* (the box
  pointer was called as raw code) -- regardless of the argument type. Routing
  through the `{env, fn}` carrier fixes that uniformly.

Kept on the nominal `TY_FN` (bare-pointer) representation -- so existing
behaviour is unchanged -- are: `^fat` and substructural (`^linear`/`^unique`/
`^affine`/`^relevant`/`^borrow`) parameters, function types carrying an effect
row (`#{...}`), polymorphic/named-tyvar signatures, variadic signatures, and any
signature with a non-scalar (struct/ADT/nested-fn) argument or result.

### Implementation pointers

- `elab_fns.c` -- parse a plain carrier-safe `(fn [A...]:R)` param as the typed
  carrier (`is_poly_fn`, `poly_type` = the concrete `TY_FN`); `fn_kind_is_carrier_scalar`
  / `fn_type_is_carrier_safe` gate eligibility; the forward-binding early update
  also propagates `arg_poly_fn` so a *recursive* self-call boxes its argument.
- `elab_call.c` -- `make_poly_wrapper` grows a `typed_concrete` flag that retypes
  every wrapper argument to its native kind; the construction-side float guard is
  skipped for a typed-carrier parameter; `elab_poly_call` infers the result kind
  from the concrete signature.
- `emit_expr.c` -- the `is_poly_call` branch uses the concrete-cast path whenever
  the carrier binding has a concrete `TY_FN` `poly_type`.
