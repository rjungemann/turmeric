# A module-level `def` whose initializer has a `:linear` type emits no global

**Severity: high** (clean build break on a natural spelling, `cc`-level
diagnostic with no `.tur` attribution). Found 2026-08-28 getting
`turmeric-spices` CI green, against `tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0` (macOS arm64 / Apple
clang), including the non-linear control.

## Repro

```turmeric
(load "stdlib/mutex.tur")
(defmodule m (export)
  (def g (mutex-new))                      ;; Mutex is (defopaque Mutex :ptr<void> :linear)
  (defn f [] : nil
    (do (mutex-lock g) (mutex-unlock g)))
  (defn main [] : int (do (f) 0)))
```

Control -- identical shape, non-linear type:

```turmeric
(load "stdlib/mutex.tur")
(defmodule m2 (export)
  (def n (vec-new))
  (defn f [] : nil (do (vec-push! n 1) nil))
  (defn main [] : int (do (f) 0)))
```

## Observed

`tur check` passes on **both**.

`tur emit-c` on the control emits the global:

```c
static int64_t n_1377;
```

On the linear version it emits **no declaration for `g`** at all, while the use
sites still reference `g_1377`:

```
linear-def_tur.c:7367:22: error: use of undeclared identifier 'g_1377'
 7367 |         mutex_hylock(g_1377);
linear-def_tur.c:7368:24: error: use of undeclared identifier 'g_1377'
 7368 |         mutex_hyunlock(g_1377);
```

So two passes disagree: something decided not to emit the definition, and
nothing told the use-site emitter. The elaborator, meanwhile, is happy with the
program in both directions.

## Expected

Either:

1. **Emit the global.** A linear value in a module-level binding is arguably
   fine -- it is created once, at static-init, and lives for the process. That
   is a legitimate reading of linearity (one production, one lifetime), and it
   is what every real use of this shape wants.
2. **Reject the `def` at check time**, with a diagnostic explaining that a
   linear value cannot live in a module-level binding and naming the
   carrier-cast workaround below.

Emitting references to a symbol that was deliberately not emitted is not a
valid third option. Whichever way the linearity question is decided, the two
passes have to agree.

The interesting question is which. If the intent is that linear globals are
disallowed, then (2) and the diagnostic should exist. If nobody ever decided --
which the silent check pass suggests -- then (1) is the smaller change and
unblocks the real code. Worth answering deliberately rather than by whichever
is easier to patch.

## Where it bit

`spices/ws-server/tests/broadcast_test.tur` and the matching
`fixtures/broadcast/server.tur`, both of which build the WS2 broadcast hub
around `(def hub-mutex (mutex-new))`. Failure was `'hub_hymutex_1866'
undeclared`.

A process-lifetime mutex behind a module-level `def` is *the* shape for a
broadcast hub, a connection pool, or any shared server-side registry, so this is
not an exotic spelling.

## Workaround in user code

Hold the carrier and cast back at each borrow:

```turmeric
(def hub-mutex (:: (mutex-new) :int))
(defn hub-lock!   [] : nil (mutex-lock   (:: hub-mutex Mutex)))
(defn hub-unlock! [] : nil (mutex-unlock (:: hub-mutex Mutex)))
```

This consumes the linear value exactly once at creation, which satisfies
linearity, and leaves an ordinary global behind. It is in use in `ws-server`
today.

Note what the workaround costs: every borrow is now an unchecked
`:int`-to-`Mutex` cast, so the type checker stops helping at exactly the point
where a mutex most needs it. It should be removed when this is fixed -- see
[workarounds-to-remove](workarounds-to-remove.md) for the shape of that
paydown.

## Fix direction

Find the module-level `def` emitter and the linearity check that suppresses it.
The suppression is presumably a "linear values are not statics" guard that
returns early without recording anything; the use-site emitter has no
corresponding guard, which is the actual defect regardless of which resolution
is chosen.

If (1): drop the guard, and make sure static-init ordering runs the initializer
before any function that reads it.

If (2): move the check into the elaborator so it fires at the `def`, not in the
emitter, and give it a code + `tur explain` entry pointing at the carrier-cast
workaround.

## Guides to update when fixed

- docs/guides/substructural-types-guide.md -- state whether a linear value may
  be bound at module level.
- docs/guides/mutable-globals-guide.md -- the module-level binding rules live
  here too.
