# An `any` widen stored into an ADT field has no owner

**PARTIALLY FIXED 2026-09-07.** An `:any` field is now an OWNING field: it sets
`needs_drop_glue`, the by-value drop glue calls `__tur_any_drop` on it, and a
non-escaping local releases it at scope exit through the same
`drop_localowned_<T>(&x)` path the recursive-spine fix introduced (generalised
from `drop_recspine_` to cover both field kinds -- one "free what this stack
local owns, do not free the local" function per type, rather than one per field
kind). `tests/fixtures/any-field-drop` pins it, including the case that must NOT
free: an `any` field holding an int, where the registry row says `boxed = 0`.

`:copy` and `:heap` owners are excluded, for the reason the recursive-field rule
records: drop glue makes a type move-only, and that move discipline is the
single-owner guarantee the free depends on.

**The residue is the same one, and it dominates.** A value handed to a callee is
MOVED, and nothing discharges ownership there -- so `saffron-higher-order`, whose
lists are passed to `lmap`/`lfilter`/`each`, went from 840 bytes in 21
allocations to 800 in 20. One box. The fix closes the non-escaping-local case
and that fixture has almost none. See
[byvalue-recursive-adt-boxes-are-never-freed](byvalue-recursive-adt-boxes-are-never-freed.md)
"Residue 1" for why parameter-side discharge is harder than it looks (a
pattern-match binder aliases the parent, so a callee-side free double-frees).

**FURTHER NARROWED 2026-09-13 -- and this report's TITLE is misleading.**
Attributing each of the fixture's 21 leaks to its allocation site (below) shows
the ADT-field boxes are not the roots at all. They leak *indirectly*, because
the thing holding them leaks. The 9 ROOTS are:

| Count | Kind | Site | Shape |
| --- | --- | --- | --- |
| 5 | Direct | `main` | argument-position widen -- a local re-boxed per call into an `any` parameter |
| 4 | Direct | `lmap`, `lfilter` | return-position widen -- the function's `any` result boxed on the way out |
| 12 | Indirect | field boxes / spine | reachable only from a leaked root |

So fixing the two root producers reclaims the field boxes transitively (the
partial fix above already makes an `:any` field owning, so the drop glue walks
them). Chasing the field case on its own would not have closed this fixture.

**Two missing walk arms were found and fixed, and they were the entry gate, not
the result gate** -- the same lesson the sibling report
[saffron-any-return-defeats-the-frame-box-rule](../archive/saffron-any-return-defeats-the-frame-box-rule.md)
records. `EX_UNION_INJECT` (the widen node) had **no arm** in either
`expr_subtree_has_inline_c` or `box_uses_confined` (both `emit_core.c`), so:

- the inline-C gate answered "may hide inline C" for any body whose result is a
  widen, and `elab_infer_nonretain_masks` skipped **the whole function**; and
- once past that, the retention walk deferred to the strict escape walk, whose
  answer for an unmodelled node is "escapes".

In Saffron that is most functions, because an unannotated return IS `any`. A
one-line bisect shows it: `(defn ignore [xs] 42)` leaks, and the identical
`(defn ignore [xs] : int 42)` does not -- the annotation is the only difference,
and with it the frame-box rule fires and there is no allocation at all.

Both arms are added. Measured effect: 5 of 2311 fixtures change, all Saffron,
each either replacing a `malloc` with a frame-box or adding a `__tur_any_drop`.
Full suite 2972 passed / 0 failed; leak-check 99 passed / 0 failed.

That node has now been the missing arm in **three** walks -- the third is
[cps-coloring-walk-has-no-arm-for-union-inject](../archive/cps-coloring-walk-has-no-arm-for-union-inject.md).
Any new walk over expressions should be checked against it specifically.

**`saffron-higher-order` itself is UNCHANGED at 840/21**, and that is expected:
its four walkers are recursive and pass a match binder (`t`, which aliases the
parent's payload) into the recursive call, so the retention walk refuses --
correctly. That is the pattern-match-alias residue named above, and it is the
only thing still standing between this fixture and zero.

**Severity: medium.** One leaked box per value widened into an `any` FIELD of a
data structure -- so a container with `any` elements leaks once per element,
unbounded in a loop that rebuilds it.

Found by measuring what `tests/fixtures/saffron-higher-order` actually
allocates, after that fixture had been marked against the wrong report.

## Repro

`tests/fixtures/saffron-higher-order` under `tests/run-leak-check.sh`:

```
KNOWN saffron-higher-order -- SUMMARY: AddressSanitizer: 840 byte(s) leaked in 21 allocation(s).
```

Its container is

```turmeric
(defdata Lst [] (Cons [hd : any tl : any]) (Nil))
```

and every allocation is the by-value widen in `elab_coerce_to_any`'s emitted
form, storing a `Lst` into the `tl : any` slot:

```c
tur_adt_Lst *__tur_box = (tur_adt_Lst *)malloc(sizeof(tur_adt_Lst));
*__tur_box = (__ps_197);
TUR_TAG(8921353031923255322, (int64_t)(intptr_t)__tur_box);
```

Counted in the emitted C: **11 widen sites, 0 recursive-carrier sites.** The
fixture was originally attributed to
[byvalue-recursive-adt-boxes-are-never-freed](byvalue-recursive-adt-boxes-are-never-freed.md)
on the assumption that a recursive `Lst` field was being boxed; it is not, and
reading the emitted C rather than the source is what settled it.

## Root cause

`any-struct-box-leak-per-widen` (RESOLVED, `docs/archive/`) gave every `any`
payload box an owner across five passes -- argument position, an owned local, an
owned temporary, past an early exit, and a narrowed binding. Every one of those
owners is a SCOPE. None of them models a box whose lifetime is a DATA
STRUCTURE's: once the widen's result is stored into a field, the box outlives
every scope the passes can attach a drop to, and correctly so.

This is the same shape as the recursive-field box in the sibling report -- an
owning field the ownership model does not name -- with a different producer.
There, the fix was to point the field's `drop_inner_def` at the ADT's own def so
the drop glue frees the box. An `any` field cannot reuse that directly: the
payload's type is whatever the tag says at run time, so the free is
`__tur_any_drop`-shaped (consult the registry row, free if `boxed`) rather than a
call to a statically-named glue.

## Fix directions

1. **Treat an `:any` field as owning.** Set `needs_drop_glue` for an ADT with
   one, and have the emitted glue call `__tur_any_drop(s->field)` -- the registry
   already carries the `boxed` flag that says whether the payload was heap-boxed,
   which is exactly the question. This inherits the recursive-field fix's
   soundness argument: drop glue makes the type move-only, so the field has a
   single owner. It also inherits its two residues -- `:copy` types, and a value
   handed to a callee.
2. **Refcount the `any` box.** Settles it wherever the box ends up, at the cost
   of a count on every widen. `docs/upcoming/saffron-lang-plan.md` S5 proposed
   this for a different leak and it turned out not to be needed there; this is
   the position where it would actually be load-bearing.

Direction 1 is small and shares its machinery with the fix already landed for
the recursive field, so it should be measured first. It is a prerequisite for
S6 (containers of `any`), where an `any` element is the normal case rather than
an unusual one.
