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

---

## KB-011 -- `stdlib/arrow.tur` does not load: typeclass methods called as free functions

**Discovered:** 2026-05-27
**Status:** Open -- stdlib bug.

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

A separate issue in the same file -- `(Pair a b)` being called as a
constructor function -- was fixed by the Pair -> Tuple2 sweep in
Phase TP2; the `arr` / `>>>` problem is the remaining blocker.

### Workaround

`stdlib/arrow.tur` is not auto-loaded by `src/main.c`, so this bug
doesn't break compiled programs unless they explicitly
`(load "stdlib/arrow.tur")`.  Tests that need Arrow combinators
inline the implementations directly -- see
`tests/fixtures/stdlib-arrow/input.tur` as the canonical pattern.

### Fix needed

Either:

- Invoke the methods via explicit dispatch (`(.arr arr (fn [x] x))`,
  `(.>>> arr f g)`) -- requires an Arrow-typed receiver in scope,
  which `arrow-id`/`arrow-comp` don't have, or
- Remove the typeclass-method calls from the free-function helpers
  and route them through the inline-C `__arrow_call*` helpers
  already in the file (the Arrow [->] instance methods already do
  this internally), or
- Rename the typeclass methods (`Arrow.arr` -> `Arrow.lift`,
  `Arrow.>>>` -> `Arrow.then`) so the free-function helpers
  don't shadow them.

---

## KB-012 -- `Eq` instance on parametric `defstruct` segfaults on call

**Discovered:** 2026-05-27
**Status:** Open -- compiler bug.

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
  see the correct struct type for `x` -- the parameter binding
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

Decide on one calling convention for typeclass instance bodies on
parametric defstructs (by-value or pointer-carrier), make the
codegen consistent, and audit the existing stdlib instances
(`Eq [Pair]`, `Eq [Option]`, `Eq [Tuple2]`) against the chosen
convention.

Also worth confirming whether `.e1 x` inside the body sees a
fully-resolved `Tuple2[int int]` for `x` or some unresolved
`Tuple2[A B]` -- if the binding type is unresolved at codegen
time, field offsets will be wrong even after the calling
convention is fixed.

---

## KB-013 -- Sized numeric type arguments collapse to generic kind in struct-app mangling

**Discovered:** 2026-05-26 (TS3.2 audit)
**Status:** Fixed 2026-05-26 -- root cause was unrecognised short-form sized
type names (`i32`, `f32`, ...).  Mangling, layout, and the let-binding carrier
(KB-014) all flow correctly once the type resolves.

### Symptom (before fix)

A parameterised struct instantiated at a *sized* numeric type lost precision
in both the mangled C name and the emitted field type.  `(Box i32)` produced
`Box__int { int64_t x; }` instead of `Box__int32 { int32_t x; }`.

### Root cause

`typekind_from_symbol` (`src/compiler/elab_core.c:23`) and `typekind_from_name`
(`src/compiler/types.c:1917`) recognised the long forms `int32` / `float32` /
etc. but not the short forms `i32` / `f32` / `i16` / `u32` / ...  An unknown
sized-type short form fell through to the opaque-struct fallback in
`type_expr_from_form`, and downstream literal inference then re-injected the
generic `TY_INT` / `TY_FLOAT` kind from the value-side, producing the
`Box__int` / `Box__float` mangling.

The mangler in `src/compiler/types.c:285` (`append_type_mangle`) already had
correct cases for `TY_INT32`, `TY_FLOAT32`, etc., so once the type resolves,
the rest of codegen is fine.

### Fix

Added `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f32`, `f64` to
both `typekind_from_symbol` (`src/compiler/elab_core.c`) and
`typekind_from_name` (`src/compiler/types.c`).  Each maps to its corresponding
`TY_INT*` / `TY_UINT*` / `TY_FLOAT*` kind.

### Verification

`tests/fixtures/typed-slots/sized-numeric-struct-app/` -- asserts that
`(Box i32)` emits `Box__int32 { int32_t x; }` and `(Box f32)` emits
`Box__float32 { float x; }`, with the function return type, local-variable
declaration, and field type all using the concrete sized C type.

### Remaining limitation

Literal inference still does not propagate the expected struct-app argument
type into the value-side literal.  This means `(make-struct Box 1)` inside a
function declared `:(Box i32)` infers `1` as `int64_t` and produces a C-level
type mismatch (`Box__int` vs. `Box__int32` return).  The workaround is to
ascribe the literal explicitly: `(make-struct Box (:: 1 i32))`.  This is a
bidirectional-checking concern, separate from TS3.2.

---

## KB-014 -- Aggregate struct-app let-bindings use `int64_t` carrier

**Discovered:** 2026-05-26 (TS3.2 audit)
**Status:** Fixed 2026-05-26 -- this turned out to be a downstream effect of
KB-013.  Once the struct-app type resolves correctly, the let-binding picks
up the concrete C type (`Box__int32 b_603 = make_i32_box();`).  The
"int64_t carrier" only appeared when the type-application's argument was
itself unresolved (short-form `i32`/`f32` fell through to opaque struct).

### Symptom

A `let` binding initialised from `make-struct` of a parameterised struct
emits an `int64_t` carrier variable holding the aggregate as a compound
literal:

```c
int64_t b32_606  = (Box__int){.x = INT64_C(1)};
int64_t bf32_607 = (Box__float){.x = 1.5};
(void)(ret_i32(b32_606));
(void)(ret_f32(bf32_607));
```

The compound literal is the correct *concrete* layout (`Box__int`,
`Box__float`), but the receiving variable is `int64_t`.  This works today
by accident: C silently truncates / sign-extends, and the codepath does
not actually access the struct's fields through the `int64_t` view.  As
soon as a downstream consumer expects the typed C struct (a typed thunk,
a specialised helper, or a typed field access), the mismatch surfaces as
either a C-level type error or wrong-width memory reads.

### Root cause

Local variable types for aggregates produced by `make-struct` are still
emitted via the legacy carrier ABI, even when the elaborated `Expr` carries
a fully concrete `TY_APP` (e.g. `Box__int`).  The let-codegen path picks
`type_c_name()` against an erased shell rather than against the
fully-instantiated `Expr->type`.

This is the same family of bug as the CS1 "finish aggregate return
instantiation" item in
[archive/history/typed-slots-gs5-compiler-support-plan.md](archive/history/typed-slots-gs5-compiler-support-plan.md):
the type system already knows the concrete instantiation, but the carrier
ABI is still picked at the emit boundary.

### Reproducer

`./build/tur emit-c tests/fixtures/typed-slots/tcons-of/input.tur` -- look
for the `main()` body and any `let` binding holding a `(Cons A)` /
`(Option A)` / etc.  The local will be `int64_t`, while the initialiser
will be `(Cons__A){...}`.

### Impact

- Direct typed field access through a let-bound local works today only
  because the carrier ABI happens to round-trip 8-byte payloads.
- Larger payloads (e.g. `Pair__int__float` is 16 bytes) silently truncate
  when funneled through an `int64_t` local.
- Blocks CS3-style specialised helpers from accepting aggregate values
  by-value -- the caller's local is the wrong width.

### Workaround

Avoid let-binding intermediate aggregate values; either chain field
accesses on the `make-struct` expression directly, or annotate the
binding with `(:: ... (Ctor A B))` *and* avoid passing it through a
specialised helper.

### Fix needed

Emit local-variable declarations using the let-RHS's fully-instantiated
`Expr->type` (via `type_c_name(expr->type)`) instead of falling back to
the carrier shell.  Concretely: audit the let-codegen path in
`src/compiler/emit_stmt.c` / `src/compiler/emit_expr.c` for sites that
hardcode `int64_t` for aggregate locals.


---

## KB-015 -- Repeated calls to a specialized typed accessor emit a spurious reinterpret

**Discovered:** 2026-05-26 (TS3.4 audit)
**Status:** Open -- pre-existing bug exposed by typed accessor composition,
not a TS3.3 regression (confirmed by stashing the TS3.3 fix and reproducing).

### Symptom

Calling a generic-but-specializable typed accessor like `thead`
(`[A] [l :(Cons A)] :A`) twice in the same function, on different bindings
of the same instantiated type, emits a spurious bit-reinterpret around
the *second* call -- producing garbage:

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

1. Audit the specialization-call cache and result-type threading in
   `src/compiler/elab_call.c` -- specifically the post-call
   `call_wrap_reinterpret(...)` site (around line 1687).  Is the
   `result_type.kind` derived from the *binding* type (still showing
   the carrier `int64_t`) instead of the *instantiated* type
   (`double`)?
2. Add a focused regression that calls a typed `[A]` helper twice on
   two bindings of the same instantiation and asserts both results.
