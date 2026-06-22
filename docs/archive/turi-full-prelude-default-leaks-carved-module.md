---
title: tests/run-turi-full-prelude.sh fails -- default prelude resolves a full-prelude-only module
category: Interpreter (turi) -- prelude carving / TUR_TURI_FULL_PRELUDE gating
severity: Low. Auxiliary harness only; the default `tests/run.sh` gate and
  `tests/run-turi.sh` are both green. The full-prelude gate test asserts that
  `mutmap-new` (and the rest of the "full prelude") is NOT reachable unless
  TUR_TURI_FULL_PRELUDE is set, and that the flag actually toggles it. Both
  assertions fail: `mutmap-new` resolves with the default prelude, and the
  TUR_TURI_FULL_PRELUDE=yes path is reported as wrongly enabling the full
  prelude.
status: RESOLVED 2026-06-22. The gate harness was rewritten (in #474, "green
  up ... turi gates") to probe `schema/alt` -- a pure-Turmeric defn that lives
  only in the still-carved `schema.tur` -- instead of the original `mutmap-new`
  probe. `contract`/`mutmap` have since graduated into the *default* prelude by
  design, so the old probe was testing a name that is correctly always-reachable
  now. With the corrected probe all three directions hold against `tur` built
  from this branch: flag OFF -> `schema/alt` unbound (rc=1, error mentions
  `schema/alt`); `TUR_TURI_FULL_PRELUDE=1` -> defined and runs (rc=0);
  `TUR_TURI_FULL_PRELUDE=yes` -> NOT enabled (rc=1), because
  `turi_full_prelude_enabled()` (`src/main.c:4971`) does a strict
  `strcmp(e,"1")==0`. `bash tests/run-turi-full-prelude.sh` => `PASS
  turi-full-prelude`. See
  `docs/archive/history/turi-full-prelude-default-leaks-carved-module.md`.
---

## Repro

```sh
cmake --build build -j --config Debug
TUR_BIN=./build/tur TUR=./build/tur ASAN_OPTIONS=detect_leaks=0 \
  bash tests/run-turi-full-prelude.sh
```

Observed (clean tree, no local changes):

```
FAIL turi-full-prelude -- mutmap-new resolved WITHOUT the flag (default prelude leaked a carved module?)
FAIL turi-full-prelude -- TUR_TURI_FULL_PRELUDE=yes wrongly enabled the full prelude
```

## Notes

- Confirmed pre-existing: stashing all local work and rebuilding reproduces the
  same two FAILs, so this is not a regression from the interpreter dispatch /
  reinterpret / time-natives work landed alongside this report.
- Likely root cause is in the prelude-registration block in `src/main.c`
  (`wk_register_stdlib_natives` and the prelude `static const char *prelude[]`
  load list) and/or the `TUR_TURI_FULL_PRELUDE` gate the harness checks --
  a module that should sit behind the full-prelude flag is being registered in
  the default set.

## Fix directions

1. Diff the default prelude module/native set against the full-prelude set and
   confirm `mutmap-new` (and any other full-prelude-only name) is only
   registered when `TUR_TURI_FULL_PRELUDE` is set.
2. Verify the harness reads the same env var name the registration path checks.
