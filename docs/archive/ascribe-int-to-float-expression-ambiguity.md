# `::` from an int-typed EXPRESSION to a float is ambiguous: convert or reinterpret?

**Severity: medium** -- silently wrong arithmetic in the "convert" reading,
with no diagnostic either way. Split out of
`ascribe-int-to-float-reinterprets` (also archived), whose literal half was
fixed first.

**Status: RESOLVED.** The ambiguous spelling is refused. Neither meaning kept
`::`; both got explicit names. Fix direction 1 and 2 below were the two
candidates, and the resolution takes neither -- see "What shipped".

## The residue

`(:: <int literal> :float)` converts. `(:: <int expression> :float)` used to
bit-reinterpret, so the second repro from the original report was unchanged:

```turmeric
(defstruct Mixed [n : int d : float])
(defn mixedfold [m : Mixed] : float
  (+ (:: (.n m) :float) (.d m)))
;; (mixedfold (Mixed 3 0.25)) => 0.25, expected 3.25
```

The denormal contributes nothing to the sum, so the wrong value reads as a
*dropped* term rather than a garbage one -- which is what makes it easy to
miss.

## Why the literal fix did not extend

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

## Fix directions considered

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

## What shipped: neither reading keeps `::`

The decisive objection to 1 and 2 alike is that **each silently miscompiles
the other's existing code**. Whichever reading won the spelling, every site
written against the loser would keep compiling, keep type-checking, and start
returning a wrong answer with no diagnostic -- which is the exact failure mode
this report exists to close, merely relocated. 3 is the principled answer but
is a representation change to the erased carrier, far out of proportion to
the defect.

So the ambiguous spelling is **refused**, and the author says which they
meant. Both meanings already had, or now have, an ordinary function name;
neither is new syntax:

| meaning | spelling | source |
|---|---|---|
| convert the NUMBER | `(int->float x)` / `(float->int x)` | `stdlib/math.tur` (existing) |
| reinterpret the BIT PATTERN | `(bits->float x)` / `(float->bits x)` | `stdlib/bits.tur` (added here) |

`elab_ascribe` (`src/compiler/elab_types.c`, immediately before the same-size
`EX_REINTERPRET` rule) now emits one error plus two notes when the source and
destination kinds are an integer/float pair in either direction:

```
error: `::` between an integer and a float kind is ambiguous: it could convert
       the NUMBER or reinterpret the BIT PATTERN, and the static types do not
       say which
note: to convert, use (int->float x) -- (load "stdlib/math.tur")
note: to reinterpret the bits, as when reading a float back out of an :int
      carrier slot (a cons cell, a variadic rest list, a HAMT value), use
      (bits->float x) -- (load "stdlib/bits.tur")
```

Both directions are covered: `float->int` / `float->bits` are named when the
ascription goes the other way. `(:: 7.1 :int)` previously produced
`4619679907765970534` silently, which is the same defect mirrored.

**Literals stay exempt and keep converting.** `(:: 7 :float)` is `7.0`. The
author wrote a constant; there is no carried value whose bits could be meant.
That arm sits directly above the new one and is untouched.

### The interpreter agrees now, where it used to diverge

`float->bits` / `bits->float` are registered as turi natives
(`src/turi/interpreter_natives.c`), beside the `int->float` / `float->int`
that were already there. This *closes* a documented compiled-vs-interpreted
divergence rather than adding one: the tagged `TuriValue` model could never
implement a bit-reinterpreting `::`, because a `TURI_INT` cannot be
distinguished as "a genuine integer" from "a carrier holding float bits". Once
the author has said which reading they meant, that ambiguity is gone and the
interpreter can answer exactly -- hand back the other tag over the same 64
bits. `docs/guides/eval-api.md` is updated accordingly.

## Migration

The four measured carrier sites, all in-tree:

- `tests/fixtures/typed-slots/ascribe-reinterpret` -- rewritten around the
  bits pair; the bool round-trip still goes through `::`, since a bool/int
  pair is not affected. Third case switched from an int to a float64 2.25
  round-trip, so all three cases now assert something the new rules reach.
- `tests/fixtures/typed-slots/fmap-float-list` -- `(:: x-bits :float)` ->
  `(bits->float x-bits)`, `(:: result :int)` -> `(float->bits result)`.
- `tests/fixtures/variadic-float-cons-collect` -- three `(:: (list-head ...)
  :float)` -> `(bits->float (list-head ...))`.
- `tests/fixtures/ascribe-bool-to-numeric-prints` -- already migrated by the
  literal fix.

`tests/fixtures/errors/ascribe-int-float-ambiguous` pins the diagnostic on the
`Mixed`/`mixedfold` repro from the top of this report.

`(:: (vec-get fs 0) :float)` in `docs/guides/data-literals-guide.md` was
checked and is **not** affected: `vec-get` on a `Vec[float]` is already typed
`:float`, so that ascription is a float-to-float no-op.

## Verification

- `bash tests/run.sh` -- 2740 passed, 0 failed.
- `bash tests/run-leak-check.sh` -- 59 passed, 0 failed, 1 known-open
  (`weak-upgrade-after-drop`, `inline-c-option-carrier-box-leaks`,
  pre-existing).
- `TUR=./build-jit/tur bash tests/run-jit.sh` -- green.
- `(mixedfold (Mixed 3 0.25))` spelled `(int->float (.n m))` returns **3.25**,
  the value the original report expected.
- The bits pair round-trips 1.5 through an `:int` carrier on both the compiled
  and the interpreted path.

## Guides updated

- `docs/guides/numeric-tower-guide.md` -- new "Crossing between `int` and
  `float`" section with the four-function table, the repro, and why neither
  reading could keep `::`.
- `docs/guides/eval-api.md` -- the `::` value-preserving section, which
  documented the float/int divergence that no longer exists.
- `docs/guides/value-representations-guide.md` -- the concrete-scalar carrier
  trap now names the source-level spellings.
