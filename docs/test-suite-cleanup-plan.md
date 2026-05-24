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

#### Missing Feature 1 -- `defclass` redefinition is not diagnosed

Tests: `errors/kinds-hkt-reserved`, `errors/typeclass-no-instance`.

These fixtures expect that redefining a typeclass produces a Turmeric
diagnostic ("typeclass `Functor` is already defined" / "typeclass `MyEq`
is already defined"). The elaborator currently silently accepts the
second `defclass` and the test exits 0.

`defstruct` already has this check (see `clone-vec` diagnostic); the
same check should be added for `defclass`. Until that lands, these two
negative-fixture tests stay failing -- they are the documentation for
the missing diagnostic.

**Decision:** Allow idempotent redefinition only if the signatures
match; otherwise hard-error. The current silent-skip in
`src/compiler/elab_typeclasses.c:557-563` exists so stdlib can
pre-declare `Eq` without colliding. The new check should compare the
second `defclass`'s method set, type-parameter count, and kinds against
the existing entry: identical = silent skip (preserves the stdlib
pre-declaration use case); different = `defclass: 'Foo' is already
defined` diagnostic. A follow-up may introduce an explicit
`(declare-class ...)` form if the signature-match check turns out to
permit too much in practice.

**Fixture follow-up:** Inspect both `errors/kinds-hkt-reserved` and
`errors/typeclass-no-instance` and confirm their second `defclass`
genuinely differs from the first (different methods, kinds, or
arity). If a fixture happens to redefine with an identical signature
it will now silently succeed and the diagnostic won't fire. Rewrite
the second definition to differ, or add a new negative fixture
covering the differing-signature case explicitly.

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

#### `scscm-compile`

The fixture does `(load "../turmeric-spices/tur-scscm/src/scscm/...")`,
which requires the `turmeric-spices` repo to be checked out as a
sibling directory. In CI / fresh containers it isn't. Two options:

* gate the fixture behind a `requires.spices` marker (analogous to
  `requires.tsan`) so it auto-skips when the dependency isn't present;
* document a setup step in `CLAUDE.md` / `README.md`.

**Decision:** Add the `requires.spices` skip marker. Mirror the
existing `requires.tsan` plumbing; the runner detects the missing
sibling `../turmeric-spices/` directory and skips. Also add a short
"Optional dependencies" note in `CLAUDE.md` / `README.md` so
developers who want the test to run know how to enable it.

## Files of note touched on this branch

* `stdlib/result.tur` -- pointer-cast warning fix in `result-eq?`,
  `result-collect`, `result-partition`.
* `stdlib/tmutmap.tur` -- pointer-cast warning fix in `tmutmap-new`.
* `tests/fixtures/**/expected.c` -- mass regeneration (~300 files).
* Renamed fixtures in section 3.
* `tests/fixtures/tzipper-basic/input.tur`,
  `tests/fixtures/typed/tzipper-basic/input.tur` -- removed double-free.
