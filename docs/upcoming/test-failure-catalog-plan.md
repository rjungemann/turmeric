# Test Failure Catalog

Cataloged 2026-06-01. All failures are "build failed" / "tur build failed" — the Turmeric
compiler emits C that clang rejects. There is also a UBSan warning present on every failing
test: `emit_fns.c:156:62: load of value N, which is not a valid value for type 'bool'`.

## Failure Groups

### 1. nested-fn-in-inline-c (24 tests) — **FIXED** ✓

`json/encode`, `json/decode`, and `json/free` in `stdlib/json.tur` contained nested
function definitions inside their inline-C bodies (e.g. `buf_ensure`, `encode_node`,
`parse_value`, `free_node`). GCC supports nested functions as an extension; clang/LLVM
does not. **Fix**: split each nested function into a top-level `defn` helper that accepts
the shared state (buf/ctx) via `:ptr` (`void *`). Each helper re-declares the local struct
and casts the opaque pointer. All helpers are forward-declared by the emitter, so mutual
and recursive calls work.

Affected tests:
- `hkt-instance-closure-to-fat`
- `json-reader-array`, `json-reader-escape`, `json-reader-nested`, `json-reader-null`, `json-reader-object`
- `schema-alternative-union`, `schema-applicative-ap`, `schema-applicative-error-accumulation`,
  `schema-applicative-user`, `schema-applicative-user-errors`, `schema-decode-array`,
  `schema-decode-errors`, `schema-decode-literal`, `schema-decode-object`, `schema-decode-optional`,
  `schema-decode-recursive`, `schema-decode-typed-user`, `schema-decode-union`,
  `schema-functor-transform`, `schema-hkt-alternative`, `schema-hkt-functor`,
  `schema-reader-json-str-runtime`, `schema-transform-closure`

### 2. bool-fn-ptr-mismatch (2 tests) — **OPEN**

`rt-return-dispatch-basic`, `rt-return-dispatch-param`

```
incompatible function pointer types initializing 'int64_t (*)()' with 'bool ()'
.default_of = __inst_Default_default_of_bool,
```

The `Default` typeclass's `bool` instance emits a function `bool ()` but the vtable slot
expects `int64_t (*)()`. Root cause: the generated code for the `Default` bool instance
uses the C `bool` return type instead of `int64_t`.

### 3. hamt-type-mismatch (1 test) — **OPEN**

`hamt-lowering-basic`

```
incompatible integer to pointer conversion passing 'int64_t' to parameter of type 'void *'
hamt_count(m_835) / hamt_set(m_835, ...)
```

HAMT lowering passes an `int64_t` variable where the HAMT runtime functions expect `void *`.
The emitter needs to wrap HAMT node values with `(void *)(intptr_t)` casts.

### 4. bool-carrier-mismatch (1 test) — **OPEN**

`wkc3-struct-map-key`

```
incompatible pointer to integer conversion returning 'bool (int64_t, int64_t)'
from a function with result type 'int64_t'
.mk_cmp = __inst_MapKey_mk_cmp_Point,
```

The `MapKey` typeclass instance for `Point` emits a carrier eq function that returns `bool`
but the vtable slot expects `int64_t`. Similar root cause to group 2 — bool/int64_t mismatch
in typeclass instance dispatch tables.

## Cross-Cutting: UBSan Warning

`src/compiler/emit_fns.c:156:62: runtime error: load of value N, which is not a valid value for type 'bool'`

This fires on every failing test during the `tur emit-c` invocation itself (i.e. inside the
Turmeric compiler binary, not the generated code). It indicates a compiler-internal UB: a
`bool` field is being read from memory that was never initialized to a valid `bool` value
(probably a struct field that should be zero-initialized). This is a latent compiler bug that
should be investigated separately.
