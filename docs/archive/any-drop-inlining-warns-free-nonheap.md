---
title: A `defopaque`-over-immediate widened to `any` makes gcc warn "'free' called on a pointer to an unallocated object" in emitted code
category: Reported
description: __tur_any_drop guards its free with a runtime registry lookup (__ti->boxed), and the row for an opaque over an immediate correctly says boxed=0 -- but when gcc inlines the drop at a site whose payload is a constant, it cannot prove the guard is false and warns. The generated program is correct; the warning is noise in a user's build.
---

> **RESOLVED 2026-09-09** via fix direction 2, because direction 1 does not
> reach the filed repro: the drop site's payload is a CALL result (`(boxed)`),
> so the boxed flag is a runtime fact there and only gcc, after inlining the
> callee, ever sees the literal. `__tur_any_drop` is emitted
> `__attribute__((noinline, noclone))` -- `noclone` because with `noinline`
> alone gcc's IPA constant propagation minted a `__tur_any_drop.constprop`
> clone specialised to `7` and warned from inside it. The warning is gone at
> `-O0` and `-O2` (gcc 13.3). `run.sh`'s emitted-C warning ratchet now also
> FAILs on `-Wfree-nonheap-object` (the self-test pattern in
> `check-cc-warn-ratchet.sh` moved with it), and
> `tests/fixtures/any-opaque-immediate-drop-no-warning` is the case that trips
> it without the fix.

# Emitted `any` drop warns `-Wfree-nonheap-object`

**Severity: low.** Cosmetic, and the program is correct -- but it is a scary
warning (`free` on a non-heap object) printed during an ordinary user build, on
code the user did not write.

Found while fixing
[interp-collection-handles-report-as-int](../archive/interp-collection-handles-report-as-int.md).

## Repro (2026-09-07)

```turmeric
(defopaque Route :int)
(defn boxed [] : any (:: 7 Route))
(defn route-num [r : Route] : int (:: r int))

(defn main [] : int
  (let [a (boxed)]
    (if (is? a Route) (println (route-num a)) (println 0)))
  0)
```

```
$ tur run p.tur
In function '__tur_any_drop', inlined from 'main' at p_tur.c:7321:13:
p_tur.c:3857:30: warning: 'free' called on a pointer to an unallocated object '7'
  [-Wfree-nonheap-object]
 3857 |     if (__ti && __ti->boxed) free((void *)(intptr_t)TUR_UNTAG(__v));
7
```

The answer (`7`) is right, and the emitted registry row is right:

```c
{ 8597138650700243088LL, "Route", 0 },     /* boxed = 0 */
```

## Root cause

`__tur_any_drop` is one shared function whose free is guarded by a runtime
lookup. Inlined into a scope whose `any` payload is the literal `7`, gcc
constant-folds the argument but cannot see through `__tur_any_find` to prove
`__ti->boxed` is 0, so it warns about the branch it cannot rule out. The guard
does hold at runtime; nothing is freed.

## Fix directions

1. **Do not emit a drop site at all when the payload's `boxed` flag is known at
   compile time to be 0.** The emitter already computes that flag
   (`emit_type_is_byvalue_adt`) when it interns the id, so a widen whose payload
   is an immediate could skip the drop entirely -- smaller code, and the warning
   disappears because the call does not exist.
2. **Keep the call but make the guard opaque to the optimiser** (e.g. route the
   free through a `noinline` helper). Cheaper, but it hides a real
   constant-folding opportunity rather than taking it.

Direction 1 is better on both counts. Worth checking whether the suite's
`cc-warn` ratchet should grow this warning once fixed -- it does not currently
carry it, because no fixture widens a `defopaque` over an immediate to `any`.
