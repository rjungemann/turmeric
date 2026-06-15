# Pure-turi silent miscompiles (11 real interpreter bugs + 1 legit carve)

> **RESOLVED:** all 12 are now closed. The 9 covered by the earlier updates
> below were fixed in W4/W1b; the **3 "deep" remainders are verified fixed on
> the current tree** under `ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret
> <fixture>/input.tur`:
>
> - **`range-bound-show-ord`** -- stdout now matches `expected.stdout` exactly
>   (Exclusive bound renders `(7`, Inclusive `[3`); the conditional-snprintf
>   inline-C path no longer always takes the first branch.
> - **`codegen-private-defn-collision`** -- prints `100` then `200` (no longer
>   `200/200`); the two modules' private `__h` defns resolve per-module instead
>   of last-wins.
> - **`rc-unique-violation`** -- `(ref/from-rc rc)` with a live `weak` now panics
>   with a nonzero exit: `panic: ref/from-rc requires unique rc
>   (strong_count==1 and weak_count==0), got strong=1 weak=1`.
>
> All three PASS under `tests/run-turi.sh` (no `requires.*` carve, not on any
> denylist) and the `reader-cond` legit carve remains `requires.compiled`.
> Moved to history.

> **Update (TI8.b/W4, 2026-06-11): 6 of the 11 fixed.** Four root causes in
> `src/turi/eval.c`:
> - **`EX_ASCRIBE` primitive coercion** -- `(:: x bool/float/int)` now reconciles
>   the runtime tag with the ascribed primitive (an int ascribed to `:bool`
>   becomes `TURI_BOOL`, so it prints `true` not `1`). Fixed
>   `rt-return-dispatch-basic` and `rt-return-dispatch-param`.
> - **`EX_ANY_TYPE_OF` coarse tags** -- returns `"struct"`/`"adt"` (matching the
>   compiled `__tur_any_type_name`) instead of the specific type name. Fixed
>   `any-box-struct`, `any-box-adt`.
> - **`EX_ANY_CAST` checked downcast** -- panics on a clear primitive/struct/ADT
>   tag mismatch instead of silently passing through. Fixed
>   `any-cast-mismatch-panic`.
> - **`catch-unwind` defer firing** -- fires the unwound frames' defers (LIFO)
>   before returning, so `(defer ...)` in a panicking thunk runs during the
>   unwind. Fixed `panic-catch-unwind-defer`.
>
> **Update 2 (cont.): 7 of 11 fixed; `reader-cond` carved.**
> - **`extern-c puts`** -- added `native_extern_puts` to the known-libc shim
>   list (alongside `printf`/`strlen`/...), so `(extern-c puts ...)` prints under
>   `--interpret` instead of hitting the nil stub. Fixed `extern-c-spaced-typeann`.
> - **`reader-cond`** is carved `requires.compiled`: `#?(:tur ... :turi ...)`
>   renders per the active reader, so its compiled-branch `expected.stdout` can
>   never match under `--interpret` -- not a bug.
>
> All fixed fixtures are on the `run-turi.sh` allowlist (harness 912 -> 919, 0
> failed; compiled 1573/0, zero regressions).
>
> **Update 3 (W1b): `result-of-typed-eq` FIXED** -- recovered when `result.tur`
> joined the prelude (dual-rep Result readers + `native_result_eq` +
> `EX_GET_FIELD` carrier-box path). Now on the allowlist.
>
> **3 remain, each deep and overlapping another workstream:**
> - `range-bound-show-ord` -- `bound-fmt` (stdlib inline-C) is
>   `if (kind==1) snprintf("[...") else snprintf("(...")`; `ic_exec_snprintf_fmt`
>   takes the *first* snprintf always, so Exclusive renders `[7` not `(7`. This
>   is the **inline-C-evaluator** conditional-snprintf gap (the 22-matcher
>   bucket), not pure-turi.
> - `codegen-private-defn-collision` -- two modules' private `__h` both register
>   under the bare name in the interpreter env (last wins -> `200/200`). Needs
>   per-module mangling in `EX_FN_DEF` registration *and* call resolution -- a
>   core module-system change.
> - `rc-unique-violation` -- `(ref/from-rc rc)` with a live `weak` must panic
>   (unique-rc violation); the interpreter's `ref/from-rc` does no strong/weak
>   count check.

**Summary:** Of the ~244 pure-turi (no inline-C) fixtures that fail under
`--interpret` after W1-W3, only **12** are *silent* (rc matches the expected exit
but the output is wrong); the other 232 are clean errors. These 12 matter most
because they are **not carve-able as inline-C** and would turn the
allowlist->denylist flip (W5) red while looking like passes. One is a legitimate
path-divergence (carve); the other **11 are real interpreter bugs** -- the
tree-walker returns a wrong value with a zero exit.

**Severity:** High (silent wrong answers on pure-turi programs -- the worst
failure mode, and the compiled path is correct).

## The 12

| Fixture | Got | Expected | Class |
| --- | --- | --- | --- |
| `reader-cond` | `interpreted` | `compiled` | **legit carve** -- `#?(:tur ... :turi ...)` reader conditional; the interpreter output is *correct*, the expected.stdout is compiled-path-specific. Carve `requires.compiled` for turi. |
| `rt-return-dispatch-basic` | `42 / 1` | `42 / true` | return-type dispatch: a `:bool` is shown as `1` (wrong Show instance / bool-as-int) |
| `rt-return-dispatch-param` | (wrong) | -- | same return-type-dispatch family |
| `any-box-adt` | (wrong tag) | -- | Any-type boxing: tag/name mismatch |
| `any-box-struct` | `Point` | `struct` | Any-type boxing: prints the struct name instead of the `struct` tag |
| `any-cast-mismatch-panic` | rc=0 | (nonzero) | Any-cast mismatch should panic; interpreter does not |
| `rc-unique-violation` | rc=0 | (nonzero) | rc-unique violation not detected at runtime |
| `result-of-typed-eq` | `false` | `true` | typed `eq?` over a Result returns wrong |
| `range-bound-show-ord` | (wrong) | -- | range bound / Show / Ord interaction |
| `panic-catch-unwind-defer` | (wrong) | -- | defer + catch-unwind ordering/value |
| `codegen-private-defn-collision` | (wrong) | -- | private defn name resolution under interpret |
| `extern-c-spaced-typeann` | (wrong) | -- | extern-c with a spaced type annotation |

(Reproduce: `ASAN_OPTIONS=detect_leaks=0 ./build/tur <flags> --interpret
tests/fixtures/<name>/input.tur` and diff against `expected.stdout`.)

## Notes / likely groupings

- **`rt-return-dispatch-*` (2)** look like one root cause: return-type-directed
  dispatch picking the wrong instance under the interpreter (a `:bool` printed as
  `1`). Start here -- highest chance of a single fix covering two fixtures.
- **`any-box-*` + `any-cast-mismatch-panic` (3)** are the Any-type machinery: tag
  naming on box, and the cast-mismatch panic not firing.
- **`reader-cond`** is not a bug: carve it. Audit the rest of the
  silent-miscompile set for other `#?(...)` reader-conditional fixtures that are
  also legit divergences (none of the other 11 use `#?`).

## Why these block W5

W5 flips `run-turi.sh` to "run everything minus markers." These 12 currently
*pass* the rc check but produce wrong output, so they would surface as harness
FAILs (or, worse, as silent passes if only exit codes were checked). Each must be
**fixed** in `src/turi/eval.c` (the 11 bugs) or **carved** with a reason
(`reader-cond`). Do NOT bulk-carve the 11 -- they are real interpreter
divergences and carving them hides wrong-answer bugs.

## Status

Filed while executing TI8.b/W4. The `ic_exec_accessor` boolean-return class was
fixed in the same pass; these pure-turi cases are the remaining W4 surface (each
its own root-cause investigation). See
[docs/upcoming/v1/turi-interpreter-gap-closure-plan.md](../upcoming/v1/turi-interpreter-gap-closure-plan.md).
