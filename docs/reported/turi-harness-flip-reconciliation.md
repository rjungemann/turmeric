# TI8 harness flip: allowlist reconciliation + full-denylist blast radius

**Summary:** `tests/run-turi.sh` was flipped from `tur run` (which compiles and
runs a native binary) to `tur --interpret` (the actual tree-walking
interpreter), resolving the blocker in
[turi-harness-compiles-instead-of-interpreting.md](turi-harness-compiles-instead-of-interpreting.md).
True interpretation turned **31 of the 146 allowlisted fixtures red**; those 31
were removed from the allowlist (they were never real interpreter coverage).
This report catalogues the 31 by root cause and records the measured blast
radius of the *full* allowlist->denylist flip (still future work): under
`--interpret`, **933 of ~1500 fixtures fail**.

**Severity:** Mixed. Some removed entries are permanent carve-outs (call/cc,
inline-C); several are real interpreter gaps (missing stdlib natives / struct
types) and a handful are **silent miscompiles** (the interpreter returns wrong
values, rc=0) -- the worst class, previously hidden by the compile-based
harness.

## What changed

- `tests/run-turi.sh` now runs each fixture with `"$TUR" $flags --interpret
  "$input"` instead of `... run "$input"`.
- The 31 false-green entries were removed from `TURI_FIXTURES_DEFAULT`. The
  harness is green again at **115 fixtures + 7 async eval scripts = 122 passed,
  0 failed**.
- `tools/check_turi_parity.py` + `docs/turi-carve-out.txt` were added and wired
  into `tests/run.sh` as a pre-test CI ratchet (the EX_* parity gate).

## The 31 removed entries, bucketed

### A. Permanent carve-outs (do not re-add without the underlying feature)

- **call/cc (EX_CALLCC, CPS carve-out):** `call-cc-star`,
  `continuation-callcc`, `continuation-escape`, `continuation-escape-fn`.
  Tracked with `EX_CALLCC` in `docs/turi-carve-out.txt`.
- **User inline-C (TI7 carve-out):** `ptc4-basic`
  (`inline-C not supported in interpreter mode`), `effect-capture-k`
  (capturing-continuation path; aborts under the interpreter).

### B. Real interpreter gaps -- missing stdlib natives / struct types

The interpreter does not register the typed-stdlib natives and struct types
that the compiled prelude provides, so any fixture that touches `stdlib/typed/*`
or the `Clone` typeclass errors out:

- **`make-struct: '<T>' is not a defined struct type`:** `typed/list-basic`
  (`Cons`), `typed/option-basic` (`Option`), `typed/result-basic` (`Result`).
- **`unknown function` / `unbound variable`:** `typed/map-basic`,
  `typed/map-collision`, `typed/map-eq` (`map-new`), `typed/set-basic`
  (`set-new`), `typed/vec-basic` (`vec-eq?`), `typed/slice-basic`
  (`slice-eq?`), `typed/grid-basic` (`grid-new`), `typed/zipper-basic`
  (`zipper-new`), `typed/pair-basic` (`pair-fst`), `typed/list-macro`
  (`tnil?`).
- **`Clone` typeclass not defined under interpret:** `clone-primitives`
  (`no typeclass method found for 'clone'`), `clone-list`, `clone-option`,
  `clone-pair` (`typeclass 'Clone' is not defined`).
- **arrow/HKT stdlib instances:** `arrow-instance-stdlib-basic`,
  `hkt-stdlib-result-ok-biased` (rc=1 -- stdlib instance resolution under
  interpret).

These are tractable but each needs native registration / struct-type seeding in
`src/turi/`; they are the bulk of the full-flip work (see blast radius below).

### C. Silent miscompiles -- interpreter returns WRONG values (rc=0)

These ran to completion under `--interpret` but produced wrong output. The
compile-based harness hid them entirely:

| Fixture | Interpreter output | Expected |
| --- | --- | --- |
| `result-basic` | `... false/true swapped; 0 where 99 expected` | correct Result values |
| `weak-dangling` | `true` (weak still "live") | `false` (dangling) |
| `instance-head-hole-pair` | `0` / `0` | `42` / `7` |
| `arrow-instance-apply` | rc=0, stdout mismatch | match |

`weak-dangling` is the most concerning: the interpreter's weak-ref liveness
check disagrees with the compiled semantics. These deserve individual
root-cause reports when the full flip is tackled.

### D. Dynamic-variable conveyance

- `dynvar-convey`, `dynvar-convey-isolation`: rc=1 under interpret (dynamic-var
  conveyance across the async/fiber boundary is not wired in the interpreter).

## Full allowlist->denylist flip: measured blast radius

Running **every** `tests/fixtures/*` under `--interpret`, minus those carrying
`requires.{compiled,tur-only,dedicated-runner,spices,tsan}` (92 skipped), gives:

```
pass=637  fail=933  skip=92
```

The 933 failures cluster into (first stderr line, deduped):

- ~396 with no stderr (stdout mismatch or abort -- includes the silent
  miscompiles in bucket C, scaled up),
- 46 `only one defmodule is allowed per file` (interpreter re-processes
  defmodule; likely a real interpreter defect in module loading),
- 43 `typeclass '...' is not defined` originating in `stdlib/schema.tur`,
  `stdlib/range.tur`, `stdlib/str.tur` (typeclass registration during stdlib
  load under interpret),
- 15 `no typeclass method found`,
- 13 `inline-C not supported` (genuine TI7 carve-outs),
- 13 `if condition must be bool, got int` (interpreter type-check divergence),
- 12 `unknown function` + several `unbound variable: {vec-of,set-of,hamt-of,
  map-count,grid-new,...}` (missing-native gap -- the plan's TI8 item about
  diffing the compiler's builtin-native registry against the interpreter's),
- ~20 use-after-move / linear-dropped / linear-used-after-consume (the
  interpreter runs the move/linearity checker and diverges),
- 10 `make-struct: '...' is not a defined struct type`,
- plus a long tail (kind mismatch, existential escape, reader-macro, etc.).

## Why the full flip is NOT in this change

Per the blocker report's own guidance ("Do NOT flip the harness without doing
this triage in the same change, or CI goes red") and the plan's "budget a day
for triage" note, closing 933 failures spanning many distinct interpreter bugs
(several of them silent miscompiles) is multi-session work. This change does the
*safe* half: it makes the harness genuinely interpret, makes the allowlist
honest (green), and stands up the CI ratchet so the EX_* gap cannot grow. The
denylist flip lands once the bucket-B/C/D gaps above are fixed or individually
carved with `requires.tur-only`.

## Remaining wiring (follow-up)

- `tests/run-flags.sh` still calls `tur run` for three specific assertions
  (`:345` try-with-basic, `:355` try-with-nested, `:408` effect-export-explicit).
  Left as-is here to keep scope contained; flip them to `--interpret` when their
  interpreter behavior is confirmed.

## Status

Filed while executing TI8 of
`docs/upcoming/v1/turi-parity-post-v1-plan.md`. Partial TI8 (parity ratchet +
harness-now-interprets + honest allowlist) landed in this change; the full
allowlist->denylist flip is tracked here as the remaining work with its measured
scope.
