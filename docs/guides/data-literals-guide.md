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

### Element types

`[...]` and the value slots of `#map{...}` accept any scalar element type --
`:int`, `:cstr`, `:bool`, `:nil`, or `:float` -- not just `:int`. All
elements of a vector must unify to one type, and all map values to one type
(a mixed literal such as `[1 "x"]` is a `TUR-E0001` on the offending
element). Elements are stored through an `:int` carrier slot, so reads
recover the type with an ascription:

```turmeric
[1.5 2.5 3.5]                 ; Vec[float]
(:: (vec-get fs 0) :float)    ; => 1.5

#map{1 "one" 2 "two"}         ; Map with :cstr values
(:: (map-get m 1 1) :cstr)    ; => "one"
```

Map keys may be **int**, **keyword**, or **string**. Int and keyword keys are
int-valued (a keyword normalizes to a content hash). A literal whose keys are
all **string literals** lowers to a *content-keyed* map (`smap-of`): two
distinct string pointers with equal text are the same key. See
[`#map{...}` keys](#map-keys) below. Aggregate (multi-word struct/ADT) element
types are not yet supported in these collections.

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

How a key lowers depends on its form:

- An **int** key passes through unchanged (identity-keyed, zero overhead).
- A **keyword** key lowers to `(hamt/hash-str "name")` -- the keyword's content
  hash *is* the int key, so two equal keywords collide deliberately. Look a
  keyword key back up by hashing it the same way:

  ```turmeric
  (let [m #map{:name 1 :age 2}
        k (hamt/hash-str "name")]
    (map-get m k k))            ; => 1
  ```

- A literal whose keys are all **string literals** lowers to `smap-of`, a
  *content-keyed* map. The string itself is the key (compared by content, not
  by pointer or hash), so two distinct pointers with equal text are one key.
  Look string keys up with `smap-get` / `smap-has?` / `smap-dissoc`:

  ```turmeric
  (let [m #map{"name" 1 "age" 2}]
    (:: (smap-get m "name") :int))   ; => 1
  ```

  A direct `(hamt-of "k" v ...)` call with a leading string key is rewritten
  to `smap-of` for the same content-keyed behavior. (Runtime-built string
  keys must outlive the map -- the HAMT does not copy keys; see
  [GMK / TCE4](../typed-collection-elements-plan.md).)

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

### Typed empty literals -- `[]:T` / `#set{}:T`

An empty literal has no element to infer a type from, so a later typed
operation (`.eq?`, `.push!`) can't recover its element type. Pin it with a
fused `:T` element-type suffix immediately after the closer:

```turmeric
[]:int             ; empty Vec[int]
#set{}:cstr        ; empty Set[cstr]
[]:(Vec int)       ; empty Vec[Vec[int]]  (parenthesize a compound element type)
```

The suffix desugars to an ascription on the literal, so `[]:int` is exactly
`(:: (vec-of) (Vec int))` and `#set{}:T` is `(:: (set-of) (Set T))` -- the
generated code is identical. It replaces the verbose
`(:: (vec-new) (Vec int))` scaffolding that empty containers previously
needed:

```turmeric
;; before
(let [a (:: (vec-new) (Vec int))]
  (vec-push! a 1) ...)

;; after
(let [a []:int]
  (vec-push! a 1) ...)
```

Rules:

- The `:` must be **immediately adjacent** to the closing `]` / `}` (no
  intervening whitespace). A space-separated `[] :int` is two forms, not a
  suffix -- this is what keeps binding vectors (`[x :int y :int]`, whose `]`
  is followed by a space) unaffected.
- A `::` after the closer is **not** a suffix (it is the ascription operator).
- The suffix also works on non-empty literals (`[1 2 3]:int`), where it is
  merely a redundant re-statement of the inferred type.
- Missing type after the `:` is a read error: *"expected an element type
  after the ':' container-literal suffix"*.
- `#map{}` has two type parameters (key and value); a typed-empty suffix for
  maps is not yet supported -- ascribe the full `(Map K V)` type instead.

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
