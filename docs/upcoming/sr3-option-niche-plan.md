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
disqualification stands; `Cons` is still ineligible and is item (2) of the
original recommendation, untouched.

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

**A declaration is a claim, not a proof, so it is also enforced at runtime.**
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

Corpus 2712 fixtures (`bash tests/run.sh`), Debug build:

| run | result |
|---|---|
| default (experiment off) | 2712 passed, 0 failed |
| `--enable=option-niche` | 2712 passed, 0 failed |

`(Option String)` verified end to end on `httpd-req-string-opt`: no
`tur_adt_Option__String` typedef emitted, identity `Some`, null `None`, expected
output.

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
2. **Static enforcement of `:non-null`**, upgrading the runtime abort to a
   compile-time check where possible -- e.g. flag `(:: <literal 0> T)` into a
   non-null opaque as an error, or lean on `:sealed` to bound who can coerce at
   all. The runtime check stays regardless (inline-C is beyond any static
   check), but a violation the elaborator can see should not wait for runtime.
3. **`Cons` eligibility** -- item (2) of the original SR3 recommendation. It
   costs the inline-C walk convention that depends on nil being 0, which is a
   real price; nothing here changes that calculus.
4. **A size measurement worth the name.** The gate measured correctness, not
   bytes. The claim is 16 -> 8 per value on the eligible population; nobody has
   run SR0(a)'s instrument over it since the population changed.
