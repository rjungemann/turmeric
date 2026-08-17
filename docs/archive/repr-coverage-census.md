# The representation coverage census (increment 5's precondition)

**Status:** census landed 2026-08-16; **increment 5 does not start.**
**Update, same day:** the coverage hole this census found has been closed --
the fn-param routing is now one named routine (`repr_of_fn_param`) that the
site consults and the census counts, so `thin-fn` is no longer empty and the
matrix has no empty cell at all.  See "Closing the hole" below.
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

*(Update 2026-08-16, after the CPS increment: the `effect-row` class is no
longer thin -- effect-annotated fn params are fat-normalized like every
other nominal fn param, and the 19 rows above now route fat at the call-site
shim/invoke.  "Thin" at THIS gate has always meant "keeps the nominal TY_FN
spelling at elaboration" -- the tyvar-sig rows were already fat-normalized
downstream when this table was measured -- so the falsification verdict
stands: the genuinely-thin remainder is cfnptr / variadic / arity>5.)*

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

## Closing the hole (2026-08-16)

The 2122 decisions the census found outside the decision function -- 24706
counting all fn params, not just the thin ones -- are now inside one.

**`repr_of_fn_param(const Binding *b, const Type *ann)`** encodes the routing
gate set that used to be an inline expression at the decision site.  It takes
a Binding AND an annotation because that is genuinely the domain: `plain`
reads the binding's substructural flags, while the effect row, cfnptr,
variadic, arity, named-tyvar and carrier-safety gates read the type.
`repr_of(type, pos)` cannot answer it, which is exactly what the census
measured.  It lives in `elab_fns.c` because its two gate predicates
(`fn_type_has_named_tyvar`, `fn_type_is_carrier_safe`) are elaboration
predicates defined there; it is declared in `types.h` so the `repr_*` family
still reads from one header.

Two sites migrated from deriving to consulting:

- the fn-param routing itself -- `carrier_ok` is now
  `repr_of_fn_param(pb, ann) == REPR_CARRIER_I64`;
- the fn-value tail walker -- a PARAM leaf's "already fat" test is now
  `repr_of_binding(vb, RESULT) == REPR_FAT_HANDLE`, deleting the derivation
  its shadow had proven redundant.  That shadow is retired by construction,
  the same end state the container-element collapse reached.  A NON-param
  leaf keeps the old test: its representation lives in its initialiser, which
  no binding-only signature can see (the grounding guard).

The tail-walker migration dropped a `fn_param_type_is_fat_normalized` call
site, so the stage-4 ratchet reported `elab_fns.c shrank 3 -> 2` and the
baseline was tightened in the same commit -- the ratchet reading a
consolidation as progress rather than as drift.

**Evidence it changed nothing it should not.** The full `repr-trace` sweep is
**byte-identical across both migrations**: 43058 lines before, 43058 after,
empty diff.  Suite 2599/0, fuzzer seed 9101 clean, no snapshot churn.  For a
pure consolidation an empty trace diff is the whole proof -- the decision
moved, the decisions did not.

**The matrix after** (`fn-param` is the new row; the position's 24706 answers
reconcile line-for-line with the elaboration trace, 14425 fat + 8112 cfnptr +
2122 thin + 47 carrier):

| position | scalar-bits | heap-ptr | byval-agg | boxed-agg | carrier-i64 | fat-handle | thin-fn |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| fn-param | -- | -- | -- | -- | 47 | 14425 | **10234** |

`thin-fn` was the one empty column, and it is empty no longer.  There is now
no empty cell in the matrix, so increment 5 stays closed on stronger evidence
than before: not "the candidate survived its falsification probe", but "there
is no candidate".

Note the granularity difference, deliberately: `ReprForm` folds cfnptr and
thin into `thin-fn`, because both are nominal TY_FN code pointers; the
elaboration trace keeps the finer split for diagnostics.  A census is a
representation-level instrument, not a diagnostic one.

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
