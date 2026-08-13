# turi: constrained by-value HKT bind/pure returns wrong values

---
status: NOT FIXED -- ABSORBED 2026-08-01 into
[`turi-return-directed-method-keeps-baked-instance`](../archive/turi-return-directed-method-keeps-baked-instance.md)
---

**The defect is still open; only this file is closed.** This was a symptom
report against one fixture. The root cause is `--interpret` keeping the
elaboration-baked instance for a RETURN-directed class method, which is
diagnosed and tracked in the report linked above -- read that one.

Two things here are wrong and are corrected there, so do not act on them:

- The "mis-slotted by-value Option carrier / interpreter readback" direction
  (and the pointer at `src/turi/interpreter_natives.c`). The payload-
  independence probe rules it out: `(pure-n (some 0) 7)` and
  `(pure-n (some 0) 99)` both give `1`, so no `pure` runs and there is no value
  to mis-slot.
- "The sibling `hkt-constrained-continuation-dict` passes under turi, so plain
  instance selection is fine." It carries inline-C and is PASS-**skipped** by
  the TI7 carve-out. Run directly it gives `207 207` where `107 207` is wanted
  -- the same defect.

What this report established and the successor keeps: the failure is
**pre-existing**, verified by A/B against the tree with the `^persistent`
cstr-key fix stashed.

---

**Severity: medium.** Interpreter-only wrong output; the compiled path is
correct. Found 2026-07-30 while running `tests/run-turi.sh` after the
`^persistent` cstr-key fix; A/B against the pre-change tree confirms it is
pre-existing (fails identically with that change stashed), so it most likely
arrived with the by-value HKT rebase window, not with any hamt work.

## Repro

`tests/fixtures/hkt-constrained-byvalue-bind-pure` under `tur --interpret`:

```
expected:  42 / -1 / 7
turi:       1 / -1 / 1
```

`bash tests/run-turi.sh` reports it as the suite's only failure
(1,680 passed, 1 failed).

## Shape

The fixture composes return-directed `pure` against a `^Monad m ^Applicative m`
constraint, the by-value carrier's aggregate spill shim, and Route B dict
clones (see the fixture's header comment). Both wrong lines are the ones whose
result flows through `pure` inside a constrained callee:

- `(bind (some 41) (fn [v] (pure (+ v 1))))` -> unwraps to 1 instead of 42;
- `(just-pure (some 0))` (body `(pure 7)`) -> unwraps to 1 instead of 7.

The `none` short-circuit line is correct. `1` looks like a some-flag or a
one-word carrier being read where the payload word was expected (or `pure`
boxing the value into the wrong slot of the by-value Option carrier) --
i.e. the interpreter's dict-slot `pure` does not agree with the by-value
carrier layout the rest of the pipeline uses.

The sibling `hkt-constrained-continuation-dict` passes under turi, so plain
instance selection is fine; the divergence is specific to the by-value
carrier + `pure` combination.

## Where to look

- turi's typeclass dict dispatch for constrained generics (`pure` slot load)
  and the by-value Option carrier construction on the interpreter path;
- `src/turi/interpreter_natives.c` option/result carrier helpers ("padded to
  int64[2]" carrier notes around line ~1052).

## Fix directions

Make the interpreter's `pure` (dict-slot path, constrained callee) construct
the same by-value Option carrier the compiled path does -- payload in the
value word, flag in the flag word -- or route it through the same native the
unconstrained `some`/`pure` path uses. `unwrap-or` reading 1 from a
just-constructed `(pure 7)` is the discriminating probe.
