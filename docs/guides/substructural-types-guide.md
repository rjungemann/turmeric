# Substructural Types in Turmeric

Turmeric supports three substructural type disciplines via the `-Xsubstructural` flag.
Each discipline restricts which structural rules a value may use:

| Annotation   | May be dropped | May be duplicated | Summary                    |
|--------------|---------------|-------------------|----------------------------|
| `^linear`    | No            | No                | Used exactly once          |
| `^affine`    | Yes           | No                | Used at most once          |
| `^relevant`  | No            | Yes               | Used at least once         |
| (none)       | Yes           | Yes               | No restrictions (default)  |

Enable the system with:

```sh
tur build -Xsubstructural myfile.tur
```

`-Xsubstructural` implies `-Xlinear`.

---

## When to use each discipline

### `^linear` -- use exactly once

Use `^linear` for values that represent exclusive ownership of a resource, where both
dropping the resource unintentionally and aliasing it are errors.

```turmeric
(defn open-file  [path : cstr]             : ^linear FileHandle)
(defn close-file [^linear fh : FileHandle] : unit)

(defn copy-file [src dst : cstr] : unit
  (let [fh (open-file src)]
    ;; fh must be consumed exactly once
    (close-file fh)))
```

Under `-Xsubstructural`, `ref<T>` bindings are automatically inferred as `^linear`
unless another annotation is present. You must explicitly `(drop! r)` or consume
the ref -- it will not be silently freed.

### `^affine` -- use at most once

Use `^affine` when a value may be discarded but must not be aliased. This is
appropriate for one-shot tokens, single-use callbacks, or initialization keys.

```turmeric
(defn initialize [^affine key : EncryptionKey] : unit
  ...)

;; OK: use once
(let [^affine k (generate-key)]
  (initialize k))

;; OK: discard without use (weakening allowed)
(let [^affine k (generate-key)]
  0)

;; ERROR TUR-E0150: affine value used more than once
(let [^affine k (generate-key)]
  (initialize k)
  (initialize k))
```

### `^relevant` -- use at least once

Use `^relevant` for values that must be observed but may be inspected multiple times.
This is appropriate for audit logs, mandatory acknowledgements, or results that
must not be silently discarded.

```turmeric
(defn log-and-store [^relevant msg : str] : unit
  (log msg)     ;; first use (duplication OK)
  (store msg))  ;; second use

;; ERROR TUR-E0151: relevant value dropped without use
(let [^relevant msg "important event"]
  0)
```

---

## Stdlib macros

### `(must-use expr)`

Wraps `expr` so that the binding holding its value is inferred as `^relevant`.
The bound value must be used at least once before its scope exits.

```turmeric
;; Under -Xsubstructural:
(let [r (must-use (acquire-resource))]
  (process r)   ;; OK -- ^relevant allows duplication
  (process r))

;; ERROR TUR-E0151: relevant value 'r' dropped without use
(let [r (must-use (acquire-resource))]
  0)
```turmeric

### `(with-resource [name init] body...)`

Scoped resource binding -- equivalent to `(let [name init] body...)`.
Useful to signal intent that `name` is a resource to be consumed within `body`.
When combined with `-Xsubstructural` and a `ref<T>` init, the binding is inferred
as `^linear` and must be explicitly consumed (via `(drop! name)` or a consuming call).

```turmeric
;; Under -Xsubstructural:
(with-resource [r (ref 42)]
  (drop! r))    ;; must consume the linear ref
```

---

## Error codes

| Code        | Meaning                                      |
|-------------|----------------------------------------------|
| `TUR-E0100` | Linear value dropped without being consumed  |
| `TUR-E0101` | Linear value used after being consumed       |
| `TUR-E0102` | Linear value captured by a closure           |
| `TUR-E0150` | Affine value used more than once             |
| `TUR-E0151` | Relevant value dropped without being used    |

Use `tur --explain TUR-E0150` (or any other code) for a detailed explanation
and fix suggestions.

---

## Interaction with other features

### `ref<T>` and linear inference

Under `-Xsubstructural`, all `ref<T>` let bindings and `:ref` parameters are
automatically inferred as `^linear`. You do not need an explicit `^linear`
annotation -- but you must explicitly consume the ref.

```turmeric
(defn consume [r :ref] :int
  (drop! r)
  0)
```

### Affine vs. move semantics

Turmeric's ownership model is affine by default for move types (`CK_UNIQUE`):
values can be dropped implicitly but cannot be aliased. The explicit `^affine`
annotation makes this restriction visible in the type signature, enabling
the elaborator to catch double-use at compile time.

### Pattern matching

Substructural annotations are checked at each match arm independently.
A `^linear` value bound inside a match arm must be consumed within that arm.

```turmeric
(match opt
  (some v) (let [^linear r (ref v)]
             (drop! r))   ;; must consume before arm exits
  none     0)
```

---

## See also

- [Uniqueness Types Guide](uniqueness-types-guide.md) -- `^unique` (at-most-one-reference discipline)
- `tur --explain TUR-E0100` -- linear drop without consume
- `tur --explain TUR-E0150` -- affine used twice
- `tur --explain TUR-E0151` -- relevant dropped without use
