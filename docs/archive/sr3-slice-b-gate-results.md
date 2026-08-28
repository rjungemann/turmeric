---
title: SR3 slice B (Option niche filling) -- gate results
category: Planning
description: What happened when `(Option P)` was carried as its bare payload pointer behind a seam -- the codegen is small and works, the SR2a graduation dissolved the obstacle the plan feared, and the phase should be shelved anyway because the population it was scoped for is ineligible: Cons's nil IS the null pointer, and String c-names to int64_t.
---

# SR3 slice B -- gate results

> **RESOLVED 2026-08-28 -- the shelving was reversed, by the route this
> document named.** Follow-up (1) below (give `defopaque` over a pointer a
> pointer C spelling) was gated, landed and graduated the next day
> ([results](opaque-pointer-c-spelling-gate-results.md)), which removed the
> `String` disqualification and with it the reason to shelve. Slice B is now
> `--enable=option-niche`, a real experiment rather than the env seam described
> here; see [sr3-option-niche-plan.md](../upcoming/sr3-option-niche-plan.md).
>
> Two things below did not survive the follow-up, and are worth reading as
> errata rather than as fact:
>
> - **"Slice B as built buys 8 bytes per value on a single fixture."** True of
>   the population as it stood; false once `String` became eligible, which is
>   the whole `(Option String)` census this document itself enumerates.
> - **The crossing table is incomplete.** It lists `emit_carrier_bridge` as the
>   one crossing, and that was right for every payload reachable at the time. An
>   inline-C body that BUILDS an Option with `tur_some_ptr` produces the carrier
>   at a `return`, and nothing bridged it -- a silent wrong answer, found only
>   once `String` (the payload people actually construct in inline-C) came into
>   scope. Two more rows: the niche let-binding and the niche call argument.
>
> The `Cons` disqualification stands unchanged -- and recommendation (2) below
> (decide a `:heap` collection's empty value) was assessed 2026-08-28 and
> DECLINED permanently: the tree-wide `option<Cons>` population is one fixture
> that never wraps an empty list, while nil-is-0 is load-bearing in ~60 sites
> including the variadic-rest ABI and every user inline-C walker (where moving
> it breaks silently, not loudly). A per-payload sentinel `None` was also
> priced and rejected: with the carrier `None` being NULL, the word 0 would
> mean `Some(nil)` on one side of a crossing and `None` on the other -- the
> silent-wrong-answer class by design. Full pricing in
> [sr3-option-niche-plan.md](../upcoming/sr3-option-niche-plan.md).

The gate for [sum-representation-plan.md](sum-representation-plan.md) SR3
slice B (`some(p)` carried AS the payload pointer, 16 bytes to 8), run
2026-08-27 immediately after the SR2a graduation, following the method SR1,
SR2 and SR4 used: force the representation behind a seam, count the crossings,
find the blockers, before writing any of the phase.

**Verdict: the codegen is real, small, and correct -- and the phase should be
SHELVED, because the population it was scoped for cannot take the niche.**
Two disqualifications, each fatal to one half of the plan's own census, and
neither one visible from the type system:

- **`Cons`'s empty list IS the null pointer.** `stdlib/list.tur` says so in its
  own header comment ("At runtime, nil is 0") and `(defn tnil [] : int 0)`
  makes it a value. A niche `(Option (Cons T))` would read `(some (tnil))` as
  `(none)` -- a silent wrong answer, not a build error.
- **`String` c-names to `int64_t`, not to a pointer.** Every `defopaque` does
  (`elab_structs.c`: "is_opaque=true ... type_c_name -> int64_t everywhere"),
  whatever its declared `:ptr<void>` underlying type. A niche `(Option String)`
  would therefore be spelled `int64_t` -- byte-identical to the *carrier* form
  of the same type, which is a pointer to a tagged box. The dozens of
  `strcmp(cname, "int64_t")` tests in the emitter would read a niche value as a
  box and dereference the payload's first word as a tag.

The plan's census named `env` (5 spellings), `httpd-string` (5), `args` (2),
`re` (2), `docstrings` (2) -- all `(Option String)` -- plus "`option<vec<...>>`
and `option<Cons ...>` were exactly the shapes the nested-monomorph fix
touched". Every one of those is on one side or the other of those two lines.

**What is left after both:** `option<Vec T>` / `option<Map ...>` /
`option<Set ...>`. In the whole tree that is **one file** --
`tests/fixtures/option-of-tvec-eq` -- against six files carrying `(Option
String)` and one carrying an `(Option (Cons ...))`. Slice B as built buys 8
bytes per value on a single fixture.

## The seam

`TUR_SR3_OPTION_NICHE=1` (env-only, default off, the SR1/SR2/SR4 precedent).
It is in the tree and works end to end. Eight lockstep changes, all small:

| site | change |
|---|---|
| `adt_app_is_niche_option` (types.c) | the predicate: Option-shaped by `adt_ctor_is_null_none`, one concrete type arg, payload on the allowlist |
| `sr3_payload_is_nonnull_pointer` (types.c) | the eligibility rule -- an ALLOWLIST, plus "c-names as a real pointer" |
| `type_c_name` TY_APP arm | a niche `(Option P)` spells P's C name; asked before the by-value arm that would mint the tagged monomorph |
| `emit_registered_adt_app_rec` typedef | skipped -- there is no struct |
| `emit_registered_adt_app_rec` ctors | `Some` is the identity, `None` returns `(P)0` |
| `record_adt_app_ctor_sigs` | the ctor return-type side table, which is what call-site temps are declared from |
| `repr_of` | `REPR_HEAP_PTR`, ahead of the by-value-product arm |
| `adt_app_byval_value_size_bytes` | 8, so the b4box wide-element rule does not heap-box the thing the niche exists to un-box |
| match (emit_expr.c) | routed to the if-chain path; the arm header tests `__scrut == 0` / `!= 0` and `Some`'s binder IS the scrutinee |
| `emit_carrier_bridge` (emit_core.c) | the niche<->carrier crossing: `p ? tur_box_some(p) : 0` out, `c ? tur_opt_value(c) : 0` back |

The emitted result for `(Option (Vec int))`, with no `tur_adt_Option__Vec__int`
typedef emitted at all:

```c
static tur_adt_Vec__int * ctor_None__Vec__int() { return (tur_adt_Vec__int *)0; }
static tur_adt_Vec__int * ctor_Some__Vec__int(tur_adt_Vec__int * _0) { return _0; }
```

## The plan's stated obstacle dissolved, and the graduation is why

The plan held slice B on this reasoning:

> The compiled pipeline's default path is semi-erased: generic bases
> (`unwrap`, `some?`, every instance-method carrier base) receive `(Option A)`
> as one int64 for EVERY `A` and read `->tag` through one shared layout.

That was largely dissolved by the graduation, and the plan predicted why. After
SR2a a concrete Option consumer **specializes**: the probe's `some?` and
`unwrap` compile as `some___spec__bool_tur_adt_Vec__int__(tur_adt_Vec__int *)`
and `unwrap__spec__tur_adt_Vec__int___tur_adt_Vec__int__(tur_adt_Vec__int *)`,
each against the niche representation directly, with no bridge at all. **The
plan's prediction that graduation would make niche filling "a layout decision
inside a monomorph the compiler always sees" was right for the direct-use
subset.**

**It was not right everywhere, and the exception is worth the whole gate.** The
suite found exactly ONE erased crossing that still happens -- a typeclass
`Eq` dictionary, whose `option-eq?` base takes its two Options as int64 and
reads `->tag` -- and it produced a **silent wrong answer**, not a build error.
`option-of-tvec-eq` printed `false` for two equal `(some v)`: the SR2a
arg-spill machinery correctly spilled the value to a stack temp and passed its
address, the base read the low half of the payload pointer as the tag, matched
neither 0 nor 1, fell out of the switch, and returned the result temp's zero
init.

The fix is one chokepoint (`emit_carrier_bridge`, the row added to the table
above) and after it the suite is green under the seam. So the
materialize/dematerialize bridge the plan called for is real, is needed, and is
**one function** rather than the long tail it feared -- because every other
crossing monomorphizes now.

So the reason to shelve is not the one the plan expected. The bridge is one
site; the *population* is the problem.

## The two disqualifications, in detail

### `Cons` -- a nil that is the niche's own bit pattern

The niche claims 0 for `None`. `Cons` already claims it for the empty list, and
the codebase is explicit about it (`stdlib/list.tur:4`, `:10`, and `tnil`).
There is no way to have both.

This is worth stating as a general rule rather than a `Cons` fact: **a niche is
only available where the payload type has not already spent its null.** In a
language whose collections use 0 for empty -- which is a deliberate,
long-standing convention here, not an accident -- pointer payloads are exactly
the types most likely to have spent it.

### `String` -- an opaque newtype that is not a pointer in C

`(defopaque String :ptr<void>)` declares a pointer and lowers to `int64_t`.
That is the "lazy `:int` stand-in" pattern CLAUDE.md warns about, one level
down in the representation rather than in a signature: the declared underlying
type says pointer, the C spelling says integer, and every consumer that asks
"is this the carrier word?" gets yes.

**This is the actionable finding.** If `defopaque T :ptr<void>` lowered to
`void *` (or to a distinct `tur_opq_T *`), `String` would be both non-null and
distinguishable, and the bulk of the census -- `env`, `httpd-string`, `args`,
`re`, `docstrings` -- would become eligible in one change. That is a better
lever than slice B itself, it is independently useful (an opaque handle that
c-names as a pointer stops aliasing every other carrier value at every
`strcmp(cname, "int64_t")` site), and slice B is downstream of it rather than
the other way round.

## Recommendation

**Shelve slice B; keep the seam.** It is default-off, provably inert (full
suite green both ways), and small enough to carry. Three things would change
the answer, in the order they should be taken:

1. **Give `defopaque` over a pointer a pointer C spelling.** Independently
   worth doing; makes `String` eligible; is the whole census.
2. **Decide what a `:heap` collection's empty value is.** If `Cons` nil stopped
   being 0 (a real cost -- the inline-C walk convention depends on it), the
   other half becomes eligible too.
3. **Then re-run this gate.** The seam reproduces everything; the codegen above
   is already written.

Absent (1), slice B is an 8-byte win on one fixture with a hand-maintained
soundness allowlist standing behind it. That is the same trade SR4 measured and
declined -- a real but small representation win against a real maintenance
liability -- reached by a different route.

## Regression cover

None added, deliberately: a default-off seam whose eligible population is one
fixture does not justify a CI harness (the SR2/SR4 seam harnesses exist because
their populations are large). If (1) above lands and this gate is re-run, that
calculus changes with it.
