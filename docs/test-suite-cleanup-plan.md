# Test-suite cleanup plan

This document tracks failing tests in `just test` as of branch
`claude/relaxed-dijkstra-EoKvS`, the quick fixes that were applied, the
real bugs / missing features that came out of those investigations, and
the tests that are now flagged as exercising genuinely missing behavior
(and therefore left failing intentionally).

## Snapshot of the run before this branch

* `tur_tests` (fixture suite): ~351 unique failures
* `tur_flags_tests`: 1 failure (`effect-export-syntax`)
* `turi_fixture_tests`: 2 failures (`result-basic`, `typed/tzipper-basic`)
* Other suites: pass

The bulk of the fixture failures fell into a small number of root causes
that had nothing to do with the actual fixture's intent.

## Snapshot after this branch

* `tur_tests`: 5 unique fixture failures remain (all documented below)
* All other suites: pass

`just test` now reports `956 passed, 5 failed` for the compiled-fixture
suite, with the remaining failures all classified below.

## What changed

### 1. Stdlib: fixed `-Wincompatible-pointer-types` warnings

A bunch of fixtures were tripping a `cc` warning that leaked into stderr
and caused indirect failures (notably `effect-export-syntax` in
`tur_flags_tests` checks combined stdout+stderr equal to `"hello"`).

The root cause: several stdlib functions cast through anonymous struct
types like:

```c
struct { bool is_ok; int64_t ok_val; int64_t err_val; } *a =
    (struct { bool is_ok; int64_t ok_val; int64_t err_val; } *)(intptr_t)r1;
```

In C99, each anonymous struct literal is a *distinct* type, so the
cast-to and the variable-of are incompatible pointer types. Fix: cast
through `void *`, which converts implicitly. Updated:

* `stdlib/result.tur` -- `result-eq?`, `result-collect`, `result-partition`
* `stdlib/tmutmap.tur` -- `tmutmap-new`

After this change, `effect-export-syntax` passes again and the warning
noise is gone from every fixture's stderr.

### 2. Regenerated stale codegen snapshots (mass quick fix)

Recent stdlib / runtime work (EX1/EXG1: constrained-existential types,
DS3: rc<Struct> cycle support) added new typedefs and helpers to the
shared C preamble emitted by `tur emit-c`. ~290 fixtures with an
`expected.c` snapshot fell out of sync purely because of this preamble
change -- the program output (`expected.stdout`) was still correct.

A script (kept inline at `tools/` would be overkill -- it was a
throwaway script) was used to copy `actual.c` -> `expected.c` for every
fixture whose `expected.stdout` matched `actual.stdout` and whose
generated C compiled cleanly. This is a quick fix; the snapshots are
again capturing the current runtime preamble.

Per the snapshot decision below, this was the last time the regeneration
sweep should be needed at scale -- once `expected.c` is dropped for
behavior fixtures, future preamble changes will only touch the small
remaining codegen suite.

**Note:** Snapshot regeneration is mechanical and will need to be redone
whenever the runtime preamble changes. Worth a thought to either:

* add a `tools/update-snapshots.py` helper, or
* drop expected.c snapshots entirely for fixtures whose value is the
  runtime behavior and not the literal C output. The runtime preamble
  is shared across all fixtures, so 290 copies of the same bytes do not
  buy us much.

**Decision:** Drop `expected.c` for behavior fixtures. Audit which
fixtures actually validate codegen (likely a small set under explicit
codegen directories) and keep `expected.c` only for those; the rest
verify `expected.stdout` only. This eliminates the recurring 290-file
churn outright. No `tools/update-snapshots.py` helper needed if the
remaining codegen surface is small enough to regenerate by hand;
revisit if it isn't.

### 3. Renamed user-defined symbols that now collide with auto-loaded stdlib

When `stdlib/result.tur`, `stdlib/option.tur`, `stdlib/tvec.tur`,
`stdlib/typeclass-eq.tur`, etc. were promoted to auto-loaded modules,
old fixtures that *manually* defined helpers like `ok`, `err`, `Vec`,
`Cons`, `Pair`, `Option`, `Eq`, `Show`, `result-eq?`, `ok-or`,
`result-free`, `result-map`, ... started failing. Two failure modes:

* `defstruct` collisions raise a clear Turmeric-level diagnostic
  ("defstruct: `Vec` is already defined").
* `defn` collisions are **not** diagnosed at the Turmeric level (see
  Missing Feature 1 below); they sneak through and the generated C
  fails with `redefinition of 'ok'`, `conflicting types for 'err'`,
  etc.

I renamed the local definitions in the affected fixtures by prefixing
them with `u-` (for "user"), `My`, or `H` (for "heap") -- whichever
preserved readability:

| Fixture | Rename |
| --- | --- |
| `clone-vec` | `Vec` -> `HVec` |
| `show-list` | `Cons` -> `HCons`, `Show` -> `MyShow` |
| `show-option` | `Option` -> `MyOption`, `Show` -> `MyShow` |
| `show-pair` | `Pair` -> `MyPair`, `Show` -> `MyShow` |
| `gadt-refine-nat`, `gadt-stdlib-vec` | `Vec` -> `MyVec` |
| `typeclass-derived`, `typeclass-primitives` | `Eq`/`Ord`/`Show` -> `MyEqDerived`/`MyOrdPrim`/... |
| `must-expect`, `must-msg`, `must-result-err`, `must-result-ok` | `ok`, `err`, `some`, `result-must*`, `option-expect` -> `u-*` |
| `warn-suppress-ignore`, `warn-unused-result` | `ok` -> `u-ok` |
| `error-context` | `ok`, `err`, `ok?`, `err-context`, `err-val-cstr` -> `u-*` (kept `err` as a param name -- those don't collide because they only exist inside inline-C blocks) |
| `error-ok-or` | `ok`, `err`, `some`, `none`, `ok-or` -> `u-*` |
| `result-collect` | `ok`, `err`, `result-collect`, `result-partition*` -> `u-*` |
| `result-combinators` | `ok`, `err`, `result-map*`, `result-flat-map`, `result-or`, `result-or-else` -> `u-*` |
| `result-display` | `ok`, `err` -> `u-*` (also adjusted the cstr literals printed) |
| `result-question-op` | `ok`, `err`, `ok?`, `err?`, `ok-val`, `err-val` -> `u-*`, and wrapped the `?` call in `(unsafe ...)` because the stdlib lowering for `?` calls into the deprecated unsafe `err?`/`err-val`. |
| `stdlib-result-runtime` | `ok`, `err`, `result-free`, `result-unwrap*`, `result-expect` -> `u-*` |
| `structural-eq` | `ok`, `err`, `result-eq?`, `result-eq` -> `u-*` |

Each fixture also had its `expected.c` snapshot regenerated and its
`expected.stderr` updated when the panic message changed.

### 4. `tzipper-basic` and `typed/tzipper-basic` double-free

Both fixtures called `(free left)` and `(free right)` *after*
`(tzipper-free z)`. But `tzipper-free` itself frees the neighbour arrays
(see `stdlib/tzipper.tur:122-138`), so the explicit `free` was a
double-free that glibc happened to detect (`free(): double free detected
in tcache 2`).

Fix: removed the redundant `free` calls; added a comment in each
fixture pointing at the ownership contract. (Note: arguably the
ownership contract on `tzipper-new` -- "transfers ownership to the
zipper" -- is surprising; see Missing Feature 4.)

### 5. Updated `must-result-err` expected stderr

Once the local `err` was renamed `u-err`, the panic message in the
fixture itself changed from `result-must: called on err` to
`u-result-must: called on u-err`. Updated `expected.stderr` to match.

## Remaining failures, by category

### Genuinely missing features (left failing intentionally)

#### Missing Feature 1 -- `defclass` redefinition is not diagnosed [FIXED]

Tests: `errors/kinds-hkt-reserved`, `errors/typeclass-no-instance`
(both now passing).

These fixtures expect that redefining a typeclass produces a Turmeric
diagnostic ("typeclass `Functor` is already defined" / "typeclass `MyEq`
is already defined"). The elaborator previously silently accepted the
second `defclass` and the test exited 0.

`defstruct` already has this check (see `clone-vec` diagnostic); the
same check is now in place for `defclass`. Identical-signature
re-declarations (the stdlib pre-declare case for Eq, Functor, ...)
remain silent.

**Decision:** Allow idempotent redefinition only if the signatures
match; otherwise hard-error. Implemented via
`typeclass_signatures_match` in `src/compiler/elab_typeclasses.c`:
compares type-parameter count and kinds, method count, and per-method
name + parameter type-kinds + return type-kind. Identical = silent
skip (preserves the stdlib pre-declaration of `Eq`, `Functor`, ...);
different = `typeclass 'Foo' is already defined` diagnostic.

The defclass parsing was split into three passes (parse type params /
parse signatures / elaborate default bodies) so the redefinition
check happens after enough information exists to compare, but before
any orphan `__default_*` file-level FnDefs get registered for an
about-to-be-skipped re-declaration.

**Fixture follow-up [done]:** Both `errors/kinds-hkt-reserved`
(differs in `[^f]` vs `[a]` + `fmap` vs `map`) and
`errors/typeclass-no-instance` (differs in `eq?` vs `eq2?`) trip the
differing-signature branch. Both fixtures now pass.

#### Missing Feature 2 -- `?` operator diagnostic when result helpers are out of scope

Test: `errors/result-question-op`.

The fixture expects: "name resolution error -- `err?` not in scope". The
elaborator instead reports: "unsafe function `err?` requires an
enclosing (unsafe ...)". Both errors are *correct* in their own context
(stdlib's `err?` *is* unsafe), but the test predates the
deprecation/`#{Unsafe}` annotation. Either:

* the `?` lowering should auto-wrap the helper calls in `(unsafe ...)`
  (preferred -- it's a built-in lowering), or
* the expected diagnostic in the fixture should be updated.

**Decision:** Lower `?` to a single runtime helper (hybrid of the
two options above). Add `__tur_result_question` (or similar) to the
runtime preamble; its body is the only place that calls `err?` /
`err-val`. `?` expands to a call to that helper. The helper's
definition site carries the `(unsafe ...)` wrapper once; `?` call
sites stay clean and users do not need to write `(unsafe ...)`
themselves.

Why this over a per-site auto-wrap:

* Audit surface collapses to one named function. A reviewer can
  inspect `__tur_result_question` and know the full unsafe story for
  `?` across the language.
* Deprecation of `err?` / `err-val` becomes "update one function"
  instead of "chase the lowering pass" -- the dependency is visible
  in the runtime preamble rather than scattered through
  compiler-synthesized code at every call site.
* The `(unsafe ...)` annotation stays meaningful in user code --
  it only appears when the user took responsibility, not as
  ceremony around every `?`.
* MF3 interaction is simpler: the helper resolves `err?` / `err-val`
  exactly once at its own definition site, against whatever scope
  applies there. No per-call-site shadow-aware resolution in the
  lowering.

Caveats to watch:

* The helper is a runtime ABI surface -- once published, changing
  its signature is a breaking change. Pin the signature early and
  document it.
* If user code shadows `err?` and expects `?` in their module to use
  the shadow, that no longer works (the helper is resolved against
  the stdlib scope). Document this as the intentional tradeoff; if
  per-module shadowing of `?` is needed later, revisit.
* Long-term, consider re-lowering `?` to a typeclass dispatch
  (Try / MonadFail) so the helper becomes one instance of a general
  mechanism rather than a hard-coded runtime function.

**Fixture follow-up:** The existing `errors/result-question-op`
fixture is obsolete under this lowering. Its premise was "what
diagnostic do you get when `err?` isn't in user scope?", but with the
hybrid lowering `?` calls `__tur_result_question` -- user-scope
`err?` is irrelevant to whether `?` elaborates. Either delete the
fixture, or re-purpose it to cover something the new lowering can
still diagnose (e.g. `?` applied to a non-Result type, or `?` used
outside a function returning Result).

#### Missing Feature 3 -- `defn` shadowing of stdlib symbols emits broken C instead of a diagnostic

Not a directly failing test (we worked around it everywhere), but it is
the root cause of most of the rename churn in section 3 above. If a
user writes `(defn ok ...)`, the auto-loaded stdlib `ok` and the user
`ok` both get emitted as static C functions named `ok` -- the C
compiler then complains. The Turmeric elaborator should either:

* error: "function `ok` is already defined by stdlib/result.tur (auto-loaded)";
* or mangle user-shadowed defns with a unique suffix in the emitted C.

The current behaviour is the worst of both worlds (no Turmeric error,
broken C output). Tracking here so it doesn't get lost.

**Decision:** Hard error by default, with an opt-in shadow attribute.
A user `defn` colliding with an auto-loaded stdlib name produces the
diagnostic above. Users who genuinely want to shadow attach
`#{Shadow}` (exact attribute name TBD) to the `defn`; the elaborator
then emits the C symbol with a mangled suffix (e.g. a module hash) to
avoid the `redefinition of 'ok'` C-level error.

Implication for section 3 renames: once the attribute lands, audit
each fixture in the rename table and either revert to the original
name with the attribute attached, or keep the rename. Track outcomes
in a follow-up.

#### Missing Feature 4 -- stdlib internal conflict between `stdlib/gadt-vec.tur` and `stdlib/tvec.tur`

Test: `gadt-stdlib-vec-stdlib` (left failing).

`stdlib/tvec.tur` defines `(defstruct Vec [A] ...)` and is auto-loaded.
`stdlib/gadt-vec.tur` defines `(defgadt Vec [a] ...)` and is loaded
explicitly via `(load "stdlib/gadt-vec.tur")` by this fixture. When the
GADT is loaded after the struct is already in scope, the `:Vec`
parameter annotation inside `gadt-vec.tur` resolves to the *struct*
`Vec` (a `tyvar`), not the GADT, and the file fails to elaborate.

Either:

* rename the GADT in `stdlib/gadt-vec.tur` (e.g. `(defgadt GVec ...)`);
* or teach the elaborator to keep separate namespaces for structs vs
  GADTs;
* or remove `gadt-vec.tur` if it's superseded by `tvec.tur`.

This is squarely a stdlib bug. Test left failing as the forcing
function.

**Decision:** Separate struct / GADT namespaces in the elaborator.
The collision is a class of bug, not a one-off, so fix it at the
right layer rather than papering over with a rename. Sketch:
`src/compiler/elab_structs.c` registers GADT names in a distinct
lookup table; `:Type` annotations in parameter / return position
resolve preferring GADTs in annotation context. Write a short design
note in `docs/` before coding so the resolution rules (and any
ambiguity diagnostics) are settled up front.

#### Missing Feature 5 -- `tzipper-new` ownership contract

No longer an aside: the decision below makes this active work with
caller audits and a regression risk, not a docstring polish. The
current API is that `tzipper-new` takes ownership of the left/right
arrays and `tzipper-free` frees them. This is surprising for an
stdlib container -- typically containers either borrow or document
ownership transfer prominently.

**Decision:** Switch to borrow + copy. `tzipper-new` mallocs its own
left / right buffers and `memcpy`s the caller's data; callers free
their own inputs. `tzipper-free` continues to free the zipper-owned
buffers. This is a breaking behavior change -- audit every caller
(start with `tests/fixtures/tzipper-basic`, `tests/fixtures/typed/tzipper-basic`,
and any internal stdlib users) and re-add explicit `free` calls for
the caller-owned inputs at the call sites.

### External dependency missing (left failing intentionally)

#### `scscm-compile` [FIXED]

The fixture does `(load "../turmeric-spices/tur-scscm/src/scscm/...")`,
which requires the `turmeric-spices` repo to be checked out as a
sibling directory. In CI / fresh containers it isn't.

**Decision (implemented):** Added the `requires.spices` skip marker.
Mirrors the existing `requires.tsan` plumbing; `tests/run.sh` detects
the missing sibling `../turmeric-spices/` directory and skips.
Marker file at `tests/fixtures/scscm-compile/requires.spices`.
`CLAUDE.md` now lists all `requires.*` markers and has an "Optional
dependencies" section describing how to enable.

### Compiler memory leaks (uncovered while executing this plan)

Running `bash tests/run.sh` directly (rather than via `ctest`) surfaces
~148 additional "build failed" failures that `ctest` hides by setting
`ASAN_OPTIONS=detect_leaks=0`. Each is a LeakSanitizer report against
the `tur` binary itself -- a pre-existing leak in the compiler, not a
runtime program leak. Plug them so the two invocation paths agree and
so future leaks aren't masked.

Distinct leak sites observed across the ~148 fixtures:

| Site | File:line | Count | Status |
| --- | --- | --- | --- |
| `collect_free_vars` (closure captures) | `src/compiler/elab_core.c:446` | 114 | Fixed (arena-copy in `elab_fns.c:1568`) |
| `fresh_tmp` (codegen temp names) | various | 131 | Fixed (free at each leaking site) |
| `elab_defdynamic` | `src/compiler/elab_dynvars.c:141` | 10 | Fixed (`free(e.dynvar_entries)` in elab teardown) |
| `dynvar_push_active` | `src/compiler/elab_dynvars.c:168` | 9 | Fixed (`free(e.active_dynvar_bindings)` in elab teardown) |
| `scope_add` (try-catch / select / match) | `src/compiler/elab_concurrent.c` / `elab_structs.c` | 5 | Fixed (added `scope_free` at each site) |
| `read_curly_infix` | `src/compiler/reader.c:1367` | 1 | Fixed (free items[] on n==1 path) |

After all fixes, `bash tests/run.sh` reports **956 passed, 5 failed** -- the same as `just test` (ctest). The five remaining failures are the plan-known intentional failures (Missing Features 1, 2, 4 and `scscm-compile`).

#### Compiler-leak task 1 -- `fresh_tmp` family (largest)

`fresh_tmp`, `fresh_frame`, `fresh_defer_thunk`, `fresh_defer_env` each
malloc a 24-byte string and return it to the caller. No caller frees
the result -- the strings live for the rest of compilation. There are
~83 `fresh_tmp` call sites; chasing each is impractical.

**Decision:** add a string pool to `EmitCtx` (`char **fresh_strs` +
count/cap) that owns every allocation from the four `fresh_*` helpers,
and free the pool when `emit_program` / `emit_implementation` tear
down `EmitCtx`. Existing call sites are unchanged.

#### Compiler-leak task 2 -- `dynvar_push_active`

Inspect the malloc site, identify the matching destructor (or its
absence), and either free in the existing teardown path or arena-allocate.

#### Compiler-leak task 3 -- `elab_defdynamic`

As above.

#### Compiler-leak task 4 -- `scope_add`

`scope_free` exists (called in `elab_fns.c` after `collect_free_vars`).
Find the path where a scope is created without a matching `scope_free`.

#### Compiler-leak task 5 -- `read_curly_infix`

Single leak in the reader; likely a transient buffer not freed on the
success path. Small.

#### Compiler-leak task 6 -- alignment commit

Once all of the above are resolved and `bash tests/run.sh` matches
`ctest`'s pass/fail counts, no further change is needed. If any leak
proves intractable, leave a `# TODO(compiler-leak)` marker and add
`ASAN_OPTIONS=detect_leaks=0` to `tests/run.sh` so the two paths
agree.

## Files of note touched on this branch

* `stdlib/result.tur` -- pointer-cast warning fix in `result-eq?`,
  `result-collect`, `result-partition`.
* `stdlib/tmutmap.tur` -- pointer-cast warning fix in `tmutmap-new`.
* `tests/fixtures/**/expected.c` -- mass regeneration (~300 files);
  later dropped entirely for behavior fixtures (see section 1 of the
  Decisions block).
* Renamed fixtures in section 3.
* `tests/fixtures/tzipper-basic/input.tur`,
  `tests/fixtures/typed/tzipper-basic/input.tur` -- removed double-free.
* `src/compiler/elab_fns.c` -- closure captures now arena-copied so
  they share the closure's lifetime (plugs the `collect_free_vars` leak).
