# reactor/fiber teardown does a bad-free on an interior closure-box pointer

> **RESOLVED (verified 2026-07-22, macOS-arm64).** The bad-free is gone: the
> reactor/fiber teardown is now header-aware. `tur_reactor_release_box`
> (`src/async/reactor.c:85`) consults the emitted program's
> `tur_closure_headers_enabled` flag (strong `= 1` in every compiled program,
> overriding libturi's weak `0`) and, when set, recovers the allocation base via
> the `env[-1]` drop-glue header (`*hdr` walks owning captures and frees the
> base) instead of interior-freeing the past-header pointer -- exactly the
> "store/back-compute the base" fix this report prescribed. Flag-off it keeps the
> plain-free ABI.
>
> **Verification:** all 14 fixtures below were built **with ASan**
> (`-fsanitize=address`, `ASAN_OPTIONS=detect_leaks=0`) and run to completion:
> 14/14 exit 0, **no AddressSanitizer bad-free, no SIGABRT**, stdout matches
> `expected.stdout`. (The default `tests/run.sh` compiles fixtures `-O2` without
> ASan, so these already show green there; the ASan build is what exercises the
> original abort.) Archived per the docs/reported STRICT RULE.

**Severity:** High -- every compiled reactor/fiber program aborts (SIGABRT)
at teardown under ASan; 13 fixtures red. Under a non-ASan Release build the
same free corrupts the heap silently.

## Symptom

`bash tests/run.sh` fails 13 `reactor-*` fixtures. Most exit 134 (SIGABRT);
two (`reactor-fibers-cancel-on-free`, `reactor-fibers-stop-mid-run`) show a
truncated-stdout mismatch because the process aborts during cleanup before
printing the final line.

ASan (from `tests/fixtures/reactor-signal/actual.stderr`):

```
==ERROR: AddressSanitizer: attempting free on address which was not malloc()-ed:
  0x603000000fa8 in thread T0
    #1 tur_reactor_free+0x3cc
    #2 main+0x278
0x603000000fa8 is located 8 bytes inside of 24-byte region [0x...fa0,0x...fb8)
allocated by thread T0 here:
    #1 main+0x1d8
```

Fiber variant is identical but in `tur_local_fiber_group_free` ->
`local_hyfiber_hygroup_hyfree`.

## Repro

Any reactor program that registers a heap closure and frees the reactor:

```turmeric
(defn main [] : int
  (let [r (tur_reactor_new)]
    (tur_reactor_add_signal r 10
      (fn [id signum ud] : nil (print-signal signum)) 0)
    (raise-signal 10)
    (tur_reactor_poll r 200)
    (tur_reactor_free r)   ;; <-- bad-free here
    0))
```

(`tests/fixtures/reactor-signal/input.tur`)

## Root cause

The 24-byte region "allocated in main" is the heap **closure box** for the
`(fn [id signum ud] ...)` handler. `tur_reactor_free` frees it via
`tur_reactor_release_box(src->tur_cb)` at `src/async/reactor.c:335`, and
`tur_local_fiber_group_free` does the same at `src/async/reactor.c:821+`.

The freed address is **8 bytes inside** the 24-byte box, i.e. `src->tur_cb`
holds an **interior pointer** (into the fat-closure struct's env/fn-pointer
field) rather than the box base. `free(base+8)` -> bad-free.

This is consistent with a fat-closure representation change (see
[[project_monomorphization_north_star]]): the reactor stores whatever the
`add_signal`/`add_*` ABI shim handed it as the "callback box," but that value
is now an interior field pointer, not the allocation base. The dedup-and-free
teardown path (`reactor.c:308` `tur_reactor_free`, `reactor.c:821`
`tur_local_fiber_group_free`) was never updated to recover the box base.

## Fix directions

- Find where `src->tur_cb` / the fiber body box is populated (the
  `tur_reactor_add_*` and fiber-spawn ABI shims) and confirm what pointer the
  closure lowering now hands across the boundary -- base vs. `&box->field`.
- Either store the allocation base in `tur_cb` (preferred: keep ownership at
  the malloc base) or teach `tur_reactor_release_box` / the fiber-group free
  to back-compute the base before `free`.
- Add an ASan assertion-style regression: these 13 fixtures already cover it;
  they must go green on macOS-arm64 with leak detection ON. Note none of them
  carry a `requires.*` marker, so this is not a leak-check opt-out situation --
  a bad-free fires regardless of `detect_leaks`.

## Affected fixtures (all one root cause)

reactor-chan-bridge, reactor-fd-modify, reactor-fd-readable, reactor-fd-remove,
reactor-fd-writable, reactor-fibers-park-chan, reactor-fibers-park-fd,
reactor-fibers-smoke, reactor-signal, reactor-stop-from-callback, reactor-timer,
reactor-wake-cross-thread (exit 134); reactor-fibers-cancel-on-free,
reactor-fibers-stop-mid-run (truncated-stdout side effect).
