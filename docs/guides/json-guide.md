# JSON in Turmeric: the `#json(...)` reader macro and `tur/json`

> **Status:** the `#json(...)` reader macro is experimental, opt-in behind
> `-Xjson-reader`. The runtime `tur/json` library is stable.

Turmeric handles JSON two ways:

- **Compile time** -- the `#json(...)` reader macro embeds a verbatim JSON blob
  in source, validates it while compiling, and lowers it to a first-class
  Turmeric value (a HAMT map, a vec, or a scalar). No runtime parse step.
- **Runtime** -- the `tur/json` library (`json/decode`, `json/encode`) parses
  and serializes JSON strings whose contents are not known until the program
  runs.

## The `#json(...)` reader macro

Enable it with the `-Xjson-reader` flag:

```sh
tur -Xjson-reader build   src/app.tur
tur -Xjson-reader emit-c  src/app.tur
tur -Xjson-reader run     src/app.tur
```

The macro uses round parens as the outer fence, so a top-level object reads
without a doubled brace and a top-level array has no doubled bracket:

```turmeric
#json({"a": 1, "b": 2})   ; object
#json([1, 2, 3])          ; array
#json(42)                 ; scalar
```

### What the reader produces

The JSON is parsed at compile time and emitted as the equivalent Turmeric
S-expression, which is then elaborated by the normal typechecker:

| JSON              | Turmeric equivalent                       |
|-------------------|-------------------------------------------|
| `{"a": 1}`        | `(hamt-of (hamt/hash-str "a") 1)`         |
| `[1, 2, 3]`       | `(vec-of 1 2 3)`                          |
| `"hello"`         | `"hello"` (`:cstr` literal)               |
| `42`              | `42` (`:int` literal)                     |
| `3.14`            | `3.14` (`:float` literal)                 |
| `true` / `false`  | `1` / `0` (`:int` literal)                |
| `null`            | `0` (nil sentinel)                        |

Object keys are JSON strings; they are hashed with `hamt/hash-str` just as the
`#map{...}` data literal hashes its keys, so a value is retrieved with the same
hashed key:

```turmeric
(let [m #json({"a": 1, "b": 2})
      k (hamt/hash-str "a")]
  (map-get m k k))            ; => 1
```

Arrays lower to `vec-of`, so the usual `tur/vec` API applies:

```turmeric
(vec-len #json([10, 20, 30]))     ; => 3
(vec-get #json([10, 20, 30]) 0)   ; => 10
```

### Type interplay with `tur/hamt`

The emitted HAMT is monomorphic in its value type (the same constraint as
`#map{...}`): every value in one `#json({...})` object must share a single
Turmeric type, since the map stores `int`-sized slots. Mixing an int and a
string in one object will not typecheck. For genuinely heterogeneous payloads,
decode at runtime with `json/decode` (below) or model the shape with a
`defstruct`.

### Errors

Malformed JSON is reported at compile time with line/column pointing into the
`#json(...)` block:

| Code        | Condition                                   |
|-------------|---------------------------------------------|
| `TUR-E0270` | Malformed JSON inside a `#json(...)` block  |
| `TUR-E0271` | Unexpected EOF inside a `#json(...)` block  |

### Optional type hint (reserved)

The `#json<Type>(...)` form is parsed and the `<Type>` hint is validated as a
type name, but it is currently ignored -- the result is still an untyped HAMT /
vec / literal. The syntax is reserved for a future typed-decoding extension
that would validate the JSON against a named `defstruct` and emit a typed
constructor call.

## Runtime JSON: `tur/json`

When the JSON is not known at compile time (read from a file, a socket, user
input), use the runtime library:

```turmeric
(let [node (json/decode "{\"x\": 1, \"y\": 2}")]
  ...)                        ; node is a heap JSON value tree
(json/encode node)            ; => "{\"x\":1,\"y\":2}"
```

`json/decode` returns a node tree (null / bool / int / float / string / array /
object); see `stdlib/json.tur` for the accessor API. Use the reader macro for
fixed, source-embedded data and `tur/json` for dynamic data.

## When to use which

| Need                                            | Use                  |
|-------------------------------------------------|----------------------|
| Embed a fixed JSON blob, validated at compile   | `#json(...)`         |
| Parse a JSON string only known at runtime       | `json/decode`        |
| Serialize a value to a JSON string              | `json/encode`        |
| Build a collection with computed slot values    | `#map{...}` / `#set{...}` / `[...]` data literals |
