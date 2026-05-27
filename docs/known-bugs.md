# Known Bugs and Footguns

A running log of subtle bugs, footguns, and surprising behaviour discovered
during development.  Each entry describes the symptom, root cause, workaround,
and fix status.

Bugs that have been fully fixed are summarised in the
[Fixed Issues](#fixed-issues) section at the bottom; consult `git log` for
the implementation details.

---

## KB-001 — `run-turi.sh` silently ignored fixtures missing from the allowlist

**Discovered:** 2026-05-26
**Status:** Fixed 2026-05-27 — `run_turi_fixture` now emits
`SKIP <name> (not in turi allowlist)` and the summary line surfaces an
"allowlist gap" count so missing fixtures are visible.

### Symptom (before fix)

A newly created fixture directory under `tests/fixtures/` passed when run
directly (`./build/tur run tests/fixtures/<name>/input.tur`) but was never
mentioned in the `turi_fixture_tests` CTest run — no PASS, no FAIL, no SKIP.
The fixture was effectively invisible to CI.

### Root cause

`tests/run-turi.sh` maintains an explicit opt-in allowlist
(`TURI_FIXTURES_DEFAULT`).  Before the fix, `run_turi_fixture()` silently
`return`ed without printing anything when a fixture was absent from this list.
There was no "SKIP (not in allowlist)" message, so the omission was invisible
in test output.

### Fix

`run_turi_fixture()` now writes a `SKIP_ALLOWLIST` result file and prints a
visible `SKIP <name> (not in turi allowlist)` line.  The tally loop
aggregates them into an "allowlist gap" count emitted alongside the run
summary.  Auto-inclusion via `requires.compiled` markers (suggested as the
alternative fix) remains a future option; the visibility change is enough
to catch regressions today.

---

## KB-002 — `TUR_TEST_FILTER` did not filter `turi_fixture_tests`

**Discovered:** 2026-05-26
**Status:** Fixed 2026-05-27 — `tests/run-turi.sh` now accepts
`TUR_TEST_FILTER` as an alias for `TURI_FILTER`.

### Symptom (before fix)

Running:

```sh
TUR_TEST_FILTER="adt-param-tyvar" ctest --test-dir build -R turi_fixture_tests
```

ran the full turi fixture suite, ignoring the filter.

### Root cause

`tests/run-turi.sh` read `TURI_FILTER` for its per-fixture grep pattern.
`TUR_TEST_FILTER` was unrelated and not forwarded by the CMake test
definition for `turi_fixture_tests`.  The two env-var names were easy to
confuse.

### Fix

The default-resolution for `TURI_FILTER` is now
`"${TURI_FILTER:-${TUR_TEST_FILTER:-}}"`, so either name works.
`TURI_FILTER` still wins when both are set, preserving the existing
explicit-override behaviour.

---

## KB-004 — `::` coercion cannot bridge `:int`-returning stdlib functions to typed accessors

**Discovered:** 2026-05-27
**Status:** Open (architectural limitation) — tracked in
[docs/aggregate-carrier-abi-plan.md](aggregate-carrier-abi-plan.md).

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
See [docs/aggregate-carrier-abi-plan.md](aggregate-carrier-abi-plan.md) (Cluster A)
for the unified plan that covers this and the related KB-010, KB-012, KB-015.

---

## KB-007 — `derive-show-struct` / `derive-show-nested` fixtures used wrong return cast in inline C

**Status:** Fixed 2026-05-27 — both fixtures now return
`(const char *)(intptr_t)out` from the `str-concat` inline-C body.

### Symptom (before fix)

```
error: incompatible integer to pointer conversion returning 'int64_t' from a function with result type 'const char *'
```

Both `tests/fixtures/derive-show-struct/input.tur` and
`tests/fixtures/derive-show-nested/input.tur` defined `str-concat` with the
inline C body ending in:

```c
return (int64_t)(intptr_t)out;
```

but the declared return type was `:cstr` (`const char *`), so the compiler
rejected the cast as an incompatible integer-to-pointer conversion.

### Fix

Changed the final `return` line in the `str-concat` inline C block in both
fixtures to:

```c
return (const char *)(intptr_t)out;
```

---

## KB-008 — `defn-spaced-compound`: kind mismatch on `(-> int int)` type annotation

**Status:** Open — compiler bug; tracked in
[docs/function-type-kind-plan.md](function-type-kind-plan.md).

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
the behaviour of the `:` shorthand syntax.  See
[docs/function-type-kind-plan.md](function-type-kind-plan.md) for the phased fix.

---

## KB-009 — `result-question-op`: `?` operator lowering returns `:int` where `:bool` expected

**Status:** Open — compiler/stdlib bug; tracked in
[docs/result-question-bool-plan.md](result-question-bool-plan.md).

### Symptom

```
error: if condition must be bool, got int
```

inside `(unsafe (? (get-value b)))` — the `?` operator is lowered to a call
that returns `:int` (via `err?`), but the surrounding `if` expects `:bool`.

### Fix needed

The `?` macro expansion (or the `err?` stdlib function) should return `:bool`.
Check whether `err?` is declared `:bool` in `stdlib/result.tur` and whether the
lowering pass is picking up the correct overload.  See
[docs/result-question-bool-plan.md](result-question-bool-plan.md) for the
phased fix.

---

## KB-010 — `vec-eq-ascribed`: `vec_new()` emits `int64_t` result, incompatible with struct type

**Status:** Open — codegen bug; tracked in
[docs/aggregate-carrier-abi-plan.md](aggregate-carrier-abi-plan.md) (Cluster B).

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
Covered as Cluster B in
[docs/aggregate-carrier-abi-plan.md](aggregate-carrier-abi-plan.md); the
unified `emit_carrier_bridge` helper proposed there inserts the right cast.

---

## KB-011 — `stdlib/arrow.tur` does not load: typeclass methods called as free functions

**Discovered:** 2026-05-27
**Status:** Open — stdlib bug; tracked in
[docs/stdlib-arrow-typeclass-plan.md](stdlib-arrow-typeclass-plan.md).

### Symptom

```
$ ./build/tur check stdlib/arrow.tur
stdlib/arrow.tur:325:3: error: 'arr' is not a function or continuation
323 | ;;; Example:
324 | ;;;   (arrow-id)  ; => arr (fn [x] x)
325 | (defn arrow-id [^arr a]
326 |   (arr (fn [x] x)))
    |   ^^^^^^^^^^^^^^^^
```

The file fails on load.  No test exercises `arrow.tur` directly (the
`stdlib-arrow` fixture inlines its own helpers), so the breakage has
gone unnoticed.

### Root cause

`Arrow` is declared as a typeclass with method names `arr`, `>>>`,
`first`, `second` (`stdlib/arrow.tur:33`).  Two helpers later in the
same file call those names as if they were free functions:

```turmeric
(defn arrow-id [^arr a]
  (arr (fn [x] x)))            ; arr is an Arrow method

(defn arrow-comp [^arr a b c f g]
  (>>> f g))                   ; >>> is an Arrow method
```

These calls bypass typeclass dispatch.  The elaborator looks up `arr`
in the value scope, finds only the typeclass method (not a free
function), and rejects the call with "is not a function or
continuation".

### Workaround

`stdlib/arrow.tur` is not auto-loaded by `src/main.c`, so this bug
doesn't break compiled programs unless they explicitly
`(load "stdlib/arrow.tur")`.  Tests that need Arrow combinators
inline the implementations directly — see
`tests/fixtures/stdlib-arrow/input.tur` as the canonical pattern.

### Fix needed

See [docs/stdlib-arrow-typeclass-plan.md](stdlib-arrow-typeclass-plan.md)
for the options under consideration (the leading recommendation is to drop
the broken helpers, since typeclass dispatch needs a receiver they don't
have).

---

## KB-012 — `Eq` instance on parametric `defstruct` segfaults on call

**Discovered:** 2026-05-27
**Status:** Open — compiler bug; tracked in
[docs/aggregate-carrier-abi-plan.md](aggregate-carrier-abi-plan.md) (Cluster C).

### Symptom

Calling `.eq?` on two values of a parametric `defstruct` (e.g.
`Pair[int int]`, `Tuple2[int int]`, `Option[int]`) crashes:

```turmeric
(let [t1 (tuple2 10 20)
      t2 (tuple2 10 20)]
  (.eq? t1 t2))            ; => Segmentation fault
```

A trivial instance body works:

```turmeric
(definstance Eq [Foo] [(Eq A)]
  (eq? [x y] true))        ; ok
```

A body that touches `x` or `y` does not:

```turmeric
(definstance Eq [Foo] [(Eq A)]
  (eq? [x y] (= (.e1 x) (.e1 y))))    ; SEGV
```

Affects all stdlib instances that follow this pattern: `Eq [Pair]`
(`stdlib/pair.tur:113`), `Eq [Option]` (`stdlib/option.tur:173`),
and `Eq [Tuple2]` (`stdlib/tuple.tur`, added Phase TP2 at parity
with the existing broken instances).

### Root cause

Unverified.  Likely candidates:

- The instance specialisation passes `x` / `y` with the wrong
  calling convention (by-value struct vs. int64 carrier), and the
  body dereferences a value that isn't a pointer.
- The `.e1 x` field-access lowering inside an instance body doesn't
  see the correct struct type for `x` — the parameter binding
  type may be unresolved (`TY_UNKNOWN` / `TY_TYVAR`) at codegen
  time, producing a wrong field-offset calculation.

The existing stdlib helpers (`pair-eq-carrier?`,
`tuple2-eq-carrier?`, `option-eq?`) all use an inline-C
"cast int64_t to struct*" carrier ABI to sidestep field access
inside the body, but they then crash for the *opposite* reason:
called with a by-value struct rather than the heap pointer they
expect to dereference.

### Workaround

Use a call-site macro that expands to direct field access at the
caller's lexical scope, where `x` / `y` have known concrete types:

- `pair-eq?` (`stdlib/pair.tur:93`)
- `tuple2-eq?` (`stdlib/tuple.tur`, Phase TP2)

```turmeric
;; Works:
(tuple2-eq? t1 t2 (fn [a b] (= a b)) (fn [a b] (= a b)))

;; Crashes:
(.eq? t1 t2)
```

### Fix needed

See [docs/aggregate-carrier-abi-plan.md](aggregate-carrier-abi-plan.md)
(Cluster C and Phase 4) for the unified plan: pick a single calling
convention for parametric typeclass instance bodies, audit existing
stdlib instances against it, and route call sites through the
`emit_carrier_bridge` helper.

---

## KB-015 — Repeated calls to a specialized typed accessor emit a spurious reinterpret

**Discovered:** 2026-05-26 (TS3.4 audit)
**Status:** Open — pre-existing bug; tracked in
[docs/aggregate-carrier-abi-plan.md](aggregate-carrier-abi-plan.md) (Cluster D).
Exposed by typed accessor composition, not a TS3.3 regression
(confirmed by stashing the TS3.3 fix and reproducing).

### Symptom

Calling a generic-but-specializable typed accessor like `thead`
(`[A] [l :(Cons A)] :A`) twice in the same function, on different bindings
of the same instantiated type, emits a spurious bit-reinterpret around
the *second* call — producing garbage:

```turmeric
(defn main [] :int
  (let [xs (tcons-of 1.5 0)
        ys (tcons-of 2.5 0)]
    (println (thead xs))   ; => 1.5  (correct)
    (println (thead ys))   ; => 9.88131e-324  (denormal garbage)
    0))
```

The first call emits a clean specialized call:

```c
printf("%g\n", (double)(thead__spec__double_Cons__float(xs_601)));
```

The second call inexplicably wraps the same specialized call in a
`int64_t <-> double` union reinterpret, which truncates the returned
`double` through an `int64_t` slot before reading it back as `double`:

```c
printf("%g\n",
       (double)(((union { int64_t s; double d; })
                 {.s = thead__spec__double_Cons__float(ys_602)}).d));
```

`thead__spec__double_Cons__float` already returns `double`, so the
extra reinterpret is wrong.

### Reproducer

`/tmp/test-thead2.tur` (also reproducible in the original first cut of
`tests/fixtures/typed-slots/cons-float/`):

```turmeric
(defn main [] :int
  (let [xs (tcons-of 1.5 0)
        ys (tcons-of 2.5 0)]
    (println (thead xs))
    (println (thead ys))
    0))
```

Calling `.head` directly avoids the bug; the bug is specifically about
the generic-helper specialization path.

### Likely cause

Specialization cache state in `elab_call.c` / the
`call_wrap_reinterpret` family appears to be sticky across repeated
calls to the same specialized helper.  The first call gets a clean
result type; the second call sees the result type already-converted
state and re-wraps as if the source were the carrier `int64_t`.

This pre-dates the TS3.3 work (`elab_ascribe` -> `EX_REINTERPRET`):
stashing the TS3.3 change still reproduces the same wrong codegen for
the second call.

### Workaround

Use direct field access (`(.head xs)`) instead of the helper, or call
the helper only once per scope.  The current TS3.4 fixtures
(`cons-float`, `option-float`) use `.head` / `.value` directly to
sidestep the bug.

### Fix sketch

See [docs/aggregate-carrier-abi-plan.md](aggregate-carrier-abi-plan.md)
(Cluster D / Phase 5) for the planned narrow fix:

1. Audit the specialization-call cache and result-type threading in
   `src/compiler/elab_call.c` — specifically the post-call
   `call_wrap_reinterpret(...)` site (around line 1687).  Is the
   `result_type.kind` derived from the *binding* type (still showing
   the carrier `int64_t`) instead of the *instantiated* type
   (`double`)?
2. Add a focused regression that calls a typed `[A]` helper twice on
   two bindings of the same instantiation and asserts both results.

---

## Fixed Issues

Brief log of previously-tracked bugs that have been fully resolved.  Refer
to the git history (and the diffs of the commits that mention each KB
identifier) for implementation detail.

- **KB-003** — GADT match arm: `TY_TYVAR` field binding caused overload-resolution failure.  Fixed in TP6: scrutinee `TY_APP` unwrapping now propagates concrete type args to match-arm bindings.
- **KB-005** — `emit_expr.c` ADT match crashed when scrutinee type was `TY_APP`.  Fixed in TP6: the emit side now unwraps the `TY_APP` chain before reading `.as.adt_.def`, matching the elab side.
- **KB-006** — 48 `expected.c` codegen snapshots were stale.  Fixed by regenerating snapshots with `./build/tur emit-c` after verifying runtime output.  Reduced unique test failures from ~142 to ~95.
- **KB-013** — Sized numeric type arguments (`i32`, `f32`, …) collapsed to generic kind in struct-app mangling.  Fixed 2026-05-26: added the short-form sized type names to `typekind_from_symbol` and `typekind_from_name`.
- **KB-014** — Aggregate struct-app let-bindings used the `int64_t` carrier instead of the concrete C struct type.  Fixed 2026-05-26 as a downstream effect of the KB-013 fix; once the struct-app argument resolves correctly, the let-binding picks up the concrete C type.
