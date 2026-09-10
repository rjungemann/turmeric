---
title: A capturing closure passed through a function-typed type parameter is silently miscompiled and crashes (SIGBUS)
category: Reported
description: When a type parameter is instantiated to a FUNCTION type, a capturing closure travelling through it is emitted into the int64 carrier slot as if it were a bare function pointer. check, emit-c and build are all clean at exit 0; the built program takes SIGBUS with no output. A capture-free lambda in the same position works, which is what makes the shape easy to miss.
---

# A capturing closure through a function-typed type parameter crashes

**Severity: medium-high.** A silent miscompile with no diagnostic from any
phase, ending in a signal rather than a wrong answer -- so it is loud at run
time and invisible before it. The narrowness is what keeps it from being
higher: it needs a type parameter instantiated to a function type, which is
mostly reached through `cata`-shaped code.

**Status: OPEN.** Found 2026-09-09 while verifying an adjacent claim in the
`turmeric-spices` docs (see "Related" below). Confirmed **pre-existing**: a
binary built from `main` at `70f079975` (v0.46.0), with no local changes,
crashes identically.

## Repro

```turmeric
(defn app [A] [f : (fn [int] A) x : int] : A
  (f x))

(defn main [] : int
  (let [g (app (fn [n : int] : (fn [int] int) (fn [s : int] : int (+ s n))) 1)]
    (println (g 5)))
  0)
```

Expected `6`. Observed:

```
$ tur check   repro.tur   ; echo $?     # 0
$ tur emit-c  repro.tur   ; echo $?     # 0
$ tur build   repro.tur -o repro ; echo $?   # 0, no warnings
$ ./repro     ; echo $?
138
```

138 is 128 + SIGBUS. No output at all -- the crash lands before `println`.

## What isolates it

Two one-line variations, and between them they name the trigger exactly.

**1. Drop the capture and it works.** Change the inner lambda's body from
`(+ s n)` to `s`:

```
$ ./repro-nocapture      # prints 5, exit 0
```

**2. Drop the type PARAMETER and it works.** Keep the capture, spell the
function type concretely instead of through `A`:

```turmeric
(defn app2 [f : (fn [int] (fn [int] int)) x : int] : (fn [int] int)
  (f x))
```

```
$ ./repro-mono           # prints 6, exit 0 -- the right answer
```

So neither a capturing closure nor a function-typed parameter is enough on its
own. It takes **both**: a type variable instantiated to a function type, with a
capturing closure travelling through it.

## Probable mechanism -- NOT confirmed, do not treat as a diagnosis

A type parameter is erased to the int64 carrier. A capture-free lambda lifts to
a plain global function, so what goes into the carrier slot is a code pointer,
and a code pointer survives the round trip -- which is exactly why variation 1
passes and why the shape is easy to miss. A capturing closure is not a code
pointer: it is a fat value (code plus environment). Something on that path
appears to write only one word into the carrier slot, so the reader
reconstitutes a function value whose environment is garbage, and the first
call through it faults.

That is a guess from the two variations, not from reading the emitter. The
emitted C is available and clean (`emit-c` exits 0), so the next step is to
diff the emitted C of variation 1 against the failing case and find where the
environment word goes.

## Why this is worth more than its blast radius suggests

It is the hybrid carrier/by-value representation split showing through, which
is the root the end-to-end monomorphization plan exists to remove
(`docs/upcoming/end-to-end-monomorphization-plan.md`). A monomorphized `app`
would instantiate `A` to a real function type and never build a carrier slot,
so this class disappears rather than being patched. Worth weighing before
anyone spends long on a targeted fix.

## Related -- and a correction to what this section first said

An adjacent defect was isolated in the `turmeric-spices` repo the same day, in
`cata` over a function-typed carrier. **This report originally described it as
capture-triggered and pointing "the opposite way" from this one. That was
wrong**, and it was corrected the same day by controls rather than by reading:

- the real trigger there is **arm order**, not capture. The emitter types the
  `match` result temporary from the **first arm as written**: a first arm
  returning a non-capturing closure yields an `int64_t` temporary while every
  arm builds a `void *` fat-closure box, and `cc` rejects the mismatch.
- three controls establish it. A capturing arm written first with the
  non-capturing arm second **compiles and runs**; the regex algebra with the
  capturing `LitF` first and the non-capturing `EmptyF`/`StarF` after it
  **prints `match`**; all-arms-non-capturing fails. A non-capturing arm
  anywhere but first is harmless.
- the regex spice hits it only because `EmptyF` is nullary -- it has nothing it
  *could* capture -- and is naturally written first.

So the two are **not** two faces of one capture-shaped gap, which is what this
section originally claimed. They may still share a root -- both are the erased
carrier meeting a function value -- but the evidence for that is now much
weaker, and nobody should go looking for a single capture-shaped fix on the
strength of it.

**This report's own finding is unaffected.** The repro above contains no
`match` at all, so arm order cannot be its trigger; the three variations in
"What isolates it" were each run directly, and capture-vs-no-capture is what
moves it. The corrected spices finding also has a workaround (reorder the arms)
and a much better consequence -- the matcher-as-`cata` is unblocked there --
whereas this one has neither.

## Fix directions

1. **Find the missing word first.** Diff the emitted C for the capturing and
   capture-free variants above. This is cheap and it decides everything else;
   the mechanism section is currently a guess.
2. **Make the carrier round-trip total for fn values.** Whatever the boxing
   path is for a fat function value elsewhere, the type-parameter slot should
   use it rather than assuming a code pointer.
3. **Refuse rather than miscompile, as a stopgap.** If (2) is not close, a
   diagnostic at the instantiation site beats a SIGBUS -- the same posture
   `jit-ffi-interp-refuses-parametric-record-field` takes for a neighbouring
   representation gap.
4. **Or let monomorphization take it.** See above.
