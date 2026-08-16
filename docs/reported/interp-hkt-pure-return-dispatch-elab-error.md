# `--interpret` rejects hkt-constrained-pure-return-dispatch that `emit-c` accepts

**Severity:** low-medium (one fixture family member; interpreter-only;
pre-existing, surfaced during turi-dict-passing step-4 measurement 2026-08-16)

## Summary

The same elaborator run accepts the program on the compiled path and rejects
it on the interpreter path:

```
$ ./build/tur emit-c tests/fixtures/hkt-constrained-pure-return-dispatch/input.tur   # OK
$ ./build/tur run    tests/fixtures/hkt-constrained-pure-return-dispatch/input.tur   # OK, prints 7 10 3
$ ./build/tur --interpret tests/fixtures/hkt-constrained-pure-return-dispatch/input.tur
input.tur:34:31: error [TUR-E0001]: function 'just-pure' arg 1:
  expected (type-app tyvar 'm' int), got int
```

Verified pre-existing at c909e790 (rebuilt HEAD binary, before the
frame_bind_constraint_dicts change): identical error. The fixture is
PASS-skipped by `tests/run-turi.sh` (TI7 inline-C carve-out), so no harness
reports it; it only shows up when the family is run by hand, which is why
the turi-dict-passing plan prescribes hand-running these.

## Root-cause direction (unverified)

Elaboration is shared, so the divergence must come from a mode difference
the `--interpret` entry point sets up (e.g. HKT by-value carrier defaults or
a different elaboration flag set in cmd handling in src/main.c) rather than
from eval.c. The error shape -- an argument typed `int` failing to unify
with `(m int)` -- suggests the interpreter-path elaboration is not running
the same result-type instantiation/pin that the compiled path applies at
this call (compare `elab_poly_call`'s MB1 expected-type pin, landed
2026-08-16). Start by diffing the elab flags between `cmd_emit_c` and the
interpret command in src/main.c.
