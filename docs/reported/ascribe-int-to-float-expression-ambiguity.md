# `::` from an int-typed EXPRESSION to a float is ambiguous: convert or reinterpret?

**Severity: medium** -- silently wrong arithmetic in the "convert" reading,
with no diagnostic either way. Split out of
`ascribe-int-to-float-reinterprets` (now archived), whose literal half is
fixed.

## The residue

`(:: <int literal> :float)` now converts. `(:: <int expression> :float)` still
bit-reinterprets, so the second repro from the original report is unchanged:

```turmeric
(defstruct Mixed [n : int d : float])
(defn mixedfold [m : Mixed] : float
  (+ (:: (.n m) :float) (.d m)))
;; (mixedfold (Mixed 3 0.25)) => 0.25, expected 3.25
```

The denormal contributes nothing to the sum, so the wrong value reads as a
*dropped* term rather than a garbage one -- which is what makes it easy to
miss.

## Why the literal fix does not extend

`::` int-to-float is genuinely overloaded, and **nothing in the static types
separates the two meanings**:

| meaning | example | what the value holds |
|---|---|---|
| convert | `(:: (.n m) :float)` where `n : int` | a real integer |
| reinterpret | `(:: (list-head c) :float)` | float bits in an int64 carrier |

Both operands are statically `:int`. The reinterpret reading is load-bearing
infrastructure -- typed slots, variadic rest collection, and the cons/HAMT
carriers all round-trip floats through an `:int`-typed slot and read them back
with `(:: ... :float)`. Measured: forcing conversion for every int-to-float
ascription fails exactly 4 fixtures, all of them that carrier path
(`typed-slots/ascribe-reinterpret`, `typed-slots/fmap-float-list`,
`variadic-float-cons-collect`, and the `(:: 1 :float)` line since removed from
`ascribe-bool-to-numeric-prints`).

A literal has no such ambiguity -- there is no carried value whose bits could
be meant -- which is why that half could be fixed on its own.

## Fix direction

The two meanings need distinct spellings; picking which one keeps `::` is the
real decision.

1. **`::` converts; carrier reads get an explicit form.** Matches the
   principle the original report argued from -- Turmeric already spells
   reinterpretation separately under `unsafe`, and `(:: true :int32)`
   *converts*, so `::` reinterpreting in one arm is the odd one out. Cost: an
   audit and migration of every carrier read-back site.
2. **`::` reinterprets; conversion goes through `int->float`.** Smaller
   change, but leaves `(:: true :int32)` converting while
   `(:: some-int :float)` reinterprets -- the inconsistency stays, just
   documented.
3. **Mark carrier-typed values distinctly** so the elaborator can tell them
   apart and keep one surface spelling. Largest change; removes the ambiguity
   at the root rather than renaming around it.

Whichever is chosen, `stdlib/math.tur`'s `int->float` converts correctly today
but is not auto-loaded, so the workaround needs `(load "stdlib/math.tur")`.

## Guides to update when fixed

- docs/guides/types-guide.md (if it documents `::`)
- docs/guides/numeric-tower-guide.md
