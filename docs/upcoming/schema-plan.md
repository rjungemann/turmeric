# `tur/schema` -- Runtime Schema Validation Plan (SC0--SC6)

> **Status:** Not started.
>
> **Location:** `stdlib/schema.tur` (pure Turmeric over existing stdlib
> primitives; no new C required until SC5).
>
> **Prerequisites:** `tur/result` (Result[A B]), `tur/hamt` (map), `tur/vec`
> (vec), `tur/option` (optional fields). All are already shipped.
>
> **Related:**
> - `docs/upcoming/json-reader-macro-plan.md` -- `#json<Schema>(...)` in JR2
>   delegates typed decoding to `schema-decode` from this library
> - `stdlib/contract.tur` -- contracts verify predicates in static code;
>   schemas validate data at dynamic boundaries (the two complement each other)
> - `stdlib/result.tur`, `stdlib/hamt.tur`, `stdlib/vec.tur`
>
> **Last updated:** 2026-05-31

---

## Motivation

Turmeric's type system is excellent at enforcing invariants *within* a program.
The gap is at **dynamic boundaries**: an HTTP response body, a config file, a
CLI argument map, a channel message from an external process. All of these
arrive as untyped `ptr<void>` values at runtime. Today the only tools are
`contract.tur` (panic on violation) and manual field extraction (no error
propagation, no composability).

A schema library fills this gap with:

1. **Schemas as first-class values** -- a schema is a runtime value you can
   pass to functions, store in a map, and compose with other schemas.
2. **Decode, don't just validate** -- `schema-decode` returns
   `Result<ptr<void>, SchemaError>` so callers can handle bad data gracefully
   instead of panicking.
3. **Structured error messages** -- errors identify the field path
   (`"user.address.zip": expected :cstr, got :int`) rather than crashing with a
   generic assertion failure.
4. **Composability** -- `optional`, `union`, `array-of`, `object`, `transform`
   are schema combinators that build complex schemas from simple ones, matching
   the ergonomics of libraries like Zod (TypeScript) and Serde (Rust).

The design deliberately mirrors `tur/result`'s philosophy: errors are values,
not panics.

---

## API sketch

### Scalar schemas

```turmeric
(import schema)

schema/str       ;; matches :cstr values
schema/int       ;; matches :int values
schema/float     ;; matches :float values
schema/bool      ;; matches :bool values (0 or 1)
schema/nil       ;; matches nil / null

(schema/literal "active")  ;; exact string match
(schema/literal 42)        ;; exact int match
```

### Object schema

```turmeric
(schema/object
  [["name"   schema/str]
   ["age"    schema/int]
   ["email"  (schema/optional schema/str)]])
```

### Array schema

```turmeric
(schema/array schema/int)             ;; vec of ints
(schema/array (schema/object [...]))  ;; vec of objects
```

### Combinators

```turmeric
(schema/optional s)         ;; nil is ok; non-nil must match s
(schema/union  [s1 s2 s3])  ;; first schema that matches wins
(schema/transform s f)      ;; decode with s, then apply f to the result
(schema/intersect [s1 s2])  ;; value must satisfy both schemas
```

### Decoding

```turmeric
;; Decode a HAMT map (e.g. from json-parse or #json(...)) against a schema.
;; Returns Result<ptr<void>, SchemaError>.
(schema-decode my-schema raw-value)

;; Decode or panic (for trusted data / tests).
(schema-decode! my-schema raw-value)
```

### Schema errors

```turmeric
(defstruct SchemaError
  [path    :cstr    ;; dot-separated field path, e.g. "user.address.zip"
   message :cstr    ;; human-readable: "expected :cstr, got :int"
   value   :ptr<void>])  ;; the offending value
```

### Named schemas and recursive schemas

```turmeric
;; Bind a schema to a name for reuse and self-reference.
(def Address
  (schema/object
    [["street" schema/str]
     ["city"   schema/str]
     ["zip"    schema/str]]))

;; Forward-declared recursive schema (e.g. tree nodes).
(schema/rec [self]
  (schema/object
    [["value"    schema/int]
     ["children" (schema/array self)]]))
```

---

## Design decisions

### Schemas are `ptr<void>` at runtime

A schema value is a heap-allocated tagged struct (discriminant int + payload).
The discriminant identifies the schema kind (scalar, object, array, optional,
union, literal, transform, rec). The payload is kind-specific.

This means schemas compose without any compiler magic -- they're plain Turmeric
values. `schema/object`, `schema/array`, etc. are ordinary `defn`s that allocate
and return schema structs.

### Decode produces `ptr<void>`, not a typed struct

`schema-decode` returns `Result<ptr<void>, SchemaError>`. The decoded value is
a HAMT map, a vec, or a primitive -- the same representation that `#json(...)`
produces. The caller extracts fields with typed accessors.

This is the correct default for SC0--SC4. Typed decode (where the result is a
`defstruct` value with known field types) is the SC5 extension and requires
compiler integration.

### Errors accumulate, not fail-fast

`schema-decode` collects *all* schema violations in the decoded value, not just
the first one. This gives the caller a complete picture of what is wrong -- the
same choice Zod makes with `.safeParse`. The return type is
`Result<ptr<void>, Vec<SchemaError>>` (a vec of errors, not a single error).

### No runtime type tags

Turmeric values don't carry runtime type tags (unlike Clojure or Python).
`schema-decode` infers the kind of a `ptr<void>` value from context: HAMT maps
decode against `schema/object`; vecs decode against `schema/array`; primitives
decode against `schema/str` / `schema/int` etc. by reading the int64 bit
pattern. This imposes a constraint: schemas must be anchored at a root kind so
the decoder knows what to expect at the top level.

---

## Phases

### SC0 -- Schema value types and constructors (`stdlib/schema.tur`)

Define the schema discriminant constants and the low-level struct layout.
Implement scalar constructors (`schema/str`, `schema/int`, etc.) and
`schema/literal`. No decoding yet.

```turmeric
;; Schema kind discriminants
(def SCHEMA_STR      0)
(def SCHEMA_INT      1)
(def SCHEMA_FLOAT    2)
(def SCHEMA_BOOL     3)
(def SCHEMA_NIL      4)
(def SCHEMA_LITERAL  5)
(def SCHEMA_OBJECT   6)
(def SCHEMA_ARRAY    7)
(def SCHEMA_OPTIONAL 8)
(def SCHEMA_UNION    9)
(def SCHEMA_TRANSFORM 10)
(def SCHEMA_REC      11)
```

### SC1 -- `schema-decode` for scalars and objects

Implement `schema-decode` for the four scalar kinds + `schema/object`.
Return `Result<ptr<void>, Vec<SchemaError>>`. Test with HAMT maps produced
by `hamt/new` + `hamt/set`.

Fixture: `tests/fixtures/schema-decode-object/` -- round-trip a simple
two-field object schema.

### SC2 -- Array, optional, union, literal

Extend `schema-decode` to handle `schema/array`, `schema/optional`,
`schema/union`, and `schema/literal`. Add `schema/transform`.

Fixtures:
- `schema-decode-array` -- array of ints
- `schema-decode-optional` -- optional field present and absent
- `schema-decode-union` -- two-variant union
- `schema-decode-literal` -- exact match pass and fail

### SC3 -- Error path accumulation and formatting

Implement the dot-path tracker so errors report `"user.address.zip"` rather
than a bare message. Add `schema-error-message` helper that formats a
`Vec<SchemaError>` into a human-readable string.

Fixture: `schema-decode-errors` -- nested schema with multiple violations;
assert the error paths are correct.

### SC4 -- Recursive schemas (`schema/rec`)

Implement `schema/rec` using a thunk / lazy cell so schemas can refer to
themselves. The thunk is forced on first decode.

Fixture: `schema-decode-recursive` -- a tree schema, three levels deep.

### SC5 -- Typed decode: `schema-decode-into`

Compiler-assisted extension. `schema-decode-into` takes a schema and a
`defstruct` type name and returns a value of the struct type rather than
`ptr<void>`. Requires the elaborator to:

1. Look up the `defstruct` field list.
2. Match schema keys to field names (exact match by default; `#[json-name]`
   annotation for custom mapping).
3. Emit field-type-checked extraction calls rather than untyped `hamt-get`.

This is the bridge to `#json<MySchema>(...)` in `json-reader-macro-plan.md`
(JR2). After SC5, `#json<User>({"name": "alice"})` can produce a `User` struct
value with static field types.

### SC6 -- `just docs` + guide

- `;;;` docstrings on every exported defn in `stdlib/schema.tur`.
- `docs/guides/schema-guide.md` covering the decode workflow, error handling,
  and the connection to `#json(...)` and `defstruct`.
- `just docs` regeneration.

---

## Non-goals for SC0--SC6

- **Serialization** -- turning a Turmeric struct back into JSON or a HAMT map.
  Useful, but a separate concern; add as SC7 if wanted.
- **Schema introspection / reflection** -- iterating a schema's fields
  programmatically to generate code or docs. The `schema/rec` thunk design
  does not preclude it, but it is not planned here.
- **SMT-backed static proof** -- `tur/contract` + `tur/refinements` handle
  static checking. Schemas are purely a runtime tool.
- **`schema/coerce`** -- auto-coercing `"42"` to `42` etc. Explicit
  `schema/transform` covers this if needed; implicit coercion is surprising.

---

## Relation to `tur/contract`

| | `tur/contract` | `tur/schema` |
|---|---|---|
| When | Compile-time + runtime | Runtime only |
| Data source | Internal (known types) | External (untyped maps) |
| On failure | Panic | `Result` (error value) |
| Composable | No (macros, not values) | Yes (schemas are values) |
| Type-checked | Yes | Partial (SC5+) |

The two are complementary: use `tur/schema` to validate data at the boundary,
then use `tur/contract` to enforce invariants on the decoded typed value inside
the program.

---

## Future work

- **`schema/coerce`** -- explicit coercion schema for lenient parsing
  (e.g. accept `"true"` for a bool field).
- **Schema serialization** -- write a schema value to JSON so it can be
  published as a machine-readable API contract.
- **`schema-gen`** -- generate random values satisfying a schema, for
  property-based testing. Natural companion to `tur/check` (if a property
  testing library is added).
- **OpenAPI / JSON Schema import** -- given a JSON Schema or OpenAPI spec
  (as a `ptr<void>` HAMT), produce a `tur/schema` value. Enables using
  published API schemas directly without hand-translating them.
