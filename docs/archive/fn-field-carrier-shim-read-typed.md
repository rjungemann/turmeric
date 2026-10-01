# A typed fn field over a parametric aggregate passed it by value to a shim that dereferenced it

**Severity: high.** Segfault:

```turmeric
(defstruct FF :copy [app : (fn [(Result float int)] (Result float int))])
(ok-val (.app (make-struct FF (fn [r : (Result float int)] : (Result float int) r))
              (:: (ok 7.1) (Result float int))))          ; SIGSEGV
```

The same happened with `(fn [(Option int)] int)` and
`(fn [(Result float int)] float)`. A non-parametric struct parameter
(`(fn [P] P)`) worked. Found 2026-09-30 by the type fuzzer's typed fn-field
crossing over a `Result` wrapper. The same input segfaults on the baseline
compiler, so the bug predates this work. The fuzzer had filed the leg under its
report-only fnsan trap, which hid the crash. **RESOLVED 2026-09-30.**

## Mechanism

The boxing site (`EX_FN_TO_FAT`) fills slot 0 with a typed shim, or with the
carrier shim when the typed one is declined. Both speak the b4box convention:
every parameter for which `type_is_b4box_closure_slot` holds is an int64 box
pointer. That covers a non-parametric ADT over 8 bytes **and a parametric
monomorph over 8 bytes**. The carrier shim also returns a b4box result boxed.

The typed fn-field call (`emit_expr.c`, the `gf->type.as.fn.boxed` arm) decided
its boxing with the narrower `type_is_wide_byval_adt`, which knows no parametric
monomorph. So it passed `(Result float int)` by value to a shim that read the
first eightbyte as a box pointer.

## Fix

The call site asks the shims' own slot question, `type_is_b4box_closure_slot`.
When the producer would have chosen the carrier shim (typed ABI declined,
`carrier_fatshim_applies`), the call also unboxes the boxed result, mirroring
the generic HOF call's `carrier_unbox` (generic-spec-carrier-crossings, third
batch).

## Verified

`tests/fixtures/fn-field-carrier-shim-read-typed` (seven signatures, including
a capturing lambda), compiled and `--interpret`.
