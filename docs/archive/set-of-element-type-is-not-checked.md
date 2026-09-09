---
title: "`(set-of 1 \"two\")` type-checks as `(Set int)` while holding a cstr"
category: Archive
description: set-of has no homogeneity check, unlike vec-of and hamt-of. A heterogeneous set literal builds, dedupes and answers membership correctly -- and reports its type as (Set int) from the first element, so it passes to a (Set int) parameter while holding a cstr pointer. The runtime behaviour is right; the exported type is a lie.
---

# `set-of` does not check its element type

**RESOLVED 2026-09-09**, in the corrected order (direction 2, then 1):

- **`Hash [any]`** (`stdlib/typeclass-hash.tur`) and **`MapKey [any]`**
  (`stdlib/map.tur`) delegate by runtime tag: each `is?` arm narrows `x` to its
  payload type and calls THAT type's instance, so an `any` holding `"two"`
  hashes and keys exactly as a bare `"two"` (content), an `any` holding `7.1`
  as a bare `7.1` (bits / value comparator), and any other payload by its type
  name (correct, coarse). Pure Turmeric, so both back ends agree without a
  native. The runtime HAMT keeps a comparator PER ENTRY, which is what makes
  mixing them in one set sound -- and is precisely how the unchecked
  heterogeneous `set-of` already behaved. A `definstance` over `any` is
  accepted; these are the first in the tree.
- **The check.** `set-add1` is now a call to the constrained typed
  `set-add-elem__ [^Hash A ^MapKey A] [s : (Set A) x : A]` -- the `vec-push!`
  shape -- so `(set-of 1 "two")` is a TUR-E0001 at the cstr, and `(set-of
  (:: 1 any) (:: "two" any))` is an honest `(Set any)`.
- **A latent use-after-move.** `set-add` / `set-remove` / `set-member?`
  spliced the caller's hash expression INSIDE the `let` that had already
  aliased -- moved -- the same element, which was fine while every element
  type was copyable and became TUR-E0201 the moment it was `any`. The hash is
  now bound before the alias.
- **`#set{...}` in a Saffron file** takes the `[...]` widen, reversing the
  plan's "no change" decision: the check made the widen necessary and the
  instances made it possible, with byte-identical behaviour.

Fixtures: `tests/fixtures/set-of-any-elements` (construction, dedup through the
box, membership by `any` and by bare value, a `(Set any)` parameter),
`errors/set-of-heterogeneous` (the report's repro, now a diagnostic), and
`saffron-set-literal` (header rewritten; output unchanged). Compiled and
interpreted suites green. Keying a MAP by `any` is unexplored: `#map{}` still
normalizes keys to one type first.

---

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

**CORRECTION 2026-09-08: direction 1 is NOT first, and cannot be alone.**

That ordering was written before `tests/fixtures/saffron-set-literal` existed.
That fixture pins a HETEROGENEOUS `#set{1 "two" 7.1}` building, deduping and
answering membership on both back ends -- which is exactly what direction 1
would turn into a diagnostic. Landing it alone would be a regression for the
dialect this work is for, trading a type lie for a lost capability.

The two are not independent, because a set has no honest heterogeneous type
until `any` is one: `(Set any)` refuses today for want of `Hash[any]` and
`MapKey[any]`. So the order is direction 2, THEN direction 1 relaxed for `any`
-- the shape the Vec and Map paths already have, where the homogeneity check
stands and `any` is the element type that satisfies it.

Direction 1 on its own remains right for a language with no dynamic dialect. It
is not right for this one, and the report said otherwise for a day.

## Not this bug

Membership answers by VALUE, not by carrier word: `(set-member? s (hash "two")
"two")` is `true` and `(set-member? s (hash 99) 99)` is `false`, and
`#set{"a" "a"}` is one member (content-keyed, distinct addresses). The
per-element resolution is working; only the type is wrong.
