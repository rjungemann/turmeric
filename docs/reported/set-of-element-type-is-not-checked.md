---
title: "`(set-of 1 \"two\")` type-checks as `(Set int)` while holding a cstr"
category: Reported
description: set-of has no homogeneity check, unlike vec-of and hamt-of. A heterogeneous set literal builds, dedupes and answers membership correctly -- and reports its type as (Set int) from the first element, so it passes to a (Set int) parameter while holding a cstr pointer. The runtime behaviour is right; the exported type is a lie.
---

# `set-of` does not check its element type

**Severity: medium.** Nothing miscomputes -- the set builds, dedupes by content,
and answers membership by value on both back ends. What is wrong is the TYPE it
reports: a heterogeneous set claims to be `(Set int)` and crosses an API
boundary as one.

Found while measuring whether Saffron's `#set{...}` needed the same `any` widen
`[...]` and `#map{...}` got (it does not -- see
`tests/fixtures/saffron-set-literal`).

## Repro

Plain Turmeric, no `any`, no Saffron:

```turmeric
(defn takes-int-set [s : (Set int)] : int (set-count s))

(defn main [] : int
  (println (takes-int-set #set{1 "two"}))     ; => 2
  (println (takes-int-set (set-of 1 "two")))  ; => 2
  0)
```

Both lines compile, run, and print `2`, on the compiled path and under
`--interpret` alike. `takes-int-set` has been handed a set holding a `const
char *`.

## Root cause

`vec-of` routes every element through `tur-vec-homog__` and `hamt-of` routes
every value through `tur-map-homog__`; those are what produce

```
error [TUR-E0001]: function 'vec-push!' arg 2: expected tyvar, got cstr
```

for a heterogeneous vector or map literal. `set-of` has no counterpart. It
expands (`stdlib/set.tur`) to a chain of `set-add1`, and `set-add1` resolves
`hash` / `mk-box` / `mk-cmp` at **each element's own type**:

```turmeric
(defmacro set-add1 [s x]
  `(let [__tur_se1__ ~x] (set-add ~s (hash __tur_se1__) __tur_se1__)))
```

So each element is hashed and boxed correctly -- which is why the runtime
behaviour is right -- and nothing ever forces the elements to agree. The
resulting `(Set A)` grounds `A` from the first element and the rest ride the
carrier.

This is the same class as the Vec and Map cases, and the one where the type
system lets it through silently rather than failing loudly.

## Fix directions

1. **Add the missing homogeneity check**, `tur-set-homog__`, the exact twin of
   `tur-vec-homog__` / `tur-map-homog__`. Smallest and most consistent: the two
   sibling literals already do this and the machinery is one `defn` plus a call
   in `set-add-each__`. It makes `(set-of 1 "two")` a diagnostic instead of a
   type lie.
2. **Admit `any` elements** (`Hash[any]` + `MapKey[any]` instances that dispatch
   on the runtime tag) and have a heterogeneous literal infer `(Set any)`. The
   honest type for what the runtime already does, and it is what Saffron would
   eventually want -- but it is a feature (hashing and comparing across types at
   run time), not a lowering, and it should not gate direction 1.

Direction 1 first: it closes the lie now and does not foreclose direction 2,
which would then relax the check for `any` the way the Vec and Map paths do.

## Not this bug

Membership answers by VALUE, not by carrier word: `(set-member? s (hash "two")
"two")` is `true` and `(set-member? s (hash 99) 99)` is `false`, and
`#set{"a" "a"}` is one member (content-keyed, distinct addresses). The
per-element resolution is working; only the type is wrong.
