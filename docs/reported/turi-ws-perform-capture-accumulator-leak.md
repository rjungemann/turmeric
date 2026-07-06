# Work-stack `perform` capture orphans arg accumulators (leak grows with performs)

Severity: low-medium (interpreter is process-lifetime; leak grows with work, not
fixed cost -- matters for a long-running interpreter session, not for `tur build`)

## Summary

When a `perform` is captured on the driver work-stack (DC path), the slice
`st[pidx+1 .. len-1]` is `memcpy`'d into a heap `TuriWsCont` (`eval.c`
`EX_PERFORM`) and `len` is truncated.  Any per-frame heap accumulator in that
slice -- the `malloc`'d `acc` array of a `DK_BUILTIN_ARG` / `DK_CALL_ARG` /
`DK_MAKE_STRUCT` frame -- is now owned only by `wc->frames`.  On `resume`,
`clone_ws_slice` *duplicates* those accumulators for the independent (multishot)
clone, but the ORIGINAL accumulators in `wc->frames` are never freed:
`TuriWsCont` is pool-allocated (`turi_val_calloc`, reclaimed only at env
teardown) while the `acc` arrays are raw `malloc`.  So each captured-then-
resumed continuation leaks its slice's accumulators.

## Minimal repro (leak detection on)

```turmeric
(defeffect Ask [] :int)
(defn main [] : int
  (println (handle (+ 1 (perform (Ask))) (Ask [] k) (resume k 41)))  ; => 42
  0)
```

```
$ ASAN_OPTIONS=detect_leaks=1 ./build/tur --interpret repro.tur
... LeakSanitizer: 105 byte(s) leaked in 4 allocation(s)
    (32 bytes at eval_drive_ex ... = the DK_BUILTIN_ARG acc for `+`)
```

The leak is proportional to the number of captured/resumed performs, so a
recursive-handler loop leaks O(N):

```turmeric
(defn hr-rec [n :int] :int
  (if (= n 0) 0 (handle (+ 1 (perform (Ask))) (Ask [] k) (resume k (hr-rec (- n 1))))))
(hr-rec 200000)   ; ~12.8 MB leaked (400000 * 32-byte acc arrays)
```

## Pre-existing, exposed at scale by C3

This is inherent to the DC work-stack capture path and predates
turi-c-scoped-forms-heap-bounding.  A simple non-recursive capturable handle
already leaks (repro above).  Phase C3 (cycle-aware `ws_capturable` + driven
`resume` value arg) routes *recursive* handlers through this same path, so the
leak now grows with the recursion depth.  The interpreter is process-lifetime
and the turi harnesses run with `ASAN_OPTIONS=detect_leaks=0` (`run-turi.sh`,
`run-flags.sh`, and the `tur_eval_tco` ctest target), so the suite stays green;
this note records the underlying growth leak.

## Fix directions

`TuriWsCont` needs to own and free its slice's heap accumulators (and any
owned frames) when the continuation is finally unreachable.  The wrinkle is
multishot: a `k` may be resumed any number of times and may escape the handler,
so a naive free-on-first-resume is wrong.  Options: (a) give `TuriWsCont` a
destructor invoked when it is dropped (needs drop tracking for effect conts),
or (b) arena-allocate the slice accumulators from the env pool
(`turi_val_alloc`) instead of `malloc` so they are reclaimed at teardown like
the rest of the continuation -- trading the growth leak for a bounded
pool-lifetime one (consistent with the interpreter's process-lifetime model,
and enough to satisfy `detect_leaks=1`).
