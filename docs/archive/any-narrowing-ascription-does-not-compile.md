---
title: "`(:: any-value T)` does not compile, and answers a raw bit pattern under --interpret"
category: Reported
description: RESOLVED. `::` now refuses an `any` operand at elaboration and names `cast`, on both back ends. It was four different cc errors compiled and four wrong answers interpreted -- plus a fifth behaviour for a union target, which type-checked and silently did not narrow at all. `cast` was always the route that works.
---

# `(:: any-value T)` does not compile; `cast` is the route that works

**RESOLVED 2026-09-08.** `::` refuses an `any` operand at elaboration, with a
diagnostic naming `cast`. One message, identical on both back ends, in place of
four `cc` errors and four wrong answers.
`tests/fixtures/errors/any-narrow-ascription` pins it.

**Severity was medium.** Not a missing capability -- `cast` narrows an `any`
correctly on both back ends, including panicking on a tag mismatch. What was
wrong is that the OTHER spelling neither worked nor said why: a `cc` error with
no Turmeric diagnostic compiled, and a silent wrong answer interpreted.

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

Widened while fixing it -- every concrete target failed, each its own way, and
the interpreter was wrong in four:

| `(:: a T)`, `a` an `any` holding 7.1 | compiled | `--interpret` |
|---|---|---|
| `T = int` | `aggregate value used where an integer was expected` | `4619679907765970534` |
| `T = float` | `aggregate value used where a floating-point was expected` | `7.1` (right by luck) |
| `T = cstr` | `incompatible type for argument 1 of 'puts'` | `7.1` |
| `T = bool` | `used struct type value where scalar is required` | `7.1` |
| `T = Pt` (a struct) | `'tur_tagged_t' has no member named 'x'` | `field access on non-struct (tag 3)` |
| `T = (int \| float)` | type-checks, **silently does not narrow** (result stays `any`) | same |

The interpreter is not "reinterpreting the bits" in general, as the first
account said -- it is TRANSPARENT for most targets, because its `EX_ASCRIBE` arm
coerces only on a tag mismatch, and reinterprets only for `int`. Either way
wrong, and wrong in more ways than one probe showed.

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

## Fix directions -- direction 1 taken

1. **Reject `(:: e T)` when `e : any` at elaboration**, with a diagnostic naming
   `cast`. **Done.** One rule in `elab_types.c`, beside the two refusals that
   already share its shape -- the owning-value rule ("cannot reinterpret an
   owning value ... use rc/ptr") and the int/float ambiguity rule ("`::` between
   an integer and a float kind is ambiguous ... use int->float / bits->float").
   Both refuse a spelling that cannot work and name the one that does; this is
   the third.

   ```
   error: `::` cannot narrow an `any` to int: `::` asserts a REPRESENTATION,
          and an `any` is a two-word tagged value with no single word to
          reinterpret from
   note:  use (cast x int), which reads the tag the `any` is carrying and
          panics if it is not int
   ```

2. ~~**Make `::` on an `any` mean `cast`.**~~ Still rejected, for the reason
   first given: `::` is unchecked everywhere else, and one operand type that
   quietly panics instead is a worse rule than a diagnostic.

### Placement, which the union target decided

The check has to run BEFORE the `ascribed->kind == TY_UNION` branch, not with
the scalar-reinterpret rules further down. That branch calls
`elab_coerce_to_union`, which accepted an `any` operand that is not a member of
the union and left the result typed `any` -- the silent no-op in the table
above. Put the check after it and the union row stays broken; put it before and
all six targets get the one message. Found by re-running the target survey after
the first placement, which is the only reason the union row was not left behind.

### What must NOT be refused

Ascribing an `any` TO `any` is identity and is load-bearing: it is what the
Saffron data-literal widen emits for an element that is ALREADY `any`
(`dl_saffron_widen_elem` wraps every element in `(:: elem any)`, and
`[a b]` over two `any` variables produces exactly that). The rule is therefore
`src == TY_ANY && dst != TY_ANY`, and
`tests/fixtures/saffron-vector-literal` exercises the exempted direction.

`cast` is untouched and remains correct for every target in the table --
scalars, structs and unions -- on both back ends, panicking with
`cast: any holds float, not int` on a tag mismatch. Eight existing
`any-cast-*` fixtures cover it, which is why this change needed only the
negative half.

## Not this bug

`(match a [(:int n) n])` on an `any` is "match: scrutinee must be an ADT type,
got any" on BOTH back ends -- a consistent refusal, not a divergence. The
`elab_structs.c` narrowing path that S5 added handles a `match` on an `any`
whose arms are ADT patterns; a primitive-tag pattern is a different feature and
is not claimed anywhere.
