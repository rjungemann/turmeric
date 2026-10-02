# `--strict-effects` and `--lint-effects` emit the same warning, and neither can become an error

**Severity: low (flag taxonomy / stale comment).** Two flags document
themselves as a strict/advisory pair, but for `TUR-W0030` they emit the same
diagnostic through duplicated code, and the "strict" one has no promotion path
-- no `-Werror` covers it and `diag.c` discards warning severity when
computing exit status. The in-tree comment asserting the distinction is wrong.
Filed 2026-10-01 while measuring whether `--strict-effects` could default on.

**Status: OPEN.** Cosmetic today. It matters because the distinction is the
stated reason both flags exist, and anyone deciding whether to default one on
will read the comment and believe a promotion mechanism is there.

## The claim, and what is actually there

`src/passes/effect_check.c:1557-1558`:

```c
/* --- ER6: --lint-effects: advisory warnings for unannotated effectful functions.
 * Behaves like --strict-effects (TUR-W0030) but is never promoted to an error. */
```

"Never promoted to an error" implies `--strict-effects` is. It is not:

- Both paths call `diag_emit_with_code(DIAG_WARNING, ..., TUR_W0030_...)` with
  a byte-identical format string (`effect_check.c:1511-1526` and
  `:1558-1578`).
- `src/compiler/diag.c:110` is `if (level == DIAG_WARNING) return false;` --
  warnings never contribute to a failing exit status, unconditionally.
- The only `-Werror=` flags in the driver are `deprecated` and
  `inline-c-narrow-params` (`src/main.c:11299-11320`). Neither covers W0030,
  and there is no general `-Werror`.

So the only behavioural difference between the two flags is that
`--strict-effects` additionally runs `effect_row_check_var_always_concrete`
(TUR-W0032, `effect_check.c:1507`). Measured over `tests/fixtures/` plus
`stdlib/`: W0030 fires 358 times, **W0032 once**. For practical purposes the
flags are the same flag.

## Minimal repro

```turmeric
(defeffect Bang [] :nil)
(defn boom [] : int (do (perform (Bang)) 0))
(defn main [] : int 0)
```

```sh
./build/tur --strict-effects check repro.tur ; echo "strict rc=$?"
./build/tur --lint-effects   check repro.tur ; echo "lint   rc=$?"
```

Both print the same `TUR-W0030` line; both exit 0.

## Fix directions

Pick one of two coherent stories; the current state is neither.

1. **Make `--strict-effects` strict.** Give W0030 a promotion path (a
   `--Werror=strict-effects`, or have `--strict-effects` itself emit
   `DIAG_ERROR`), leaving `--lint-effects` as the advisory form. This is what
   the names and the comment already promise, and it is the version that would
   make the flag worth defaulting on later. Needs the W0030 message fixed
   first -- see
   [strict-effects-w0030-names-synthesized-lambdas](strict-effects-w0030-names-synthesized-lambdas.md)
   -- since promoting an unactionable warning to an error is worse than
   leaving it a warning.
2. **Retire `--lint-effects`.** If W0030 is to stay advisory, one flag is
   enough; deprecate the alias (TUR-W0050 already has machinery for a retired
   flag) and delete the duplicated block.

Either way: **delete or correct the comment at `effect_check.c:1558`**, and
de-duplicate the two emit sites into one helper so a future message change
cannot touch only one of them.
