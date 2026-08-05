# nng Spice Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-07-26
> **Type:** Networking / spice (turmeric-spices)

---

## Overview

A new `spices/nng` spice wrapping **nng** (nanomsg-next-generation, the
maintained successor to nanomsg, itself a rethink of ZeroMQ), giving
Turmeric programs the classic scalability protocols -- req/rep, pub/sub,
push/pull (pipeline), pair, bus, surveyor/respondent -- over nng's
`inproc://`, `ipc://`, and `tcp://` transports.

The valkey spice is the structural model: a Tier-3 spice whose
`:cmake-deps` fetches the native library statically, opaque `:linear`
connection handles closed exactly once, blocking calls annotated
`#fx{Net}`, `(Result T int)` returns built with `tur_ok_*`/`tur_err_int`
in inline C, and `tests/errors/` compile-fail fixtures asserting the
linear lifecycle diagnostics.

nng manages its own internal worker threads and poller, so the wrapper
stays thin: v0 is the blocking API only (nng's blocking calls park the
calling thread, they do not block nng's internals), with socket-level
send/recv timeouts so nothing can hang a test. The `inproc://` transport
makes the whole test suite network-free and CI-safe.

The companion `docs/upcoming/msgpack-spice-plan.md` is deliberately
paired with this one: msgpack-encoded payloads over nng sockets is the
intended typed-messaging showcase once both land.

---

## Goals / Non-Goals

### Goals (v0)

- One `(defopaque Socket :int :linear)` handle; per-protocol constructors
  `req-open` / `rep-open` / `pub-open` / `sub-open` / `push-open` /
  `pull-open` / `pair-open` / `bus-open` / `surveyor-open` /
  `respondent-open`, each `: (Result Socket int)`.
- `dial` / `listen` (convenience forms; no explicit dialer/listener
  handles in v0) `: (Result nil int)`, `#fx{Net}`.
- Blocking `send-buf` / `recv-buf` on owned byte buffers, plus
  `send-str` / `recv-str` cstr conveniences; all `#fx{Net}`.
- `sub-subscribe [s topic]` / `sub-unsubscribe` (topic prefix matching;
  empty topic = everything).
- `set-recv-timeout-ms` / `set-send-timeout-ms`; a timed-out call
  returns `err` with `NNG_ETIMEDOUT` rather than blocking forever.
- `err-str [code]` via `nng_strerror` for readable failures.
- `close [s : Socket]` as the single linear consumer; `tests/errors/`
  fixtures for double-close, use-after-close, and leak-no-close.
- Round-trip tests over `inproc://` for req/rep, push/pull, pub/sub,
  pair, and bus -- no network, no external daemon.

### Non-Goals (v0)

- No `nng_aio` async operations and no `nng_ctx` per-request contexts
  (the concurrent-server story) -- blocking calls on OS threads
  (`stdlib/thread.tur`) are the v0 concurrency answer, matching valkey.
- No reactor integration. The seam is known -- nng exposes pollable
  receive/send fds via socket options, which plug into
  `reactor-add-fd` -- but it is a follow-up phase, not v0.
- No TLS, WebSocket, or ZeroTier transports (`NNG_ENABLE_TLS=OFF`
  keeps the mbedTLS dependency out of the build).
- No per-protocol socket types in v0 (see Resolved Decisions).
- No zero-copy message API (`nng_msg` stays internal; recv copies into
  an owned buffer and frees the nng allocation immediately).
- No raw-mode sockets, no explicit dialer/listener option tuning.

---

## API Surface

```turmeric
;; spices/nng/src/nng/socket.tur

(defopaque Socket :int :linear)   ;; packs the by-value nng_socket id

;;; req-open -- open a REQ (request) socket.
;;;
;;; Returns:
;;;   (Result Socket int) -- err carries the nng error code.
;;;
;;; Example:
;;;   (let [s (ok-val (req-open))]
;;;     ...
;;;     (close s))
;;;
;;; Since: Phase NG1
(defn req-open [] : (Result Socket int) ...)

;; rep-open, pub-open, sub-open, push-open, pull-open, pair-open,
;; bus-open, surveyor-open, respondent-open -- same shape.

;;; dial -- connect the socket to a remote address.
;;;
;;; Parameters:
;;;   s   -- the socket (borrowed)
;;;   url -- e.g. "inproc://jobs", "ipc:///tmp/x.sock", "tcp://host:5555"
;;;
;;; Returns:
;;;   (Result nil int)
;;;
;;; Since: Phase NG1
(defn dial [^borrow s : Socket url : cstr] #fx{Net} : (Result nil int) ...)

;;; listen -- bind the socket to a local address.
;;; Since: Phase NG1
(defn listen [^borrow s : Socket url : cstr] #fx{Net} : (Result nil int) ...)

;;; close -- close the socket. The single linear consumer.
;;; Since: Phase NG1
(defn close [s : Socket] : nil ...)

;;; err-str -- human-readable message for an nng error code.
;;; Since: Phase NG1
(defn err-str [code : int] : cstr ...)
```

```turmeric
;; spices/nng/src/nng/msg.tur

;;; send-str -- send a NUL-terminated string as one message.
;;;
;;; Blocks until the message is accepted (or the send timeout fires).
;;;
;;; Returns:
;;;   (Result nil int)
;;;
;;; Example:
;;;   (send-str s "job:42")
;;;
;;; Since: Phase NG2
(defn send-str [^borrow s : Socket msg : cstr] #fx{Net} : (Result nil int) ...)

;;; recv-str -- receive one message as a malloc'd cstr; caller frees.
;;;
;;; Blocks until a message arrives (or the recv timeout fires ->
;;; err NNG_ETIMEDOUT). The payload must not contain NUL bytes; use
;;; recv-buf for binary payloads (e.g. msgpack).
;;;
;;; Since: Phase NG2
(defn recv-str [^borrow s : Socket] #fx{Net} : (Result cstr int) ...)

;; send-buf / recv-buf -- same shape over the owned byte-buffer type
;; (length-prefixed; layout matches msgpack's Buf so encoded values
;; flow through without copying twice).

;;; sub-subscribe -- add a topic prefix filter to a SUB socket.
;;;
;;; Parameters:
;;;   s     -- the socket (borrowed); must be a SUB socket (runtime
;;;            NNG_ENOTSUP otherwise -- see Resolved Decisions)
;;;   topic -- byte prefix; "" subscribes to everything
;;;
;;; Since: Phase NG3
(defn sub-subscribe [^borrow s : Socket topic : cstr] : (Result nil int) ...)

;;; set-recv-timeout-ms / set-send-timeout-ms -- socket timeouts.
;;; Since: Phase NG1
(defn set-recv-timeout-ms [^borrow s : Socket ms : int] : (Result nil int) ...)
```

Canonical req/rep round trip:

```turmeric
(let [rep (ok-val (rep-open))
      req (ok-val (req-open))]
  (listen rep "inproc://demo")
  (dial req "inproc://demo")
  (send-str req "ping")
  (let [m (ok-val (recv-str rep))]
    (send-str rep "pong")
    ...)
  (close req)
  (close rep))
```

---

## Implementation Notes

- **`:cmake-deps`** fetches nng from source, static, everything optional
  off (exact tag pinned during NG0; the v1.10.x line is current):

  ```turmeric
  :cmake-deps #map{
    "nng" #map{:url     "https://github.com/nanomsg/nng"
               :ref     "v1.10.1"                    ;; confirm at NG0
               :targets ["nng"]
               :options #map{:BUILD_SHARED_LIBS "OFF"  :NNG_TESTS "OFF"
                             :NNG_TOOLS "OFF"         :NNG_ENABLE_TLS "OFF"}}
  }
  ```

- **Handle packing:** `nng_socket` is a by-value struct holding a
  `uint32_t` id. The opaque `Socket` carries the id in its int64;
  inline C reconstitutes `nng_socket s = { (uint32_t)handle };` at each
  call boundary. Same trick works for `nng_dialer`/`nng_listener` if
  explicit endpoint handles arrive later.
- **Protocol constructors** are one-liners over `nng_req0_open`,
  `nng_rep0_open`, `nng_pub0_open`, `nng_sub0_open`, `nng_push0_open`,
  `nng_pull0_open`, `nng_pair1_open`, `nng_bus0_open`,
  `nng_surveyor0_open`, `nng_respondent0_open` -- all ten ship in v0
  because each costs three lines.
- **recv ownership:** `nng_recv` with `NNG_FLAG_ALLOC` hands back an
  nng-owned buffer; the wrapper copies into a malloc'd cstr / owned buf
  and calls `nng_free` before returning, so no nng allocator ownership
  ever escapes into Turmeric code.
- **Threads:** nng runs its own worker pool; a blocking `recv` parks
  only the calling thread. Concurrent patterns (a rep server thread +
  req client on main) use `stdlib/thread.tur`; the pub/sub test uses
  the standard slow-joiner mitigation (subscribe, then retry the first
  recv with a short timeout while the pub side re-sends).
- **Linear fixtures:** mirror valkey's `tests/errors/` trio --
  `nng-double-close.tur` (TUR-E0101), `nng-use-after-close.tur`
  (TUR-E0101), `nng-leak-no-close.tur` (TUR-E0100).
- **inproc URLs in tests** get unique names per test (`inproc://<test>`)
  so suites can share a process without cross-talk.

---

## Phases

### NG0 -- Research and build spike

Pin the nng tag; prove the `:cmake-deps` FetchContent static build links
into a spice (smoke test: open a pair socket, close it, exit 0). Confirm
macOS + Linux both build with the options above. Record the pinned
version and any CMake option surprises in this plan.

### NG1 -- Socket lifecycle

`nng/socket` module: the ten protocol constructors, `dial`, `listen`,
`close`, `err-str`, both timeout setters. `tests/errors/` linear
fixtures. A lifecycle test that opens/dials/listens/closes over inproc.

### NG2 -- Send / receive

`nng/msg` module: `send-str` / `recv-str` / `send-buf` / `recv-buf`.
req/rep, push/pull, and pair round-trip tests over inproc, including a
timeout test (recv on an empty socket with a 100ms timeout returns
`err NNG_ETIMEDOUT`, not a hang).

### NG3 -- Pub/sub, bus, survey

`sub-subscribe` / `sub-unsubscribe`; pub/sub topic-filter test (two
subscribers, disjoint topics, each sees only its own), bus fan-out test,
surveyor/respondent round trip.

### NG4 -- Documentation

Docstrings to the house standard, spice README with a protocol-selection
table, register in the top-level `:members` list, and a worked
msgpack-over-nng example once the msgpack spice exists (typed job queue:
`derive-msgpack` a Job struct, push/pull it across inproc).

### NG5 -- Follow-ups (explicitly out of v0)

Per-protocol typed sockets, `nng_ctx` concurrent request contexts,
`nng_aio` async, reactor fd integration, explicit dialer/listener
handles, TLS transport. Each graduates into its own plan if wanted.

---

## Resolved Decisions

- **One `Socket` type, not ten.** Per-protocol opaques (`PubSocket`,
  `SubSocket`, ...) would make "recv on a PUB socket" a compile error
  instead of a runtime `NNG_ENOTSUP`, which is the right end-state --
  but it multiplies every shared op by ten or forces a typeclass over
  socket kinds, and the shared-ops class design deserves its own pass.
  v0 ships the single linear `Socket` (still a real opaque, never a
  bare `:int`) and NG5 owns the typed refinement.
- **Blocking-only v0.** Matches valkey, keeps the wrapper thin, and
  nng's internal thread pool means blocking calls are cheap to park.
  The reactor seam (pollable fd socket options) is documented, not
  built.
- **`NNG_FLAG_ALLOC` + immediate copy** rather than exposing `nng_msg`:
  one ownership model (malloc'd, caller frees) across the whole spice
  beats a faster path nobody needs yet.
- **pair1, not pair0** for `pair-open` (pair1 is the maintained
  protocol; polyamorous mode stays off).

---

## See Also

- `spices/valkey/` -- the structural model (linear handles, cmake-deps,
  error fixtures)
- `stdlib/thread.tur`, `stdlib/chan.tur` -- v0 concurrency companions
- `stdlib/reactor.tur` -- the NG5 async integration seam
- `docs/upcoming/msgpack-spice-plan.md` -- companion codec plan; the
  cross-spice typed-messaging showcase
