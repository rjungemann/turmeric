---
title: `is?` and `cast` on an `any` holding a parametric value are broken all four ways -- including "cast: any holds Option, not Option"
category: Archive
description: RESOLVED 2026-09-07. emit_any_type_id keys on type_name, which renders a TY_APP per instantiation ((type-app Option float)), while is?/cast resolve a bare `Option` to the head type -- a different key. Both intern, both display as "Option", so the ids differ while the names collide. `(is? x Option)` is silently false, `(cast x Option)` panics saying a value is not its own type, and the parameterised spellings are rejected by the reader. There is no working narrowing route for a parametric or HKT receiver.
---

# `any` narrowing is broken for parametric receivers

**RESOLVED 2026-09-07.** All three fix directions landed together, because they
are one seam: `is?` and `cast` now share a single target resolver
(`any_narrow_target`, `elab_toplevel.c`), so the two forms cannot drift -- which
matters because `is?` guards a narrowing that `cast` then has to accept.

1. **An applied target resolves through the shared annotation parser.**
   `(is? x (Option float))` / `(cast x (Option float))` go through
   `fn_type_from_form`, so the resolved Type is the SAME `TY_APP` the widen site
   interned and the ids line up. Previously both were rejected outright
   (`'is?' expects a type name as second argument`), which is why there was no
   spelling that worked.
2. **A bare type constructor is a hard error**, naming the arity and showing the
   applied form. The report offered "hard error or match every instantiation";
   the error is the smaller change and is forward-compatible -- relaxing it to
   head-matching later replaces an error, it does not break working code.
   Silently answering `false` is gone, which was the requirement.
3. **The panic no longer names one constructor twice.** A mismatch whose two
   ids share a display name now reads `cast: any holds a different
   instantiation of Option` instead of `cast: any holds Option, not Option`.

**Interpreter parity** was restored in the same change and is a deliberate,
documented limit rather than a fix: a `TuriValue` records the ADT it was built
from, not the instantiation, so `turi_any_target_name` (`eval.c`) head-matches
an applied target. The common test -- "is this an Option" -- agrees on both
paths. Only a test that discriminates two instantiations of the SAME
constructor diverges, and it diverges toward `true`. Returning `false` instead
would have made the whole type-case idiom silently fail under `--interpret`,
which is the worse trade.

**What remains, deliberately.** The message says *that* the instantiations
differ, not *which*: `holds (Option float), not (Option int)` needs a second
per-id table carrying the applied spelling. Widening `type-of` itself to report
`(Option float)` would do it more cheaply but is a user-visible behaviour change
that does not belong inside a bug fix. Not filed as a follow-up -- the message
is actionable now, and this is a nicety.

**Fixtures:** `any-narrow-parametric-roundtrip` (the type-case + `fmap` round
trip, both paths), `any-narrow-parametric-discriminates` and
`any-cast-wrong-instantiation` (compiled-only, per the parity limit above),
`errors/any-narrow-bare-type-constructor` and
`errors/any-cast-bare-type-constructor`. Suites at the fix: `run.sh` 2831/0,
`run-turi.sh` 1926/0.

---

## The original report

**Severity: high.** A **silent wrong answer** (`is?`) and a panic whose message
is self-contradictory (`cast: any holds Option, not Option`), with no working
alternative spelling. `any` is a shipping type and `Option`/`Result` are the
most common parametric values in the language, so the combination is not
exotic.

Found while checking whether the `is?`-narrowing escape hatch -- which works
for monomorphic receivers -- extends to higher-kinded ones, for
[docs/upcoming/saffron-lang-plan.md](../upcoming/saffron-lang-plan.md) D8. It
does not, and the failure is worse than the limitation it was being tested
against.

## All four routes fail

```turmeric
(defn boxed [] : any (Some 7.1))
```

| Attempt | Result |
| --- | --- |
| `(is? x Option)` | **silently `false`** for a value that is an `Option` |
| `(cast x Option)` | **panics**: `cast: any holds Option, not Option` |
| `(is? x (Option float))` | `error: 'is?' expects a type name as second argument` |
| `(cast x (Option float))` | `error: 'cast' expects a type name as second argument` |

And `type-of` reports `"Option"` throughout, so nothing in the language
surface hints at what is wrong:

```
$ tur run h7.tur
Option        # (type-of x)
0             # (if (is? x Option) 1 0)
```

## Root cause

`emit_any_type_id` (`emit_module.c:752`) interns by `type_name(r)`, and its own
comment says why that key renders a `TY_APP` per instantiation:

> `type_name` renders a TY_APP per instantiation ("(type-app Box int)"), so
> `(Box int)` and `(Box float)` are distinct.

The widen site therefore interns `(type-app Option float)`. The `is?`/`cast`
target is written as a bare `Option`, which resolves to the head ADT -- a
**different key**, interned separately. Two ids result:

```c
/* emitted for the h7 probe */
case 1000: return "Option";
case 1001: return "Option";

/* widen: (Option float) */
TUR_TAG(1000, (int64_t)(intptr_t)__tur_box);

/* is? x Option */
if ((TUR_GETTAG(x_1437) == 1001)) { ... }
```

The per-instantiation key is deliberate and correct -- it is what keeps
`(Box int)` and `(Box float)` apart. The bug is that **the display name is not
keyed the same way.** `emit_any_type_id` sets `shown` to `app_def->name` for a
`TY_APP`, so every instantiation and the bare head all render as `"Option"`.
That is what turns a distinguishable id mismatch into an indistinguishable
one, and it is what produces `holds Option, not Option`.

## Fix directions

1. **Make the surface able to say what the id means.** `is?` and `cast` reject
   a parameterised second argument outright (`expects a type name`), so there
   is currently no way to name the thing the widen site interned. Accepting
   `(Option float)` there is the direct fix and makes the ids line up for the
   caller who knows the instantiation.

2. **Decide what a bare head means, and implement it.** `(is? x Option)`
   should either be a hard error ("`Option` is a type constructor; write
   `(Option T)`") or match *any* instantiation. The second is what a reader
   expects and what a dynamic dialect would want, and it is implementable:
   intern the head alongside the instantiation and have the head id match on a
   `shown`-name or head-def comparison rather than id equality. Silently
   answering `false` is the one option that should not survive.

3. **Independently: make `shown` distinguish instantiations**, or at least
   stop two live ids sharing a display name. `cast: any holds Option, not
   Option` is unactionable; `holds (Option float), not (Option int)` is a bug
   report a user can act on. This is worth doing even if (1) and (2) slip,
   because it converts every future instance of this class of confusion into a
   legible message.

A fixture belongs on the round trip specifically -- widen a `(Option float)`
to `any`, then `type-of` / `is?` / `cast` it back -- because `type-of` alone
passes today and is what makes the bug look absent.

## Related

Same area, different root cause: the ids are also assigned in per-TU intern
order, so they disagree across translation units --
[any-type-ids-are-per-tu](any-type-ids-are-per-tu.md). That one is about
*where* an id is minted; this one is about *which key* mints it. Both feed the
same conclusion, that the `any` box id needs one deterministic identity
function rather than a per-site convention.
