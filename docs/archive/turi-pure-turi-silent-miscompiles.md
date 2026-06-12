# Pure-turi silent miscompiles (11 real interpreter bugs + 1 legit carve)

> **RESOLVED (2026-06-12): all 11 bugs fixed; `reader-cond` carved.** The final
> holdout `rc-unique-violation` is fixed -- the interpreter's `__rc` control
> block now carries a weak count (`cnt[1]`) alongside the strong count
> (`cnt[0]`); `EX_WEAK` bumps it, and `EX_REF_FROM_RC` enforces the same
> uniqueness invariant as the compiled `tur_ref_from_rc` (strong==1 && weak==0),
> panicking with the matching message otherwise (`src/turi/eval.c`). Fixture on
> the `run-turi.sh` allowlist; harness 982/0, full suite 1596/0 (with run.sh
> validating the stderr substring). With the three 2026-06-12 fixes
> (`range-bound-show-ord`, `codegen-private-defn-collision`,
> `rc-unique-violation`) the silent-miscompile surface for pure-turi is empty.
> This report is archived; the per-fix paper-trails live alongside it in
> `docs/archive/`.
>
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
> **Update 4 (2026-06-12): `range-bound-show-ord` FIXED** -- the
> inline-C-evaluator conditional-snprintf gap is closed. `ic_exec_snprintf_fmt`
> now resolves a guarding `if (COND) snprintf(...); else snprintf(...);` by
> evaluating `COND` (via the existing `ic_eval_binexpr` precedence climber) and
> formatting only the live branch, instead of always taking the first snprintf.
> `bound-fmt`'s Exclusive endpoint renders `(7` again. The snprintf-formatting
> body was factored into `ic_format_snprintf_call`, and branch selection into
> `ic_snprintf_cond_branch` (`src/turi/eval.c`). Fixture added to the
> `run-turi.sh` allowlist (harness green). Resolution paper-trail archived at
> [turi-inline-c-conditional-snprintf-branch.md](turi-inline-c-conditional-snprintf-branch.md).
>
> **Update 5 (2026-06-12): `codegen-private-defn-collision` FIXED.** The
> interpreter now mirrors the compiled per-module private mangling. A `defn`
> inside a `defmodule` that is not in the module's export list is registered
> under the qualified key `"<module>/<name>"` (plus a bare alias only when the
> bare slot is still free, so the entry-point `main` and legacy cross-module
> bare references stay reachable). Each closure is tagged with its owning module;
> `eval_apply` publishes that as `env->current_module` while the body runs, and
> `eval_lookup` probes `"<module>/<name>"` before the flat global name. So
> `alpha/__h` (100) and `beta/__h` (200) no longer collide on the bare `__h`
> slot. Fixture added to the `run-turi.sh` allowlist; harness green (980/0),
> full suite 1596/0, `tur_eval_import` ctest green. Resolution archived at
> [turi-interpreter-module-private-defn-collision.md](turi-interpreter-module-private-defn-collision.md).
>
> **Update 6 (2026-06-12): `rc-unique-violation` FIXED -- 0 remain.** The
> interpreter's `__rc` control block became a 2-slot counter (`cnt[0]` strong,
> `cnt[1]` weak); `EX_RC_OF` allocates both, `EX_WEAK` bumps the weak count
> (while staying value-transparent), and `EX_REF_FROM_RC` now enforces
> strong==1 && weak==0 -- panicking via `turi_runtime_panic` with the same
> `"ref/from-rc requires unique rc ..."` message the compiled `tur_ref_from_rc`
> aborts with. Legit unique `ref/from-rc` (no live weak) is unaffected. Fixture
> on the `run-turi.sh` allowlist; harness 982/0, full suite 1596/0 (run.sh
> validates the stderr substring). Resolution archived at
> [turi-interpreter-rc-from-rc-unique-check.md](turi-interpreter-rc-from-rc-unique-check.md).
>
> **0 remain -- this report is fully resolved (see the RESOLVED banner at top).**

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
