# Structural Equality for Compound Types — Implementation Plan

> **Status:** Planned
> **Prerequisites:** None — stdlib-only work; no compiler changes required for Phase 1
> **Related:** `stdlib/typeclass.tur` (Eq class), `stdlib/vec.tur`, `stdlib/list.tur`, `stdlib/pair.tur`, `stdlib/option.tur`, `stdlib/result.tur`, `stdlib/hamt.tur`, `stdlib/map.tur`, `stdlib/str.tur`

---

## Motivation

Turmeric's `Eq` typeclass only has instances for primitive types (`int`, `bool`, `float32`, fixed-width integers). Compound types — `vec`, `list` (Cons/nil), `Pair`, `option`, `result`, `map`/HAMT, and `str` — have no equality support at all. Users who need to compare two lists, two optional values, or two maps must write their own comparison functions from scratch every time.

Adding `Eq` instances for compound types requires:

1. An element-wise comparison strategy (since all values are erased to `int64_t` at runtime, the element comparison function must be passed in explicitly or the instance must call `eq?` recursively via the typeclass).
2. Inline-C helpers that walk the runtime representation of each type.
3. A typeclass instance declaration for each type.

Because generic type parameters are erased to `int64_t` in v1, compound `Eq` instances cannot be fully polymorphic without passing an element-equality function explicitly. The cleanest approach is to add **standalone equality functions** (like the existing `str-eq?`) alongside **`Eq` instances that compare values by identity** as a safe baseline, with a note that element-level structural equality requires explicit element comparators.

This plan is split into two phases:

- **Phase E1 (stdlib-only):** Add `eq?` instances for types whose elements are always comparable by value (`option` of int, `result` of int, `Pair` of int). Add standalone structural-equality helpers (`vec-eq?`, `list-eq?`, `pair-eq?`, `option-eq?`, `result-eq?`, `map-eq?`) that accept an element-comparator function argument.
- **Phase E2 (stretch):** If the compiler grows support for constrained typeclass dispatch (e.g., `Eq a => Eq (vec a)`), update the `definstance` forms to use recursive `eq?` dispatch instead of explicit comparator arguments.

---

## Runtime Type Representations (Reference)

These are the C struct layouts used throughout the stdlib inline-C blocks. All values are `int64_t` (pointer-sized) at the ABI boundary.

| Type | C struct |
|---|---|
| `vec` | `struct { int64_t *data; size_t len; size_t cap; }` |
| `list` (Cons) | `struct { int64_t value; int64_t next; }`, nil = `0` |
| `Pair` | `struct { int64_t first; int64_t second; }` |
| `option` | `struct { bool is_some; int64_t value; }`, none = `NULL` |
| `result` | `struct { bool is_ok; int64_t ok_val; int64_t err_val; }` |
| `map`/HAMT | opaque `tur_hamt_*` C API; iteration via `tur_hamt_iter_*` |
| `str` | `struct { const char *p; size_t len; }` |

---

## Phase E1 — Standalone Equality Helpers + Basic Instances

### Task E1-1: `str-eq?` — already done; add `Eq [str]` instance

**File:** `stdlib/str.tur`

`str-eq?` already exists and compares `{ const char *p; size_t len; }` structs via `memcmp`. What is missing is a formal `Eq` instance that wires `eq?` to it.

However, because `str` is currently represented as a heap pointer and the `Eq` dispatch in v1 works on the concrete type name, we first need to confirm that `definstance Eq [str]` is accepted by the elaborator. If accepted, add:

```clojure
(definstance Eq [str]
  (eq? [x y] (str-eq? x y)))
```

**Acceptance criteria:** `(eq? (str-from-cstr "hello") (str-from-cstr "hello"))` returns `true`; `(eq? (str-from-cstr "a") (str-from-cstr "b"))` returns `false`.

---

### Task E1-2: `pair-eq?` — structural equality for `Pair`

**File:** `stdlib/pair.tur`

Add a function that compares two `Pair` values using a caller-supplied element comparator:

```clojure
;; pair-eq? -- compare two Pairs element-wise using a comparator fn.
;; cmp-fn must be a fn [a :int b :int] :bool
(defn pair-eq? [p1 p2 cmp-fn] :bool
  ```c
  struct { int64_t first; int64_t second; } *a = (void*)(intptr_t)p1;
  struct { int64_t first; int64_t second; } *b = (void*)(intptr_t)p2;
  if (a == b) return true;
  if (!a || !b) return false;
  bool first_eq  = ((bool(*)(int64_t, int64_t))(intptr_t)cmp_fn)(a->first,  b->first);
  bool second_eq = ((bool(*)(int64_t, int64_t))(intptr_t)cmp_fn)(a->second, b->second);
  return first_eq && second_eq;
  ```)
```

Also add a simple identity-comparison `Eq` instance (compares `int64_t` fields directly, appropriate when both elements are primitives):

```clojure
(definstance Eq [Pair]
  (eq? [x y] (pair-eq? x y (fn [a b] (= a b)))))
```

**Acceptance criteria:** `(pair-eq? (pair-new 1 2) (pair-new 1 2) (fn [a b] (= a b)))` returns `true`; `(pair-eq? (pair-new 1 2) (pair-new 1 3) (fn [a b] (= a b)))` returns `false`.

---

### Task E1-3: `option-eq?` — structural equality for `option`

**File:** `stdlib/option.tur`

```clojure
;; option-eq? -- compare two options using a comparator fn for the contained value.
;; cmp-fn must be a fn [a :int b :int] :bool
(defn option-eq? [o1 o2 cmp-fn] :bool
  ```c
  struct { bool is_some; int64_t value; } *a =
      (struct { bool is_some; int64_t value; } *)(intptr_t)o1;
  struct { bool is_some; int64_t value; } *b =
      (struct { bool is_some; int64_t value; } *)(intptr_t)o2;
  bool a_some = a && a->is_some;
  bool b_some = b && b->is_some;
  if (!a_some && !b_some) return true;   /* none == none */
  if (a_some != b_some)   return false;  /* some != none */
  return ((bool(*)(int64_t, int64_t))(intptr_t)cmp_fn)(a->value, b->value);
  ```)
```

Add a primitive-value `Eq` instance:

```clojure
(definstance Eq [option]
  (eq? [x y] (option-eq? x y (fn [a b] (= a b)))))
```

**Acceptance criteria:** `(option-eq? (some 42) (some 42) (fn [a b] (= a b)))` returns `true`; `(option-eq? (some 1) (none) (fn [a b] (= a b)))` returns `false`; `(option-eq? (none) (none) (fn [a b] (= a b)))` returns `true`.

---

### Task E1-4: `result-eq?` — structural equality for `result`

**File:** `stdlib/result.tur`

```clojure
;; result-eq? -- compare two results using separate comparators for ok and err values.
;; ok-cmp and err-cmp must each be fn [a :int b :int] :bool
(defn result-eq? [r1 r2 ok-cmp err-cmp] :bool
  ```c
  struct { bool is_ok; int64_t ok_val; int64_t err_val; } *a =
      (struct { bool is_ok; int64_t ok_val; int64_t err_val; } *)(intptr_t)r1;
  struct { bool is_ok; int64_t ok_val; int64_t err_val; } *b =
      (struct { bool is_ok; int64_t ok_val; int64_t err_val; } *)(intptr_t)r2;
  if (!a && !b) return true;
  if (!a || !b) return false;
  if (a->is_ok != b->is_ok) return false;
  if (a->is_ok) {
      return ((bool(*)(int64_t, int64_t))(intptr_t)ok_cmp)(a->ok_val, b->ok_val);
  } else {
      return ((bool(*)(int64_t, int64_t))(intptr_t)err_cmp)(a->err_val, b->err_val);
  }
  ```)
```

Add a primitive `Eq` instance (uses identity comparison for both arms):

```clojure
(definstance Eq [result]
  (eq? [x y] (result-eq? x y (fn [a b] (= a b)) (fn [a b] (= a b)))))
```

**Acceptance criteria:** `(result-eq? (ok 1) (ok 1) ...)` returns `true`; `(result-eq? (ok 1) (err 1) ...)` returns `false`; `(result-eq? (err 2) (err 2) ...)` returns `true`.

---

### Task E1-5: `vec-eq?` — structural equality for `vec`

**File:** `stdlib/vec.tur`

```clojure
;; vec-eq? -- compare two vecs element-wise using a comparator fn.
;; cmp-fn must be a fn [a :int b :int] :bool
(defn vec-eq? [v1 v2 cmp-fn] :bool
  ```c
  struct { int64_t *data; size_t len; size_t cap; } *a =
      (struct { int64_t *data; size_t len; size_t cap; } *)(intptr_t)v1;
  struct { int64_t *data; size_t len; size_t cap; } *b =
      (struct { int64_t *data; size_t len; size_t cap; } *)(intptr_t)v2;
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->len != b->len) return false;
  for (size_t i = 0; i < a->len; i++) {
      if (!((bool(*)(int64_t, int64_t))(intptr_t)cmp_fn)(a->data[i], b->data[i])) {
          return false;
      }
  }
  return true;
  ```)
```

Add a primitive `Eq` instance:

```clojure
(definstance Eq [vec]
  (eq? [x y] (vec-eq? x y (fn [a b] (= a b)))))
```

**Acceptance criteria:**
- Two empty vecs are equal.
- Two vecs with the same integer elements in the same order are equal.
- Vecs of different lengths are not equal.
- Vecs with the same length but different elements are not equal.

---

### Task E1-6: `list-eq?` — structural equality for linked list

**File:** `stdlib/list.tur`

```clojure
;; list-eq? -- compare two singly-linked lists element-wise using a comparator fn.
;; cmp-fn must be a fn [a :int b :int] :bool
(defn list-eq? [l1 l2 cmp-fn] :bool
  ```c
  struct { int64_t value; int64_t next; } *a =
      (struct { int64_t value; int64_t next; } *)(intptr_t)l1;
  struct { int64_t value; int64_t next; } *b =
      (struct { int64_t value; int64_t next; } *)(intptr_t)l2;
  while (a && b) {
      if (!((bool(*)(int64_t, int64_t))(intptr_t)cmp_fn)(a->value, b->value)) {
          return false;
      }
      a = (struct { int64_t value; int64_t next; } *)(intptr_t)a->next;
      b = (struct { int64_t value; int64_t next; } *)(intptr_t)b->next;
  }
  return a == b; /* both must be NULL (nil) for equality */
  ```)
```

Add a primitive `Eq` instance:

```clojure
(definstance Eq [list]
  (eq? [x y] (list-eq? x y (fn [a b] (= a b)))))
```

**Acceptance criteria:**
- Two nil lists are equal.
- `(cons 1 (cons 2 (nil-value)))` equals itself.
- Lists of different lengths are not equal.
- Lists with the same length but differing values are not equal.

---

### Task E1-7: `map-eq?` — structural equality for HAMT-backed maps

**File:** `stdlib/map.tur` (and/or `stdlib/hamt.tur`)

Map equality is the most complex case because maps are unordered. The algorithm is:

1. Check that both maps have the same `count`.
2. Iterate all key/value pairs in map `a`.
3. For each key, look it up in map `b` (using `hamt/get`).
4. If the key is absent in `b`, return `false`.
5. Compare the values using the provided value comparator.

Note: step 3 uses pointer identity for key lookup (since `hamt/hash-ptr` hashes by address). This is correct for keys that are the same pointer (interned strings or shared values), but **will not work for keys that are equal-by-value but at different addresses**. A full solution requires a key-comparator argument too; for Phase E1 the simpler pointer-identity key lookup is documented as a known limitation.

```clojure
;; map-eq? -- compare two maps for structural equality.
;; Keys are compared by pointer identity (same limitation as hamt/get).
;; val-cmp must be a fn [a :ptr<void> b :ptr<void>] :bool
(defn map-eq? [m1 m2 val-cmp] :bool
  ```c
  /* Check counts */
  if (tur_hamt_count((void*)(intptr_t)m1) !=
      tur_hamt_count((void*)(intptr_t)m2)) return false;
  /* Allocate iterator (64 bytes is sufficient per hamt.tur docs) */
  char iter_buf[64];
  void *iter = (void*)iter_buf;
  tur_hamt_iter_init(iter, (void*)(intptr_t)m1);
  int64_t hash_out, key_out, val_out;
  while (tur_hamt_iter_next(iter, &hash_out, &key_out, &val_out)) {
      void *val_in_b = tur_hamt_get(
          (void*)(intptr_t)m2, hash_out, (void*)(intptr_t)key_out);
      if (!val_in_b) { tur_hamt_iter_free(iter); return false; }
      bool vals_eq = ((bool(*)(int64_t, int64_t))(intptr_t)val_cmp)(
          val_out, (int64_t)(intptr_t)val_in_b);
      if (!vals_eq) { tur_hamt_iter_free(iter); return false; }
  }
  tur_hamt_iter_free(iter);
  return true;
  ```)
```

No `Eq` instance is added for map in Phase E1 because the `Eq` typeclass dispatch uses `eq?` with two arguments and cannot easily pass a val-cmp. Document this as requiring Phase E2.

**Known limitation:** Key equality uses pointer identity. Two maps built with `(assoc m "key" val)` where the `"key"` literals are at different addresses will not compare correctly. A Phase E2 follow-on should accept a key-comparator as well.

**Acceptance criteria:**
- Two empty maps are equal.
- Maps with the same pointer-keyed entries and equal values are equal.
- Maps with a different number of entries are not equal.
- Maps with the same keys but different values are not equal.

---

### Task E1-8: Tests

**File:** `tests/structural_eq_test.tur` (new file)

Write one test per type covering:
- Equal base cases (including nil/none/empty).
- Unequal cases (wrong length, wrong element, wrong variant).
- Nested case: a vec of options, compared with `vec-eq?` passing `option-eq?` as the comparator.

Use the existing `stdlib/test.tur` assertion helpers.

---

## Phase E2 — Constrained Typeclass Dispatch

Phase E2 upgrades the compiler so that `definstance` declarations can carry constraints (`[(Eq a)]`) that are satisfied at each call site by substituting concrete type arguments and threading the resolved method implementation as a function pointer. This eliminates the need to pass comparators explicitly and makes `eq?` work uniformly for nested compound types.

### Background: what the compiler already has

| Phase | Status | What it does |
|---|---|---|
| PTC1 | Done | `definstance` parses a constraint vector `[(Clone a) (Clone b)]` and stores `TypeConstraint[]` on the instance |
| PTC3 | Done | `typeclass_instance_constraints_satisfied` (`src/typeclass.c:183`) checks constraints at lookup time — but only for **concrete** type arguments (e.g., `[(Eq int)]`), not type-variable substitution |
| PTC4 | **Missing** | Substitution of instance type-variable arguments with the concrete types from the lookup call, enabling `[(Eq a)]` to resolve to `[(Eq int)]` when looking up `Eq[vec]` with `int` elements |
| Dictionary passing | Deferred to v2 | Full runtime dictionary threading; comment at `src/elab.c:10731`: "Full dictionary passing deferred to v2" |

The approach for Phase E2 is **monomorphization rather than dictionary passing**: when the elaborator processes a constrained `definstance` method body, it resolves each constraint to a concrete instance at the call site and emits a direct function pointer to the resolved method. This avoids the complexity of runtime dictionaries while still making `(eq? a b)` work on element types inside compound-type instances.

---

### Task E2-1: PTC4 — type-variable substitution in constraint checking

**File:** `src/typeclass.c` — `typeclass_instance_constraints_satisfied`

Currently (PTC3), when checking whether an instance `Eq[vec]` with constraint `[(Eq a)]` is valid for a lookup of `Eq[vec<int>]`, the function only handles concrete constraints and explicitly comments:

```c
/* Phase PTC4 will handle substitution of type parameters with
 * their concrete types from the lookup. */
```

**What to add:**

The function receives `lookup_type_args` and `n_lookup_args` — these are the concrete types the caller is looking up (e.g., `[int]` when looking up `Eq` for a `vec<int>`). The `type_param_constraints` on the instance can contain `TY_UNKNOWN` entries where the type parameter (e.g., `a`) appears.

Extend the loop in `typeclass_instance_constraints_satisfied` (around line 191):

1. For each `type_param_constraints[i]`, check if `required_type.kind == TY_UNKNOWN`. If so, it represents an unresolved type parameter.
2. Match the parameter by position against `inst->type_arg_syms` (already stored on the instance from the `definstance` parse).
3. Substitute `required_type` with the corresponding entry from `lookup_type_args`.
4. Then proceed to check the substituted type against the environment as before.

**Acceptance criteria:** Given `(definstance Eq [vec] [(Eq a)] ...)` and a call site where the vec holds `int` elements, `typeclass_env_lookup_instance` for `Eq` on `vec` correctly finds and validates the instance only when `Eq[int]` exists.

---

### Task E2-2: Elaborator — resolve constrained method calls in `definstance` bodies

**File:** `src/elab.c` — `elab_definstance` and/or `elab_form`

When elaborating the body of a constrained `definstance` method, calls to typeclass methods on constrained type variables (e.g., `(eq? a b)` where `a` and `b` have type `int` because the `vec` element type was resolved to `int`) need to be redirected from a generic dispatch to the concrete resolved implementation.

The existing monomorphic path in `elab_method_call` (`src/elab.c:10786`) already does type-based dispatch for method calls like `(.eq? obj arg)`. What is missing is the ability to do the same for calls like `(eq? a b)` inside a constrained instance body where the type of `a` and `b` is known from the constraint resolution.

**What to add:**

1. When `elab_definstance` processes a constrained instance, record a mapping from each constrained type-variable name to its resolved concrete type and the resolved `TypeClassInstance`. Store this in a small lookup table local to the instance elaboration.

2. During body elaboration, when `elab_form` encounters a typeclass method call (currently dispatched via the global environment), check the local constraint table first. If the call's argument types match a constrained type variable, substitute the resolved instance's method function directly.

3. Emit the resolved method as a direct function reference (the same `EX_CALL` path already used for monomorphic typeclass dispatch), not as a dictionary call.

**Acceptance criteria:** `(definstance Eq [vec] [(Eq a)] (eq? [x y] (vec-eq? x y (fn [a b] (eq? a b)))))` elaborates without error, and the inner `(eq? a b)` resolves to the concrete `eq?` for the element type rather than emitting a generic or unresolved dispatch.

---

### Task E2-3: Update `Eq [vec]` to use constrained element dispatch

**File:** `stdlib/vec.tur`

Replace the Phase E1 `definstance` (which hard-codes `(= a b)` for primitive elements) with a constrained version that calls `eq?` recursively on elements:

```clojure
(definstance Eq [vec] [(Eq a)]
  (eq? [x y] (vec-eq? x y (fn [a b] (eq? a b)))))
```

The constraint vector `[(Eq a)]` uses the syntax already supported by PTC1. With PTC4 (E2-1) and the elaborator fix (E2-2) in place, `(eq? a b)` in the lambda body will resolve to whatever `Eq` instance is in scope for the element type.

**Why this is better than E1:** A `vec` of `option` values can now be compared with `eq?` without any explicit comparator — the elaborator threads `option-eq?` (via the `Eq[option]` instance) automatically.

**Acceptance criteria:**
- `(eq? (vec-of-ints) (vec-of-ints))` still works (same behavior as E1).
- `(eq? (vec-of-options) (vec-of-options))` works without passing a comparator.
- `(eq? (vec-of-vecs) (vec-of-vecs))` works (nested `vec`, requires recursive dispatch).

---

### Task E2-4: Update constrained `Eq` instances for `list`, `Pair`, `option`, `result`

**Files:** `stdlib/list.tur`, `stdlib/pair.tur`, `stdlib/option.tur`, `stdlib/result.tur`

Apply the same transformation as E2-3 to each remaining type:

**`list`** (`stdlib/list.tur`):
```clojure
(definstance Eq [list] [(Eq a)]
  (eq? [x y] (list-eq? x y (fn [a b] (eq? a b)))))
```

**`Pair`** (`stdlib/pair.tur`):
```clojure
(definstance Eq [Pair] [(Eq a)]
  (eq? [x y] (pair-eq? x y (fn [a b] (eq? a b)))))
```

**`option`** (`stdlib/option.tur`):
```clojure
(definstance Eq [option] [(Eq a)]
  (eq? [x y] (option-eq? x y (fn [a b] (eq? a b)))))
```

**`result`** (`stdlib/result.tur`):

`result` has two type parameters (ok and err), so it needs two constraints. The constraint vector syntax supports this:

```clojure
(definstance Eq [result] [(Eq ok) (Eq err)]
  (eq? [x y] (result-eq? x y (fn [a b] (eq? a b)) (fn [a b] (eq? a b)))))
```

Note: because `result` is currently dispatched as `ptr<void>` (not a named type constructor), this may require assigning `result` a dedicated `TypeKind` or opaque struct tag so that `typeclass_env_lookup_instance` can distinguish `Eq[result]` from `Eq[ptr<void>]` (which already has a `Foldable` instance). This is a prerequisite sub-task; see E2-4a below.

**E2-4a (sub-task): Give `result` a distinct type identity**

Currently `result` is represented as `TY_PTR_VOID` at the type level, which means `Eq[result]` and `Eq[ptr<void>]` are indistinguishable to the instance lookup. Options:

- Introduce `TY_RESULT` as a new `TypeKind` in `src/types.h`, and update `typekind_from_symbol` in `src/elab.c` to return it for the string `"result"`.
- Alternatively, register `result` as a synthetic struct type with a fixed `StructDef` so it gets `TY_STRUCT` with a known name, matching the pattern used for `Pair` and `Cons`.

The first option (new `TypeKind`) is lower risk since it doesn't change struct elaboration. The second option is more consistent with how user-defined types work.

**Acceptance criteria for E2-4:**
- `(eq? (some (some 1)) (some (some 1)))` returns `true` (nested option).
- `(eq? (cons (ok 1) nil) (cons (ok 1) nil))` returns `true` (list of result).
- `(eq? (pair-new (cons 1 nil) (cons 2 nil)) (pair-new (cons 1 nil) (cons 2 nil)))` returns `true` (pair of lists).

---

### Task E2-5: Update `map-eq?` to accept a key comparator

**File:** `stdlib/map.tur`

The E1 `map-eq?` uses `hamt/hash-ptr` for key lookup, which hashes by pointer address. This means two maps with equal-valued string keys at different addresses will not compare correctly.

Replace with a version that accepts both a key-hash function and a key-comparator alongside the value comparator:

```clojure
;; map-eq? -- compare two maps for structural equality.
;; key-hash must be a fn [k :ptr<void>] :int  (must match how the map was built)
;; key-cmp  must be a fn [a :ptr<void> b :ptr<void>] :bool
;; val-cmp  must be a fn [a :ptr<void> b :ptr<void>] :bool
(defn map-eq? [m1 m2 key-hash key-cmp val-cmp] :bool
  ```c
  if (tur_hamt_count((void*)(intptr_t)m1) !=
      tur_hamt_count((void*)(intptr_t)m2)) return false;
  char iter_buf[64];
  void *iter = (void*)iter_buf;
  tur_hamt_iter_init(iter, (void*)(intptr_t)m1);
  int64_t hash_out, key_out, val_out;
  while (tur_hamt_iter_next(iter, &hash_out, &key_out, &val_out)) {
      /* Recompute hash using the caller's key-hash fn (handles value-equal keys
       * at different addresses, e.g. string keys) */
      int64_t rehash = ((int64_t(*)(int64_t))(intptr_t)key_hash)(key_out);
      void *val_in_b = tur_hamt_get(
          (void*)(intptr_t)m2, rehash, (void*)(intptr_t)key_out);
      if (!val_in_b) { tur_hamt_iter_free(iter); return false; }
      /* Optionally verify key equality (handles hash collisions) */
      if (!((bool(*)(int64_t, int64_t))(intptr_t)key_cmp)(key_out, (int64_t)(intptr_t)val_in_b)) {
          tur_hamt_iter_free(iter); return false;
      }
      if (!((bool(*)(int64_t, int64_t))(intptr_t)val_cmp)(
              val_out, (int64_t)(intptr_t)val_in_b)) {
          tur_hamt_iter_free(iter); return false;
      }
  }
  tur_hamt_iter_free(iter);
  return true;
  ```)
```

**Migration note:** E1's `map-eq? [m1 m2 val-cmp]` becomes `map-eq? [m1 m2 hamt/hash-ptr (fn [a b] (= a b)) val-cmp]` for maps with pointer-identity keys. Update E1-8 tests accordingly.

**Acceptance criteria:**
- Two maps with string keys built via `hamt/hash-str` compare correctly when both maps hold the same string content at different addresses.
- Two maps with integer keys compare correctly.

---

### Task E2-6: Add `Eq [map]` instance

**File:** `stdlib/map.tur`

With E2-1 through E2-5 in place, add a constrained `Eq` instance for maps. Because map requires both a key-hash and two comparators, the instance must pass the resolved `eq?` implementations and hash functions via the constraint mechanism. The constraint vector needs `Eq` for both the key type and the value type, plus a `Hash` typeclass for the key (so the instance can call `hash` on keys).

If a `Hash` typeclass does not yet exist, this task includes adding it:

```clojure
;; Hash typeclass -- compute an int64_t hash of a value.
(defclass Hash [a]
  (hash [x] :int))

;; Primitive Hash instances
(definstance Hash [int]
  (hash [x] x))

(definstance Hash [cstr]
  (hash [x] (hamt/hash-str x)))

(definstance Hash [str]
  (hash [x] ...))   ;; hash the str bytes via hamt/hash-str after extracting .p
```

Then the map `Eq` instance:

```clojure
(definstance Eq [map] [(Eq k) (Eq v) (Hash k)]
  (eq? [x y]
    (map-eq? x y
             (fn [k] (hash k))
             (fn [a b] (eq? a b))
             (fn [a b] (eq? a b)))))
```

**Acceptance criteria:**
- `(eq? (assoc (map-new) :foo 1) (assoc (map-new) :foo 1))` returns `true` (keyword keys).
- `(eq? (assoc (map-new) :foo 1) (assoc (map-new) :foo 2))` returns `false`.
- `(eq? (map-new) (map-new))` returns `true`.

---

### Task E2-7: Tests for constrained dispatch

**File:** `tests/structural_eq_constrained_test.tur` (new file)

Write tests specifically exercising the constrained dispatch path (things that fail or give wrong answers in Phase E1):

1. **Nested containers:** `eq?` on `vec<option<int>>`, `list<vec<int>>`, `option<pair<int,int>>`.
2. **Result nesting:** `eq?` on `vec<result<int,int>>` and `list<result<int,int>>`.
3. **Map with value-equal string keys:** Build two maps with `hamt/hash-str` string keys at different addresses; verify `eq?` returns `true`.
4. **Depth-3 nesting:** `vec<vec<option<int>>>` — verifies that recursive dispatch works more than one level deep.
5. **Negative cases:** Mismatched nested values at every depth level.

---

## Summary of Deliverables

| Task | File(s) | Deliverable |
|---|---|---|
| E1-1 | `stdlib/str.tur` | `(definstance Eq [str] ...)` wired to `str-eq?` |
| E1-2 | `stdlib/pair.tur` | `pair-eq?` + `(definstance Eq [Pair] ...)` |
| E1-3 | `stdlib/option.tur` | `option-eq?` + `(definstance Eq [option] ...)` |
| E1-4 | `stdlib/result.tur` | `result-eq?` + `(definstance Eq [result] ...)` |
| E1-5 | `stdlib/vec.tur` | `vec-eq?` + `(definstance Eq [vec] ...)` |
| E1-6 | `stdlib/list.tur` | `list-eq?` + `(definstance Eq [list] ...)` |
| E1-7 | `stdlib/map.tur` | `map-eq?` (no Eq instance; pointer-identity key lookup) |
| E1-8 | `tests/structural_eq_test.tur` | Tests for all E1 types |
| E2-1 | `src/typeclass.c` | PTC4: type-variable substitution in constraint checking |
| E2-2 | `src/elab.c` | Elaborator: resolve constrained method calls in instance bodies |
| E2-3 | `stdlib/vec.tur` | Upgrade `Eq[vec]` to constrained form |
| E2-4 | `stdlib/list.tur`, `pair.tur`, `option.tur`, `result.tur` | Upgrade all remaining instances; sub-task E2-4a for `result` type identity |
| E2-5 | `stdlib/map.tur` | Upgrade `map-eq?` with key-hash and key-cmp arguments |
| E2-6 | `stdlib/map.tur`, `stdlib/typeclass.tur` | `Hash` typeclass + `Eq[map]` instance |
| E2-7 | `tests/structural_eq_constrained_test.tur` | Tests for constrained dispatch and nesting |

### Known limitations (Phase E1)

- All `Eq` instances assume elements are primitives (`int64_t` compared with `==`). Nesting compound types (e.g., `vec` of `vec`) requires calling the standalone `*-eq?` function with an explicit comparator rather than using `eq?` via the typeclass.
- `map-eq?` uses pointer identity for key lookup, so maps keyed by value-equal-but-distinct-pointer values will not compare correctly.
- No `Eq [map]` instance is provided in Phase E1.
