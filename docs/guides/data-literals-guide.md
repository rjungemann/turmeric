# Data Literals: `#map{...}`, `#set{...}`, and `[...]`

> **Status:** experimental, opt-in behind `-Xdata-literals`.

Turmeric's data literals give map, vec, and set construction a compact
literal syntax whose slot values are ordinary expressions, evaluated at
runtime in the surrounding scope. They are sugar over the `hamt-of`,
`vec-of`, and `set-of` stdlib macros.

```turmeric
(def payload #map{:name name :age (+ age 1) :active 1})
(def points  [(make-point 0 0) (make-point 1 1) origin])
(def small   #set{1 2 3})
```

## Enabling the feature

The syntax is gated behind a flag so programs that don't use it are
completely unaffected:

```sh
tur -Xdata-literals build   src/app.tur
tur -Xdata-literals emit-c  src/app.tur
tur -Xdata-literals run     src/app.tur
```

Without the flag, `#map{` / `#set{` are not recognized and a bare `[...]`
in expression position keeps its pre-existing meaning (a binding spec only).

## The three forms

| Syntax | Lowers to | Notes |
|---|---|---|
| `[e1 e2 e3 ...]` *(expression position)* | `(vec-of e1 e2 e3 ...)` | element type inferred from the first element |
| `#map{k1 v1 k2 v2 ...}` | `(hamt-of k1' v1 k2' v2 ...)` | keys normalized (see below); last duplicate key wins |
| `#set{e1 e2 e3 ...}` | `(set-of e1 e2 e3 ...)` | duplicate elements collapse |

```turmeric
[1 2 3]                       ; => (vec-of 1 2 3)
#map{:a 1 :b x}               ; => map {:a -> 1, :b -> x}
#set{:literal x (compute)}    ; (compute) evaluated once
```

Slots are arbitrary expressions -- variable references, calls, nested
literals -- and the normal typechecker handles them.

## `[...]` is only a vec in *expression* position

The reader parses every `[...]` as the same vector form. Binding forms
(`defn`/`fn`/`defmethod` parameter lists, and `let`/`loop`/`for`/... binding
vectors) consume that form as a *binding spec* before it is ever treated as a
value. Only a `[...]` that reaches expression position lowers to `(vec-of
...)`. This is the same rule Clojure uses: `[x y]` is always a vector; the
surrounding form decides whether it is read as a binding spec or a value.

```turmeric
(defn add [x :int y :int] :int (+ x y))   ; [x :int y :int] is a param list
(let [a 1 b 2] (+ a b))                   ; [a 1 b 2] is a binding vector
(def v [1 2 3])                           ; [1 2 3] is a vec literal
```

## `#map{...}` keys

Keys must be a **keyword**, **string literal**, or **int literal** -- anything
else (a variable, a call) is rejected at read time with `TUR-E0282`. Computed
keys are intentionally disallowed so a reader can tell keys from values at a
glance.

Keys are normalized to the int key the typed `Map` expects:

- An **int** key passes through unchanged.
- A **keyword** or **string** key lowers to `(hamt/hash-str "name")`, so two
  equal keyword/string keys hash identically (content equality). To look a
  keyword/string key back up, hash it the same way:

```turmeric
(let [m #map{:name 1 :age 2}
      k (hamt/hash-str "name")]
  (map-get m k k))            ; => 1
```

An odd number of slot forms is a `TUR-E0280` read error.

## `#set{...}` elements and hashing

Set elements are arbitrary value expressions (no key-form restriction). Each
element is used as **its own hash** -- the identity-hash convention the typed
`Set[A]` uses everywhere (`(set-add s 42 42)`). Scalar element types (int,
etc.) dedupe by value:

```turmeric
#set{1 1 2}        ; => set with two elements, {1, 2}
#set{x (+ x x) y}  ; each element expression evaluated exactly once
```

> **Note on the `Hash[A]` typeclass.** The original design sketched injecting
> a `(hash x)` typeclass call per element. The `Hash[A]` method does not
> monomorphize in the current compiled codegen, so `set-of` uses identity
> hashing instead (matching all existing `Set[A]` usage). Keyword *values*
> are not runtime values in Turmeric, so `#set{:a :b}` does not type-check;
> use scalar elements.

## Empty literals

```turmeric
#map{}   ; => empty Map
#set{}   ; => empty Set
[]       ; => empty Vec  (expression position)
```

## Errors

| Code | Condition |
|---|---|
| `TUR-E0280` | Odd number of slot forms in `#map{...}` (unmatched key) |
| `TUR-E0281` | Unexpected EOF inside `#map{...}` or `#set{...}` |
| `TUR-E0282` | Invalid key form in `#map{...}` (must be keyword, string, or int literal) |
| `TUR-E0283` | Unknown `#<tag>{...}` dispatch tag (only `#map{`/`#set{` are defined) |

## Relationship to the JSON reader macro

The data literals supersede the [JSON reader-macro
plan](../json-reader-macro-plan.md) for the common "literal shape, computed
values" case: `#map{...}` already accepts arbitrary value expressions, which a
JSON-only reader cannot. A `#json(...)` reader would remain useful only for
pasting a literal JSON blob verbatim; for everything else, write the map
literal directly.

## Sweet-expression interaction

The reader dispatch sits below the sweet-expression layer, so the literals
work transparently inside `#lang sweet-exp` files alongside neoteric calls
and curly-infix arithmetic:

```turmeric
#lang sweet-exp

(defn build [] :int
  (let [m #map{:a 1 :b 2}]
    map-count(m)))
```
