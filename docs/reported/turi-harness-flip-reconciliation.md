# TI8 harness flip: allowlist reconciliation + full-denylist blast radius

> **Prereq decomposition (2026-06-12):** the "de-risked roadmap" below is broken
> into independently-landable groundwork (native-registry parity diff,
> benchmark-stub overlap audit, opt-in `TUR_TURI_FULL_PRELUDE` flag, carve
> markers for the move/linearity + `if-bool` divergences) in
> [docs/upcoming/v1/turi-open-reports-prereqs.md](../upcoming/v1/turi-open-reports-prereqs.md).

**Summary:** `tests/run-turi.sh` was flipped from `tur run` (which compiles and
runs a native binary) to `tur --interpret` (the actual tree-walking
interpreter), resolving the blocker in
[turi-harness-compiles-instead-of-interpreting.md](../archive/history/turi-harness-compiles-instead-of-interpreting.md).
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

## TI8.b progress: defmodule concatenation defect fixed

The `46x only one defmodule is allowed per file` bucket was root-caused and
**fixed**. Root cause: `cmd_eval` (the `--interpret` entry) preloaded
`macros.tur` via `turi_eval_file`, which **concatenates** the source into a
single `<eval>` blob with `file_id = 0`. `macros.tur` carries `(defmodule
tur/macros ...)`, so when a user fixture *also* declared a defmodule, both forms
landed in `file_id 0` and the per-file `has_defmodule` reset
(`elab_toplevel.c:1183`, which keys on a `span.file_id` change between
consecutive forms) never fired -- tripping the one-defmodule-per-file check in
`elab_module.c:553`.

Fix (`src/main.c`, `cmd_eval`): preload `macros.tur` via a `(load "...")` form
instead of `turi_eval_file`. The `(load ...)` preprocessing assigns the loaded
file its own `file_id`, so the boundary reset fires and a user defmodule no
longer collides with the preloaded one. Verified: `defmodule-fat-fn-param-export`
/ `defmodule-pap-forward-ref-fat-fn` and the `module-*` family now pass under
`--interpret`. Post-fix probe: **660 pass / 910 fail / 92 skip** (was 637/933).
23 module/defmodule fixtures were added to the `run-turi.sh` allowlist (TI8.b
block); the harness is green at **145 passed, 0 failed**.

## De-risked roadmap for the remaining recovery (next session)

The biggest *recoverable* bucket is the typed-stdlib cluster (typed/*, clone-*,
make-struct, map-new/...): the interpreter does **not** preload the
typed-collection + typeclass-stub modules that the compiled path auto-loads in
`compile_to_c()` (`src/main.c:646`), so `Cons`/`Option`/`Result`, the
`Clone`/`Eq`/`Hash` classes, and `map-new`/`vec-eq?`/`tcons`/... are unbound
under `--interpret`. An experiment preloading that set surfaced three issues
that must be handled together before it can land:

1. **Use the `(load ...)` mechanism, not `turi_eval_file`.** Several modules
   (`safe.tur`, `contract.tur`) carry their own defmodule; only `(load ...)`
   assigns distinct file_ids so the per-file reset keeps them from colliding
   (same root cause as the fix above, scaled up).
2. **Drop the benchmark stubs that real modules provide.** `cmd_eval` injects
   no-op stubs (`vec-get`, `vec-set!`, `hamt-*`, `ok?`, `some?`, ...) for
   benchmark scripts that do not load stdlib. The real modules then fail to
   elaborate with `defn: 'vec-get' is already defined by an auto-loaded stdlib
   module`. The overlapping stubs must be removed (keeping only the genuinely
   benchmark-only ones: `run-ring`/`run-nbody`/`io-*`/`random-access-bench`/
   `cstr->parse-int`/`bit-*`/...), and benchmark fixtures re-checked (most
   already self-provide local inline-C stubs per CLAUDE.md).
3. **Cost.** Preloading ~24 modules adds ~300ms per interpreter invocation and
   makes the full probe run ~6x slower (the interpreter re-elaborates all
   accumulated source each `turi_eval` call). Acceptable for one-shot script
   runs; consider a cached/precompiled prelude for the REPL.

`wk_eval_fixture` (`src/main.c:6671`) already preloads the full typed-stdlib set
via `turi_eval_file` -- it is a useful reference, but note it would hit the same
defmodule-concatenation issue and should be re-validated when the prelude path
is unified.

The other large buckets stay as-is for now: typeclass-registration gaps during
stdlib load (schema/range/str), the move/linearity-checker divergence (~20),
`if condition must be bool` type-check divergence, genuine inline-C carve-outs,
and the silent miscompiles in bucket C (which deserve individual root-cause
reports). Each must be fixed in `src/turi/eval.c` or carved with
`requires.tur-only` before the allowlist->denylist flip can land green.

## Remaining wiring (follow-up)

- `tests/run-flags.sh` still calls `tur run` for three specific assertions
  (`:345` try-with-basic, `:355` try-with-nested, `:408` effect-export-explicit).
  Left as-is here to keep scope contained; flip them to `--interpret` when their
  interpreter behavior is confirmed.

## Status

Filed while executing TI8 of
`docs/upcoming/turi-parity-post-v1-plan.md`. TI8.a (parity ratchet +
harness-now-interprets + honest allowlist) and the TI8.b defmodule fix have
landed; the full allowlist->denylist flip remains, with the de-risked roadmap
above and the measured per-bucket scope.
