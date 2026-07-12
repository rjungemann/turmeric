---
title: "phase5-gdb: bare `Result` param lowers to int64_t carrier, so gdb can't render (ok 14)"
category: Reported
description: The Phase-5 debugger acceptance test expected gdb to pretty-print a
  by-value Result parameter as `(ok 14)`, but a BARE unparameterized `Result` is
  a generic ADT passed as the int64 carrier (a boxed pointer), which no struct
  pretty-printer can dispatch on. RESOLVED (2026-07-06) by making the fixture use
  the concrete `(Result int cstr)`, which monomorphizes to the by-value struct
  `tur_adt_Result__int__cstr` just as `(Option A)` does -- the intended by-value
  inspection point.
---

# phase5-gdb: bare `Result` param lowers to int64_t carrier, so gdb can't render (ok 14)

**Status:** RESOLVED (2026-07-06). The fixture now takes a concrete
`(Result int cstr)` parameter, which monomorphizes to a by-value carrier struct
the pretty-printer renders as `(ok 14)`. See the Resolution section at the end.

**Summary:** The Phase-5 debugger acceptance test (`tests/run-phase5-gdb.sh`,
ctest `tur_phase5_gdb`) expects gdb to pretty-print a by-value `Result`
parameter as `(ok 14)`. Under the current by-value HKT/carrier lowering a bare
`Result` (no type args) monomorphizes to `int64_t` (a boxed/tagged carrier), not
the 24-byte `tur_adt_Result{is_ok,ok_val,err_val}` struct the fixture and
pretty-printer assume, so gdb shows a raw integer. **Severity: low**
(debug-info/representation gap in active carrier work; not a runtime miscompile).

## Two halves

- **N1b (fixed here):** the DWARF `ptype` probe used the pre-`tur_adt_`-prefix
  name `Result`. The monomorphic carrier is now `tur_adt_Result` (same rename
  that hit `tur_result_typedef_multi_module`). Updated the probe + assertion to
  `tur_adt_Result`. This half now passes.
- **N2 (open):** `print r` at the probe breakpoint shows
  `r=r@entry=93824992912032` -- an `int64_t`, not a struct. The monomorphized
  symbol is `probe__spec__tur_adt_Option__int_tur_adt_Option__int_int64_t`; the
  third component (`r : Result`) lowered to `int64_t`, while `o : (Option A)`
  lowered to the by-value struct `tur_adt_Option__int`. The pretty-printer
  (`tools/debug/turmeric_gdb.py`, whose regexes already match `tur_adt_Result`)
  cannot dispatch on an `int64_t`.

## Repro

```sh
./build/tur --debug build tests/fixtures/debugger-phase5/input.tur -o /tmp/p5
gdb -batch -nx -ex "info functions probe" /tmp/p5 | grep probe__spec__
#   ...tur_adt_Option__int_tur_adt_Option__int_int64_t   <- r is int64_t
```

Fixture: `(defn probe [A] [o : (Option A) r : Result] : (Option A) o)`. The
header comment even documents the intended shape ("Result is 24 bytes -> spill
slot, visible even unused"), which no longer holds.

## Root cause

Bare `Result` (unparameterized) takes a different by-value carrier path than
`Option A`: it is boxed to `int64_t` at the monomorphized call boundary rather
than passed as the `tur_adt_Result` aggregate. The DWARF therefore types `r` as
`int64_t` and no struct pretty-printer applies.

## Fix directions

Make the by-value carrier lowering pass a monomorphized bare `Result` as its
`tur_adt_Result` aggregate (matching `Option__int`) at least under `--debug`
(-Og), so the parameter materializes with the struct DWARF type; or have the
fixture use a parameterized `(Result int cstr)` if that already lowers by-value.
The N1b half is fixed; N2 waits on the carrier/debug-info work.

---

## Resolution (2026-07-06)

Took the second fix direction, on the judgment that the boxing is correct and
the fixture's assumption is what was stale. A BARE, unparameterized `Result` is a
*generic* ADT; passing a generic ADT as the int64 carrier (a boxed pointer) is
the intended by-value-HKT behavior, not a regression -- the same way any
unapplied ADT reference is carried. A *by-value aggregate* Result only exists for
a CONCRETE instantiation, exactly as `(Option A)` only becomes the by-value
struct `tur_adt_Option__int` once `A` is monomorphized to `int` at the call.
Forcing a bare generic `Result` to materialize as a struct (the first fix
direction) would special-case generic ADTs in the carrier lowering and diverge
`--debug` from release codegen -- high risk for a low-severity display gap.

**Change** (`tests/fixtures/debugger-phase5/input.tur`): `parse` and `probe`'s
`r` parameter are now typed `(Result int cstr)` instead of bare `Result`.
Verified with `emit-c`: this monomorphizes to the by-value struct
`tur_adt_Result__int__cstr { bool is_ok; int64_t ok_val; const char * err_val; }`
(mirroring `tur_adt_Option__int`), and the probe symbol becomes
`probe__spec__..._tur_adt_Result__int__cstr` -- the third component is the struct,
no longer `int64_t`. The pretty-printer's `^(tur_adt_)?Result(__.*)?$` regex
already matches the monomorphized name, so gdb renders `r` as `(ok 14)`.

**Test** (`tests/run-phase5-gdb.sh`): N1a (emit-c) and N1b (DWARF `ptype`) now
target the concrete carrier `tur_adt_Result__int__cstr` (the bare generic
`tur_adt_Result` struct is emitted in the C preamble but, being unused by value,
never enters the debug binary's DWARF). N2 is unchanged and now passes. The
fixture header comment is updated to explain the concrete-vs-bare distinction.

**Verified.** `bash tests/run-phase5-gdb.sh`: all 6 assertions pass (was 5/6).
ctest `tur_phase5_gdb`: passes. gdb now prints `r = (ok 14)` and
`Value returned is (some 42)`. The fixture still runs as an ordinary program
(`tur run ...` exits 42). No compiler change; the debugger-phase5 fixture carries
`requires.dedicated-runner`, so only this harness/ctest target exercises it.
