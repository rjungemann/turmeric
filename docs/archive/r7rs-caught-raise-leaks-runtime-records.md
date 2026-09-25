# `#lang r7rs`: every `raise` caught by `guard` leaks about 1 KB of runtime records

**RESOLVED 2026-09-25 (compiled back end).** The r7rs-gc collector graduated
([r7rs-gc-plan](r7rs-gc-plan.md)): it is the allocator of every compiled
single-unit `#lang r7rs` program on Linux and macOS, so what this report
describes is garbage the next collection reclaims. `TUR_R7RS_GC=0` or
`--no-r7rs-gc` builds without it (a program that starts threads). The
interpreter keeps its values for the life of the process by design
(gc-guide). Original report follows.


**Severity:** low-medium. Compiled back end. A program that raises and
catches in a loop (a parser that reports errors, a retry loop) grows by about
1 KB per caught raise, most of it runtime bookkeeping rather than Scheme
data. Found by r7rs-lang-plan T8's audit.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (loop i acc)
  (if (= i 1000) acc
      (loop (+ i 1) (+ acc (guard (e (#t 1)) (raise 'boom))))))
(write (loop 0 0))
(newline)
```

Built with `TUR_CC_FLAGS="-O1 -foptimize-sibling-calls -g -fsanitize=address
..."` and run with `detect_leaks=1`, measured 2026-09-25:

| raises | leaked |
|---|---|
| 10 | 10,232 bytes in 207 allocations |
| 1,000 | 1,016,072 bytes in 20,007 allocations |

The largest sites for 1,000 raises:

| bytes | allocations | site |
|---|---|---|
| 240,000 | 2,000 | `dk_new` (DK frames, under `dk_prompt` in `with-exception-handler` and `raise-continuable`) |
| 64,000 | 2,000 | `R7rsPair` (the handler stack's entries) |
| 48,000 | 1,000 | `r7rs-call/ec__` (the one-shot escape record) |
| 48,000 | 1,000 | `r7rs-raise-continuable` |
| 24,000 | 1,000 | `__tur_dyn_pack_rest` (the rest argument of a dynamic call) |

## Root cause

`guard` escapes to its handler through `r7rs-call/ec__`, a longjmp. Every
frame the longjmp skips was going to free what it allocated on its way out:

- a CPS procedure's DK driver (`dk_prompt`) frees its frames when it returns;
  a longjmp past it abandons them;
- the escape record and the handler-stack entry are the procedure's own, and
  its normal return path is what would drop them.

The pairs are Scheme data and fall under
[r7rs-heap-data-never-reclaimed](r7rs-heap-data-never-reclaimed.md); the DK
frames, the escape records and the packed rest chains do not.

## The experiment (`--enable=r7rs-gc`)

Under the r7rs-gc experiment ([docs/archive/r7rs-gc-plan.md](r7rs-gc-plan.md))
every record above is a collected object: the abandoned DK frames, the
escape and `raise-continuable` records and the packed rest chain are
garbage once the escape has jumped over them, and the next collection
reclaims them. Peak RSS of the repro at 200,000 caught raises, measured
2026-09-25 (default `-O2`):

| build | peak RSS | time |
|---|---|---|
| plain | 266 MB | 0.25 s |
| `--enable=r7rs-gc` | 30 MB | 0.28 s |

This report stays open until the collector graduates; the fix directions
below are what a build without it would need.

## Fix directions

- Have the escape unwind what it skips. The runtime already keeps the live
  escape set (`tur_escape_live_*`); a driver registry like the interpreter's
  (`DriveReg`, eval.c) would let `r7rs-call/ec__`'s longjmp free the frames of
  every driver it jumps over. This must stay off once `tur_dk_pinned` is set
  (a T5 continuation may still point at them).
- Free the escape record when its `call/ec` returns or is escaped to, since a
  one-shot escape cannot be invoked twice.

## Guide upkeep

One clause of `docs/guides/r7rs-guide.md`'s "**Data is never freed.**" bullet
("Where it differs from R7RS") is this report; see the Guide upkeep section of
[r7rs-heap-data-never-reclaimed](r7rs-heap-data-never-reclaimed.md), which
maps each clause of that bullet to its report.
