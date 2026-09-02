# Erased `(fn [a] b)` method sink invokes a native-float poly wrapper through the int64 carrier cast

> **Status:** Fixed (2026-09-02). Found closing the `float32` residue of
> [erased-generic-field-read-overruns-subword-monomorph-box.md](../erased-generic-field-read-overruns-subword-monomorph-box.md).

**One-line summary:** A float-typed named function (or non-capturing
lambda) handed to a typeclass method whose declared `:fn` param is erased
(`fmap [container : (f a) g : (fn [a] b)]`) is boxed by `make_poly_wrapper`
as `static float __poly_N(void *, float)` -- the float rides xmm0 -- while
the instance body, compiled once for every `a`, calls it through
`((int64_t (*)(void*, int64_t))g.fn)(g.env, x)`: the argument goes out in
`rsi`, the result is read from `rax`. Silent wrong-number miscompile; a
capturing lambda's own `float (*)(void *, float)` thunk had the same
mismatch.

**Severity:** Silent miscompile (register-class mismatch, no crash). Any
`Functor`/`Monad`-style instance over a parametric record applied at
`float32`/`float`/`float64` through a rank-2 or dict-dispatched call.

## Minimal repro

```turmeric
(defstruct Identity :copy [a] (wrapped : a))
(defn mk-id  [A] [x : A]            : (Identity A) (make-struct Identity :wrapped x))
(defn run-id [A] [i : (Identity A)] : A            (.wrapped i))
(definstance Functor [Identity]
  (fmap [i g] (mk-id (g (run-id i)))))

(defn keep-f32 [v : float32] : float32 v)
(defn poly-f32 [^f] [^Functor f x : (f float32)] : (f float32) (fmap x keep-f32))
(defn use-f32 [g (forall [(f :: * -> *)] [(Functor f)] (-> (f float32) (f float32)))
               v : float32] : float32
  (run-id (g (mk-id v))))

(defn main [] : int
  (println (use-f32 poly-f32 2.5f32))     ; printed 7.216115e+12; expected 2.5
  (println (use-f32 poly-f32 -7.25f32))   ; expected -7.25
  0)
```

The `float` twin printed `4.68544e-310` (a denormal: the double's bits
read back out of an integer register).

## Root cause

Two consumers of the same `tur_poly_fn_t` disagree on the thunk ABI:

- A typed `:fn` cast (F5) and the typed poly-to-fat shim
  (`__tur_poly_to_fat1_double_double`) invoke the thunk NATIVELY. That is
  why `make_poly_wrapper` retypes float-class args to their real kind
  ([poly-wrapper-forces-int64-args-non-int-fat-sink.md](poly-wrapper-forces-int64-args-non-int-fat-sink.md)).
- An erased typeclass-method sink -- a param whose DECLARED signature has
  type variables at the float positions -- is compiled once and invokes
  through the int64 carrier: `emit_type_c_name` of a tyvar is `int64_t`.

Neither side can change: the erased instance has no type to cast to, and
the typed consumers must keep matching a closure's native thunk.

## Fix

Bridge at the pack site, at exactly the positions the sink erases:

- `elab_typeclasses.c` `poly_wrap_stamp_carrier_erased` stamps the
  `EX_POLY_WRAP` with `carrier_erased_arg_mask` / `carrier_erased_result`
  from the method param's declared `(fn ...)` type (bit i = arg i is a
  TY_TYVAR). A typed `:fn` / `^fat` sink leaves both clear.
- `emit_module.c` `ensure_float_carrier_shim` emits
  `static int64_t __tur_fltcarrier___poly_N(void *__e, int64_t a0)` which
  calls the native wrapper with `tur_sc_f32_from_bits(a0)` and returns
  `tur_sc_bits_f32(__r)` (f64 twins for `double`); non-erased positions
  keep the wrapper's own C type and are forwarded untouched.
  `ensure_fat_float_carrier_shim` is the signature-keyed twin for a
  capturing lambda, reading the thunk out of slot 0 of its env, wired
  where the fat aggregate spill already is (`emit_expr.c` EX_POLY_WRAP).
- The bits are byte-positioned (memcpy), so a `float32` lands in the
  first four bytes of the int64 -- the same bytes the erased record reader
  recovers it from once the record monomorph pads the field to the word
  (`types.c` `emit_registered_adt_app_rec`, `int32_t __pad_<field>`).

Pinned by `tests/fixtures/erased-reader-float32-record-monomorph`
(`2.5`, `-7.25`). Probed by hand under ASan: the `float` twin, a capturing
lambda (`+ v k`, 3.75), a non-capturing lambda (`* v 2.0f32`, -14.5) and
concrete dispatch `(fmap (mk-id 2.5f32) keep-f32)`.

## Not covered

An erased `(fn [a] b)` box that the instance body then hands to a TYPED
float `^fat` sink would see the bits shim through the typed poly-to-fat
cast. No fixture does this; it needs a bits-to-native bridge at the
poly-to-fat site if it ever surfaces.
