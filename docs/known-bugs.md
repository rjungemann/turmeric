# Known Bugs and Footguns

A running log of subtle bugs, footguns, and surprising behaviour discovered
during development.  Each entry describes the symptom, root cause, workaround,
and fix status.

---

## KB-001 — `run-turi.sh` silently ignores new fixtures

**Discovered:** 2026-05-26  
**Status:** Open (partially mitigated — see Workaround)

### Symptom

A newly created fixture directory under `tests/fixtures/` passes when run
directly (`./build/tur run tests/fixtures/<name>/input.tur`) but is never
mentioned in the `turi_fixture_tests` CTest run — no PASS, no FAIL, no SKIP.
The fixture is effectively invisible to CI.

### Root cause

`tests/run-turi.sh` maintains an explicit opt-in allowlist
(`TURI_FIXTURES_DEFAULT`, lines 71–183).  `run_turi_fixture()` silently
`return`s without printing anything when a fixture is absent from this list.
There is no "SKIP (not in allowlist)" message, so the omission is invisible in
test output.

### Affected fixtures (as of 2026-05-26)

The three TP1 fixtures added in the adt-type-params work were not in the list:

- `tests/fixtures/adt-param-tyvar/`
- `tests/fixtures/adt-param-match-type/`
- `tests/fixtures/kind-inference-adt/`

### Workaround

Manually add the fixture name (one per line) to `TURI_FIXTURES_DEFAULT` in
`tests/run-turi.sh`.  The fix is trivially a one-liner per fixture.

### Suggested fix

Either:
- Emit `SKIP <name> (not in turi allowlist)` so gaps are visible, or
- Auto-include all fixtures that lack a `requires.compiled` marker and let
  explicit `requires.compiled` / `requires.dedicated-runner` opt out, matching
  the pattern used by `tests/run.sh`.

---

## KB-003 — GADT match arm: `TY_TYVAR` field binding causes overload-resolution failure

**Discovered:** 2026-05-26  
**Status:** Open (by design until TP6)

### Symptom

A `defgadt` constructor whose field type is a bare type variable (e.g. `a` in
`(MkBox a : (Box a))`) compiles fine.  But extracting the field in a match arm
and passing it to any concrete function (e.g. `println`) fails with:

```
error [TUR-E0006]: operator lookup failed for 'println': got 1 arg(s), first arg type tyvar
```

### Root cause

`gadt_build_skolem_env` only resolves type parameters that appear as *primitive*
concrete types in the return-type annotation (e.g. `(Box int)` → `a = TY_INT`).
When the return type still contains a type variable (e.g. `(Box a)`), the
skolem env is empty, so `gadt_resolve_type_from_form` returns an anonymous
`TY_TYVAR(NULL)` for the bound variable.  That type has no concrete overloads.

### Workaround

Avoid using the polymorphic field value in type-specific operations within the
match arm.  Either:
- Leave the binding unused (discard pattern `_x`), or
- Use a concrete GADT instantiation where the return type pins the type param
  (e.g. `(MkBox2Int :int : (Box2 int))`).

### Fix

Phase TP6 (`adt-type-params-plan.md`) — "match-arm binding types reflect type
parameters" — will apply the scrutinee's type arguments to the field's
`TY_TYVAR full_type`, producing the concrete binding type.

---

## KB-002 — `TUR_TEST_FILTER` does not filter `turi_fixture_tests`

**Discovered:** 2026-05-26  
**Status:** Open

### Symptom

Running:

```sh
TUR_TEST_FILTER="adt-param-tyvar" ctest --test-dir build -R turi_fixture_tests
```

runs the full turi fixture suite, ignoring the filter.

### Root cause

`tests/run-turi.sh` reads `TURI_FILTER` for its per-fixture grep pattern.
`TUR_TEST_FILTER` is unrelated and not forwarded by the CMake test definition
for `turi_fixture_tests`.  The two env-var names are easy to confuse.

### Workaround

Use `TURI_FILTER` when calling `run-turi.sh` directly:

```sh
TURI_FILTER="adt-param-tyvar" bash tests/run-turi.sh
```

Or set `TURI_FILTER` in the environment before `ctest` if the CMake test
definition passes it through.

### Suggested fix

Rename `TURI_FILTER` to `TUR_TEST_FILTER` in `run-turi.sh`, or add a
compatibility shim that copies `TUR_TEST_FILTER` → `TURI_FILTER` at the top
of the script, so both names work and behave consistently with `run.sh`.
