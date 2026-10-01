# `musttail` with a by-value aggregate over 16 bytes dangles on arm64

**Severity: high on arm64 (a silent use-after-return), none elsewhere.
RESOLVED 2026-10-01.**

## Symptom

`tests/fixtures/saffron-class-fn-extra` passed on Linux and Windows, under gcc
and clang, and under `TUR_GC_TORTURE` at every interval from 1 to 64. On
`macos-latest` (Apple Silicon) it printed nothing and aborted on its first
line, in both `Test (macos-latest)` and the r7rs-gc torture run of
`R7RS suites (macos-latest)` (#1007). The GC harness cut the panic line at
120 columns, and the long macOS `$TMPDIR` path pushed the whole message past
the cut. The harness now prints the full stderr tail.

## Root cause

The Saffron dynamic witness forwards its receiver to the spec with a
guaranteed tail call:

```c
static tur_tagged_t __dynwit_Comb_comb_Two(tur_adt_Two__any __r, tur_tagged_t __a1) {
    TUR_MUSTTAIL return __inst_Comb_comb_Two__spec__...(__r, __a1);
}
```

`tur_adt_Two__any` is 32 bytes. AAPCS64 passes an aggregate over 16 bytes
**indirectly**: the caller copies it into its own frame and passes the copy's
address. With `musttail`, that frame is released before the jump. Apple
clang at `-O0`:

```
add  x0, sp, #16      ; &copy, in wit's frame
add  sp, sp, #96      ; frame released
b    _spec            ; spec reads a dead slot its own frame overwrites
```

SysV x86-64 passes the same aggregate in the outgoing argument area. A
`musttail` call writes it into the caller's incoming area, which outlives the
jump. So the same text is correct there, which is why only arm64 saw it.
`tur_tagged_t` (16 bytes) travels in registers on both ABIs.

`tail_call_musttail_ok` (src/compiler/emit_fns.c) already refused a
`const T *` pass-by-pointer parameter, which is a pointer into some caller's
frame by construction. It did not refuse a by-value aggregate, which is the
same pointer on arm64, made implicitly by the ABI.

## Fix

`musttail_param_in_registers`: a `musttail` call may carry only pointers,
scalars and `tur_tagged_t`. The size of a by-value ADT is not visible from
its C spelling, so every other aggregate is refused. Such a call is still the
ordinary `return f(args);`, so only the guarantee is lost, and only for
functions with by-value aggregate parameters.

## Not covered

Windows x64 passes any aggregate over 8 bytes by reference, `tur_tagged_t`
included. `TUR_MUSTTAIL` engages for clang on `__x86_64__`, so a clang build
targeting the MS ABI would hit the same hazard for `tur_tagged_t`. CI's
Windows legs use gcc, where `TUR_MUSTTAIL` is empty.
