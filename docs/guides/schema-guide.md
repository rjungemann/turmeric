---
title: Runtime Schema Validation
category: Data & Libraries
description: Validate untyped boundary data (HTTP bodies, config, IPC) with composable schema values using stdlib/schema.tur, with accumulating, path-tagged errors.
---

# Runtime Schema Validation with `tur/schema`

> **Status:** SC0--SC4 shipped (scalar/object/array/optional/union/literal/
> transform/recursive schemas, accumulating path-tagged errors). SC5 shipped:
> the `HasSchema` typeclass with return-type-directed `decode!` and the
> `#json-str<T>(...)` reader macro. SC7 shipped its combinator layer
> (`always`/`never`/`ap`/`field-of`/`fmap`/`alt`, Validation semantics); the
> `Functor`/`Applicative`/`Alternative` typeclass *instances* remain deferred --
> see "Applicative combinators" below and [docs/schema-plan.md](../schema-plan.md).

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

## Typed decoding with `HasSchema` (SC5)

`schema-decode` hands back the *validated node* (read fields with `json/get!`);
to land directly in a typed value, implement the `HasSchema` typeclass. Its one
method, `decode!`, is **return-type-directed**: the concrete instance is chosen
from the expected type at the use site (an ascription `(:: e T)` or a typed
binding), not from any argument.

```turmeric
(load "stdlib/json.tur")
(load "stdlib/schema.tur")

(defstruct User :copy [name :cstr age :int])

(defn user-schema [] :int
  (schema/field
    (schema/field (schema/object-new) "name" (schema/str))
    "age" (schema/int)))

(definstance HasSchema [User]
  (decode! [node]
    (let [v (schema-decode! (user-schema) node)]
      (make-struct User
        (json/get-string (json/get! v "name"))
        (json/get-int    (json/get! v "age"))))))

;; The ascription drives instance selection; decode! returns a real User.
(let [u (:: (decode! (json/decode body)) User)]
  (.name u))
```

`decode!` returns the typed value directly (a genuine by-value struct -- the
compiler bridges the typeclass dictionary's `int64` carrier ABI back to the
concrete struct at the resolved call site). It **panics** on any schema
violation, like `schema-decode!`; for graceful handling, validate with
`schema-decode` first and branch on `schema-decode-ok?`.

Ascribing `decode!` to a type with no `HasSchema` instance is a **compile-time**
error (`no instance 'HasSchema T'`).

## The `#json-str<T>(...)` reader macro

Under `-Xschema-reader` (which implies `-Xjson-reader` and auto-loads
`json.tur` + `schema.tur`), `#json-str<T>(expr)` desugars to
`(:: (decode! (json/decode expr)) T)` -- a one-liner from a runtime JSON
`:cstr` to a typed value:

```turmeric
(defn parse-user [body :cstr] :User
  #json-str<User>(body))
```

Unlike `#json(...)`, the inner is an ordinary Turmeric expression (read with the
normal reader), not a verbatim JSON blob. The bare `#json<T>(...)` literal form
now also wraps its node tree in `(:: node T)`.

The panic-on-violation `#json-str<T>` is implemented; the Result-returning
`#json-str?<T>` and file-reading `#json-file<T>` remain future work
(`#json-str?` emits a "not yet implemented" diagnostic).

## Applicative combinators (SC7)

Object decoders compose from smaller pieces with the **Validation applicative**:
`schema/ap` decodes both arms against the same input and **accumulates** their
errors (it does not fail fast), so a malformed object reports every bad field at
once.

| Combinator | Role | Notes |
|---|---|---|
| `schema/always v` | `pure` | always succeeds with `v` (ignores the node) |
| `schema/never msg` | `empty` | always fails with `msg`; identity of `schema/union` |
| `schema/field-of k s` | extract | decode an object, pull key `k` via schema `s` |
| `schema/ap sf sa` | `<*>` | apply the function decoded by `sf` to the value decoded by `sa`, accumulating errors |
| `schema/fmap s f` | `<$>` | map `f` over the decoded value (alias for `schema/transform`) |
| `schema/alt a b` | `<\|>` | two-arm first-match union |

```turmeric
(defn double-it [x :int] :int (* x 2))

;; pure(double-it) <*> field-of("n", int)  -- on {"n":21} => 42
(schema-decode! (schema/ap (schema/always double-it)
                           (schema/field-of "n" (schema/int)))
                (json/decode "{\"n\": 21}"))
```

Decoding `{"a": "x", "b": "y"}` against an `ap` of two `int` fields yields **two**
path-tagged errors (`a: expected :int, got :cstr` and `b: ...`), one per arm.

### Why no `Monad`

The lawful `>>=` for Validation is fail-fast, which contradicts the accumulating
`<*>`. Per the design, `Monad Schema` is deliberately omitted; if you genuinely
need to choose a later schema based on a decoded value, decode in two explicit
steps (decode the discriminant, then dispatch on it) rather than reaching for a
monad. Note also the O(arms) cost: `ap`/`union` decode every arm.

### Deferred: the typeclass instances

The combinators above are plain functions over the int-carrier schema
representation. The `Functor`/`Applicative`/`Alternative` **typeclass
instances** -- so you could write `(<$> f s)` / `(<*> sf sa)` / `(<|> a b)` and
build a struct field-by-field applicatively -- were deferred on three
independent compiler obstacles. **Two are now cleared** (2026-06-01); the third
still blocks the operator sugar.

1. **(CLEARED) HKT dispatch over a parametric wrapper.** A user-defined
   *parametric* struct (`(defstruct Schema [A] (raw :int))`) now reports kind
   `* -> *`, so `(definstance Functor [Schema])` kind-checks and dispatches by
   struct identity like the built-in `rc<T>`. Argument-directed `.fmap`/`.unwrap`
   resolve on a `(Schema a)` receiver. See the `hkt-user-parametric-struct`
   fixture. (The phantom-type-parameter inference half was already done: a
   phantom struct param is inferred from an ascription, e.g.
   `(:: (make-struct Schema 0) (Schema cstr))`.)
2. **(CLEARED for `fmap`/`transform`) Capturing closures in the decoder.**
   `schema/transform` and `schema/fmap` carry a fat-closure handle (a `^fat`
   parameter) and the inline-C decoder invokes it via `TUR_APPLY1`, so a
   *capturing* closure -- not only a top-level function -- can map the decoded
   value (`schema-transform-closure` fixture). The accumulating `schema/ap`
   function-arm still takes a top-level fn (see obstacle 3).
3. **(STILL DEFERRED) By-value aggregate carrier + the typeclass-method closure
   ABI.** Two things still block the `definstance` sugar itself. First, a
   typeclass method receives its closure argument as a `tur_poly_fn_t` `{env,fn}`
   struct, while the `^fat`/decoder path expects the single-int64 fat handle, so
   the instance body cannot hand its closure to a `^fat` combinator without a
   `tur_poly_fn_t -> fat-handle` shim (a closure-ABI change). Second, applicative
   *struct* building ends in a by-value aggregate the int64 decoder carrier
   cannot hold without boxing, and `schema/ap`'s direct C call rules out
   multi-argument currying.

See [docs/schema-plan.md](../schema-plan.md) for the full design and the
rationale behind the Validation (accumulating) semantics.
