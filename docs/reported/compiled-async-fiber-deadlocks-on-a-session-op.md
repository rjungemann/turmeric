# A session op inside `async` deadlocks the compiled program, with no diagnostic

**Severity: medium-high.** The most natural way to write the peer side of a
session protocol -- `(async (fn [] (server ch)))` -- compiles clean and then
**hangs forever** on the compiled path. No warning at elaboration, no runtime
error, no timeout: the process must be killed. The identical program runs
correctly under `tur --interpret`.

This is a parity gap in the direction nobody looks for: the tree-walking
interpreter is the **more** capable backend here.

## Repro

```turmeric
;; Minimal: an async fiber does a session recv.
(defn main [] : int
  (let [[s r] (make-session (Send int Close))]
    (let [t (async (fn [] (let [[n r] (recv r)] (println n) (close r))))]
      (let [s (send s 42)] (close s) (await t))))
  0)
```

```
$ ./build/tur build t-cc-async.tur -o t-cc-async.bin     # exits 0, no diagnostic
$ timeout 20 ./t-cc-async.bin ; echo "exit=$?"
exit=124                                                  <-- hung, killed at 20s

$ ASAN_OPTIONS=detect_leaks=0 ./build/tur interpret t-cc-async.tur
42                                                        <-- correct
```

Measured against `./build/tur` at v0.48.0 (2026-09-16). Reproduces with every
session op, not just `recv`: `offer`, `recv-from`, and any `send` whose peer has
not yet arrived hang the same way.

## Root cause

Two cooperative-vs-preemptive runtimes that do not compose:

- The compiled session runtime (emitted as a C preamble by `emit_module.c`)
  blocks on a **pthread mutex/condvar**: `tur_session_recv` waits on
  `pthread_cond_wait` until a peer deposits.
- Compiled `async` runs the fiber body on the **same OS thread** as its spawner.

So the fiber's `recv` blocks the one thread that could ever run the `send`, and
neither side can make progress. It is a true deadlock, not a slow path.

The interpreter has the same single-threaded shape but a *cooperative* channel:
`session_park_or_spin` in `src/turi/eval.c` yields to the fiber scheduler
instead of blocking the thread, so the peer gets to run. That is why turi is
correct here.

This is known inside the test suite -- `tests/fixtures/session-send-turi/input.tur`
carries the note "a session recv would block a cooperative async fiber under the
compiled pthread runtime" -- but the knowledge lives only in a fixture comment.
Nothing in [session-types-guide.md](../guides/session-types-guide.md) or
[async-await-guide.md](../guides/async-await-guide.md) says so, and the
compiler is silent.

## Why this matters beyond the hang

It is the reason the session fixture suite is written twice. Every compiled
session fixture hand-rolls its peer as raw inline-C over `pthread_create`:

```turmeric
(defn spawn [f : ptr<void>] : ptr<void>
  ```c
  pthread_t *tid = (pthread_t *)malloc(sizeof(pthread_t));
  pthread_create(tid, NULL, tur_session_thread_wrapper, f);
  return (void *)tid;
  ```)
```

That inline-C is exactly what `tests/run-turi.sh` PASS-skips, so **25 of 54
session fixtures never run under the interpreter** -- not because sessions do not
interpret (they do; see the expansion plan) but because the peer-spawn does not.
The ten `-turi` twins exist solely to re-spell `spawn`/`join` as `async`/`await`.
Close this and one source file serves both suites.

It is also a `:int`/`ptr<void>` stand-in of exactly the kind
[CLAUDE.md](../../CLAUDE.md) forbids, reproduced verbatim in 20+ fixtures and in
the `session-types-guide.md` examples, which use a `spawn` the guide never
defines.

## Fix directions

**1. Make compiled `async` yield across a session block (real fix).** The
compiled session runtime would need a cooperative wait when the caller is on a
fiber -- the compiled analog of `session_park_or_spin`. Largest, and it makes
`async` + sessions genuinely compose.

**2. Ship a real `session-spawn` / `session-join` in stdlib (cheapest useful).**
A typed pair backed by `pthread_create(..., tur_session_thread_wrapper, ...)` on
the compiled path and by the fiber scheduler under turi, replacing 20+ hand-rolled
`(defn spawn [f : ptr<void>] : ptr<void>)` blocks. Types it properly --
`(fn [] nil)` in, an opaque handle out -- and, because stdlib inline-C can carry a
native override (`wk_register_*_natives`), one fixture source then runs on both
paths. Does not fix the hang for user code that writes `async` directly.

**3. Diagnose it (floor).** Reject, or at minimum `TUR-W`-warn, a session op
lexically inside an `async` body on the compiled path. A named limitation beats
a silent hang even if neither 1 nor 2 lands.

1 and 2 are independent and 2 does not depend on 1. 3 is worth doing regardless,
since it is the only one that helps a user who has already written the hang.

## See also

- `emit_module.c` -- the emitted `tur_session_send` / `tur_session_recv` /
  `tur_session_thread_wrapper` preamble.
- `src/turi/eval.c` -- `session_park_or_spin`, the cooperative wait that makes
  the interpreter correct.
- `tests/fixtures/session-*-turi/` -- the ten duplicate fixtures this causes.
- [turi-session-expansion-plan.md](../upcoming/turi-session-expansion-plan.md) -- phase S2.
