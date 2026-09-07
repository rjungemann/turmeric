---
title: `is?` and `cast` on an `any` holding a parametric value are broken all four ways -- including "cast: any holds Option, not Option"
category: Reported
description: emit_any_type_id keys on type_name, which renders a TY_APP per instantiation ((type-app Option float)), while is?/cast resolve a bare `Option` to the head type -- a different key. Both intern, both display as "Option", so the ids differ while the names collide. `(is? x Option)` is silently false, `(cast x Option)` panics saying a value is not its own type, and the parameterised spellings are rejected by the reader. There is no working narrowing route for a parametric or HKT receiver.
---

# `any` narrowing is broken for parametric receivers

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
