---
title: SR3 slice B -- Option niche filling (--enable=option-niche)
category: Planning
description: An `(Option P)` over a non-nullable pointer carried AS that pointer -- 16 bytes to 8, `(none)` as NULL, no tag word. Unshelved once a pointer `defopaque` got a pointer C spelling, which is what admits `(Option String)` and with it the whole census. The codegen is done and the corpus is green; eligibility is a `:non-null` declaration on the payload type (enforced at the Some ctor) for opaques plus a short compiler-warranted list for the heap collections; what keeps it a prototype is that the declaration is runtime-checked, not proven.
---

# SR3 slice B -- Option niche filling

**Status:** prototype, behind `--enable=option-niche`. Introduced 0.41.0,
`expires_at` 0.44.0 (advisory -- it never blocks a release).

Slice B of [sum-representation-plan.md](sum-representation-plan.md) SR3. Slice A
(nullary `None` as the null carrier) shipped default-on 2026-08-27. This is the
other half: `some(p)` carried AS `p`.

```c
/* (Option (Vec int)), with no tur_adt_Option__Vec__int typedef emitted at all */
static tur_adt_Vec__int * ctor_None__Vec__int()                       { return (tur_adt_Vec__int *)0; }
static tur_adt_Vec__int * ctor_Some__Vec__int(tur_adt_Vec__int * _0)  { return _0; }
```

## Why it was shelved, and what changed

The [2026-08-27 gate](../archive/sr3-slice-b-gate-results.md) shelved it -- not
for the reason the plan expected. The codegen was small and correct; the
POPULATION was the problem. Two disqualifications, neither visible from the type
system, between them covered the entire census:

- **`Cons`'s empty list IS the null pointer** ("At runtime, nil is 0" --
  `stdlib/list.tur`), so `(some (tnil))` would read as `(none)`.
- **`String` c-named to `int64_t`**, as every `defopaque` did whatever its
  declared `:ptr<void>`, so a niche `(Option String)` was byte-indistinguishable
  from the *carrier* form of the same type -- and the emitter's dozens of
  `strcmp(cname, "int64_t")` sites would read a niche value as a box.

That left `option<Vec T>` / `option<Map ...>` / `option<Set ...>`: one fixture.

**The second disqualification is gone.** `defopaque` over a pointer now c-names
as `void *` ([gate results](../archive/opaque-pointer-c-spelling-gate-results.md),
graduated 2026-08-28), so `String` and `StringBuilder` are eligible and the
`(Option String)` census the plan named -- `env` (5 spellings), `httpd-string`
(5), `args` (2), `re` (2), `docstrings` (2) -- is in scope. The first
disqualification stands; `Cons` is still ineligible -- and item (2) of the
original recommendation is now closed as DECLINED (see What is left, item 3).

## What the phase is

Eight lockstep codegen changes, all small, all landed:

| site | change |
|---|---|
| `adt_app_is_niche_option` (types.c) | the predicate: Option-shaped by `adt_ctor_is_null_none`, one concrete type arg, payload eligible |
| `sr3_payload_is_nonnull_pointer` (types.c) | the eligibility rule -- `:non-null` declaration / heap-collection list, plus "c-names as a real pointer" (see Eligibility below) |
| `type_c_name` TY_APP arm | a niche `(Option P)` spells P's C name; asked before the by-value arm that would mint the tagged monomorph |
| `emit_registered_adt_app_rec` | no typedef; `Some` is the identity plus the `:non-null` check, `None` returns `(P)0` |
| `record_adt_app_ctor_sigs` | the ctor return-type side table call-site temps are declared from |
| `repr_of` | `REPR_HEAP_PTR`, ahead of the by-value-product arm |
| `adt_app_byval_value_size_bytes` | 8, so the b4box wide-element rule does not heap-box the thing the niche exists to un-box |
| match (emit_expr.c) | the if-chain path; the arm header tests `__scrut == 0` / `!= 0` and `Some`'s binder IS the scrutinee |

plus the crossings between a niche value and a carrier value:

| crossing | site |
|---|---|
| Turmeric value <-> carrier | `emit_carrier_bridge` (emit_core.c): `p ? tur_box_some(p) : 0` out, `c ? tur_opt_value(c) : 0` back -- and the CONCRETE->CARRIER direction passes a recorded-carrier-spelled source through unchanged, so an already-carrier inline-C value is never double-boxed |
| inline-C producer -> niche let-binding | emit_expr.c let/letrec |
| inline-C producer -> niche call argument | emit_expr.c arg loop |
| inline-C producer -> match scrutinee | emit_expr.c if-chain niche arm, bridged before the `__scrut` bind |
| inline-C producer -> constructor argument | emit_expr.c ctor arg loop, ahead of the case-A straddle cast |
| niche value -> escaping container element | `emit_carrier_bridge_escaping` delegates a niche option to the standard bridge (its carrier box is heap-allocated, so it escapes safely) instead of heap-promoting the payload into a bare `P **` cell |

Every row keys on the value's RECORDED emitted C spelling (the localvar side
table), never on its type alone -- a niche-producing Turmeric value is already
the payload and must not be double-bridged.  Audited clean with no change
needed: closure captures (captures are variables; the let-binding bridge has
normalized the value by then).  Unreachable: variadic rest (a rest annotation
cannot be a type application, so the checker rejects `& rest : (Option
String)` before codegen).  All pinned by
`tests/fixtures/option-niche-crossings`.

The last two are the hole the first gate could not have found. An inline-C body
declared `: (Option String)` builds its result with the preamble's typed
builders:

```c
if (n == 0) return tur_none();
return tur_some_ptr(tur_string_from_bytes("hi", 2));
```

`tur_some_ptr` returns the CARRIER -- a pointer to a tagged box -- and the
function's C signature is `int64_t` accordingly, which is honest. But a niche
consumer reads that word as the payload, so `(string/to-cstr (unwrap o))`
printed blank. `tur_none()`'s `0` happens to be right; `Some` was not. No
`Vec`/`Map`/`Set` fixture builds an Option in inline-C; `String` is the payload
people actually do that with, which is why it surfaced only once `String` became
eligible. Both crossings now route through the same `emit_carrier_bridge` row
the Turmeric-side crossing uses.

## Eligibility: declared for opaques, listed for the heap collections

The niche claims the bit pattern 0 for `None`, so it is sound exactly when P's
valid values exclude 0 -- and nothing in the type system PROVES that. The
2026-08-28 follow-up answered the "can non-nullness be declared instead of
listed?" question for the half of the population where an author exists to
declare it (`sr3_payload_is_nonnull_pointer`, types.c):

1. **Declared -- opaque newtypes.** `(defopaque String :ptr<void> :non-null)`
   is the author's claim that no producer of the handle returns 0
   (`tur_string_from_bytes` mallocs unconditionally). The attribute is legal
   only over a pointer base -- on an int newtype "non-null" is meaningless, and
   accepting it there would let a later base-type edit keep a stale claim -- and
   it is what makes the type niche-eligible. `String` and `StringBuilder` carry
   it in `stdlib/string.tur`; the former hard-coded name rows for them are gone.

2. **Listed -- compiler-lowered `:heap` collections.** `Vec` / `Map` / `Set` /
   `MutableMap` stay a name list, deliberately: they are defstruct-lowered heap
   ADTs with no author-facing attribute slot, and the compiler ITSELF emits
   their constructors as unconditional mallocs -- a stronger warrant than any
   annotation. Extending the attribute to `:heap` defstructs is possible but
   buys nothing until a user-defined heap type wants the niche.

The polarity is what makes both halves survivable: an unrecognised payload
merely misses the optimisation, where a denylist that missed an entry would
make `(some x)` and `(none)` the same value. And `:heap`-ness is provably not
the condition -- `Cons` is `:heap` and ineligible (its nil IS 0).

**A declaration is a claim, not a proof, so it is enforced twice.** A
violation the elaborator can PROVE -- the literal 0 ascribed in, through any
nesting of relabels -- is TUR-E0303 at compile time and never reaches the
runtime check. Everything else falls through to the runtime half:
Inline-C and the coercing `::` can both smuggle a 0 into a `:non-null` handle
(`String` is not `:sealed`, and sealing it would break its own module's
internal coercions). The niche `Some` ctor therefore checks its payload and
aborts with a message naming the type and the violated declaration -- one
compare on a path that exists to remove a malloc -- instead of letting
`(some null)` silently read back as `(none)`. Pinned by
`tests/fixtures/option-niche-null-payload-aborts` (the abort) and
`tests/fixtures/errors/defopaque-non-null-int-base` (the misuse diagnostic);
the happy path by `tests/fixtures/option-niche-string`.

Known residual: the carrier->niche BRIDGE direction (`c ? tur_opt_value(c) :
0`) extracts whatever the carrier box holds, so a carrier `Some` built around a
null payload in inline-C (`tur_some_ptr(0)`) still degrades to `(none)` at the
crossing -- the bridge is an expression, with nowhere to put the check. The
ctor covers every Turmeric-side construction; closing the bridge direction
means either a checked `tur_opt_value` variant or a statement-form bridge.

## Measurements

Corpus (`bash tests/run.sh`), Debug build, green both ways at every step of
the phase (2712/0 at landing through 2718/0 after the crossing fixes and the
graduation-probe fixtures).

`(Option String)` verified end to end on `httpd-req-string-opt`: no
`tur_adt_Option__String` typedef emitted, identity `Some`, null `None`, expected
output.

**Representation cost, measured 2026-08-28** (SR-family method: 2e6-iteration
loops, wall + ru_maxrss, 3 runs, -O2, shared String payload so only the Option
representation is in the loop):

| workload | default (16B by-value) | niche (8B pointer) | delta |
|---|---|---|---|
| direct positions (construct + `some?` + branch) | 11-14 ms | 2-3 ms | **~5x faster** |
| 2e6 `(Option String)` vec elements | 0.080 s / 79.8 MB | 0.071 s / 79.8 MB | **parity** |

The direct-position number carries a caveat -- at -O2 a one-word value inlines
and registers where a 16-byte aggregate does not, so a synthetic loop
amplifies the gap -- but the direction is real and free.  The container row is
the finding: **the 16->8 headline does NOT apply to container elements as
implemented.** Both representations materialize a heap carrier box at the
erased `vec-push!` boundary (the niche must, or the erased Eq-dictionary
crossing from the first gate comes back), so a `(Vec (Option String))` costs
identical memory either way.  The niche's win is confined to direct positions
-- locals, params, returns, match, struct fields -- which is where the census
population (`env`, `httpd-string`, `args`, `re`, `docstrings`) actually lives,
but those are request-scoped flows, not hot loops.

## The graduation call -- assessed 2026-08-28: HOLD as prototype

Everything a graduation needs is either done or measured, and the measurement
says there is no urgency to flip:

**Done.** All known crossings bridged and pinned; eligibility declared
(`:non-null`) or compiler-warranted; the declaration enforced at three doors
-- TUR-E0303 at elaboration for the provable forge, the niche `Some` ctor for
computed/inline-C construction, and `tur_opt_value_checked` at the
carrier->niche read for a `tur_some_ptr(0)` box (the residual this section
used to carry; closed, pinned by
`tests/fixtures/option-niche-carrier-some-null-aborts`).

**What holds it:**

1. **Defect discovery has not gone quiet.** Five silent-wrong-answer crossings
   were found and fixed in this representation's first two days -- the last
   two by an audit, not by the suite.  Nothing suggests the next probe finds
   zero.  The soak instrument is `tests/run-option-niche-seam.sh` (the SR4
   harness pattern: a canary that fails loudly if the flag stops biting, plus
   the eligible population run under the flag); "no new crossing defects over
   a release cycle" is now a checkable claim, and it should be checked before
   the flip.
2. **Default-on is a semantic break, not just a representation change.** On
   today's default a carrier `Some(NULL)` is a legal, distinct value
   (`tur_some_ptr(0)`; `some?` true).  Under the niche it is an abort at the
   construction or crossing door.  That is the `:non-null` declaration being
   enforced -- but code that never opted in would start aborting, which wants
   a release-notes entry and a deliberate decision, not a default flipped in
   passing.
3. **The measurement removes the urgency.** Parity at container elements and
   a direct-position win that is real but synthetic-loop-amplified is not the
   SR2a shape (3.6x + 71x RSS on real workloads); it is closer to the SR4
   shape, which was measured and deliberately NOT defaulted.

**The flip becomes right when:** the seam harness has run quiet across a
release cycle (0.41), the `Some(NULL)` break has a release-notes entry, and
-- ideally -- end-to-end monomorphization shrinks the erased boundary so
container elements stop boxing under EITHER representation, at which point the
niche's 8-byte word is what lands in the slot and the container row stops
being parity.  `expires_at` 0.44.0 leaves room for exactly that sequence.

## What is left

1. ~~The unbridged inline-C carrier crossings~~ -- **closed 2026-08-28**
   ([archived report](../archive/option-niche-inline-c-carrier-crossings-incomplete.md)):
   match scrutinee and ctor argument bridged, and the audit found and fixed two
   more (the `vec-of` first-element heap-promotion and the `vec-push!`
   double-box); captures clean, rest args unreachable. One ADJACENT finding
   stays open on the DEFAULT path --
   [inline-c-carrier-producer-byval-container-element](../reported/inline-c-carrier-producer-byval-container-element.md)
   (a loud compile error, not a wrong answer; the niche path already handles
   the shape, which is the fix template).
2. ~~Static enforcement of `:non-null`~~ -- **closed 2026-08-28.** A literal 0
   ascribed into a `:non-null` opaque is now **TUR-E0303** at elaboration
   (`ascribe_check_non_null_zero`, elab_types.c, beside the sealed check --
   both are "may this coercion be EXPRESSED" questions). The literal is peeled
   through nested ascriptions/reinterprets, so `(:: 0 :String)` and
   `(:: (:: 0 :ptr<void>) String)` are both caught; a COMPUTED zero is not
   provable and deliberately still compiles -- that, and inline-C, is what the
   runtime Some-ctor abort remains for, and
   `tests/fixtures/option-niche-null-payload-aborts` now smuggles its null
   through a `(zero)` call to pin exactly that split. The literal form is
   pinned by `tests/fixtures/errors/ascribe-zero-into-non-null-opaque`, and
   `tur explain TUR-E0303` carries the full rationale.
3. ~~`Cons` eligibility~~ -- **assessed 2026-08-28: DECLINED, permanently.**
   Item (2) of the original SR3 recommendation, closed by measuring both sides
   of its own trade:

   **The population is one fixture.** A tree-wide census found exactly one
   `(Option (Cons T))` user (`constrained-instance-dispatch-nested-parametric-
   element`, which never wraps an EMPTY list), zero `option<Cons>` APIs in
   `stdlib/list.tur`, and nothing in examples or benchmarks. The original
   census listed `option<Cons ...>` as one of two motivating shapes; the other
   (`(Option String)`) is served, and this one turned out not to exist.

   **The convention is everywhere.** nil-is-0 is load-bearing in ~22 stdlib
   inline-C cons-walker references, ~22 stdlib zero-test sites
   (`tnil?` IS `(= l 0)`), 11 fixtures with hand-written walks, 7
   compiler-emitted conventions (`__tur_cons_cell`, `rest = 0`,
   `g_tur_args = 0`), and the variadic-rest ABI ("nil when absent -- rest =
   0") documented in CLAUDE.md's own arity guide. Moving nil to a non-null
   singleton breaks every user inline-C walker SILENTLY -- `while (p)` derefs
   the sentinel instead of stopping -- which is the mis-run kind of break, not
   the loud compile-error kind the pointer-spelling migration was.

   **The third option -- a per-payload sentinel `None` (`(Cons *)1`) -- was
   priced and is worse than it looks.** It is coherent in isolation, but the
   carrier `None` is NULL (slice A, default-on), so under a sentinel niche the
   word 0 would mean `Some(nil)` on one side of a crossing and `None` on the
   other. Every crossing defect this phase fixed was a case of two sites
   disagreeing about what one word means; a representation where that
   disagreement is BY DESIGN re-introduces the silent-wrong-answer class the
   phase exists to close, and `tur_opt_value_checked` (which aborts on a
   tag-Some/payload-0 box) would need per-payload-class parameterization since
   `Some(nil)` legitimately carries payload 0 at the carrier boundary.

   The exclusion is already structurally safe: `Cons` is neither opaque nor on
   the heap-collection list, so it can never accidentally take the niche, and
   the polarity means it merely misses an optimisation worth 8 bytes per value
   on a population of approximately zero. Verified both ways:
   `(some (tnil))` is a legal, distinguishable value with the experiment on
   and off, and the one census fixture is bit-identical under the flag.
4. **A size measurement worth the name.** The gate measured correctness, not
   bytes. The claim is 16 -> 8 per value on the eligible population; nobody has
   run SR0(a)'s instrument over it since the population changed.
