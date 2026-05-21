---
title: Uniqueness Types
category: Type Safety
description: `^unique`: at-most-one-reference ownership
---

# Uniqueness Types in Turmeric

Uniqueness types enforce the **at-most-one-reference** discipline: a value with `^unique`
has at most one live reference at any point. Unlike linear types (`^linear`, which enforce
exactly-once usage), a unique value may be dropped freely -- it just cannot be aliased.

Enable uniqueness checking with:

```sh
tur build -Xunique-types myfile.tur
```

---

## The two ownership primitives

| Kind | May be dropped | May be duplicated | Aliasing | Summary |
|---|---|---|---|---|
| `^unique` | Yes | No | No | At most one live reference |
| `^linear` | No | No | No | Must be used exactly once |
| (none) | Yes | Yes | Yes | No restrictions (default) |

`^unique` is the **aliasing axis**: you can drop it, but you cannot create a second
reference to the same value. `^linear` is the **usage-count axis**: you must consume it
exactly once.

`ref<T>` is modelled as `CK_UNIQUE` internally -- uniqueness types make this
explicit in the source language.

---

## Basic usage

### In-place mutation

```turmeric
;; sort! requires exclusive access -- no other reference may exist
(defn sort! [^unique v : (vec int)] : unit
  (in-place-quicksort v))

(defn example [] : unit
  (let [v (vec-new 10)]
    (sort! v)         ;; ownership transferred into sort!
    (println v)))     ;; OK: sort! returned
```

```sweet-exp
;; sort! requires exclusive access -- no other reference may exist
defn sort! [^unique v : (vec int)] : unit
  in-place-quicksort(v)

defn example [] : unit
  let [v vec-new(10)]
    sort!(v)         ;; ownership transferred into sort!
    println(v)       ;; OK: sort! returned
```

### Pipeline of unique operations

```turmeric
;; Each step hands off ownership to the next
(defn buf-write [^unique buf : Buffer data : cstr] : ^unique Buffer ...)

(defn pipeline [^unique buf : Buffer] : ^unique Buffer
  (-> buf
    (buf-write "header")
    (buf-write "body")
    (buf-write "footer")))
```

```sweet-exp
;; Each step hands off ownership to the next
defn buf-write [^unique buf : Buffer data : cstr] : ^unique Buffer ...

defn pipeline [^unique buf : Buffer] : ^unique Buffer
  -> buf
    buf-write("header")
    buf-write("body")
    buf-write("footer")
```

---

## Alias prevention

Under `-Xunique-types`, creating a second binding that refers to the same unique
value is a compile-time error.

```turmeric
(defn modify [^unique ^mut x : int] : unit
  (set! x (+ x 1)))

(defn bad [] : unit
  (let [x 42
        y x]       ;; alias created
    (modify x)))  ;; ERROR TUR-E0200: 'x' is not unique (aliased by 'y')
```

```sweet-exp
defn modify [^unique ^mut x : int] : unit
  set!(x {x + 1})

defn bad [] : unit
  let [x 42
       y x]       ;; alias created
    modify(x)     ;; ERROR TUR-E0200: 'x' is not unique (aliased by 'y')
```

Passing a `^unique` value to a function **transfers ownership** -- the source
binding is consumed, just like a move.

---

## Unique mutable references: `^unique ^mut`

Combine `^unique` with `^mut` for exclusive mutable access. This is the ownership-
transfer equivalent of `&mut T` borrows.

```turmeric
(defn increment! [^unique ^mut n : int] : ^unique int
  (set! n (+ n 1))
  n)
```

```sweet-exp
defn increment! [^unique ^mut n : int] : ^unique int
  set!(n {n + 1})
  n
```

Rules:

- A `^unique ^mut` parameter may mutate the value in place and return a new `^unique T`.
- You cannot create a `^unique ^mut` reference while any `&T` or `&mut T` borrows are live.

---

## Interaction with `rc<T>`

Wrapping a `^unique` value inside `rc<T>` is forbidden -- shared reference counting
would introduce aliases:

```turmeric
(rc/new my-unique-val)  ;; ERROR TUR-E0202: cannot wrap unique value in rc<T>
```

```sweet-exp
rc/new(my-unique-val)  ;; ERROR TUR-E0202: cannot wrap unique value in rc<T>
```

---

## Error codes

| Code | Meaning |
|---|---|
| `TUR-E0200` | Value is not unique -- aliased by another binding |
| `TUR-E0201` | Cannot copy a unique value |
| `TUR-E0202` | Cannot wrap a unique value in `rc<T>` |

Use `tur --explain TUR-E0200` (or any other code) for a detailed explanation and
fix suggestions.

---

## Relationship to other type disciplines

| Feature | Relationship |
|---|---|
| `ref<T>` | Internally `CK_UNIQUE`; `-Xunique-types` makes the constraint explicit |
| `&mut T` | Temporary unique borrow; `^unique` is a permanent ownership transfer |
| `^linear` | Orthogonal: linear controls usage count; unique controls alias count |
| `^affine` | `^affine` (via `-Xsubstructural`) restricts duplication but not aliasing |
| `rc<T>` | Explicitly non-unique -- wrapping a `^unique` value is rejected |

---

## See also

- [Substructural Types Guide](substructural-types-guide.md) -- `^linear`, `^affine`, `^relevant`
- `tur --explain TUR-E0200` -- alias violation
- `tur --explain TUR-E0201` -- copy of unique value
