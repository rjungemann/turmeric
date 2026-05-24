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

**Note:** Snapshot regeneration is mechanical and will need to be redone
whenever the runtime preamble changes. Worth a thought to either:

* add a `tools/update-snapshots.py` helper, or
* drop expected.c snapshots entirely for fixtures whose value is the
  runtime behavior and not the literal C output. The runtime preamble
  is shared across all fixtures, so 290 copies of the same bytes do not
  buy us much.

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

Leaving the fixture failing as a forcing function for someone to make a
deliberate choice on which direction to go.

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

#### Missing Feature 5 -- `tzipper-new` ownership contract

(Aside, not a failing test now -- folded into the docstring of
`stdlib/tzipper.tur`.) The current API is that `tzipper-new` takes
ownership of the left/right arrays and `tzipper-free` frees them. This
is surprising for an stdlib container -- typically containers either
borrow or document ownership transfer prominently. Worth a follow-up to
either document better or change the contract to "borrow, caller
frees."

### External dependency missing (left failing intentionally)

#### `scscm-compile`

The fixture does `(load "../turmeric-spices/tur-scscm/src/scscm/...")`,
which requires the `turmeric-spices` repo to be checked out as a
sibling directory. In CI / fresh containers it isn't. Two options:

* gate the fixture behind a `requires.spices` marker (analogous to
  `requires.tsan`) so it auto-skips when the dependency isn't present;
* document a setup step in `CLAUDE.md` / `README.md`.

Test left failing pending a decision.

## Files of note touched on this branch

* `stdlib/result.tur` -- pointer-cast warning fix in `result-eq?`,
  `result-collect`, `result-partition`.
* `stdlib/tmutmap.tur` -- pointer-cast warning fix in `tmutmap-new`.
* `tests/fixtures/**/expected.c` -- mass regeneration (~300 files).
* Renamed fixtures in section 3.
* `tests/fixtures/tzipper-basic/input.tur`,
  `tests/fixtures/typed/tzipper-basic/input.tur` -- removed double-free.
