# SR0 results -- the construction census and the migration surface

Instruments for SR0 in [docs/upcoming/sum-representation-plan.md](../../docs/upcoming/sum-representation-plan.md).
Both halves ran. **The gate does not close cleanly either way, and the case for
SR1 as a performance change came back weaker than the plan assumed.**

## SR0(b) -- the migration surface. ANSWERED, and it is not a blocker.

The plan flagged `(.value o)` on a `none` -- which silently reads a zero today
and becomes a partial operation under a real sum -- as the thing that decides
whether SR2 is a week or a month. The `.value` grep count (942 tree-wide) was
useless because `Option` and `Ref` both declare a field of that name.

**Method: use the type checker as the oracle, not grep.** Rename `Option`'s
field in `stdlib/option.tur`, make stdlib self-consistent, then sweep the
corpus: every `no typeclass method found for 'value'` is an Option `.value`
site and nothing else is. `.value` on a `Ref` still resolves, so the separation
is exact rather than heuristic. Repeat for `Result`'s two payload fields.

| | files | code sites |
|---|---:|---:|
| `Option` `.value` | 33 | 39 |
| `Result` `.ok-val` / `.err-val` | 6 | 8 |

**Of all 47 sites, zero rely on reading a zero from the dead arm.** Every one is
either inside an `.is-some` / `.is-ok` guard or reads a value that was
constructed live a few lines above. The four that are not locally obvious are
still provably live -- `(.value (opt-if true))` where `opt-if` returns
`(if b (some 1) (none))`, and `(.value res)` where `res` came from a function
that returns its argument.

So the migration is mechanical: `.value` becomes a match arm or an explicit
`unwrap`. Only the four non-local cases need a human decision, and none of them
needs a semantic change. **This was the risk the plan was most worried about,
and it is not one.**

Every stdlib file other than `option.tur` itself accesses Options through
functions, never raw field reads -- 13 internal sites in `option.tur`, zero
elsewhere in stdlib.

## SR0(a) -- the construction census. ANSWERED, and it argues against SR1.

**Method.** Inject a per-constructor counter into emitted C
(`instrument.py`), compile, run, and dump `ctor<TAB>count<TAB>repr` at exit.
Instrumenting the emitted C rather than adding a codegen flag was deliberate:
the measurement must not perturb the codegen it measures, and the ADT slab work
had already broken every fixture snapshot by emitting into the preamble
unconditionally.

### Fixtures: 2148 attempted, 1970 (92%) built and ran

| | |
|---|---:|
| constructed at least one ADT | 527 |
| constructed nothing | 1440 |
| cc failed | 170 |
| emit-c failed | 8 |

**Depth is unusable and must not be quoted.** Total constructions were 246,080,
but the median fixture constructs **2**, and 98% of the total comes from two GC
stress fixtures deliberately allocating in a loop (`rc-free-queue-deep-cascade`
100,001; `gc-auto-collects-without-gc-call` 80,000). Any percentage taken over
that total describes those two fixtures, not the language. The "Option/Result
are 0.2% of constructions" figure this census first produced is exactly such an
artifact and is withdrawn.

**Breadth, of the 527 fixtures that construct anything:**

| class | fixtures | what it means |
|---|---:|---|
| `product` | 382 | single-variant -- already by value |
| `sum-flat` | 76 | **SR1's customers** |
| `gadt` | 69 | keeps the tagged union by design (`!def->is_gadt`) |
| `heap` | 34 | typed pointer by design |
| `sum-rec` | 16 | **SR4's customers** |

### Examples: the only real programs in-tree

`datalog` constructs almost entirely sums (`StrVal`, `EntityVal`, `LongVal`,
`Var`, `Lit`) -- 94% `sum-flat`. That is the strongest single data point *for*
SR1.

`minikanren` contributes **nothing**: it constructs no ADTs at all. Despite the
name it implements relations with plain ints and loops, not `stdlib/logic.tur`.
Worth recording on its own, because it means the recursive-sum hot path that
motivated the whole allocation report has **no example program exercising it** --
its only measurement is the synthetic `benchmarks/bench-logic-subst.tur`.

`snake` and `guestbook` need external libraries and did not link here.

### Spices (727 `.tur` files, `rjungemann/turmeric-spices` @ 408c6c5)

Spice tests need vendored C headers that only `tur build` resolves, so the
dynamic census does not reach them. The declaration profile is a plain grep and
is reliable:

| form | count |
|---|---:|
| `defopaque` | 244 |
| `defstruct` | 122 |
| `defdata` | **20** |
| `defgadt` | 0 |

| construction | sites |
|---|---:|
| `ok` / `err` | 155 |
| `some` / `none` | 40 |

**Real library code barely uses sums.** Twenty `defdata` in 727 files, and
several of those are single-variant `Roll` Fix-point carriers (`CNode`, `Sx`,
`Re`, `Tpl`) rather than genuine multi-variant sums. Meanwhile `Result` alone is
constructed at 155 sites.

`static-census.py` also exists and reports per-class site counts, but its
numbers are **not** trustworthy and nothing here rests on them: constructor
names are not unique across a multi-repo corpus (stdlib's `Cons` is a `:heap`
defstruct; fixtures and spices declare their own `Cons` variants), so the
name-to-class map takes whichever declaration is scanned first. The caveat is
recorded at the top of that file.

## What this means for the plan

1. **SR1's reach as a malloc-removal is narrow in the ecosystem as it exists.**
   76 fixtures and roughly ten real types across 727 spice files. The
   85%-of-instructions figure in the allocation report came from `logic.tur`,
   which the spices do not use and which no example program exercises.

2. **SR2's reach is much broader, but its win is smaller per use.** `Result` is
   constructed at 155 sites in the spices alone -- but it is already by value,
   so SR2 buys 8 bytes and the dead-arm write, not a malloc.

3. **The confound, stated plainly.** This census measures the ecosystem that
   exists *under today's costs*. A language whose sums malloc and never free
   trains its users toward `defopaque` and `defstruct`, which is exactly the
   distribution observed. Low sum usage is therefore weak evidence that sums are
   unwanted and reasonable evidence that they are currently expensive. The
   census can say what code does today; it cannot say what code would do if
   sums were cheap.

**Recommendation.** Do not start SR1 for performance on this evidence. The
honest case for the sum work is now expressiveness -- the dead-arm default that
forces every `E` to have a zero value -- and SR0(b) shows that case can be
collected cheaply, because the migration surface is 47 sites and none of them
is load-bearing.
