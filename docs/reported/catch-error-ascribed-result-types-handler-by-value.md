# An ascribed `catch-error` hands its handler a by-value shim through the carrier ABI

**Severity:** medium. A silent miscompile (a segfault at run time), or a `cc`
error, for an ordinary typed use of the stdlib `MonadError [(Result _ B)]`
instance. Filed 2026-09-28 while re-measuring
[carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md).
It reproduces on `bf31e725` (v0.56.2) too, so it predates that work.

## Repro

```turmeric
(defn main [] : int
  (println (ok-val (:: (catch-error (:: (err 5) (Result int int))
                                    (fn [e] (ok (* e -1))))
                       (Result int int))))
  0)
```

Expected `-5`. Built with `tur build`, it segfaults (exit 139).

The same call bound by a `let` whose binding carries the ascription fails
in `cc`:

```turmeric
(let [r (:: (catch-error (:: (err 5) (Result int int)) (fn [e] (ok (* e -1))))
            (Result int int))]
  (println (ok? r)))
```

```
error: aggregate value used where an integer was expected
  tur_adt_Result__int__int r_12 = (*(tur_adt_Result__int__int *)(intptr_t)((*(tur_adt_Result__int__int *)(intptr_t)(__ps_176))));
```

Two shapes work: binding the call unascribed and ascribing at the use,
`(let [r (catch-error ...)] (ok-val (:: r (Result int int))))`, which prints
`-5`; and the stock fixture `hkt-stdlib-result-ok-biased`, which reads the
result with `:int` inline-C readers.

## Root cause

`catch-error` is an inline-C instance method over the erased carrier
(`stdlib/result.tur`, `definstance MonadError`):
`return handler.fn(handler.env, r->payload);`. It calls its handler through
`tur_poly_fn_t`, whose `fn` is `int64_t (*)(void *, int64_t)`, and returns
what the handler returns, a carrier word.

When the `(Result int int)` ascription wraps the call itself, the expected
type flows into the handler lambda, and its shim is emitted returning the
by-value struct:

```c
static tur_adt_Result__int__int __poly_6(void *, int64_t);
...
__inst_MonadError_catch_hyerror_Result_tyvar((int64_t)(intptr_t)(&__t175),
    (tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_6 });
```

The cast hides an ABI mismatch: the instance reads a struct-returning
function's result as an `int64_t`. The caller then bridges that "carrier"
back to the struct by dereferencing it (`*(tur_adt_Result__int__int *)...`),
which is the segfault. In the let shape the bridge is applied twice, which is
the `cc` error. In the stock fixture the lambda has no expected type, so its
shim returns `int64_t` (`__poly_44`) and nothing mismatches.

## Fix directions

- When a lambda is passed to a `tur_poly_fn_t` parameter of an erased
  (tyvar or inline-C) instance method, emit its shim on the carrier ABI
  (`int64_t` return, boxing a by-value result), whatever the expected type
  above the call says. The cast `(int64_t(*)(void*,int64_t))` at the
  construction site is the place to check that the shim's real return type
  is `int64_t`; it should never cast a struct-returning function.
- Separately, the let-binding bridge must not deref a value that is already
  the aggregate (the double `*(T *)` above); compare
  `emit_value_is_recorded_as` in the CPS letraw bridge.
- Pin both shapes as fixtures (`catch-error` ascribed at the call, and
  let-bound with the ascription).
