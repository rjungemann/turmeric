# RESOLVED: interpreter inline-C `snprintf` ignored its guarding if/else branch

**Status:** Fixed 2026-06-12 (`src/turi/eval.c`). This is the archived
resolution paper-trail for the conditional-snprintf gap that surfaced as the
`range-bound-show-ord` divergence in
[turi-pure-turi-silent-miscompiles.md](turi-pure-turi-silent-miscompiles.md)
and was cross-referenced from
[../reported/turi-inline-c-silent-miscompiles.md](../reported/turi-inline-c-silent-miscompiles.md).

## Summary

Under `--interpret`, the best-effort inline-C evaluator's snprintf matcher
(`ic_exec_snprintf_fmt`) scanned for the **first** `snprintf(buf, ...)` in a
function body and always formatted that one -- ignoring any `if/else` that
selected between two snprintf calls. So a formatter shaped like:

```c
char *buf = (char *)malloc(32);
if (kind == 1) snprintf(buf, 32, "[%lld", (long long)v);
else           snprintf(buf, 32, "(%lld", (long long)v);
return (const char *)buf;
```

(`stdlib/range-bound.tur`'s `bound-fmt`) rendered an **Exclusive** endpoint as
`[7` instead of `(7`: a silent miscompile (wrong output, rc=0) on the
interpreter path. The compiled path was always correct.

**Severity:** High class (silent wrong answer), but bounded to `--interpret`.

## Repro (pre-fix)

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret \
  tests/fixtures/range-bound-show-ord/input.tur | head -2
# got:      [3 / [7
# expected: [3 / (7
```

## Root cause

`src/turi/eval.c`, `ic_exec_snprintf_fmt` "Step 3": a linear
`strstr(body, "snprintf(")` scan returned on the first call whose first
argument matched the buffer variable. The guarding `if (kind == 1) ... else ...`
was never consulted, so both `Inclusive` (kind 1) and `Exclusive` (kind 2)
formatted through the `[%lld` branch. Step 2 only understood `if (COND) return
"literal";` early-returns, not `if/else` over two snprintf statements, and even
then collapsed the condition to a bare truthiness test of a single parameter --
it would not have evaluated `kind == 1` correctly either.

## Fix

1. Factored the snprintf-formatting body out of `ic_exec_snprintf_fmt` into a
   reusable helper `ic_format_snprintf_call(fp, bufvar, ...)` that formats the
   single `snprintf` call at `fp` (or returns nil if it does not target the
   buffer / cannot be parsed). No behavior change -- same parsing/format code.
2. Added `ic_snprintf_cond_branch(...)`: it finds a word-boundary `if (COND)`,
   extracts the parenthesised condition, evaluates it with the existing
   `ic_eval_binexpr` precedence-climbing evaluator (which already handles
   `==`, `!=`, `<`, params, literals, etc.), and formats only the live branch's
   snprintf -- the consequent when `COND` is true, otherwise the snprintf after
   the matching `else`. It is fail-closed: if the condition does not fully parse,
   it returns nil and the caller falls back to the legacy first-match scan.
3. `ic_exec_snprintf_fmt` Step 3 now tries `ic_snprintf_cond_branch` first, then
   falls back to the linear scan via `ic_format_snprintf_call`.

## Validation

- `range-bound-show-ord` matches `expected.stdout` under `--interpret`, and was
  added to the `tests/run-turi.sh` allowlist.
- `TUR=./build/tur bash tests/run-turi.sh` -> `980 passed, 0 failed`.
- Confined to interpreter-only `ic_exec_*` helpers; the compiled `emit-c`/`build`
  codegen path is untouched, so the full fixture suite is unaffected.

## Still open (not this fix)

Other `snprintf`-bucket inline-C fixtures in
[../reported/turi-inline-c-silent-miscompiles.md](../reported/turi-inline-c-silent-miscompiles.md)
miscompile through *different* paths (`%s`/string arguments, multi-statement
bodies) and remain carve-outs. The two non-snprintf pure-turi holdouts
(`codegen-private-defn-collision`, `rc-unique-violation`) are independent
core-system gaps tracked in the pure-turi report.
