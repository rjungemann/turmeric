# The representation coverage census (increment 5's precondition)

**Status:** census landed 2026-08-16; **increment 5 does not start.**
**Parent:** [representation-consolidation-meta-plan.md](representation-consolidation-meta-plan.md)
(increment 5 -- conditional representation retirement).
**Sibling:** [repr-decision-function-plan.md](repr-decision-function-plan.md)
(increment 4, complete).

## Why this exists

Increment 5 is conditional, and its condition is a measurement nobody had
taken:

> If the decision function shows a form with no remaining (type, position)
> pairs -- the by-value fat struct in-flight form is the likely candidate --
> delete it CPS-style [...]

`TUR_REPR_CENSUS=1` makes `repr_of` print one line per answer, so a corpus
sweep aggregates into a position x form table.  An empty cell is a
**candidate**, never a decision: the archive's most repeated stall verdict is
"load-bearing, not redundant", so a candidate still owes a
redundancy-falsification probe before any code moves (landing checklist item
11).

## The matrix

2028 fixtures, `emit-c --emit-abi-trace`, **257,005 answers**:

| position | scalar-bits | heap-ptr | byval-agg | boxed-agg | carrier-i64 | fat-handle | thin-fn |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| param | 3 | 13587 | 210 | -- | -- | -- | **0** |
| result | 14979 | 10204 | 1579 | -- | 780 | 397 | **0** |
| let-bind | 33215 | 15149 | 427 | -- | 2062 | -- | **0** |
| container-elem | 289 | -- | -- | 67 | 1 | -- | **0** |
| struct-field | 54 | 12 | 42 | 8 | 2044 | 26 | **0** |
| carrier-sink | 4 | 8 | -- | 6 | 161824 | 28 | **0** |

Every form is populated somewhere except `thin-fn`, which is zero in all six
positions.

## The falsification probe: `thin-fn` is NOT redundant

The probe is the one the checklist demands -- its only job is to falsify
"this form is unused" -- and it falsifies it immediately.

In the **same corpus sweep**, elaboration routes **2122 fn parameters onto
the thin representation**:

| reason the param is thin | count |
| --- | ---: |
| `tyvar-sig` | 2088 |
| `effect-row` | 19 |
| `non-scalar-sig` | 15 |

So the thin representation is not merely alive, it is the routing outcome for
two thousand parameters.  `repr_of` returns it zero times because **it is
never asked**: the fn-value axis is decided from per-Binding flags
(`is_poly_fn`, `is_fat`) that elaboration sets, and `repr_of_binding`
short-circuits on those flags before delegating.  A zero in this table means
"no site consults the decision function here", not "no code needs this form".

**Verdict: increment 5 has no candidate.  It does not start.**

## What the census actually found

A coverage hole, which is more useful than a retirement.

The param-position boundary recorded in the increment-4 plan said the fn axis
could not be shadowed by a Type-only signature and was "COVERED, by trace
rather than shadow".  That is still true, and the census puts a number on
what it costs: **2122 representation decisions per corpus sweep are made
outside the decision function.**  Increment 4 consolidated six positions; the
fn axis is the seventh, and it is consolidated only in the sense that one
predicate is shared -- not in the sense that `repr_of` owns the answer.

Two honest limits on this census:

- **It measures consulting sites, not the compiler.**  A form is counted when
  some site asks `repr_of` about it.  That is the right denominator for "does
  the decision function still need this arm" and the wrong one for "does the
  compiler still need this representation".  The `thin-fn` row is exactly
  that distinction, which is why the probe matters more than the table.
- **The plan's guessed candidate is not representable here.**  The by-value
  fat struct *in-flight* form has no distinct `ReprForm` -- it would surface
  as `byval-agg` or `fat-handle` -- so this census cannot speak to it either
  way.  Retiring it needs an instrument that distinguishes in-flight forms,
  which is a different measurement from this one.

## Reusing it

```sh
for d in tests/fixtures/*/; do
  TUR_REPR_CENSUS=1 ./build/tur emit-c --emit-abi-trace "$d/input.tur" 2>>census.txt >/dev/null
done
grep '^repr-census' census.txt | awk '{print $2, $3}' | sort | uniq -c
```

Worth re-running whenever a position is migrated: a cell that empties out is
the signal increment 5 was waiting for, and the same falsification probe
applies before anything is deleted.
