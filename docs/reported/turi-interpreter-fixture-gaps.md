---
status: open
severity: medium
discovered: 2026-07-24
updated: 2026-07-24
area: tree-walking interpreter (turi/eval) -- fixture parity with the compiled backend
---

# `turi_fixture_tests`: interpreter-vs-compiled parity gaps

## Summary

The `turi_fixture_tests` ctest target (driver: `tests/run-turi.sh`) runs the
fixture corpus through `tur --interpret` (the tree-walking interpreter,
`src/turi/eval.c`) instead of compiling each fixture. This entry tracks the
interpreter-only divergences so the red signal is explained rather than mistaken
for a compiled-backend regression: the **compiled** path is fully green
(`bash tests/run.sh` = 2278 passed / 0 failed), so none of these affect the v1
ship backend.

## Resolution log (2026-07-24)

The 2026-07-24 pass (branch `claude/turi-interpreter-fixture-gaps-dg7kc3`) fixed
three real interpreter engine bugs and marked the genuinely-inline-C fixtures
out-of-scope. Current interpreter-suite state: **1727 passed, 8 failed, 495
skipped**. Of the 8 remaining fails, 6 are the rc/continuation groups below and
2 are pre-existing timeout-flaky fixtures (`cps-tramp-resume-deep`,
`cps-tramp-resume-multicase` -- 1M/500k-iteration continuation loops that emit
the correct result in isolation but exceed `run-turi.sh`'s 15 s per-fixture
timeout under parallel CPU contention on the Debug+ASan build; not correctness
failures and not in the original 14).

**Fixed (now pass under `--interpret`):**

- `string-basic`, `string-slice` (Group 1) and the Group-1 half of
  `range-string` -- root cause was a **`last_tc_env` publish-timing bug**, not
  the opaque-newtype re-resolution the original triage guessed. `env->last_tc_env`
  (the runtime typeclass-instance registry read by `gde_reresolve_method` /
  `turi_try_show`) was assigned only in the *success epilogue* of
  `turi_eval_impl`, so the FIRST program to introduce a class's instances --
  e.g. `Show [String]`, first loaded when the user program pulls in
  `string.tur` -- evaluated against the PREVIOUS eval's tc_env, which lacked
  those instances. A generic `(show-line s)` then fell back to the baked
  int-carrier representative (`Show [int]`), printing `(int->string ptr)` -- the
  raw String pointer. Fix: publish `env->last_tc_env = tc_env_slot`
  immediately after elaboration succeeds, before evaluating the new forms
  (`src/turi/eval.c`).

- `range-string` fully (Group 1 + the Group-5 double-free) -- the
  `string/adopt-cstr` interpreter native (`n_adopt_cstr`,
  `src/turi/string_native.c`) called `tur_string_adopt_cstr`, which does
  `from_bytes(s) + free(s)`. Under `--interpret` the formatter's inline-C
  `malloc`/`snprintf` body is reproduced by the tree-walker via the env value
  **arena** (`turi_val_alloc`), not raw `malloc`, so handing that arena pointer
  to libc `free()` is a bad-free / double-free abort -- the same hazard the
  `extern-c free` native already no-ops for (`native_extern_free`). Fix: the
  interpreter's `adopt-cstr` now copies the bytes (`from_bytes`) but skips the
  `free`, consistent with the interpreter's process-lifetime allocation model.

- The Group-3 **`cstr`-head `cons` elaboration error** -- the preload native
  stub typed `cons` as `(defn cons [v :int n :int] :int ...)`, so the elaborator
  rejected a `cstr` head (`(cons "a" 0)`) under `--interpret` while the compiled
  path accepted it (the `cons` builtin's `cons_wildcard` bypass in
  `elab_call.c`). Fix: the preload stub head is now a polymorphic tyvar
  (`(defn cons [A] [v :A n :int] :int ...)`), matching the builtin's wildcard
  head; the runtime `native_cons` boxes the head through `intptr_t` exactly as
  codegen does. `re/replace-string` / `re/replace-all-string` now interpret
  correctly.

**Marked `requires.compiled` (genuinely out of scope for the tree-walker):**
`term-string`, `csv-string`, `digest-string`, `path-string`, `re-string`. Each
depends on stdlib inline-C with no interpreter native override -- the colorizers
(isatty-gated strdup/malloc), CSV emitters, md5/sha256, path helpers, and the
`re/union-patterns` builder. `--interpret` either aborts with the documented
"inline-C not supported" carve-out or, for the colorizers, mis-executes the
`isatty` branch. The `term`/`csv`/`path` module docstrings themselves state they
"run on the compiled path; use the underlying `*` cstr forms under
`--interpret`." These join the ~400 inline-C fixtures the harness already skips;
all five are green on the compiled path.

## Remaining open gaps (6 fixtures)

### Group 4 -- rc / closure-env drop counts diverge by one (3)

**Fixtures:** `rc-auto-drop-closure-capture` (prints `1`, expected `2`),
`rc-elision-negative-closure-capture` (prints `2`, expected `3`),
`closure-env-free-with-owning-sibling` (prints `37`, expected `39`).

**Root cause:** the interpreter's reference-count and closure-environment drop
semantics do not exactly mirror the compiled backend's scoped-free / rc-elision
analysis for a value captured by a closure (and, in the sibling case, an env
shared by an owning sibling). The compiled path inserts a retain on
closure-capture that the tree-walker does not, so a `rc/strong-count` read is
short by one.

**Fix direction:** align the interpreter's closure-capture retain/drop with the
compiled scoped-free pass for these three shapes; the off-by-one suggests a
single missing retain on capture (or an extra drop on scope exit) in the
interpreter's closure-env handling. Low value for v1 (the compiled path is
correct); risk of perturbing other interpreter fixtures is why it was not forced
in the 2026-07-24 pass.

### Group 5 -- shift/reset cross-fn resume and multishot (3)

**Fixtures:** `shift-crossfn-resume-named-fn` (missing `23 / 101 / 23 / 23`),
`shift-crossfn-resume-works` (missing `23 / 306`),
`multishot-effect-cont-kv-sugar` (missing `23`).

**Root cause:** the interpreter's heap-continuation resume path
(`src/turi/eval.c`, the `shift`/`reset`/effect-continuation machinery) does not
re-enter a continuation captured across a named-function call boundary, and does
not support invoking a captured continuation more than once (multishot). ASan
`__asan_handle_no_return` warnings around these are expected for the
longjmp-style unwinds. (The `range-string` double-free once listed here is
fixed -- see the resolution log.)

**Fix direction:** the cross-fn / multishot resume gap in the interpreter's
continuation runtime -- a distinct effort from the rc accounting above.

## Severity / priority

**Medium**, interpreter-only. None of these touch the compiled backend or the v1
ship path; the compiled fixture suite is fully green. The remaining six keep
`turi_fixture_tests` at a small, explained red under the repo's "red suites do
not block forward progress" policy. They are real parity gaps worth closing so
the interpreter (used by `tur repl`, `tur --interpret`, and the WASM
Try-Turmeric REPL) matches compiled behavior on rc accounting and delimited
continuations.
