---
title: Reactor Guide
category: Concurrency and Async
description: Lightweight evented I/O with tur/reactor
---

# Reactor Guide

`tur/reactor` is a single-threaded event loop for multiplexing file
descriptors, timers, OS signals, and channel receives without requiring
the cooperative fiber scheduler.

## When to use `tur/reactor`

Use the reactor when you need to:

- Watch one or more file descriptors in a dedicated listener thread (e.g.
  a TCP accept loop in `tur/httpd`).
- Fire callbacks after a delay or on a repeating interval without a
  background thread.
- Catch an OS signal cleanly from within an event loop.
- Bridge a cross-thread channel into a reactor loop with low latency.

If your code already runs inside a fiber and uses `async-socket-*` /
`async-file-*`, those facilities already park on the same `IOBackend`
underneath -- you do not need the reactor. The reactor is for code that
runs *outside* the fiber scheduler, or for dedicated non-fiber threads.

## Quick start

```turmeric
(import reactor)

(defn main [] :int
  (let [r (reactor-new)]
    (reactor-add-timer r 0
      (fn [id user] :nil (println "hello from timer"))
      nil)
    (reactor-run r)
    (reactor-free r)
    0))
```

## Programming styles

### Style 1 -- Pure callback (no scheduler needed)

The simplest pattern: register sources, run the loop, clean up.
This is the pattern used by the `tur/httpd` listener thread.

```turmeric
(import reactor)

;; accept-loop is a plain OS thread entry point -- no fibers required.
(defn start-listener [listen-fd :int stop-ch :ptr<void>] :nil
  (let [r (reactor-new)]
    ;; Accept connections: each READ event means accept() will not block.
    (reactor-add-fd r listen-fd READ
      (fn [id events user]
        (let [client (accept-conn listen-fd)]
          (when (!= client -1)
            (pool-submit worker-pool client))))
      nil)
    ;; Shutdown: one value on stop-ch ends the loop (one-shot).
    (reactor-add-chan r stop-ch
      (fn [id v user] :nil
        (reactor-stop r))
      nil)
    ;; Block until stopped.
    (reactor-run r)
    (reactor-free r)))
```

### Style 2 -- Reactor + channels (cross-thread coordination)

A channel bridges the reactor thread and any other OS thread. The sender
calls `reactor-wake` after `chan-send` so the blocking poll returns
promptly.

```turmeric
(import reactor)
(import chan)

;; In the reactor thread:
(defn run-with-stop [stop-ch :ptr<void>] :nil
  (let [r (reactor-new)]
    ;; ... register fd sources, timers, etc. ...

    ;; Register one-shot shutdown watcher.
    (reactor-add-chan r stop-ch
      (fn [id v user] :nil (reactor-stop r))
      nil)

    (reactor-run r)
    (reactor-free r)))

;; From any other thread (safe because reactor-wake is thread-safe):
(defn request-shutdown [stop-ch :ptr<void> r :ptr<void>] :nil
  (chan-send stop-ch 1)
  (reactor-wake r))
```

The key constraint: `reactor-add-chan` is one-shot. If you need a
persistent channel watcher, re-register from inside the callback:

```turmeric
(defn watch-chan-loop [r :ptr<void> ch :ptr<void>] :nil
  (reactor-add-chan r ch
    (fn [id v user] :nil
      (handle-message v)
      ;; Re-register to watch the next value.
      (watch-chan-loop r ch))
    nil))
```

### Style 3 -- Reactor drives a local fiber group (future milestone)

For libraries that want fibers but don't want to take over the global
scheduler, `reactor-run-fibers` (a planned future addition) will be a
thin convenience layer that:

- pumps a local fiber queue between `reactor-poll` calls
- maps fiber park/unpark to `reactor-add-fd` / source removal

This is the path where a future rewrite of `src/async/scheduler.c` would
land -- the scheduler becomes "the global reactor's fiber driver." This is
not yet implemented; see `docs/tur-reactor-plan.md` (Phase R9).

## API reference

### Lifecycle

| Function | Description |
|---|---|
| `reactor-new` | Create a reactor for the current thread. Returns nil on WASM or init failure. |
| `reactor-free` | Free the reactor and unregister all attached sources. |

### Source registration

| Function | Returns | Notes |
|---|---|---|
| `reactor-add-fd` | source-id | Watch an fd for READ, WRITE, ERROR, or HUP. |
| `reactor-modify-fd` | 0 / -1 | Change the event mask for an existing fd source. |
| `reactor-remove` | 0 / -1 | Unregister any source by id. Does not close the fd. |
| `reactor-add-timer` | source-id | One-shot timer; auto-deactivates after firing. |
| `reactor-add-interval` | source-id | Repeating timer; stays active until `reactor-remove`. |
| `reactor-add-signal` | source-id | Catch an OS signal. Not available on WASM. |
| `reactor-add-chan` | source-id | One-shot channel watcher. Re-register for persistence. |

### Running

| Function | Notes |
|---|---|
| `reactor-poll` | Single iteration; blocks up to `timeout-ms` (-1 = forever, 0 = non-blocking). |
| `reactor-run` | Loop until stopped or all sources removed. |
| `reactor-stop` | Thread-safe. Causes `reactor-run` to return after the current dispatch round. |
| `reactor-wake` | Thread-safe. Interrupts a blocking `reactor-poll` / `reactor-run`. |

### Event mask constants

| Constant | Value | Meaning |
|---|---|---|
| `READ` | 1 | fd is readable or accept is ready |
| `WRITE` | 2 | fd is writable or connect completed |
| `ERROR` | 4 | Error condition (always reported; cannot be masked) |
| `HUP` | 8 | Peer hung up or EOF |

## Callback signatures

Each source type uses a distinct callback arity:

```turmeric
;; reactor-add-fd, reactor-add-signal:
(fn [id :int events :int user :ptr<void>] :nil ...)

;; reactor-add-timer, reactor-add-interval:
(fn [id :int user :ptr<void>] :nil ...)

;; reactor-add-chan:
(fn [id :int value :int user :ptr<void>] :nil ...)
```

`id` is the source id returned at registration. `events` is a bitmask of
`READ | WRITE | ERROR | HUP`. For signal callbacks, `events` carries the
signal number. For channel callbacks, `value` is the int64 dequeued from
the channel.

## Threading model

A reactor is **not** thread-safe. One reactor per thread. The only
thread-safe operations are `reactor-wake` and `reactor-stop`. All source
registration, modification, and callback dispatch must happen on the
thread that calls `reactor-poll` or `reactor-run`.

To hand work across threads: use a channel and call `reactor-wake` after
`chan-send`. The channel watcher fires on the next poll iteration.

## Linking

Programs that import `reactor` must link against `libturi`:

```turmeric
;; The autolink hint is included when you (import reactor).
;; For standalone files without import, add this defn:
(defn reactor-link [] :int
  ```c /* __tur_autolink__: -lturi */
  return 0;
  ```)
```

When running `tur build`, this is handled automatically by the
`reactor/autolink-hint` function that ships with the stdlib module.

## Platform notes

| Platform | I/O Backend | Signal backend |
|---|---|---|
| Linux | epoll | signalfd |
| macOS / BSD | kqueue | self-pipe + sigaction |
| WASM | no-op sentinel | not available |

`reactor-new` returns nil on WASM. Guard with a nil check if your code
must compile for multiple targets.

## See also

- `stdlib/reactor.tur` -- module source and full API docstrings
- `docs/tur-reactor-plan.md` -- design rationale and phase roadmap
- `docs/tur-httpd-plan.md` -- first consumer; shows the listener-thread pattern
- `stdlib/chan.tur` -- channel creation (`chan-new`, `chan-send`, `chan-recv`)
- `docs/guides/async-await-guide.md` -- fiber-based async I/O (alternative for fiber code)
