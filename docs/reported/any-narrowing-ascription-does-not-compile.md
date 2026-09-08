---
title: "`(:: any-value T)` does not compile, and answers a raw bit pattern under --interpret"
category: Reported
description: Narrowing an `any` back to a concrete type with `::` is "aggregate value used where an integer was expected" on the compiled path, and prints the raw carrier word with no diagnostic under --interpret. `cast` is the route that works, and it works correctly on both back ends -- so this is a diagnostic gap, not a missing capability.
---

# `(:: any-value T)` does not compile; `cast` is the route that works

**Severity: medium.** Not a missing capability -- `cast` narrows an `any`
correctly on both back ends, including panicking on a tag mismatch. What is
wrong is that the OTHER spelling neither works nor says why: it is a `cc` error
with no Turmeric diagnostic compiled, and a silent wrong answer interpreted.

Found while measuring
[map-of-any-is-broken-on-both-back-ends](../archive/map-of-any-is-broken-on-both-back-ends.md),
where `(:: (map-get m 1) int)` failed and the Map was not involved.

## Measured

| spelling | tag matches | compiled | `--interpret` |
|---|---|---|---|
| `(cast a int)` | yes | `42` | `42` |
| `(cast a int)` | no (holds float) | `panic: cast: any holds float, not int` | same panic |
| `(:: a int)` | yes | **does not compile** | `42` |
| `(:: a int)` | no (holds float) | **does not compile** | **`4619679907765970534`** |

```turmeric
(defn dyn [x : any] : any x)
(defn main [] : int
  (let [a (dyn 42)]
    (println (:: a int)))
  0)
```

```
/tmp/tur-build/n3_tur.c:7298:13: error: aggregate value used where an integer
                                       was expected
tur: cc invocation failed (status 256)
```

The last row is the one that matters most: an `any` holding `7.1`, read as
`(:: a int)`, prints `4619679907765970534` -- the IEEE-754 bits of 7.1 -- with
no diagnostic anywhere.

## Root cause, as far as it is established

`::` is a REPRESENTATION ASSERTION, not a conversion -- the `EX_ASCRIBE` arm in
`src/turi/eval.c` says so, and on the compiled path it reinterprets the carrier
word bit-for-bit. That is coherent while the operand is a one-word carrier. An
`any` is not: it is the two-word `tur_tagged_t`, so there is no single word to
reinterpret from, and the emitted C hands an aggregate to an integer context.

The interpreter's arm coerces only on a TAG MISMATCH, and a `TURI_FLOAT` read as
`:int` is a mismatch -- so it reinterprets the bits, which is exactly what `::`
means and exactly the wrong answer for an `any`.

So both back ends are doing what `::` says. The defect is that `::` accepts an
`any` operand at all.

## Fix directions

1. **Reject `(:: e T)` when `e : any` at elaboration**, with a diagnostic naming
   `cast`. Cheapest, and it turns two different wrong behaviours into one right
   diagnostic. `any` is the type whose content IS a runtime tag, so an unchecked
   bit-reinterpret out of it is never what the author meant -- if they know the
   tag, `cast` costs one check and panics when they are wrong; if they do not,
   the reinterpret is a bug either way. This is the direction to measure first.
2. **Make `::` on an `any` mean `cast`.** Silently checked rather than refused.
   Rejected on inspection: `::` is unchecked everywhere else, and one operand
   type that quietly panics instead is a worse rule than a diagnostic.

Direction 1 is one `TY_ANY` arm beside the existing ascription checks, plus a
negative fixture under `tests/fixtures/errors/`.

## Not this bug

`(match a [(:int n) n])` on an `any` is "match: scrutinee must be an ADT type,
got any" on BOTH back ends -- a consistent refusal, not a divergence. The
`elab_structs.c` narrowing path that S5 added handles a `match` on an `any`
whose arms are ADT patterns; a primitive-tag pattern is a different feature and
is not claimed anywhere.
