---
title: dsp.tur Pair64 bit-cast helpers were obsolete; removed in the signal rebuild
category: Reported
severity: low
description: Paper-trail entry. The pre-rebuild `tur-signal` spice carried `__dsp_pair_first_float` / `__dsp_pair_second_float` inline-C helpers that walked a manually-laid-out `Pair64 { int64_t first; int64_t second; }` via `memcpy(&v, &bits, 8)`. The G4 readiness fixture (`tests/fixtures/pair-signals-typed/`) proved typed `(Pair float float)` survives fat-closure dispatch cleanly, so the bit-cast pattern was never needed. The rebuild deletes `dsp.tur` entirely; `signal/shaper`'s `mix` / `add` / `multiply` now use `stdlib/pair.tur`'s `pair-fst` / `pair-snd`.
---

# `__dsp_pair_*_float` bit-cast helpers were obsolete; removed in the rebuild

## Summary

The pre-rebuild `tur-signal` spice's `src/signal/dsp.tur` shipped
inline-C helpers of the shape

```c
__dsp_pair_first_float(int64_t pair_handle) {
    Pair64 p; memcpy(&p, &pair_handle, 8);
    double v; memcpy(&v, &p.first, 8);
    return v;
}
```

with a sibling `__dsp_pair_second_float`. These were used by the
old `mix` / `add` SFs to read a `(Pair Sample Sample)` produced by
`pair-signals` -- the author assumed typed pairs couldn't survive
the fat-closure boundary.

The G4 readiness fixture `tests/fixtures/pair-signals-typed/` (added
under `[[language-readiness-for-typed-signal-plan]]`) proves the
typed pair *does* round-trip through fat dispatch with no bit-pattern
dance: `(make-struct Pair (av t) (bv t))` returned from a fat closure
is read back via stdlib's `pair-fst` / `pair-snd`, both typed `:float`,
with no `:int` round-trip.

## Severity

Low. The helpers no longer exist; this entry is the paper-trail entry
the rebuild plan asks for (`[[tur-signal-rebuild-plan]]` Bug reports
owed, item 1) so the cleanup isn't silently lost.

## Status

Resolved by the rebuild. `dsp.tur` is deleted. `signal/shaper`'s `mix`
/ `add` / `multiply` consume a `(Pair float float)`-returning fat
closure with `pair-fst` / `pair-snd` from `stdlib/pair.tur`. The
`(:: x :int)` / `memcpy(&v, &bits, 8)` boundary patterns are gone
from `src/signal/` (`grep -rE
"__arrow_call1|__signal_call1|memcpy\(&|::\s+:(int|float)"
src/signal/` is empty).

## Related

- `[[tur-signal-rebuild-plan]]` -- Bug reports owed, item 1.
- `[[language-readiness-for-typed-signal-plan]]` -- G4 verdict block
  that confirmed typed Pair through closures is supported.
- `tests/fixtures/pair-signals-typed/` -- the readiness fixture.
