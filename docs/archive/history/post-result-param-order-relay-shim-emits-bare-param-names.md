---
title: Generic relay shims emit bare parameter names as undeclared C calls after #268
category: Reported
severity: critical
description: After commit 804f24ec ("Resolve result-param-order report", #268), `bash tests/run.sh` regresses from clean to ~957/1481 failures. Generated C for `__poly_*` relay shims emits parameter names (e.g. `m1`, `keyeq`, `x`, `container`, `fn_right`, `l1`) as bare function calls that are never declared. Reproduces on trivial fixtures like `tests/fixtures/adt-basic/` whose source has no typeclasses at all.
---

# Generic relay shims emit bare parameter names as undeclared C calls (post-#268)

> **RESOLVED (verified 2026-06-05).** This critical suite-wide regression
> (957 failures) no longer reproduces: the current tree builds clean and the
> full suite reports `1518 passed, 0 failed`. The cited trivial repro
> `tests/fixtures/adt-basic/` builds with no bare-parameter undeclared-C-call
> shims and runs correctly. The `__poly_*` relay-shim codegen was repaired in
> the follow-up to the #268 result-param-order work that originally introduced
> the regression. Archived after empirical verification.

## Summary

The test suite is failing at HEAD (`874824ca`, `main`):

```
$ bash tests/run.sh 2>&1 | grep ^summary
summary: 524 passed, 957 failed   ;; or 420/1061 on a flake-adjacent rerun
```

CLAUDE.md states the expected count is `~1442 passed, 0 failed`. The regression
makes the in-flight plan execution gates (e.g.
`docs/upcoming/still-in-flight-plan.md` "tur-signal spice broken build" Phase 0a)
unmeetable, because the documented validator (`./build/tur check` clean) cannot
distinguish work-product bugs from baseline bugs.

Severity: **critical.** ~64% suite failure on baseline ADT/affine/comonad
fixtures blocks every plan in `docs/upcoming/` whose validation gate is
`tests/run.sh`.

## Observed vs. expected

### Expected (per CLAUDE.md)

> `bash tests/run.sh` runs ~1442 fixtures, expected `summary: 1442 passed, 0 failed`.

### Observed (HEAD)

```
FAIL adt-basic — build failed
FAIL adt-copy — build failed
FAIL adt-nested — build failed
FAIL affine-basic — build failed
FAIL affine-fn-param — build failed
...
FAIL zipper-basic — build failed
FAIL zipper-comonad — build failed
summary: 524 passed, 957 failed
```

`tests/fixtures/adt-basic/input.tur` is a 12-line program with no typeclasses,
no closures, just `(defdata Color (Red)(Green)(Blue))` plus a `match`. It
fails `tur build` because the generated C references undeclared functions:

```
/tmp/tur-build/tests_fixtures_adt-basic_input_tur.c:3086:16:
  error: call to undeclared function 'm1'
/tmp/tur-build/tests_fixtures_adt-basic_input_tur.c:3098:16:
  error: call to undeclared function 'keyeq'
/tmp/tur-build/tests_fixtures_adt-basic_input_tur.c:3102:16:
  error: call to undeclared function 'x'
/tmp/tur-build/tests_fixtures_adt-basic_input_tur.c:3261:16:
  error: call to undeclared function 'container'
/tmp/tur-build/tests_fixtures_adt-basic_input_tur.c:3385:16:
  error: call to undeclared function 'l1'
...
fatal error: too many errors emitted, stopping now
tur: cc invocation failed (status 256)
```

Inspecting the generated C:

```c
static int64_t __poly_558(void * __poly_env_559, int64_t __poly_x0_561) {
        return m1(__poly_x0_561);
}

static int64_t __poly_578(void * __poly_env_579, int64_t __poly_x0_581) {
        return m1(__poly_x0_581);
}

static int64_t __poly_584(void * __poly_env_585, int64_t __poly_x0_587) {
        return keyeq(__poly_x0_587);
}

static int64_t __poly_596(void * __poly_env_597, int64_t __poly_x0_599) {
        return x(__poly_x0_599);
}
```

`m1`, `keyeq`, `x` are the **parameter names** of the stdlib typeclass methods
the relay is forwarding to (e.g. `mutmap-eq?` uses `m1`/`m2`/`val-cmp`,
`MapKey.eq?` uses `keyeq` etc.). They should be dispatched via the typed
closure carrier in the relay's env (`__poly_env_*`), not emitted as bare C
function calls.

A UBSan trip also fires during the same build:

```
src/compiler/elab_call.c:2595:17: runtime error: load of value 2, which is not
a valid value for type 'bool'
```

## Bisect

Range probed:

| Commit | undeclared-function errors on adt-basic |
| --- | --- |
| 8e187b04 fn-first-class F5 | 0 |
| 07bae50f fn-first-class F6 | 0 |
| 4e34c13b method-vs-defn TUR-W0039 | 0 |
| 404a58c2 free defn + user method coexist | 0 |
| **804f24ec result-param-order #268** | **12** |
| 024877e5 generic-from-generic relay (#269) | 12 |
| 874824ca HEAD | 12 |

First-bad-commit: **`804f24ec`** "Resolve result-param-order report:
trailing-parameter instance heads (Result _ B) (#268)".

The commit's own description flags a "latent dispatch-ABI bug fixed alongside"
in `elab_typeclasses.c` that "lowers TY_APP receivers to the int64 carrier
like a parameterized-struct receiver." The relay-shim regression on totally
unrelated fixtures (`adt-basic` has no typeclasses) strongly suggests that
dispatch-ABI change broadened the "lower receiver to int64 carrier" code path
to a case it wasn't supposed to cover, dropping the typed-closure indirection
and leaving the parameter name as a bare identifier in the relay body.

## Minimal repro

```sh
# at turmeric repo root
cmake --build build -j
./build/tur build tests/fixtures/adt-basic/input.tur
# expected: clean build, prints "red"
# observed: 12 undeclared-function errors (m1, keyeq, x, container, l1, ...)
```

To confirm bisect:

```sh
git checkout 404a58c2 -- src && cmake --build build -j
./build/tur build tests/fixtures/adt-basic/input.tur   # clean

git checkout 804f24ec -- src && cmake --build build -j
./build/tur build tests/fixtures/adt-basic/input.tur   # 12 errors
```

## Root-cause analysis (direction)

The undeclared names (`m1`, `keyeq`, `x`, `container`, `l1`, `fn_right`) are
parameter identifiers from stdlib typeclass methods (e.g. `mutmap-eq?` in
`stdlib/mutmap.tur:297`, `MapKey.eq?`, various `Functor`/`Bifunctor` methods).
Relay shims (`__poly_NNN(void *env, int64_t x0)`) are generated by the
codegen path for partial-application / typeclass-method forwarding to wrap a
Turmeric function in the `tur_poly_fn_t` carrier ABI.

In the broken builds, those shims emit:

```c
return m1(__poly_x0_561);   // m1 is the Turmeric param name, never declared
```

The non-regressing shim shape (from the same generator on `404a58c2`) would
have either:
- looked up the bound closure via the env pointer, or
- emitted a properly-prefixed C identifier for the bound symbol.

The "lower TY_APP receiver to the int64 carrier" change in 804f24ec
(`elab_typeclasses.c`) seems the most likely vector: if a partial-app
receiver lowering kicks in for non-Result heads too, the relay shim no
longer knows the receiver type and falls through to a name-only path.

The UBSan hit at `src/compiler/elab_call.c:2595:17` (`load of value 2,
which is not a valid value for type 'bool'`) is observable on the *prior*
commit (`024877e5`) too, so it's an older latent issue, not the cause of
this regression — but it's worth investigating in the same session since
both bugs sit on the call-elaboration path.

## Proposed fix direction

1. Revert or narrow the 804f24ec receiver-lowering change. The commit message
   says it was scoped to "partial-application head", but the failures span
   non-partial-app, non-typeclass fixtures, so the gate is leaky.
2. Add a fixture identical to `tests/fixtures/adt-basic/` that asserts the
   simple `defdata` + `match` path stays clean — this regression would have
   been caught instantly.
3. Run `bash tests/run.sh` before tagging any commit in
   `src/compiler/elab_typeclasses.c` or `src/compiler/elab_call.c` as
   merge-ready; the per-plan fixture coverage is clearly insufficient to
   catch broad codegen regressions on this path.

## Validation of a fix

A fix is good if:

1. `bash tests/run.sh 2>&1 | grep ^summary` reports `1442 passed, 0 failed`
   (or whatever the current fixture count is — back to 0 failed).
2. The new `Result _ B` instance-head fixture from 804f24ec stays green.
3. The generated C for `tests/fixtures/adt-basic/` contains no bare
   parameter-name function calls in `__poly_*` shims.
