---
status: open
severity: medium
discovered: 2026-07-24
area: tree-walking interpreter (turi/eval) -- fixture parity with the compiled backend
---

# `turi_fixture_tests`: 14 interpreter-vs-compiled parity gaps

## Summary

The `turi_fixture_tests` ctest target runs the fixture corpus through
`tur --interpret` (the tree-walking interpreter, `src/turi/eval.c`) instead of
compiling each fixture. As of 2026-07-24 it carries **14 failures out of ~2290**
fixtures. Every one is an interpreter-only divergence: the **compiled** path is
clean (`bash tests/run.sh` = 2278 passed / 0 failed), so none of these affect
the v1 ship backend. They are interpreter parity gaps, tracked here as a single
entry so the red `turi_fixture_tests` signal is explained rather than mistaken
for a compiled-backend regression.

These are **pre-existing** and were not introduced by the 2026-07-24 CI-fix work
(commit `96d761947`); that work *reduced* the count from 16 to 14 by fixing
`eval-async-timeout` / `eval-async-error` (see the fiber-alignment and
`error-message`-dispatch changes) and added zero new failures.

Reproduce any single fixture with:

```sh
./build/tur --interpret tests/fixtures/<name>/input.tur
diff <(./build/tur --interpret tests/fixtures/<name>/input.tur 2>/dev/null) \
     tests/fixtures/<name>/expected.stdout
```

The 14 group into five root causes.

## Group 1 -- `show-line` on a `String` yields the raw carrier pointer (4)

**Fixtures:** `string-basic`, `string-slice`, `term-string`, `range-string`
(the last also hits Group-5-style memory trouble, below).

**Repro (minimal):**

```turmeric
(load "stdlib/string.tur")
(show-line 42)                        ; prints "42"          -- OK
(show-line (string/from-cstr "hi"))   ; prints a pointer, e.g. 105759275480176
```

**Symptom:** `(show-line x)` prints the raw heap pointer instead of the string
content when `x` is a `String`. `string-basic` line 3 prints
`105759275482768` where `Hello, World` is expected; `string-slice` line 8 prints
a pointer where `world` is expected; `term-string` produces no lines at all.

**Root cause:** `show-line` (`stdlib/typeclass-show.tur:287`) is generic over
`^Show a` and calls `(show x)` through the class dictionary. Concrete-receiver
dispatch is fine -- `(string/to-cstr (show (string/from-cstr "hi")))` prints
`hi`, and `(show-line 42)` prints `42` -- but the **generic** (tyvar-receiver)
dispatch fails to re-resolve `Show [String]` for a `String` receiver and falls
back to a representative/identity that hands back the raw `ptr<void>` carrier.
This is the same mechanism documented (and previously resolved for the
`Size [Box]` case) in
`docs/archive/turi-generic-dict-dispatch-bakes-representative-instance.md`; the
Stage-4 flip of `Show` to return an owned `String` (commit `cb414fbd8`) opened a
new instance -- `String` is a `defopaque :ptr<void>`, so head-constructor-name
re-resolution (`gde_reresolve_method` in `src/turi/eval.c`) does not match it the
way it matches a `TY_ADT`/`TY_REC` head.

**Fix direction:** extend the generic-dict re-resolution so a tyvar pinned to a
`defopaque` (here `String`) resolves its `Show` instance -- match on the opaque
newtype name, not only `TY_ADT`/`TY_REC` heads. Validate with `show-line` over a
`String` returning the content, not a pointer.

## Group 2 -- inline-C String ops have no interpreter native override (3)

**Fixtures:** `csv-string`, `digest-string`, `path-string`.

**Symptom:** the run aborts with
`eval: inline-C not supported in interpreter mode (function uses a native C
implementation; run it with tur build/tur run instead of --interpret)` and
produces no stdout.

**Root cause:** the CSV, digest (md5/sha256), and path stdlib modules back their
core operations with inline-C bodies that the tree-walker cannot execute, and --
unlike the `string/*` core (`src/turi/string_native.c`) or the collection ops --
they have no registered native override in
`src/turi/interpreter_natives.c`. The interpreter hits the documented clean
carve-out (`src/turi/eval.c`, the `EX_INLINE_C` case) rather than running them.

**Fix direction:** register interpreter natives mirroring these inline-C bodies
(as was done for `string/*`, contracts, `#map` hashing, etc. under
`turi_env_register_interpreter_natives`), or mark the fixtures
`requires.compiled` if interpreter support for digest/csv/path is out of scope.

## Group 3 -- `String` value flows into an `int`-typed `cons` (1)

**Fixture:** `re-string`.

**Symptom:** `<eval>:...: error [TUR-E0001]: function 'cons' arg 1: expected int,
got cstr`, triggered from `re/replace-string` (`stdlib/re.tur`), so the fixture
produces no output.

**Root cause:** under the interpreter, a `String`/`cstr` value reaches a `cons`
cell slot that the interpreter type-checks as `int` (the cons carrier is an
`int64` head; the compiled path boxes the cstr pointer as `int64` transparently,
but the interpreter's runtime arg check rejects the `cstr` tag). A representation
mismatch between the interpreter's cons element typing and the compiled variadic
`__tur_cons_of` boxing.

**Fix direction:** allow the interpreter's cons-cell head to carry a boxed
`cstr`/`String` pointer (box as `int64`, matching codegen's
`(int64_t)(intptr_t)elem`) rather than requiring a literal `int` tag on the
`cons` argument path used by `re/replace-string`.

## Group 4 -- rc / closure-env drop counts diverge by one (3)

**Fixtures:** `rc-auto-drop-closure-capture` (prints `1`, expected `2`),
`rc-elision-negative-closure-capture` (prints `2`, expected `3`),
`closure-env-free-with-owning-sibling` (prints `37`, expected `39`).

**Symptom:** a strong-count / retained-count read is short by one under the
interpreter versus the compiled scoped-free path.

**Root cause:** the interpreter's reference-count and closure-environment drop
semantics do not exactly mirror the compiled backend's scoped-free / rc-elision
analysis for a value captured by a closure (and, in the sibling case, an env
shared by an owning sibling). The tree-walker drops (or elides a drop) one step
differently than the emitted C.

**Fix direction:** align the interpreter's closure-capture retain/drop with the
compiled scoped-free pass for these three shapes; the off-by-one suggests a
single missing retain on capture (or an extra drop on scope exit) in the
interpreter's closure-env handling. Low value for v1 (the compiled path is
correct) -- fix only if interpreter rc-accuracy is wanted for these fixtures.

## Group 5 -- shift/reset cross-fn resume, multishot, and a double-free (3)

**Fixtures:** `shift-crossfn-resume-named-fn` (missing `23 / 101 / 23 / 23`),
`shift-crossfn-resume-works` (missing `23 / 306`),
`multishot-effect-cont-kv-sugar` (missing `23`). `range-string` additionally
raises `AddressSanitizer: attempting double-free` in the same family of paths.

**Symptom:** delimited-continuation resumes that cross a function boundary, and
multishot continuation invocations, produce no output (the resumed body never
runs to its `println`); the interpreter also emits ASan
`__asan_handle_no_return` warnings around these (expected for its longjmp-style
unwinds) and, for `range-string`, a genuine double-free.

**Root cause:** the interpreter's heap-continuation resume path
(`src/turi/eval.c`, the `shift`/`reset`/effect-continuation machinery) does not
re-enter a continuation captured across a named-function call boundary, and does
not support invoking a captured continuation more than once (multishot). The
`range-string` double-free is a separate memory-management bug in the
interpreter's String/range path surfacing under the same corpus.

**Fix direction:** two distinct efforts -- (a) the cross-fn / multishot resume
gap in the interpreter's continuation runtime, and (b) the `range-string`
double-free, which should be pinned with a dedicated ASan repro
(`ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret
tests/fixtures/range-string/input.tur`) and fixed on its own, independently of
the continuation work.

## Severity / priority

**Medium**, interpreter-only. None of these touch the compiled backend or the v1
ship path; the compiled fixture suite is fully green. They keep
`turi_fixture_tests` red, which is acceptable under the repo's "red suites do not
block forward progress" policy, but they are real parity gaps worth closing so
the interpreter (used by `tur repl`, `tur --interpret`, and the WASM Try-Turmeric
REPL) matches compiled behavior on String rendering, digest/csv/path ops,
`re/replace-string`, rc accounting, and delimited continuations.

Groups 1 and 3 are the highest-leverage: they are the String-type parity gaps
opened by the Stage-4 `Show`-returns-`String` work and affect the most fixtures
per fix. Group 5's `range-string` double-free is the only memory-safety item and
should be triaged first within that group.
