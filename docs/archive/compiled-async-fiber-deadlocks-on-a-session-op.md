# A session op inside `async` deadlocks the compiled program, with no diagnostic

**RESOLVED 2026-09-26, by a smaller route than direction 1.** Direction 1
asked for a session op to become an `await`-shaped suspension point in a
compiled async body -- a CPS colouring of every function on the path. It is
not needed: the deadlock is only that the body shares the spawner's thread,
and `session-spawn` already shows the fix for that shape is a thread.

An `(async ...)` whose body **captures** a Session / Role endpoint -- the
exact condition TUR-W0043 used to warn on -- is now spawned on its own OS
thread (`tur_async_thread_via`, the emitted async runtime in emit_module.c;
`on_thread` on the node, set by `elab_async`). The body runs there, fulfills
its future there, and every `await` of that future (`tur_await_future`, and
the CPS `__tur_await_body`) joins the thread first, after which it is an
ordinary completed future. `on_complete` is deliberately not fired from the
worker: a parked continuation must resume on its own thread. A panic in the
body rejects the future and re-raises at the `await`, as for the inline
spawns (`tur_panicking` and the handler chain are thread-local). Every other
`async` is emitted exactly as before.

TUR-W0043 no longer fires for a captured endpoint. It stays for the one
shape a thread cannot help: a body that makes BOTH endpoints and spells ops
on them itself, where both ends of the protocol are in one straight-line
body. `tur explain TUR-W0043`, the session-types guide and the turi-parity
guide say so.

Pinned by `tests/fixtures/session-async-peer-on-thread` -- the repro below,
a typed payload through the thread's future, a recursive `Rec` server loop
as the async peer, and a multi-party role endpoint -- identical compiled and
under `--interpret`. `session-async-warn` now pins the lexical shape that
still warns.

Known limits: a future that is never awaited leaves its thread unjoined
until exit; and a thread-backed body that itself awaits a still-pending
future on another thread's scheduler is not a supported shape (the park
state it would use is process-global).

**Severity: medium-high.** The most natural way to write the peer side of a
session protocol -- `(async (fn [] (server ch)))` -- compiles clean and then
**hangs forever** on the compiled path. No warning at elaboration, no runtime
error, no timeout: the process must be killed. The identical program runs
correctly under `tur --interpret`.

This is a parity gap in the direction nobody looks for: the tree-walking
interpreter is the **more** capable backend here.

## Status (2026-09-17)

Two of the three fix directions have landed; the hang itself remains.

- **Direction 2 (2026-09-16, turi-session-expansion-plan S2):** `session-spawn`
  / `session-join` in `stdlib/session.tur` (typed `(fn [] nil)` in, an opaque
  `SessionPeer` out; a pthread compiled, a scheduler fiber under `--interpret`
  via a native override). Every session fixture and every guide example uses
  it, the hand-rolled `ptr<void>` spawn is gone, the `-turi` twins are deleted,
  and 57 of 58 session fixtures run under `run-turi.sh`.
- **Direction 3 (2026-09-17):** `TUR-W0043` warns at every `(async ...)` whose
  body captures a `Session`/`Role` endpoint (the op may be inside a callee, as
  in `(async (fn [] (server-loop r)))`) or spells a session op itself. The
  message names the endpoint and points at `session-spawn`; `tur explain
  TUR-W0043` has the long form. It is not emitted under `--interpret`, where
  the shape is correct. It is a warning, not a rejection, because the peer may
  legitimately be on another OS thread (a `session-spawn` peer), in which case
  the program works and still warns -- `tests/fixtures/session-async-warn` pins
  exactly that shape. Implemented in `elab_async` (`src/compiler/elab_concurrent.c`).
- **Direction 1 -- still open, and larger than the report first suggested.**
  The compiled `async` is not a fiber that could yield: `tur_async_fiber` /
  `tur_async_fiber_closure` (`emit_module.c`) run the body **synchronously on
  the spawner's stack** and only park when a CPS-lowered `await` inside it hits
  a pending future (`__tur_await_body`, a DK shift). So `(async (fn [] (recv
  r)))` blocks inside the `async` call itself, before it returns a future --
  the `send` that would satisfy it is not merely on the same thread, it is
  later in the same straight-line code. Making sessions compose with compiled
  `async` therefore means making each session op an `await`-shaped suspension
  point: elaborate `recv`/`offer`/`recv-from` (and a `send` whose peer has not
  arrived) inside an async body into a shift on a channel-backed future that
  the peer's op fulfills, with the CPS colouring that implies for every
  function on the path. That is a plan, not a fix, and it should be weighed
  against the fact that `session-spawn` already gives one working spelling on
  both backends.

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
