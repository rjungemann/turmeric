---
status: resolved
severity: medium
discovered: 2026-07-24
resolved: 2026-07-24
area: tree-walking interpreter (turi/eval) -- fixture parity with the compiled backend
---

# `turi_fixture_tests`: interpreter-vs-compiled parity gaps -- RESOLVED

## Summary

The `turi_fixture_tests` ctest target (driver: `tests/run-turi.sh`) runs the
fixture corpus through `tur --interpret` (the tree-walking interpreter,
`src/turi/eval.c`) instead of compiling each fixture. It carried 14 documented
interpreter-only divergences (the compiled `bash tests/run.sh` path was clean
throughout). As of the 2026-07-24 resolution pass the interpreter suite is
**1735 passed, 0 failed, 495 skipped**; the compiled suite is unchanged
(**2278 passed, 0 failed**).

## Resolution (2026-07-24, branch `claude/turi-interpreter-fixture-gaps-dg7kc3`)

Six real interpreter engine bugs fixed, five genuinely-inline-C fixtures marked
`requires.compiled`, and two pre-existing timeout-flaky fixtures given an
`expected.timeout` override.

### Fixed engine bugs

- **`last_tc_env` publish timing** (Group 1: `string-basic`, `string-slice`,
  Show half of `range-string`). `env->last_tc_env` -- the runtime
  typeclass-instance registry read by `gde_reresolve_method` / `turi_try_show`
  -- was assigned only in `turi_eval_impl`'s success epilogue, so the FIRST
  program to introduce a class's instances (e.g. `Show [String]`, loaded via
  `string.tur`) dispatched a generic `(show-line s)` against the PREVIOUS eval's
  tc_env, fell back to the baked `Show [int]` representative, and printed the raw
  String pointer. Fix: publish `tc_env_slot` right after elaboration succeeds,
  before evaluating the new forms.

- **`adopt-cstr` bad-free / double-free** (Group 5 memory-safety item;
  `range-string`, and the `term-string` crash). The interpreter's
  `string/adopt-cstr` native called `tur_string_adopt_cstr`, which frees its
  source cstr; under `--interpret` the source is arena memory (`turi_val_alloc`),
  not `malloc`, so `free()` aborts. Fix: the interpreter native copies the bytes
  without freeing, mirroring the existing `extern-c free` no-op.

- **`cstr`-head `cons` elaboration** (Group 3; `re/replace-string`). The preload
  `cons` stub typed head `:int`, so the elaborator rejected `(cons "a" 0)` under
  `--interpret` while the compiled `cons` builtin's `cons_wildcard` bypass
  accepted it. Fix: the stub head is now a polymorphic tyvar.

- **Multishot / cross-fn continuation resume** (Group 5;
  `shift-crossfn-resume-named-fn`, `shift-crossfn-resume-works`,
  `multishot-effect-cont-kv-sugar`). Two sub-bugs: (a) `ws_has_perform` /
  `ws_capturable` did not see through the transparent fn-wrapper nodes
  `EX_POLY_WRAP` / `EX_FN_TO_FAT` / `EX_POLY_TO_FAT`, so a `^multishot` receiver
  auto-shimmed into a fat closure forced the handle onto the one-shot ucontext
  fiber path (which aborts on the second resume); (b) the work-stack handler case
  body was dispatched with `tail = st[pidx].tail`, but a case body is delimited
  by its `DK_PROMPT` -- a tail call in it tripped the tail-fold invariant assert.
  Fix: the capturability analysis unwraps the wrapper nodes (so the handle runs
  on the multishot-capable work-stack path), and the case body runs non-tail (the
  prompt forwards its value to the true continuation, preserving enclosing TCO).

- **`rc<T>` retain-on-capture** (Group 4; `rc-auto-drop-closure-capture`,
  `rc-elision-negative-closure-capture`, `closure-env-free-with-owning-sibling`).
  The compiled closure env owns a strong reference to each captured `rc`; the
  interpreter shares the elaborator but not codegen and captures the lexical
  frame by pointer, so it never performed that retain and `rc/strong-count` read
  one short. Fix: at `EX_CLOSURE` evaluation, increment the strong count of every
  captured `__rc` value (structural detection, as `rc/clone` / `rc/drop` use).
  No matching decrement -- interpreter frames are never freed
  (`eval_frame_free` is a deliberate process-lifetime no-op), consistent with its
  leak-on-exit model.

### Marked `requires.compiled`

`term-string`, `csv-string`, `digest-string`, `path-string`, `re-string` --
each depends on stdlib inline-C with no interpreter native override (colorizers,
CSV emitters, md5/sha256, path helpers, the `re/union-patterns` builder). The
`term`/`csv`/`path` module docstrings themselves state they "run on the compiled
path; use the underlying `*` cstr forms under `--interpret`." All five are green
on the compiled path and join the ~400 inline-C fixtures the harness skips.
(The `re-string` `cons`/`show` gaps were real and are fixed; only the inline-C
union builder is out of scope, so the fixture as a whole stays compiled-only.)

### Timeout overrides

`cps-tramp-resume-deep` and `cps-tramp-resume-multicase` (1M / 500k-iteration
delimited-continuation loops, NOT part of the original 14) produced correct
output in isolation but exceeded `run-turi.sh`'s 15 s per-fixture timeout under
parallel CPU contention on the Debug+ASan build. Each now carries
`expected.timeout = 60`, which both harnesses honor; a genuine hang still fails.

## Verification

- Interpreter suite (`bash tests/run-turi.sh`): 1735 passed, 0 failed, 495 skipped.
- Compiled suite (`bash tests/run.sh`): 2278 passed, 0 failed (unchanged).
- No regressions across 409 effect/shift/cont/handle/resume and 124
  rc/closure/weak/drop/elision fixtures under `--interpret`.
