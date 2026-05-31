---
title: Runtime Schema Validation
category: Data & Libraries
description: Validate untyped boundary data (HTTP bodies, config, IPC) with composable schema values using stdlib/schema.tur, with accumulating, path-tagged errors.
---

# Runtime Schema Validation with `tur/schema`

> **Status:** SC0--SC4 shipped (scalar/object/array/optional/union/literal/
> transform/recursive schemas, accumulating path-tagged errors). The
> typeclass-driven `HasSchema` / generic `decode` layer (SC5) and the
> `Functor`/`Applicative`/`Alternative` combinator instances (SC7) are not
> yet implemented -- see [docs/schema-plan.md](../schema-plan.md).

Turmeric's type system enforces invariants *within* a program. The gap is at
**dynamic boundaries** -- an HTTP response body, a config file, a channel
message -- where data arrives untyped at runtime. `tur/contract` panics on a
violation; manual field extraction gives no error propagation. `tur/schema`
fills the gap with **schemas as first-class values** that **decode** data into
a `Result`, accumulating every violation with a dot-separated field path.

```
                      tur/contract            tur/schema
  When                compile + runtime       runtime only
  Data source         internal (known types)  external (untyped)
  On failure          panic                   Result (error value)
  Composable          no (macros)             yes (schemas are values)
```

## Loading the library

`schema.tur` is not auto-loaded (to keep it out of every program's codegen).
Load it explicitly, alongside `json.tur` -- schema decoding is anchored on the
tagged JSON node representation, because those nodes carry the runtime type tag
(`0=null 1=bool 2=int 3=float 4=string 5=array 6=object`) that makes genuine
"expected `:cstr`, got `:int`" validation possible.

```turmeric
(load "stdlib/json.tur")
(load "stdlib/schema.tur")
```

The input to `schema-decode` is a JSON node, from either `json/decode` (runtime
text) or the `#json(...)` reader macro (compile-time literal).

## Scalar and literal schemas

```turmeric
(schema/str)               ; matches JSON strings, decodes to the :cstr
(schema/int)               ; matches JSON integers
(schema/float)             ; matches JSON floats
(schema/bool)              ; matches JSON booleans
(schema/nil)               ; matches JSON null

(schema/literal-str "active")  ; matches only the exact string "active"
(schema/literal-int 42)        ; matches only the exact integer 42
```

## Decoding: errors are values

`schema-decode` returns a `Result<ptr<void>, Vec<SchemaError>>`. Inspect it
with the helper accessors:

```turmeric
(let [r (schema-decode (schema/int) (json/decode "42"))]
  (if (schema-decode-ok? r)
    (schema-decode-value r)            ; => 42
    (schema-error-message (schema-decode-errors r))))
```

| Helper | Purpose |
|---|---|
| `schema-decode-ok?`     | did decoding succeed? |
| `schema-decode-value`   | the decoded value (on success) |
| `schema-decode-errors`  | the `Vec` of `SchemaError` (on failure) |
| `schema-decode!`        | decode or **abort** with a formatted message (trusted data / tests) |

### What the decoded value is

The decoded value mirrors the schema shape, so you keep working with ordinary
Turmeric values:

| Schema | Decoded value |
|---|---|
| scalar  | the underlying value (`:cstr` ptr / `:int` / float bits / bool) |
| literal | the matched value |
| object  | the validated JSON object node (read fields with `json/get`) |
| array   | a freshly allocated `Vec` of decoded element values |
| optional / union / transform / rec | the inner decoded value |

## Object schemas

A field list mixes `:cstr` keys with schema pointers, which a homogeneous
`Vec` cannot hold, so object schemas are built incrementally:

```turmeric
(defn user-schema [] :int
  (schema/field
    (schema/field (schema/object-new) "name" (schema/str))
    "age" (schema/int)))

(schema-decode (user-schema) (json/decode "{\"name\": \"alice\", \"age\": 30}"))
;; => ok
```

A field whose schema is `(schema/optional ...)` may be **absent** entirely or
present-but-`null`; any other missing field is a `"missing required field"`
error.

## Arrays, optionals, unions, transforms

```turmeric
(schema/array (schema/int))                        ; [1, 2, 3] -> Vec of ints
(schema/optional (schema/str))                     ; null / absent ok
(schema/union (vec-of (schema/int) (schema/str)))  ; first matching arm wins
(schema/transform (schema/int) (fn [x] (* x 2)))   ; decode, then map
```

Union arms are all schema pointers (`:int`), so a plain `(vec-of ...)` works.
`schema/transform` applies its function only when the inner schema succeeds, so
the function never sees invalid data.

## Errors accumulate, with paths

Decoding does **not** fail fast. Every violation is collected, each tagged with
a dot-separated path (`address.zip`) or an array index (`children[0].value`):

```turmeric
(defn address-schema [] :int
  (schema/field
    (schema/field (schema/object-new) "city" (schema/str))
    "zip" (schema/str)))

(defn user-schema [] :int
  (schema/field
    (schema/field (schema/object-new) "name" (schema/str))
    "address" (address-schema)))

(let [r (schema-decode (user-schema)
          (json/decode
            "{\"name\": 42, \"address\": {\"city\": \"NYC\", \"zip\": 10001}}"))]
  (println (schema-error-message (schema-decode-errors r))))
;; name: expected :cstr, got :int
;; address.zip: expected :cstr, got :int
```

Walk the errors programmatically with `schema-error-count`, `schema-error-at`,
`schema-error-path`, and `schema-error-text`. This is the same accumulation
policy as Zod's `.safeParse` -- a complete picture of what is wrong, not just
the first failure.

## Recursive schemas

Self-referential data (trees, nested comments) uses `schema/rec`, which passes
the schema itself in as `self`. The body is forced lazily on first decode and
memoized:

```turmeric
(defn tree-schema [] :int
  (schema/rec
    (fn [self]
      (schema/field
        (schema/field (schema/object-new) "value" (schema/int))
        "children" (schema/array self)))))

(schema-decode (tree-schema)
  (json/decode
    "{\"value\": 1, \"children\": [{\"value\": 2, \"children\": []}]}"))
;; => ok, validated to arbitrary depth
```

A violation deep in the tree still reports a full path, e.g.
`children[0].value: expected :int, got :cstr`.

## Relation to `tur/contract`

The two are complementary: use `tur/schema` to validate data **at the
boundary**, turning untyped JSON into values you trust, then use `tur/contract`
to enforce invariants on those values **inside** the program.

## Not yet implemented

- **`HasSchema` typeclass + generic `decode`** (SC5) -- dispatching the schema
  off a target type, and the `#json-str<T>(expr)` reader-macro family. These
  need return-type-directed typeclass dispatch and reader-table support.
- **`Functor`/`Applicative`/`Alternative` instances** (SC7) -- writing object
  schemas applicatively (`(<$> ->User (field "name" ...) <*> ...)`).

See [docs/schema-plan.md](../schema-plan.md) for the full design and the
rationale behind the Validation (accumulating) semantics.
