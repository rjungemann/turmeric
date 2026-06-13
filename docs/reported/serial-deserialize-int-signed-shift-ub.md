# `serial.tur` Serializable[int].deserialize: signed left-shift overflow (UB)

**One-line summary:** `stdlib/serial.tur`'s `Serializable [int]` `deserialize`
reconstructs the int64 with `(int64_t)p[7] << 56` (and the lower bytes via
signed shifts). When the top byte has its high bit set (e.g. deserializing `-1`,
where `p[7] == 0xff`), `255 << 56` is not representable in `int64_t` -- a signed
left-shift overflow, which is undefined behavior in C.

**Severity:** Low-medium -- **latent UB**, works by luck on two's-complement
targets. Produces the correct value today because the wrap matches the intended
bit pattern on common platforms, but it is UB the standard does not guarantee
and a UBSan build flags it.

## Repro / evidence

`stdlib/serial.tur`, `Serializable [int]` `deserialize` body:

```c
int64_t v =
    (int64_t)p[0]
  | ((int64_t)p[1] <<  8)
  ...
  | ((int64_t)p[7] << 56);   /* p[7]=0xff -> 255 << 56 is UB */
return v;
```

Surfaced via the R1 interpreter native shim for the same instance: compiling the
shim with `-fsanitize=undefined` (the Debug `tur` build) reported, when
deserializing `-1` in `tests/fixtures/serial-primitive-roundtrip`:

```
runtime error: left shift of 255 by 56 places cannot be represented in type 'long int'
```

The compiled fixture passes only because `tur build`'s generated C is not
UBSan-instrumented, so the UB goes uncaught at runtime.

## Observed vs expected

- Observed: correct value, but undefined behavior on the `<< 56` (and any shift
  that lands a set bit in or above the sign position).
- Expected: well-defined reconstruction for all int64 values, including
  negatives.

## Proposed fix

Accumulate into a `uint64_t` and shift unsigned, then convert once at the end
(implementation-defined, not UB):

```c
uint64_t v = 0;
for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
return (int64_t)v;
```

The R1 interpreter native (`native_deserialize_int`, `src/main.c`) already uses
this unsigned form; mirror it in `serial.tur` for the compiled path.

## Validation

- Rebuild and run `serial-primitive-roundtrip` (covers `-1`) -- UBSan clean.
- No behavior change on two's-complement targets.

## Notes

Found while implementing the R1 Serializable interpreter natives
([turi-interpret-flip-residual-plan.md](../upcoming/turi-interpret-flip-residual-plan.md)).
