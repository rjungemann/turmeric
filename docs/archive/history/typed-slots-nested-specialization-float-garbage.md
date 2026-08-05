# typed-slots/cs3-nested-specialization: float slot prints garbage -- cc and jit agree

**RESOLVED 2026-07-30.** The inner call resolved to a return-differentiated
`__spec__int64_t` sibling of `pair-second` (which returns the float's BIT
PATTERN in an int64), and the caller handed that back through an implicit
int64->double NUMERIC conversion. The CS3 result recovery that exists to
prevent exactly this was gated to passed-closure clones; it now applies to any
active specialization, held to register-class primitives. See
[jit-engine-j0-findings.md](../../upcoming/jit-engine-j0-findings.md) section 30.
The fixture is off the tests/run-jit.sh denylist and `tests/run.sh` now scans
group directories, so `typed/*` and `typed-slots/*` are compiled by the default
suite. The original report follows.

**Severity: medium.** A latent WRONG-OUTPUT miscompile in a fixture no
compiling harness ever ran (run.sh scans only `tests/fixtures/*/`; this
lives one level down and was interpreter-covered only). Found 2026-07-30
by the J3 jit harness.

## Repro

```sh
tur run tests/fixtures/typed-slots/cs3-nested-specialization/input.tur
#   3.14
#   4.61425e+18        <- expected 3.14
tur --enable=jit jit ...   # same garbage second line
tur --interpret ...        # correct: 3.14 / 3.14
```

Both compiling engines (gcc via `tur run`, MIR via `tur jit`) print the
same garbage double for the SECOND (nested-specialization) read, so the
defect is in the emitted C, not an engine: a float slot read through the
nested specialization path reinterprets an integer bit pattern
(4.61425e+18 is what a pointer/int bit pattern looks like through a
double).

## Where to look

The CS3 nested-specialization slot read: the outer specialization's float
slot flows through an int64 carrier without the bit-reinterpret bridge on
one side (box on write vs raw read, or vice versa).

## Fix directions

Find the nested-slot read in the emitted C (grep the fixture's emit-c
output for the second println's temp chain) and match the carrier
convention of the single-level path, which prints correctly.
