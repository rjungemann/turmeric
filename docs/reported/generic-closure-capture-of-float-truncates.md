---
title: A generic type parameter bound to float is TRUNCATED when captured into a closure
category: Reported
description: (capture 7.25) returns 7. The monomorphized spec assigns a `double` parameter into the shared closure-env struct's `int64_t` field, which numerically converts instead of bit-reinterpreting. The same spec also drops the TUR_REGION_NOTE_WORDS the carrier base emits. Silent, exit 0, no diagnostic.
---

# A generic type parameter bound to `float` is truncated when captured into a closure

**Severity: high.** A **silent wrong answer** -- the worst class. No
diagnostic, exit 0, and the value is off by the fractional part. A second,
independent defect at the same emission site drops a region hook.

**Status:** OPEN. Filed 2026-09-19 while investigating the remaining
[stdlib-int-stand-in-audit](stdlib-int-stand-in-audit.md) S2 sites, which it
blocks (see "Why this blocks S2"). **Pre-existing and unrelated to that audit**
-- verified on a clean `origin/main` tree at `a94dd5a86`.

## Repro

```turmeric
(defn capture [A] [v : A] : (fn [] A)
  (fn [] v))
(defn main [] : int
  (println ((capture 42)))
  (println ((capture 7.25)))
  0)
```

```
42
7          <-- should be 7.25
```

## The boundary, measured

| Shape | Result |
| --- | --- |
| `(defn ident [A] [v : A] : A v)` -- generic pass-through | **7.25** correct |
| `(defn cap0 [A] [v : A] : (fn [] A) (fn [] v))` -- captured | **7** WRONG |
| `(defn cap1 [A] [v : A x : int] : (fn [] A) (fn [] v))` | **7** WRONG |
| `vec-push!` / `vec-get` -- parametric container | **7.25** correct |

So it is not "generics and floats" and not "containers": it is specifically **a
type parameter bound to `float` crossing into a closure environment.**

## Root cause

The closure env struct is shared between the carrier base and every
monomorphized spec, and its payload field is the int64 carrier:

```c
struct __env_1610 { int64_t __fn; int64_t v; };
```

The base fills it from an `int64_t` parameter (a no-op) and the float spec
fills it from a `double` parameter -- which is an implicit **numeric
conversion**, not a bit-reinterpret:

```c
static void * capture(int64_t v) {
        __t275->v = v;                                              /* fine */
        TUR_REGION_NOTE_WORDS(&(__t275->v), sizeof(__t275->v));
        ...
}

static void * capture__spec__void___double(double v) {
        __t287->v = v;            /* 7.25 -> 7, silently */
        ...                       /* and the region note is GONE */
}
```

Emission site: `src/compiler/emit_expr.c:12496-12500` (the field fill) and
`:12508-12518` (the region note, guarded by `regions_enabled()`).

**The compiler already knows the correct idiom** and uses it one line away in
the same translation unit -- `bt-cell-new` receives its float argument as
`bt_hycell_hynew(((union { double s; int64_t d; }){.s = 0.0}).d)`. The CPS
backend has it factored as `slot_store` / `slot_load`
(`src/compiler/emit_cps_ir.c:360,387`), including a Tier-C heap-box arm for
aggregates. The direct backend's env fill uses neither.

## Second defect at the same site: the region note is dropped

The base emits `TUR_REGION_NOTE_WORDS(&(__t275->v), ...)`; the spec does not.
Both are in the same translation unit, so `regions_enabled()` cannot explain
it -- the spec is emitted by a different path that does not carry the hook.

Per [CLAUDE.md](../../CLAUDE.md)'s region rule, the closure-env fill is
explicitly in the hooked set, and "a missed hook is a silent use-after-rewind
on the default build". A region-allocated node captured by a
**monomorphized** closure is therefore unnoted today.

This half is independent of the float bug -- it drops for **every** spec, not
just float ones.

## Why this blocks S2

`dfs-set` (`stdlib/backtrack-dfs.tur`) returns a closure capturing its payload:

```turmeric
(defn dfs-set [c : BtCell v : int] : (fn [(fn [] bool)] bool)
  (fn [k : (fn [] bool)] (if (bt-set! c v) (k) true)))
```

Parameterising it on the `Vec` model (`[A] [c : (BtCell A) v : A]`) is
mechanically ~20 lines and was **tried and reverted**. Measured, with the same
program either way:

| | `(dfs-set c 7.25)` |
| --- | --- |
| today, `v : int` | `error [TUR-E0001]: expected int, got float` -- **loud** |
| parameterised | runs, prints `3.45846e-323` -- **silent wrong answer** |

`3.45846e-323` is the integer `7` reinterpreted as double bits, i.e. exactly
this bug seen from the read side.

**So parameterising `dfs-set` today makes the language strictly worse**: it
converts a correct rejection into a wrong answer. That is why the S2 work
stopped at 13 of 19 sites rather than continuing, and it is the one place a
`Vec`-shaped parameterisation is not simply an improvement.

## Fix directions

1. **Bridge at the fill.** At `emit_expr.c:12496`, when the captured value's
   emitted C type and the env field's declared type disagree across the
   float/word boundary, emit the union reinterpret instead of a plain
   assignment. Smallest change, and it matches the carrier convention the rest
   of the emitter already follows. Needs the symmetric treatment on the read
   side -- the thunk returns `int64_t` and the caller must reinterpret -- so it
   is at least two coordinated sites, not one.
2. **Give each spec its own env struct** with properly typed fields (`double
   v`) and a matching thunk signature. The "real" fix, and a substantially
   larger change to the mono-spec machinery.
3. **Restore the region note on the spec path** regardless of which of the
   above lands; it is a separate one-line-ish omission with its own
   consequence.

Direction 1 with fixture coverage for float, float32 and a by-value aggregate
capture is the cheapest thing that makes the S2 remainder safe to land.

## See also

- [stdlib-int-stand-in-audit](stdlib-int-stand-in-audit.md) -- S2, which this
  blocks.
- [parametric-stdlib-diagnostics-print-tyvar-internals](parametric-stdlib-diagnostics-print-tyvar-internals.md)
  -- the other finding from the same parametric-container work.
