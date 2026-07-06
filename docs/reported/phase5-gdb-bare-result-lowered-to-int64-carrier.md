# phase5-gdb: bare `Result` param lowers to int64_t carrier, so gdb can't render (ok 14)

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
