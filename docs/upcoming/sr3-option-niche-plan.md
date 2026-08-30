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
~~`(Option String)` census the plan named -- `env` (5 spellings), `httpd-string`
(5), `args` (2), `re` (2), `docstrings` (2) -- is in scope.~~ **That census is
wrong, and item 4's measurement is what found it**
([results](../../benchmarks/option-niche-size/RESULTS.md)): `env`, `args` and
`re` are `(Option cstr)`, not `(Option String)` -- verified ineligible against
the emitter, since a `cstr` is a raw `const char *` that cannot carry
`:non-null` -- and the `docstrings` hits are inside string LITERALS, the
documentation text of two httpd functions. `httpd-string` is the only real
row of the five, and it is the one file the emitted census independently
found. The first disqualification stands; `Cons` is still ineligible -- and
item (2) of the original recommendation is now closed as DECLINED (see What
is left, item 3).

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
2. **Default-on is a semantic break, not just a representation change.**
   Filed 2026-08-30 as
   [option-niche-graduation-breaks-carrier-some-null](../reported/option-niche-graduation-breaks-carrier-some-null.md),
   which carries the drafted release-notes entry so the flip is a copy-paste
   rather than a prose exercise.  On
   today's default a carrier `Some(NULL)` is a legal, distinct value
   (`tur_some_ptr(0)`; `some?` true).  Under the niche it is an abort at the
   construction or crossing door.  That is the `:non-null` declaration being
   enforced -- but code that never opted in would start aborting, which wants
   a release-notes entry and a deliberate decision, not a default flipped in
   passing.
3. **The measurement removes the urgency.** Filed 2026-08-30 as
   [option-niche-container-elements-box-at-parity](../reported/option-niche-container-elements-box-at-parity.md).
   Parity at container elements and
   a direct-position win that is real but synthetic-loop-amplified is not the
   SR2a shape (3.6x + 71x RSS on real workloads); it is closer to the SR4
   shape, which was measured and deliberately NOT defaulted.

**The flip becomes right when:** the seam harness has run quiet across a
release cycle (0.41), the `Some(NULL)` break has a release-notes entry, and
-- ideally -- end-to-end monomorphization shrinks the erased boundary so
container elements stop boxing under EITHER representation, at which point the
niche's 8-byte word is what lands in the slot and the container row stops
being parity.  `expires_at` 0.44.0 leaves room for exactly that sequence.

## The container-boxing story -- sketched 2026-08-28

What "container elements stop boxing" would actually take, mapped so the
sentence above is a design constraint rather than a hope.

**Why the box exists.** Not because Vec wants it -- `vec-push!` / `vec-get`
move an opaque int64 word and never interpret it.  The box is the ERASED
BOUNDARY's convention: an element enters through `vec-push!`'s `val : A`
carrier param, and the concrete->carrier crossing materializes the one form
every erased consumer agrees on (the tagged carrier box).  The read side
undoes it (`tur_opt_value_checked` at the `(:: (vec-get v i) T)` ascription).
Both representations pay the same box because both cross the same boundary --
which is exactly why the graduation measurement showed parity.

**Why "just put the word in the slot" is unsound as an interim step.** The
slot convention must be decidable at EVERY site that touches the slot, and
two classes of site cannot decide it:

1. **Erased stores.** A generic body (`(defn push-it [A] [v : (Vec A) x : A]
   (vec-push! v x))`) receives `x` already boxed -- the CALLER boxed it at
   the erased call boundary, before any container was in sight.  If concrete
   stores put bare niche words in the slot while generic-body stores put
   boxes in the SAME vec, one vec holds two conventions and no reader can
   tell them apart.  The compiler cannot see, at the erased call boundary,
   that a value is headed for a container.
2. **Inline-C higher-order bases.** `vec-eq?` (stdlib/vec.tur:513) hands raw
   slot words to its comparator closure from INSIDE its C loop
   (`cmp(a->data[i], b->data[i])`) -- there is no per-element site where a
   compiler bridge could normalize.  The callback's convention IS the slot's
   convention, decided at closure-creation time, possibly in another
   function.

And the convention cannot be carried at runtime instead: the homogeneity
machinery (`tur-vec-homog__`) is a compile-time no-op, and the wide/rc
element predicates are per-monomorph constants baked into emitted glue.  A
per-vec "element form" header flag would be new libturi ABI, a branch in
every push/get/free/eq native, and a second source of truth for a fact the
type system already holds.  Priced and declined.

**So the story is a corollary of end-to-end monomorphization, not a niche
feature.** The invariant -- every site that touches the slot must know the
element form -- is satisfied exactly when every base the element crosses is a
per-monomorph spec: a `vec-push` spec for `(Vec (Option String))` takes
`void *` and stores the word, the `vec-get` spec returns it, and the
comparator handed to `vec-eq?` is compiled against the same monomorph.  The
pieces visibly exist today: SR2a already mints
`some___spec__bool_void__(void *)` / `unwrap__spec__void___void__(void *)`
against the niche form, and the by-value twin redirect (Option C,
emit_module.c) already retargets carrier-helper calls to `*-byval` twins at
call boundaries.  What is missing is the container bases themselves
monomorphizing their STORAGE, which is the end-to-end-monomorphization
program's root-2 work, not this plan's.

**What the niche contributes when that lands:** nothing extra to build.  The
niche monomorph's slot form is already the 8-byte word (repr, ctor sigs, and
specs all say so); the moment container storage is per-monomorph, niche
elements are word-in-slot for free and the measurement's container row stops
being parity -- 8 bytes inline against the default's 16-byte aggregate (or
its box, for wide elements).  That is the point at which the graduation
calculus changes, and it is also why nothing container-shaped should be
built inside this experiment on its own: the interim designs are either
unsound (mixed conventions) or new ABI for a fact monomorphization makes
free.

**Refined into a concrete plan 2026-08-28:**
[container-element-form-plan.md](container-element-form-plan.md) (CE) --
the invariant made enforceable (census the undecidable sites first, then a
loud diagnostic on the residue, never a guess), one chokepoint
(`container_elem_form` beside `repr_of`), Vec-only, scoped INSIDE
`--enable=option-niche` since the form only diverges for niche elements.
Its exit gate is exactly this section's condition: the measurement's
container parity row breaking.

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
4. ~~A size measurement worth the name~~ -- **DONE 2026-08-30.** Instruments
   and full results in
   [benchmarks/option-niche-size/](../../benchmarks/option-niche-size/RESULTS.md).
   Three findings:

   **The per-value claim is exact: 16 -> 8, measured from the emitted
   typedef.** Composition the plan never recorded -- a 4-byte tag, 4 bytes of
   ALIGNMENT PADDING, and the 8-byte payload -- so half of what the niche
   recovers is padding the tag's alignment forces, and the win would not
   shrink if the tag were narrowed to a byte.

   **The eligible population is two monomorphs in eight files**, out of 4736
   Option monomorph instances the corpus emits: `Option__String` (7 files,
   `stdlib/httpd-string.tur` plus 6 fixtures) and `Option__Vec__int` (1
   fixture). Eligibility was decided by the compiler rather than re-derived --
   a monomorph is eligible exactly when its typedef is present by default and
   absent under the flag. Zero inputs emit by default and fail under the flag.
   This is also what corrected the census above.

   **And the size trade is not one-directional.** The niche costs 184-310
   bytes of emitted `.text` per translation unit that uses it, because it must
   ENFORCE what the default representation can simply represent: a null
   payload is a legal value in a 16-byte tagged Option and an impossibility in
   an 8-byte niche one, so the `:non-null` checks are emitted at the `Some`
   ctor and at the carrier crossing. Three ineligible control fixtures are
   byte-identical under the flag (+0), which is an object-code proof of the
   inertness the corpus result asserts at the level of test outcomes.

   No aggregate "bytes saved" figure is produced, deliberately: per-value
   bytes need live values rather than emission sites (a corpus-wide sum would
   be one stdlib body times the file count -- the CE0 trap), and container
   elements are already known to be at exact parity.

5. **`(Option cstr)` is 2.6x the eligible population, and reaching it is an
   API decision rather than a compiler feature.** INVESTIGATED 2026-08-30;
   probes in
   [benchmarks/option-niche-size/probes/](../../benchmarks/option-niche-size/probes/README.md).

   Surfaced by item 4's payload tally: 34 `(Option cstr)` spellings against 13
   `(Option String)` (8 against 2 counting only stdlib API). The plan unshelved
   slice B on the reasoning that making `String` eligible "is the whole
   census"; `String` was made eligible and it is not.

   **`#refine{}` is NOT the key, and the reason is documented rather than
   incidental.** A refinement in type-argument position -- the payload slot of
   a container, which is exactly the position at issue -- is peeled to its base
   with `TUR-W0380` (`rt_peel_type_arg_contract`, elab_types.c:640). It is
   peeled because keeping it is worse: a live `TY_CONTRACT` inside a type
   application makes ordinary uses of the payload fail, since operator lookup,
   overload resolution and return-type checking all compare kinds without
   peeling. The diagnostic names what enforcement would take -- the refinement
   surviving as a type argument down to the unpacking binder, plus a checked
   crossing at the constructor -- and says it is "a real feature, not an
   oversight, and it is not built."

   **`:non-null` on `cstr` is not the key either, and should not be.**
   `opaque_base_is_ptr` admits `ptr` / `ptr<...>` only, and more fundamentally
   `cstr` is `TY_CSTR` -- a builtin TypeKind with no declaration site. But the
   deeper reason is that the claim would be FALSE: `env/get-raw` returns a null
   `cstr` for an unset variable, so "every `cstr` is non-null" is not a fact
   about the type. Nullability here is per-BOUNDARY, not per-type.

   **What does work, today, with no compiler change: a `defopaque` newtype at
   that boundary.** Measured, all four ways:

   ```turmeric
   (defopaque Cstr! :ptr<void> :non-null)

   (defn env-get [name : cstr] : (Option Cstr!)
     (let [v (getenv-raw name)]
       (if (= v 0) (none) (some (:: v Cstr!)))))
   ```

   The monomorph's typedef is emitted by default and ABSENT under the flag --
   it takes the niche -- with identical correct output both ways. A consumer
   pays one `::` ascription to get a usable `cstr` back, the same ceremony
   `String` consumers already pay. And the declaration is enforced on a
   user-defined newtype at both doors with nothing added: `TUR-E0303` at
   elaboration for a literal zero, the ctor abort under the niche for a
   computed one.

   This is not a workaround; it is the granularity the invariant actually has.
   `env/get` already tests its raw pointer against 0 and maps null to
   `(none)`, so the value inside its `Some` is non-null BY CONSTRUCTION and
   the newtype merely writes down what the function already guarantees.

   **And on size grounds it is still not worth doing.** The trade is 8 bytes
   per live value against 184-310 bytes of `.text` per translation unit, on
   functions called a handful of times per program. If these sites are ever
   retyped it should be for the reason section 4 of
   [sum-representation-plan.md](sum-representation-plan.md) gives for the whole
   SR programme -- expressiveness, an invariant the type currently cannot
   state -- and the niche then follows for free. Retyping stdlib's `(Option
   cstr)` API to chase 8 bytes would be a breaking change to a public surface
   bought with a rounding error.
