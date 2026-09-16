# The CPS edge walk still misses most node kinds, and widening it leaks

**RESOLVED 2026-09-16**, in the order the report prescribed: the ownership gap first, then the walk. See Resolution at the end.

**Severity: medium.** Two findings that are only meaningful together: the
call-graph walk that feeds CPS coloring drops edges under most node kinds, and
the obvious fix for that is currently unshippable because the CPS emission path
does not free what the direct path frees.

**Status: open.** Found 2026-09-15 while fixing
[effect-row-lost-through-a-constructor-argument](../archive/effect-row-lost-through-a-constructor-argument.md),
which closed the same hole for `EX_MAKE_STRUCT` / `EX_GET_FIELD` only. The wider
fix was written, measured, and deliberately reverted; this records what it cost
so the next attempt starts from the measurement rather than repeating it.

## Finding 1 -- `cps_collect_calls` has arms for ~30 of ~119 node kinds

`cps_collect_calls` (`src/passes/cps.c`) builds the call-graph edges the
coloring fixpoint propagates along. It is a switch over the kinds that existed
when it was written, with `default:` meaning "no children", so a call under any
later node kind records no edge. `EX_MATCH` is the one that matters most: it has
**no arm**, so a call in a match arm has never contributed an edge.

The sibling walk `cps_directly_uses_control` now falls back to a shared
enumeration (`cps_visit_children`) covering every kind that carries an evaluated
operand, and that one is safe -- it only answers "is a control op in here", so a
missing arm could only produce a false negative. The edge walk is the one still
switching on kinds.

## Finding 2 -- why the same fix on the edge walk cannot land yet

Giving `cps_collect_calls` the same enumerator is a three-line change and it
does not work, for a reason that is not obvious from the diff.

The edge walk sets `has_indirect` for an unresolved callee (CPS0.1 rule 3), so
descending into a node it never descended into before can color a function that
merely calls a callback there. Measured on `tests/fixtures/typed/result-basic`:

| | colored |
| --- | --- |
| `main` | 10 |
| with the enumerator on the edge walk | 23 |

The 13 are `result-map`, `option-map`, `option-eq?`, `main`, three `test-*`
functions and six typeclass instances (`Functor_fmap_Option`,
`Monad_bind_Result_tyvar`, ...) -- because every stdlib HOF calls its callback
inside a `match`, which the walk could not previously see.

That coloring is defensible on its own terms. What makes it unshippable is
**Finding 2 proper**: a newly colored function LEAKS. The direct path emits the
region frees; the CPS path does not.

Direct (uncolored), from the emitted C:

```c
int64_t __ps_330 = (ok(INT64_C(1)));
...
if (__ps_330) tur_region_free((void *)(intptr_t)__ps_330);   /* freed */
```

CPS (colored), same source:

```c
int64_t __ps_330 = (ok(INT64_C(1)));
__t2 = __ps_330;                                              /* never freed */
```

`tests/fixtures/typed/result-basic` goes from clean to
`16 byte(s) leaked in 1 allocation(s)` in `ctor_Result_Ok` / `ok`.

**`bash tests/run.sh` cannot see this** -- it compiles fixture programs without
sanitizers and only compares printed output. Only `tests/run-leak-check.sh`
(and CI's `tur_leak_check`) catches it, which is exactly how it was caught here.

## Why this was not fixed alongside the constructor-argument report

That fix needed two arms, `EX_MAKE_STRUCT` and `EX_GET_FIELD`, and those two
are safe: neither introduces an indirect call the walk did not already see
through the ordinary `EX_CALL` arm, and the coloring on
`tests/fixtures/typed/result-basic` is byte-identical to `main` with them in.
The blanket version is a different change with a different blast radius, and the
leak it exposes is ownership-subsystem work -- the CPS emitter needs the same
drop discipline the direct emitter has -- not a follow-on to a coloring fix.

## Fix directions

1. **Close the CPS ownership gap first.** Until a colored frame frees what an
   uncolored one frees, every widening of the coloring is a leak regression
   waiting to happen, and this is not the only thing that widens coloring.
2. **Then** give `cps_collect_calls` the shared enumerator, and expect the
   coloring to grow substantially -- budget for a large snapshot regen, and run
   `tests/run-leak-check.sh` (not just `run.sh`) as the gate.

An interim that captures most of the value at little cost: add arms for the node
kinds that carry an evaluated operand but CANNOT introduce a new indirect callee
(the aggregate and reader family, the erased wrappers), leaving `EX_MATCH` and
the call-bearing composites for step 2.

## Repro

```sh
bash tests/run-leak-check.sh     # typed/result-basic, with the enumerator on the edge walk
tur check --dump-cps-coloring tests/fixtures/typed/result-basic/input.tur | grep -c COLORED
```

## Resolution (2026-09-16)

Both fix directions, in the order given, and the report's measurements
reproduced exactly first (23 colored on `typed/result-basic`, then
`16 byte(s) leaked in 1 allocation(s)` in `ctor_Result_Ok` / `ok`).

**Finding 2 -- the CPS ownership gap.** The leak was narrower than "the
CPS path does not free what the direct path frees". The direct emitter
frees a fresh sum-carrier box handed to a non-retaining callee by queueing
the argument temp at the hoist (`sum_pending`, with `any_pending` and
`vsp_pending` as siblings) and draining the queue after the consuming call
materialises -- and that drain lives in emit_value's call hoist. On the CPS
path the argument is lowered to its own `CT_LETRAW`, which is delegated to
emit_value, so the queue PUSH still happened; but the consuming call is a
`CT_LETCALL` / `CT_TAILCALL` the CPS emitter writes itself, and no CPS
statement ever drained the queue. The entry sat in `ctx->sum_pending` for
the rest of the program (below every later direct mark, so never freed by
anyone) -- an orphaned drop, not a missing one.

The CPS emitter now carries the same discipline at its own statement
boundary (`emit_cps_ir.c`, the deferred-drop table): `emit_letraw` takes the
three queue marks around its delegation and moves anything pushed into a
per-function table keyed by the binder the value lands in; every CPS
consumer of an atom -- letcall, letprim, the cps->direct tail arm -- fires
the matching entry's drop right after its call statement, spelled on the
ATOM (the binder is in scope in the consuming segment where the direct temp
may not be, once a continuation is lifted). An entry never consumed stays a
status-quo leak, never a free; the table is cleared per emitted function so
a binder id cannot match across functions; a cps->cps tail call cannot free
after itself and is left as before. The emitted line is

```c
if ((__t2)) tur_region_free((void *)(intptr_t)(__t2));
```

right after the consuming `result_map__spec...(__t2, __t3)`, which is the
direct emitter's own shallow free.

**Finding 1 -- the walk.** With the drain in place, `cps_collect_calls`'s
`default:` falls back to `cps_visit_children`, as the report's step 2 says.
`typed/result-basic` colors 23 functions and runs leak-clean under
`tests/run-leak-check.sh`. The stale "DELIBERATELY NOT the shared
enumerator" note at the arm is rewritten to say why it is shared now.
