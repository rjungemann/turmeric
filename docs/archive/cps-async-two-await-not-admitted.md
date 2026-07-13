# cps-async: a function body with two awaits is not CPS-admitted

**Status:** RESOLVED (2026-07-13). Two (and more) sequential awaits now color
onto the heap-continuation path; the compiled re-park chain works end-to-end.
See the Resolution section at the end.

**Severity:** medium (blocks the deferred re-park path end-to-end on the
compiled path; the runtime seam is proven, but only single-await function
bodies reach it).

## Summary

Under `--enable=cps-async`, only a function whose body contains a **single**
`await` (more generally, a single delimited-control op) is CPS-colored and
emitted through the heap-continuation path (`f__cps` + `dk_shift` +
`__tur_await_body`). A body with **two** awaits -- whether nested
(`(let [x (await a)] (let [y (await b)] (+ x y)))`) or flat
(`(+ (await a) (await b))`) -- falls back to the **direct** emitter, which
lowers each `await` to `tur_await_future`. For a *pending* future not running
inside a fiber, `tur_await_future` busy-loops
(`while (!tur_future_done(f)) tur_scheduler_run_one(...)`) and hangs, because
nothing on that path ever fulfills the future.

This is why the F3.2 re-park chaining (the `__tur_async_resume` re-park branch)
cannot be exercised by a compiled fixture today; it is proven instead by the
`cps-async-await-repark-chain` unit probe in `tests/cps_prompt_unit.c`, which
drives the same park/re-park/fulfill logic against the reference DK runtime.

## Minimal repro

```turmeric
;; --enable=cps-async
(defn future-a [] : ptr<void>
  ```c
  static TurFuture *g = NULL; if (!g) g = tur_future_new(); return (void *)g;
  ```)
(defn future-b [] : ptr<void>
  ```c
  static TurFuture *g = NULL; if (!g) g = tur_future_new(); return (void *)g;
  ```)
(defn work [] : int
  (+ (await (future-a)) (await (future-b))))   ;; two awaits -> evicts to direct
```

`tur emit-c --enable=cps-async` on this produces a `static int64_t work()` that
calls `tur_await_future(...)` twice -- no `work__cps`, no `dk_shift`. The
single-await variant `(+ 1 (await (future-a)))` *does* emit `work__cps` with the
`dk_shift(DK_ROOT_TAG, __tur_await_body, ...)` lowering.

## Root cause (direction)

The CPS backend's admissibility subset (`src/compiler/emit_cps_ir.c`:
`term_core_ok` / `is_cps_island` / the signature and join gates) admits the
single-control-op shape but not two sequential control ops threaded through a
`letcont`/heap-join in one body. A body with a second `await` produces a
CT-IR shape the subset rejects, so `emit_cps_ir_try_fn` declines and the
function is emitted by the direct lowerer (which owns `tur_await_future`). This
is a general CPS-subset limitation (it applies to two `perform`s / two
`shift`s as well), not specific to async.

## Fix directions

- Extend the CT-IR admissibility so a body with a second delimited-control op
  threads the first op's continuation as a heap-join `letcont` into the second
  (the machinery in `letcont_is_heap_join` / `emit_heap_join` already reifies a
  join as a DK frame -- the gap is admitting the *sequential* case at the
  top-level `k`, not just a single-op tail).
- Independently, `tur_await_future`'s not-in-a-fiber pending branch should not
  silently busy-loop with no progress source; a clearer diagnostic ("await on a
  pending future with no driver") would surface the eviction instead of hanging.

## Resolution (2026-07-13)

Admitted a full (but statically BOUNDED) CPS continuation for `await`, lifting it
like a RESET continuation. Three coordinated changes in `emit_cps_ir.c`:

1. **Admissibility** -- a new `await_cont_reset_ok` predicate admits an await
   continuation that is a branch or a further *sequential* `await` (appcont /
   letval / letprim / letcall / letraw / if / nested await), and `term_core_ok`'s
   `CT_AWAIT` arm now accepts `perform_body_ok(body) || await_cont_reset_ok(body)`.
   It rejects **every tail call** (not just cps->cps ones): whether a callee is
   CPS-emitted (`binding_in_s`) is not yet settled while the predicate runs during
   S-classification -- a self-recursive callee reads back `false` mid-fixpoint --
   so keying on that flag would admit the very recursion that must evict (the
   gap-1 O(N) heap-recursion hazard). A tail call is never part of a two-await
   shape (it ends in an appcont), so rejecting all of them is both sound and
   sufficient.
2. **Emission** -- `emit_await` gains a third arm: trivial -> straight-line
   `LH_PERFORM_CONT` (F3.1) -> full `LH_RESET_CONT` (the frame threads the
   enclosing k itself, `next = dk_done()`), so the first await's continuation
   (which contains the second await) emits its own nested `dk_shift`, and the
   value is delivered exactly once.
3. **Capture walk** -- `collect_caps_rec` gained a `CT_AWAIT` case (it previously
   fell to the conservative `default: cs->ok = false`), so a lifted continuation
   carrying a nested await no longer spuriously evicts.

**Ownership fix (parked re-park).** A RESET_CONT await frame carries `k = __root`
in its env, but the direct entry wrapper frees `__root` right after `f__cps`
returns -- which for a *parked* body dangles the retained continuation. Both entry
wrappers now emit `if (!tur_async_suspended) dk_free(__root);` -- a parked body
leaks `__root` (suspension is rare) rather than dangling it; byte-identical for
any synchronous / effect-only body (the flag is 0 there).

**Tests.**
- `tests/fixtures/async-await-cps-two` (`--enable=cps-async`, `42`): two ready
  awaits `(+ (await a) (await b))` resume inline through the heap path.
- `tests/fixtures/async-await-cps-repark` (`42`): two *pending* awaits drive the
  deferred re-park chain **end-to-end on the compiled path** -- the case that was
  previously only provable at the unit level.
- gap-1 recursive await still evicts (async-rec probe 1,000,000 deep under 256KB);
  F3.1 (43) and F3.2 single-pending (42) unchanged; run.sh 2117/0.

The second fix direction (a clearer diagnostic for `tur_await_future`'s
not-in-a-fiber pending busy-loop) was not pursued -- the shape that hit it now
colors onto the heap path instead of evicting.

## Status

RESOLVED. The F3.2 deferred suspend/resume seam (park on `on_complete`, resume
via `tur_future_fulfill`, outer-future chaining) is now proven end-to-end on the
compiled path for multi-await bodies, not just the unit level.
