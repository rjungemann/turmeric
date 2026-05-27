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
**Status:** Fixed by TP6 — scrutinee TY_APP unwrapping now propagates concrete type args to match-arm bindings

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

---

## KB-004 — `::` coercion cannot bridge `:int`-returning stdlib functions to typed accessors

**Discovered:** 2026-05-27  
**Status:** Open (architectural limitation)

### Symptom

`result-map`, `option-map`, and similar stdlib functions return `:int` (an
opaque heap-pointer representation).  When the result is passed to a typed
accessor (`ok-val`, `err-val`, `unwrap`) via a `(:: expr (Result A B))` type
annotation, the compiler emits a specialisation of the accessor that takes the
struct by value — but `result-map` still returns `int64_t`.  This produces a
C-level type error at link time:

```
error: passing 'int64_t' to parameter of incompatible type 'Result__int__int'
```

### Root cause

The `::` coercion changes the *callsite type* used for overload resolution but
does not insert a pointer-dereference cast.  The typed specialisation of
`ok-val` (`ok_val__spec__int64_t_Result__int__int`) expects its argument
passed by value as a struct, while the heap-pointer representation stores it
as an `int64_t` (cast from a `void *`).  The two conventions are incompatible.

### Workaround

For direct `ok`/`err`/`some`/`none` calls, use `make-struct` to construct the
typed struct directly instead of going through the untyped constructor:

```turmeric
;; BROKEN: (ok-val (ok 42))
;; WORKS:
(ok-val (make-struct Result true 42 0))
```

For functions that return `:int` (e.g. `result-map`, `option-map`), use the
equality-based API (`result-eq?`, `option-eq?`) to test values instead of
trying to extract through `ok-val`/`err-val`.

### Fix

Needs a dedicated cast/coerce form that understands the pointer-as-int
representation and emits the correct `*(ResultType *)(intptr_t)r` dereference.
Track as a future compiler improvement (post-TP6).


---

## KB-005 -- `emit_expr.c` ADT match crashes when scrutinee type is `TY_APP`

**Discovered:** 2026-05-26
**Status:** Fixed in TP6 (commit accompanying this note)

### Symptom

A `match` expression whose scrutinee has a `TY_APP`-wrapped ADT type (produced
by an explicit type annotation like `:(Opt2 int)` on a function parameter)
crashes at code generation with a SEGV inside `mangle_field_name`:

```
AddressSanitizer: SEGV ... in _platform_strlen
  emit_core.c:410 mangle_field_name
  emit_expr.c:3164 emit_value      <- was: AdtDef *adt = scrutinee->type.as.adt_.def
  emit_fns.c:220  emit_fn_def
```

### Root cause

`emit_expr.c` (ADT match fallthrough branch) did:

```c
AdtDef *adt = e->as.match_.scrutinee->type.as.adt_.def;
```

When `scrutinee->type.kind == TY_APP`, `.as.adt_.def` reads the wrong union
member (the TY_APP's `.fn`/`.arg` pointers), yielding a garbage `AdtDef *`
whose `name` field is NULL.  `mangle_field_name` then crashes in `strlen`.

The elab side (`elab_structs.c`) already handled TY_APP scrutinees correctly
(TP6 work); the emit side had not been updated to match.

### Fix

Unwrap the TY_APP chain before accessing `.as.adt_.def`:

```c
const Type *base = &e->as.match_.scrutinee->type;
while (base && base->kind == TY_APP && base->as.app.fn)
    base = base->as.app.fn;
AdtDef *adt = (base && base->kind == TY_ADT) ? base->as.adt_.def
                                              : e->as.match_.scrutinee->type.as.adt_.def;
```

### Affected file

`src/compiler/emit_expr.c` (line ~3161 before fix).

---

## KB-006 — 48 `expected.c` codegen snapshots were stale

**Status**: Fixed (snapshots regenerated).

### Symptom

48 fixture tests reported "codegen mismatch" — the test programs ran correctly
and produced the right stdout, but the `expected.c` codegen snapshot differed
from the current emit-c output.

### Root cause

The codegen for stdlib structs and various features evolved (e.g. `void *`
→ `int64_t` for some parameterized fields, changed function/helper names) but
the snapshot files were never regenerated.  This is a maintenance issue, not a
compiler defect.

### Fix

Regenerated `expected.c` for all 48 affected fixtures by running
`./build/tur emit-c <input.tur>` after verifying runtime output was still
correct.  Reduced unique test failures from ~142 to ~95.

---

## KB-007 — `derive-show-struct` / `derive-show-nested` fixtures use wrong return cast in inline C

**Status**: Open — fixture bug.

### Symptom

```
error: incompatible integer to pointer conversion returning 'int64_t' from a function with result type 'const char *'
```

Both `tests/fixtures/derive-show-struct/input.tur` and
`tests/fixtures/derive-show-nested/input.tur` define `str-concat` with the
inline C body ending in:

```c
return (int64_t)(intptr_t)out;
```

but the declared return type is `:cstr` (`const char *`), so the compiler now
rejects the cast as an incompatible integer-to-pointer conversion.

### Fix needed

Change the final `return` line in the `str-concat` inline C block in both
fixtures to:

```c
return (const char *)(intptr_t)out;
```

---

## KB-008 — `defn-spaced-compound`: kind mismatch on `(-> int int)` type annotation

**Status**: Open — compiler bug.

### Symptom

```
error [TUR-E0012]: kind mismatch: cannot apply a type of kind '*' as a type constructor;
type must have kind '* -> *' or '* -> * -> *'
```

on `(defn apply1 [f : (-> int int)] :int ...)`.

### Root cause

When a function type is written in spaced annotation form (`f : (-> int int)`),
the `->` symbol is resolved to a concrete `TY_FN` type of kind `*` rather than
being treated as a type constructor of kind `* -> * -> *`.  The non-spaced form
(`f :(-> int int)`) may or may not have the same issue.

### Fix needed

The type-annotation elaborator needs to recognise `->` in spaced annotation
position as a type constructor and expand it to a `TY_FN` application, matching
the behaviour of the `:` shorthand syntax.

---

## KB-009 — `result-question-op`: `?` operator lowering returns `:int` where `:bool` expected

**Status**: Open — compiler bug.

### Symptom

```
error: if condition must be bool, got int
```

inside `(unsafe (? (get-value b)))` — the `?` operator is lowered to a call
that returns `:int` (via `err?`), but the surrounding `if` expects `:bool`.

### Fix needed

The `?` macro expansion (or the `err?` stdlib function) should return `:bool`.
Check whether `err?` is declared `:bool` in `stdlib/result.tur` and whether the
lowering pass is picking up the correct overload.

---

## KB-010 — `vec-eq-ascribed`: `vec_new()` emits `int64_t` result, incompatible with struct type

**Status**: Open — codegen bug.

### Symptom

```
error: initializing 'Vec__int' (aka 'struct Vec__int') with an expression of incompatible type 'int64_t'
  Vec__int a_532 = vec_new();
```

### Root cause

`vec_new()` is a stdlib function that returns an opaque `int64_t` handle.  The
codegen emits `Vec__int a = vec_new()` when the declared type is `:(Vec int)`,
creating a C type mismatch.  The initialisation needs either a cast or the
emitter needs to know `Vec` fields are struct-typed, not handle-typed.

### Fix needed

Investigate whether the code path that monomorphises `Vec[A]` fields is
emitting the right C struct name vs. treating Vec as an opaque handle.

