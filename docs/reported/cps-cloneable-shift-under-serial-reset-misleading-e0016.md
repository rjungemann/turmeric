---
title: "cloneable (resuming) shift under a serial-reset reports a misleading TUR-E0016 instead of a flavor-mismatch error"
category: Reported
severity: low (misleading diagnostic on an exotic mixed-surface slip; rejection is correct, only the message misdirects -- no miscompile)
status: open (found 2026-07-21, while landing cps-shift-reset-capability-folding item 1)
---

# cloneable shift under `serial-reset` -> misleading TUR-E0016

## Summary

A **resuming** cloneable `shift` (receiver is `cont` / `cloneable-cont` and
invokes its continuation) placed lexically inside a `serial-reset` is rejected
with **TUR-E0016** *"a resuming shift ... has no plain enclosing reset to
capture the continuation up to -- there is no delimiter that can catch it"* and
a `note` about *cross-function* resume. Both are misleading: there **is** an
enclosing reset (the `serial-reset` right there), and cross-function resume is
irrelevant -- the shift is lexical.

This is the exact **symmetric twin** of the slip fixed under the same plan for
the other direction (a `serial-cont` shift under `cloneable-reset`, now a clear
`TUR-E0019` -- see `docs/archive/cps-shift-reset-capability-folding-plan.md`).
The correct outcome here is the mirror-image flavor-mismatch diagnostic: a
cloneable (in-memory multi-shot snapshot) continuation cannot be hosted by a
`serial-reset`'s marshal substrate, so reject with a message that names the
mismatch and the fix ("use a plain `reset` or `cloneable-reset`").

Rejection itself is correct (no wrong code is emitted); only the message
misdirects. Severity is low, but the misdirection is real -- the message sends
the reader hunting for a missing reset / a cross-function issue that isn't
there.

## Minimal repro

```turmeric
;; resuming cloneable receiver (invokes k) under a serial-reset
(defn ck [k : cont] : int (+ (k 1) (k 2)))
(defn main [] : int
  (println (serial-reset (+ 10 (shift ck 0))))   ; TUR-E0016 (misleading)
  0)
```

Emits:

```
error [TUR-E0016]: a resuming shift (its receiver invokes the continuation) has
no plain enclosing reset to capture the continuation up to -- there is no
delimiter that can catch it
  = note: a `reset` that also reifies a *lexical* resuming shift cannot
    additionally catch a *cross-function* one ...
  = help: wrap the computation ... in a dedicated (reset ...) ...
```

Boundary cases (characterized):

- Inline receiver `(shift (fn [k : cont] (+ (k 1) (k 2))) 0)` under
  `serial-reset` -- same misleading TUR-E0016.
- **Abortive** (ignore-k) cloneable shift under `serial-reset` -- does NOT hit
  this path (it takes the abortive route and runs), so the defect is specific to
  a *resuming* receiver.
- The identical resuming shift under a plain `reset` works (`=> 23`), and under a
  `cloneable-reset` works -- confirming the only problem is the `serial-reset`
  delimiter not being recognized as (in)capable of hosting the cloneable shift.

## Root cause

The cloneable-shift lowering keys its "am I inside a reset?" gate off
`cloneable_reset_depth`, but `serial-reset` bumps a *separate* counter:

- `elab_serial_reset` bumps only `e->serial_reset_depth`
  (`src/compiler/elab_effects.c:1263`); it never touches `cloneable_reset_depth`.
- `elab_cont_shift_core`'s gate `if (e->cloneable_reset_depth == 0)`
  (`src/compiler/elab_effects.c:939`) does not consult `serial_reset_depth`, so a
  cloneable resuming shift lexically inside a `serial-reset` sees depth 0 and
  falls into the *"no lexical reset"* branch -> the cross-function `__Shift`
  desugar (sets `e->uses_crossfn_resume`).
- Post-elaboration, `elab_wrap_resets_for_crossfn_resume` looks for a plain
  `EX_RESET` node to host the `__Shift` handler. The only delimiter present is
  `EX_SERIAL_RESET`, so `n_plain_resets == 0` and it emits the TUR-E0016 at
  `src/compiler/elab_effects.c:858`.

So the shift is misclassified as *cross-function* purely because the two
reset-flavor depth counters are not unified, and the terminal error is the
cross-function "no plain reset" message rather than a flavor-mismatch one.

## Fix directions

Mirror the fix already landed for the other direction
(`pinned_cloneable_at_depth[d]` + a tailored `TUR-E0019`), but detect the
serial delimiter:

1. Before the cloneable shift falls into the cross-function `__Shift` path (the
   `cloneable_reset_depth == 0` branch at `elab_effects.c:939`), check whether
   the nearest *lexically* enclosing delimiter is a `serial-reset`
   (`e->serial_reset_depth > 0` with no intervening cloneable delimiter). If so,
   emit a clear flavor-mismatch diagnostic: *"a `cont` (cloneable) shift receiver
   needs a cloneable-capable delimiter, but the nearest enclosing reset is a
   `serial-reset`; use a plain `reset` (it adopts the receiver's flavor) or
   `cloneable-reset`."* -- and return before the cross-function fallback.
2. To honour **nearest-delimiter** binding (a cloneable shift inside a plain
   `reset` inside a `serial-reset` must still work), the check needs to know the
   *nearest* delimiter's flavor, not merely that some serial-reset is open. The
   cleanest structural fix is to unify the reset-flavor tracking into one
   nearest-delimiter stack (or add a `serial` companion to the existing per-depth
   `pinned_cloneable_at_depth[]` / plain-reset tracking) so the shift reads one
   coherent "nearest delimiter flavor" instead of two disjoint counters. That
   also removes the root asymmetry that caused this misclassification.

Keep the cross-function TUR-E0016 path intact for the genuine case (a resuming
shift with truly no enclosing reset anywhere): only the "enclosing delimiter is
serial-reset" subcase is redirected to the new mismatch error.

## Related (lower severity, pre-existing, not introduced by the folding)

Nesting a serial delimiter directly inside a cloneable one --
`(cloneable-reset (reset (+ 10 (shift <serial-cont receiver> 0))))`, or the
all-keyword `(cloneable-reset (serial-reset (+ 10 (serial-shift ...))))` --
reports **TUR-E0706** *"serial-shift context is not capturable"*. The inner
serial context (`(+ 10 [])`) is trivially within the DK grammar in isolation;
the real blocker is the heterogeneous nesting (a serial reset inside a cloneable
reset). The message points at the context *shape* rather than the nesting, so it
is mildly misleading too, but: (a) it is **pre-existing** -- the sugar
(`reset`/`shift`) and keyword (`serial-reset`/`serial-shift`) forms produce the
identical TUR-E0706, so capability-folding preserves the behaviour exactly, and
(b) it represents a genuine unsupported-lowering case (nested heterogeneous
delimiters), for which "restructure into a supported shape" is at least in the
right ballpark. Captured here for completeness; not worth its own fix ahead of
v1 unless nested heterogeneous delimiters become a target.
