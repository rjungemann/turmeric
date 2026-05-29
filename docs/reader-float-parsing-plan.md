# Plan: Reader Float-Literal Parsing -- Dropped Exponents & Precision Drift

> **Status:** Implemented (Option B)
> **Last Updated:** 2026-05-29
> **Type:** Compiler correctness (reader/lexer) + precision
> **Related:**
> - `src/compiler/reader.c` -- `read_number` (the lossy parser)
> - `src/compiler/fmt.c` -- `fmt_num_literal` (round-trips the parsed double;
>   surfaced these issues while fixing the formatter)
> - `docs/asan-debug-leaks-plan.md` -- the fmt-corruption cleanup that exposed
>   these bugs

---

## Overview

`read_number` (`src/compiler/reader.c`) converts a numeric lexeme into a
`double` (`F_FLOAT`) or `int64_t` (`F_INT`) using a hand-rolled digit
accumulator rather than the C library's `strtod`/`strtoll`. The float path has
two independent defects:

1. **Negative exponents are dropped entirely (correctness).** A literal like
   `1e-10` parses to `1.0` -- the exponent is silently ignored, so the value is
   wrong by ten orders of magnitude.
2. **Precision drift (accuracy).** Even when the magnitude is right, the
   accumulated `double` is not the nearest representable double to the source
   text, so values are subtly off.

Both are pre-existing and were uncovered while repairing the `tur fmt`
literal-corruption bugs (see `docs/asan-debug-leaks-plan.md`): the fixed
formatter round-trips the parsed `double` via `strtod`, and because the reader
produces a *different* double than `strtod` would, reader-parsed values format
into long/ugly forms (e.g. `0.0001` -> `0.00010000000000000003`).

## Symptoms

```turmeric
1e-10      ; parses as 1.0          (exponent dropped)
1.0e-10    ; parses as 1.0          (exponent dropped)
2.5e-3     ; parses as 2.5          (exponent dropped)
0.0001     ; parses as 0.00010000000000000003 (drift; not nearest double)
6.022e23   ; parses as 6.0219999999999996e+23  (drift)
```

Positive exponents have the right magnitude (`1.5e3` -> `1500.0`) but still
carry drift.

## Root cause

In `read_number` (`reader.c:272`):

### Bug 1 -- negative exponent (`reader.c:348-355`)

```c
double exp_factor = 1.0;
int abs_exp = exp_sign < 0 ? -exp_val : exp_val;   /* (!) goes NEGATIVE */
for (int i = 0; i < abs_exp; i++) {                /* 0 < -10 is false  */
    if (exp_sign > 0) exp_factor *= 10.0;
    else              exp_factor /= 10.0;
}
fval *= exp_factor;                                /* exp_factor stays 1.0 */
```

`exp_val` is always non-negative (it is accumulated from digits). For `e-10`,
`exp_sign < 0` so `abs_exp = -exp_val = -10`. The loop guard `i < abs_exp`
(`0 < -10`) is immediately false, so `exp_factor` is never divided and stays
`1.0`. The exponent is discarded.

### Bug 2 -- digit-by-digit accumulation (`reader.c:302-356`)

- Integer part: `fval = fval * 10.0 + digit`.
- Fractional part: `frac += digit * scale; scale *= 0.1`. `0.1` is not exactly
  representable, and repeatedly multiplying `scale` by `0.1` compounds the
  error, so the assembled fraction is not the correctly-rounded value.
- Exponent: repeated `*= 10.0` / `/= 10.0` accumulates further rounding error
  (on top of Bug 1).

The net effect is that `Form.as.f` is not `strtod(<lexeme>)` -- it can differ
in the low bits, or (Bug 1) be wildly wrong.

### Why it matters now

The repaired formatter (`fmt_num_literal`) emits the shortest decimal that
`strtod` recovers exactly. When the reader's double disagrees with
`strtod(<lexeme>)`, the shortest round-tripping string is long and ugly, and a
clean source literal (`0.0001`) no longer formats back to itself. Making the
reader use `strtod` would make the reader and formatter agree, so simple
literals round-trip to their natural form.

## Goals / non-goals

Goals:
- Parse float literals to the correctly-rounded nearest `double` (so `1e-10`
  is `1e-10`, `0.0001` is the nearest double to `0.0001`).
- Keep the existing lexeme delimiting, `is_float` detection, type-suffix
  handling (`i8`..`f64`), `0x`/`0b` integer prefixes, sign handling, and
  overflow diagnostics unchanged in behavior.
- Make reader output agree with `fmt`'s `strtod`-based round-trip so clean
  literals format back to themselves.

Non-goals:
- Changing the integer path (`0x`/`0b`, decimal int accumulation) beyond what
  is needed to keep it working. Int parsing is not implicated.
- Adding hex-float (`0x1p4`) or `inf`/`nan` literal support. Out of scope; the
  lexeme grammar stays as it is today.
- Reworking the numeric **type** system or literal **suffix** semantics.

## Options

### A. Minimal correctness fix for the exponent only (low effort)

Fix just Bug 1 so negative exponents are applied:

```c
double exp_factor = 1.0;
for (int i = 0; i < exp_val; i++) exp_factor *= 10.0;
if (exp_sign < 0) fval /= exp_factor; else fval *= exp_factor;
```

- Pro: tiny, removes the dangerous silent-wrong-value bug.
- Con: leaves the precision drift (Bug 2); reader output still disagrees with
  `strtod`, so `fmt` still renders some literals in long form.

### B. Re-parse the delimited lexeme with `strtod` (the principled fix)

Keep the existing scan loop purely to **delimit** the lexeme (advance `r->pos`,
set `is_float`, detect the suffix, run overflow checks), but compute the float
**value** by calling `strtod` over the matched characters instead of
accumulating `fval`.

The lexeme bounds are already tracked (`start_off` .. `r->pos`, and the source
buffer is addressable), so the value can be recovered exactly:

```c
/* after the lexeme (digits/./exponent) is delimited, before the suffix */
char buf[64];
size_t n = <end_off> - <num_start_off>;            /* numeric chars only */
if (n < sizeof buf) {
    memcpy(buf, r->src + <num_start_off>, n);
    buf[n] = '\0';
    fval = strtod(buf, NULL);                       /* correctly rounded   */
}
```

Notes:
- Exclude the type suffix from the slice (`strtod("1.5f32")` would stop at `f`
  anyway, but slicing is explicit and avoids the `f`/`inf` ambiguity).
- Handle the leading sign consistently with the current `sign < 0` negation
  (either include it in the slice or keep negating afterwards -- pick one).
- Fixes Bug 1 and Bug 2 together, and harmonizes reader output with `fmt`.

- Pro: correct, simple to reason about, removes both defects, and makes
  `fmt` round-trips clean.
- Con: must thread the numeric-substring start offset and confirm the source
  buffer pointer (`r->src` or equivalent) is available in `read_number`.

### C. Replace the whole numeric scan with `strtod`/`strtoll` + suffix tail

Let `strtod`/`strtoll` both **delimit and value** the number (via their
`endptr`), then scan the suffix from `endptr`. Largest change; risks behavioral
drift in edge cases (hex, `0b`, leading `+`, embedded underscores if any).

- Pro: least bespoke code long-term.
- Con: biggest blast radius; the current lexeme grammar (e.g. `0b`, suffix
  rules, overflow messages) must be exactly preserved. Not worth the risk now.

## Recommendation

**Option B.** It fixes both the correctness bug and the precision drift with a
contained change, preserves all existing lexeme/suffix/overflow behavior, and
makes the reader agree with the (already-fixed) formatter. Option A is an
acceptable stop-gap if B is blocked, but it leaves drift and the fmt mismatch.

## Plan

1. In `read_number`, record the offset where the numeric characters begin
   (after any `0x`/`0b` handling is ruled out for the float path) and where
   they end (before the type suffix).
2. Replace the `fval` accumulation (integer part, fractional part, exponent
   apply, including the buggy loop at `reader.c:348-355`) with a `strtod` call
   over the delimited slice. Keep the loops only insofar as they advance the
   cursor and set `is_float` / detect the exponent for `is_float`.
3. Preserve: sign handling (`sign < 0`), the `1f32`/`1f64` "float suffix on an
   integer-looking literal" path (`reader.c:379-380`, which sets
   `fval = (double)ival`), the `0x`/`0b` integer path, and all overflow
   diagnostics.
4. Confirm `strtod` is locale-independent here (the build assumes `"C"`
   locale; decimal point is `.`). Add a comment if relying on that.
5. Fixtures (new, under `tests/fixtures/`):
   - `float-negative-exponent` -- `1e-10`, `1.0e-10`, `2.5e-3`, `6.022e-23`
     print the correct values.
   - `float-positive-exponent` -- `1.5e3`, `6.022e23` unchanged-correct.
   - `float-precision-roundtrip` -- a value like `0.0001` / `PI` parses and
     prints back cleanly (ties the reader to `fmt`).
   - Negative test stays: empty exponent (`1e`) still errors.
6. Regenerate any `expected.c` codegen snapshots that embed float literals
   whose bits change under correct rounding (per `CLAUDE.md`'s snapshot rule),
   and re-run `tur fmt --check stdlib/` to confirm stdlib stays self-formatted
   (PI is already in its canonical `3.141592653589793` form).
7. Run `bash tests/run.sh` (zero `FAIL`) and `ctest`.

## Risks

1. **Codegen snapshot churn.** Correct rounding changes the low bits of some
   emitted float constants, so `expected.c` snapshots that contain floats may
   shift. This is expected; regenerate them in the same change (never a
   separate PR) per `CLAUDE.md`.
2. **Behavioral edge cases.** Slicing the wrong span (including the suffix, the
   sign, or a trailing delimiter) would feed `strtod` bad input. Mitigate with
   the negative-exponent and round-trip fixtures plus the existing `1e` error
   test.
3. **`fmt` interaction.** After the fix, a few stdlib/fixture float literals
   may format slightly differently (now that reader == strtod). Re-run
   `fmt-bootstrap-stdlib` / `fmt-idempotence-stdlib` and re-canonicalize if
   needed (expected to be a no-op or near-no-op).

## Validation checklist

- [x] `1e-10`, `1.0e-10`, `2.5e-3` parse to the correct magnitude (not `1.0`).
      Covered by `tests/fixtures/float-negative-exponent`.
- [x] `0.0001` parses to the nearest double and `tur fmt` prints it back as
      `0.0001` (not a 17-digit form). The reader now agrees with `strtod`, so
      reader-parsed values format cleanly.
- [x] Empty-exponent literal `1e` still raises "expected exponent digits".
- [x] `1f32` / `1f64` (float suffix on integer-looking literal) still parse as
      floats with the right value (`42f64` -> `42.0`).
- [x] `0x`/`0b` integer literals and integer overflow diagnostics unchanged
      (that code path is untouched; `errors/literal-overflow` still passes).
- [x] `bash tests/run.sh` reports zero `FAIL` (1046 passed; no `expected.c`
      snapshot needed regenerating -- no embedded float literal's bits shifted).
- [x] `ctest` green; `fmt-bootstrap-stdlib` and `fmt-idempotence-stdlib` pass.
      (The RUN_SERIAL `tur_repl_spice_*` FIFO/timeout tests flake intermittently
      under parallel ASAN load -- unrelated; they pass in isolation.)

## Implementation notes (what was actually done)

Option B, as recommended. In `read_number` (`src/compiler/reader.c`) the
decimal-number branch now uses its scan loops purely to **delimit** the lexeme
(advance the cursor, accumulate `ival` for the integer/`1f32` paths, set
`is_float`, and validate that an `e`/`E` is followed by digits). The float
**value** is recovered with `strtod` over the matched slice
`r->src[num_start, r->pos)` (a 64-byte stack buffer, falling back to an
arena allocation for pathologically long literals). The hand-rolled `fval`
accumulation -- the integer `*10`, the `frac += digit * scale; scale *= 0.1`
fraction, and the buggy exponent-apply loop -- was removed. The type suffix is
not yet consumed at that point, so the slice is pure decimal-float syntax and
`strtod` stops exactly at its end; the leading sign is still applied via the
existing `sign < 0` negation. The `1f32`/`1f64`, `0x`/`0b`, and overflow paths
were left untouched.

## Appendix: the two defects, side by side

| Defect | Location | Input | Current | Correct |
| --- | --- | --- | --- | --- |
| Dropped negative exponent | `reader.c:350` | `1e-10` | `1.0` | `1e-10` |
| Precision drift | `reader.c:314-322` | `0.0001` | `0.00010000000000000003` | nearest double to `0.0001` |
