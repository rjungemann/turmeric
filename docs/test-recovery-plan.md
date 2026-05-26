# Test Recovery Plan

**Date:** 2026-05-26  
**Status:** In progress

## What happened

Commit `3e4258c8` ("Drop typed prefix plan") introduced a systematic
find-and-replace corruption across 600+ files:
- `ter` → `err?` (broke `extern-c`, `register`, `intersect`, `filter`, …)
- `to`  → `ok?`  (broke `auto-spice`, `store`, `directory`, …)

It also added a batch of new fixture files with the wrong API (mixing untyped
constructors `some`/`ok`/`err` with typed accessors `unwrap`/`ok-val`, etc.).

## Current state (2026-05-26)

**Done:**
- 721 pre-existing corrupted files restored from `068abead` via bulk `git checkout`
- `run.sh`, `run-turi.sh`, `spice-resolver-tests.sh` corrected
- Newly-created corrupted fixtures manually patched: `option-basic`,
  `typed/option-basic`, `typed/result-basic`, `typed/slice-basic`,
  `typed/zipper-basic`, `typed/set-basic`, `set-typed-basic`, `slice-basic`,
  `zipper-basic`, `set-basic`, `result-typed-basic`

**Still failing (12 fixtures):**

| Fixture | Error | Category |
|---------|-------|----------|
| `option-basic` | `unwrap` expects typed Option, got int | API-mismatch |
| `result-typed-basic` | same pattern for ok/err | API-mismatch |
| `zipper-basic` | same (zipper-move-right result) | API-mismatch |
| `set-basic` | same (set-intersect) | API-mismatch |
| `toption-basic` | `tsome__NNN` undeclared in codegen | compiler-bug |
| `tset-basic` | same pattern | compiler-bug |
| `tresult-of-typed-eq` | same pattern | compiler-bug |
| `toption-of-tvec-eq` | same pattern | compiler-bug |
| `adt-param-tyvar` | type var in defdata field position | TP1-missing |
| `adt-param-match-type` | same | TP1-missing |
| `kind-inference-adt` | same | TP1-missing |
| `weak-dangling` | `some?` conflicts with stdlib name | pre-existing |
| `ptc4-basic` | inline-C not supported in interp | pre-existing |

## Steps (in order)

### Step 0 — Complete drop-typed-prefix Phase 3 (fixture rename)

The stdlib t-prefix rename already happened (tmap.tur → map.tur etc.) but the
fixture tests were never updated. They call deleted functions and fail to build.

**0a — Audit:** for each `tests/fixtures/t<X>-*` directory, decide:
- **Delete** if a non-t equivalent already exists covering the same ground
- **Rename+update** if it tests unique functionality not covered elsewhere

T-prefix fixtures to triage (21 total):
`tcons-of-tcons-eq`, `tgrid-basic`, `tmap-basic`, `tmap-eq`, `tmap-of-tvec-eq`,
`tmutmap-basic`, `tmutmap-delete`, `tmutmap-eq`, `tmutmap-resize`,
`toption-basic`, `toption-of-tvec-eq`, `tpair-basic`, `tpair-of-typed-eq`,
`tresult-of-typed-eq`, `tset-basic`, `tset-of-tvec-eq`, `tvec-basic`,
`tvec-eq-ascribed`, `tvec-eq-ascribed-multi`, `tvec-of-tvec-eq`,
`tvec-of-tvec-eq-manual`

**0b — Execute:** delete superseded; rename remaining dirs and rewrite
`input.tur` call sites (`tvec-push!` → `vec-push!`, `tmap-new` → `map-new`,
`tsome` → `some`, etc.)

**0c — Verify `src/main.c` autoload list** has no stale `t*.tur` filenames.

### Step 1 — Diagnose API-mismatch fixtures
Files: `option-basic`, `result-typed-basic`, `zipper-basic`, `set-basic`

These fixtures were *created* in the corruption commit. Check:
1. What API they were intending to test (untyped `u-some`/`u-none` or the new
   typed `some`/`ok`/`err` API)?
2. Does the stdlib actually export an untyped `unwrap` that accepts `int`?
3. Fix fixture to use the correct constructor+accessor pair, or update
   `expected.stdout` if the API has legitimately changed.

Quick check: `grep -n "^(defn some\|^(defn unwrap" stdlib/option.tur`

### Step 2 — Diagnose `tsome__NNN` codegen bug
Files: `toption-basic`, `tset-basic`, `tresult-of-typed-eq`, `toption-of-tvec-eq`

The compiler emits `tsome__540` (double-underscore = curried form?) but never
defines it. Check:
- Is `tsome` defined as a single-arity or two-arity function in stdlib?
- Is the callsite trying to curry `tsome` when it shouldn't?
- Likely a recent change in `emit_expr.c` or `emit_module.c` broke the
  name-mangling for `t`-prefixed ADT constructors.

Quick check: `git log --oneline -5 -- src/compiler/emit_expr.c src/compiler/emit_module.c`

### Step 3 — TP1: type variables in defdata fields
Files: `adt-param-tyvar`, `adt-param-match-type`, `kind-inference-adt`

Implement per `docs/adt-type-params-plan.md` Phase TP1:
- `src/compiler/types.h`: add `Type *full_type` to `CtorField`
- `src/compiler/elab_structs.c` `elab_defdata` field-type loop (~line 1107):
  before the "not a keyword" error, check if `ft_form->tag == F_SYM` and name
  matches a declared type param; if so use `TY_INT` carrier and store
  `type_tyvar_named(name)` as `full_type`

### Step 4 — Pre-existing issues
- `weak-dangling`: fixture defines `some?` which now conflicts with the stdlib
  name. Either rename the fixture's local def or add `(defn weak-some? ...)`.
- `ptc4-basic`: uses inline-C in interp path; add `requires.compiled` marker.

### Step 5 — Verify
Run: `ctest --test-dir build -R "turi_fixture_tests|tur_fixture_tests|tur_spice_resolver_tests"`

Target: all 3 suites green.

## What to NOT do
- Do not run the full `bash tests/run.sh` without a `TUR_TEST_FILTER` — it takes
  too long and produces too much noise. Run targeted subsets.
- Do not touch stamp cache files.
- Do not modify files in `tests/.stamp-cache*/`.
