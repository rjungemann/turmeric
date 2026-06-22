# Fix paper trail -- turi full-prelude gate

Resolved 2026-06-22.

## Original report

`tests/run-turi-full-prelude.sh` reported two FAILs:

1. `mutmap-new resolved WITHOUT the flag (default prelude leaked a carved module?)`
2. `TUR_TURI_FULL_PRELUDE=yes wrongly enabled the full prelude`

## Root cause / why it is no longer a defect

Both FAILs stemmed from a stale probe, not from a real gate defect:

1. **`mutmap-new` is intentionally always reachable now.** `contract` and
   `mutmap` graduated out of the carved bucket into the *default* interpreter
   prelude (see the comment at `src/main.c:5216` and the carve-out note in the
   harness header). So a harness that asserts `mutmap-new` is unreachable
   without the flag is asserting the wrong invariant -- the module is correctly
   in the default set. The probe needed to target a module that is *still*
   carved.

2. **The `=yes` case was never actually a bug.** `turi_full_prelude_enabled()`
   (`src/main.c:4971`) gates strictly on `strcmp(getenv("TUR_TURI_FULL_PRELUDE"),
   "1") == 0`. A value of `yes` does not enable the full prelude. The original
   FAIL was a side effect of the broken `mutmap-new` probe (the name resolved
   regardless of the flag because it is in the default prelude), making it look
   like `=yes` had "enabled" something.

## Fix

The harness was rewritten (in #474) to probe `schema/alt` -- a pure-Turmeric
defn that lives only in the still-carved `schema.tur` and has no unconditional
native override -- so it is genuinely flag-sensitive. With that probe the gate
exercises the real invariant and all three directions hold:

- flag OFF        -> `schema/alt` unbound (rc=1, error mentions `schema/alt`)
- `=1`            -> defined and runs (rc=0)
- `=yes` (junk)   -> NOT enabled (rc=1), per the strict `=1` convention

## Verification

```sh
cmake --build build -j --config Debug
TUR_BIN=./build/tur TUR=./build/tur ASAN_OPTIONS=detect_leaks=0 \
  bash tests/run-turi-full-prelude.sh
# => PASS turi-full-prelude   (rc=0)
```

No source change was required at resolution time on this branch; the gate
already passes with the built `tur`. This entry records the diagnosis and moves
the report out of `docs/reported/`.
