---
title: SR3 slice B -- Option niche filling (--enable=option-niche)
category: Planning
description: An `(Option P)` over a non-nullable pointer carried AS that pointer -- 16 bytes to 8, `(none)` as NULL, no tag word. Unshelved once a pointer `defopaque` got a pointer C spelling, which is what admits `(Option String)` and with it the whole census. The codegen is done and the corpus is green; what keeps it a prototype is that "P excludes 0" is a hand-maintained allowlist rather than something the type system knows.
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
| `adt_app_is_niche_option` (types.c) | the predicate: Option-shaped by `adt_ctor_is_null_none`, one concrete type arg, payload on the allowlist |
| `sr3_payload_is_nonnull_pointer` (types.c) | the eligibility rule -- an allowlist, plus "c-names as a real pointer" |
| `type_c_name` TY_APP arm | a niche `(Option P)` spells P's C name; asked before the by-value arm that would mint the tagged monomorph |
| `emit_registered_adt_app_rec` | no typedef; `Some` is the identity, `None` returns `(P)0` |
| `record_adt_app_ctor_sigs` | the ctor return-type side table call-site temps are declared from |
| `repr_of` | `REPR_HEAP_PTR`, ahead of the by-value-product arm |
| `adt_app_byval_value_size_bytes` | 8, so the b4box wide-element rule does not heap-box the thing the niche exists to un-box |
| match (emit_expr.c) | the if-chain path; the arm header tests `__scrut == 0` / `!= 0` and `Some`'s binder IS the scrutinee |

plus the crossings between a niche value and a carrier value:

| crossing | site |
|---|---|
| Turmeric value <-> carrier | `emit_carrier_bridge` (emit_core.c): `p ? tur_box_some(p) : 0` out, `c ? tur_opt_value(c) : 0` back |
| inline-C producer -> niche let-binding | emit_expr.c let/letrec (added 2026-08-28) |
| inline-C producer -> niche call argument | emit_expr.c arg loop (added 2026-08-28) |

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

## Why it is a prototype and not on

Not the codegen, which is done. **The eligibility rule.**

The niche claims the bit pattern 0 for `None`, so it is sound exactly when P's
valid values exclude 0 -- and nothing in the type system records that. So
`sr3_payload_is_nonnull_pointer` is an explicit ALLOWLIST:

```
Vec, Map, Set, MutableMap     -- :heap collections; every ctor mallocs a header
String, StringBuilder         -- defopaque over tur_string_from_bytes, which
                                 mallocs unconditionally and has no NULL return
```

plus the structural half of the rule: the payload must c-name as a real pointer,
which is what tells a niche value apart from a carrier box at the emitter's
`strcmp(cname, "int64_t")` sites.

The polarity is the part that makes the allowlist survivable: an unrecognised
payload merely misses the optimisation, where a denylist that missed an entry
would make `(some x)` and `(none)` the same value. But a WRONG entry is still a
silent wrong answer, and `:heap`-ness is provably not the condition -- `Cons` is
`:heap` and ineligible.

**The question graduation has to answer: can non-nullness be DECLARED instead of
listed?** An attribute the checker enforces at the constructor (every `Some`
payload provably non-zero) would replace the allowlist with something a reader
of the type can verify. Until that exists, this is a flag.

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

1. **Non-nullness as a declaration**, replacing the allowlist. This is the
   graduation blocker, and it is a type-system question, not a codegen one.
2. **`Cons` eligibility** -- item (2) of the original SR3 recommendation. It
   costs the inline-C walk convention that depends on nil being 0, which is a
   real price; nothing here changes that calculus.
3. **A size measurement worth the name.** The gate measured correctness, not
   bytes. The claim is 16 -> 8 per value on the eligible population; nobody has
   run SR0(a)'s instrument over it since the population changed.
