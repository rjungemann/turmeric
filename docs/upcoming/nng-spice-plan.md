# nng Spice Plan

> **Status:** v0 shipped -- `turmeric-spices/spices/nng`
> **Last Updated:** 2026-09-16
> **Type:** Networking / spice (turmeric-spices)

---

## Overview

A `spices/nng` spice wrapping **nng** (nanomsg-next-generation, the
maintained successor to nanomsg, itself a rethink of ZeroMQ), giving
Turmeric programs the classic scalability protocols -- req/rep, pub/sub,
push/pull (pipeline), pair, bus, surveyor/respondent -- over nng's
`inproc://`, `ipc://`, and `tcp://` transports.

The valkey spice is the structural model: a Tier-3 spice whose
`:cmake-deps` fetches the native library statically, opaque `:linear`
connection handles closed exactly once, blocking calls annotated
`#fx{Net}`, `(Result T int)` returns built with `tur_ok_*`/`tur_err_int`
in inline C, and `errors/` compile-fail fixtures asserting the linear
lifecycle diagnostics.

nng manages its own internal worker threads and poller, so the wrapper
stays thin: v0 is the blocking API only (nng's blocking calls park the
calling thread, they do not block nng's internals), with socket-level
send/recv timeouts so nothing can hang a test. The `inproc://` transport
makes the whole test suite network-free and CI-safe.

The companion `docs/upcoming/msgpack-spice-plan.md` was deliberately
paired with this one: msgpack-encoded payloads over nng sockets is the
typed-messaging showcase. Both have landed, and the showcase is real --
`spices/nng/tests/nng/msgpack_test.tur` derives a codec for a `Job`
struct, pushes it across `inproc://`, and decodes it on the other side.

---

## Status

| Phase | State | Notes |
| --- | --- | --- |
| NG0 research and build spike | **done** | pinned **v1.12.4**, not the 1.10.x the draft guessed |
| NG1 socket lifecycle | **done** | 10 constructors, dial/listen/close, timeouts, error helpers |
| NG2 send / receive | **done** | str and binary paths, round trips, timeout tests |
| NG3 pub/sub, bus, survey | **done** | topic filters, fan-out, survey round trip |
| NG4 documentation | **done** | docstrings, README, `:members`, msgpack showcase |
| NG5 follow-ups | **not started** | out of v0 by design; see below |

**52 assertions across 5 suites, all over `inproc://`** -- no network, no
port, no daemon -- plus **3 compile-fail fixtures** under `errors/run.sh`.
Both fetch and build take about 13 seconds from cold on a 4-core box.

Two compiler defects were found on the way and are filed rather than
worked around silently; each has a pointer from the spice source to its
report. See "Found on the way" below.

---

## Goals / Non-Goals

### Goals (v0) -- all shipped

- One `(defopaque Socket :int :linear)` handle; per-protocol constructors
  `req-open` / `rep-open` / `pub-open` / `sub-open` / `push-open` /
  `pull-open` / `pair-open` / `bus-open` / `surveyor-open` /
  `respondent-open`, each `: (Result Socket int)`.
- `dial` / `listen` (convenience forms; no explicit dialer/listener
  handles in v0) `: (Result nil int)`, `#fx{Net}`.
- Blocking `send-payload` / `recv-payload` on owned byte buffers, plus
  `send-str` / `recv-str` cstr conveniences; all `#fx{Net}`.
- `sub-subscribe [s topic]` / `sub-unsubscribe` (topic prefix matching;
  empty topic = everything).
- `set-recv-timeout-ms` / `set-send-timeout-ms`; a timed-out call
  returns `err` with `NNG_ETIMEDOUT` rather than blocking forever, and
  `timed-out?` recognises it without the caller learning the number.
- `err-str [code]` via `nng_strerror` for readable failures.
- `close [s : Socket]` as the single linear consumer; `errors/`
  fixtures for double-close, use-after-close, and leak-no-close.
- Round-trip tests over `inproc://` for req/rep, push/pull, pub/sub,
  pair, bus, and survey -- no network, no external daemon.

One name moved from the draft and stuck: `Buf` / `send-buf` / `recv-buf`
became `Payload` / `send-payload` / `recv-payload`, recorded under Resolved
Decisions with the reason. A second moved and moved back -- `(Result nil int)`
briefly became `(Result nil int)` around a compiler defect, and is `nil` again
now that the defect is fixed.

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

(defn dial   [^borrow s : Socket url : cstr] #fx{Net} : (Result nil int) ...)
(defn listen [^borrow s : Socket url : cstr] #fx{Net} : (Result nil int) ...)
(defn close  [s : Socket] : nil ...)

(defn set-recv-timeout-ms [^borrow s : Socket ms : int] : (Result nil int) ...)
(defn set-send-timeout-ms [^borrow s : Socket ms : int] : (Result nil int) ...)

(defn err-str    [code : int] : cstr ...)   ;; nng_strerror
(defn timed-out? [code : int] : bool ...)   ;; == NNG_ETIMEDOUT
```

```turmeric
;; spices/nng/src/nng/msg.tur

;;; send-str -- send a NUL-terminated string as one message.
;;; Blocks until nng accepts it (or the send timeout fires).
;;; Since: Phase NG2
(defn send-str [^borrow s : Socket msg : cstr] #fx{Net} : (Result nil int) ...)

;;; recv-str -- receive one message as a malloc'd cstr; caller frees.
;;; Blocks until a message arrives (or the recv timeout fires ->
;;; err NNG_ETIMEDOUT). The payload must not contain NUL bytes; use
;;; recv-payload for binary payloads (e.g. msgpack).
;;; Since: Phase NG2
(defn recv-str [^borrow s : Socket] #fx{Net} : (Result cstr int) ...)

;;; send-payload BORROWS its Payload (nng copies); recv-payload returns a
;;; fresh owned one. Binary-safe: embedded NUL bytes survive both ways.
(defn send-payload [^borrow s : Socket b : Payload] #fx{Net} : (Result nil int) ...)
(defn recv-payload [^borrow s : Socket] #fx{Net} : (Result Payload int) ...)

;;; sub-subscribe -- add a topic prefix filter to a SUB socket.
;;; `s` must be a SUB socket (runtime NNG_ENOTSUP otherwise); "" takes
;;; everything, and a SUB socket with NO subscription receives nothing.
;;; Since: Phase NG3
(defn sub-subscribe   [^borrow s : Socket topic : cstr] : (Result nil int) ...)
(defn sub-unsubscribe [^borrow s : Socket topic : cstr] : (Result nil int) ...)
```

```turmeric
;; spices/nng/src/nng/payload.tur -- the owned byte buffer

(defopaque Payload :ptr<void>)    ;; { int64 len; uint8 data[] }

payload-alloc  payload-of-cstr
payload-len    payload-data     payload-byte
payload-empty? payload=?
payload->cstr  payload->hex
payload-free
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
  off:

  ```turmeric
  :cmake-deps #map{
    "nng" #map{:url     "https://github.com/nanomsg/nng"
               :ref     "v1.12.4"
               :targets ["nng"]
               :options #map{:BUILD_SHARED_LIBS "OFF"
                             :NNG_TESTS         "OFF"
                             :NNG_TOOLS         "OFF"
                             :NNG_ENABLE_NNGCAT "OFF"
                             :NNG_ENABLE_TLS    "OFF"}}
  }
  ```

  `NNG_TESTS` / `NNG_TOOLS` default to **ON** for a native build
  (`NNG_NATIVE_BUILD` in nng's `cmake/NNGOptions.cmake`), which is what
  FetchContent gives us -- so both have to be turned off explicitly or
  the dep build also compiles nng's own test corpus and `nngcat`.
  `NNG_ENABLE_TLS` already defaults OFF; it is pinned so a future
  default flip cannot silently pull in mbedTLS.
- **Headers need no system package.** nng lays its public headers under
  `include/nng/`, so `${nng_SOURCE_DIR}/include` -- which `tur fetch`
  puts on the compile line whenever a `include/` exists -- is exactly
  what `#include <nng/nng.h>` wants. This is the opposite of valkey,
  whose fetched hiredis tree has no `hiredis/` subdirectory and so needs
  `libhiredis-dev` for its headers alone. **Nothing to add to
  `spices/ci.yml`'s apt/brew steps.**
- **Handle packing:** `nng_socket` is a by-value struct holding a
  `uint32_t` id. The opaque `Socket` carries the id in its int64;
  inline C reconstitutes `nng_socket sk; sk.id = (uint32_t)s;` at each
  call boundary. Same trick would work for `nng_dialer`/`nng_listener`.
- **Protocol constructors** are one-liners over `nng_req0_open`,
  `nng_rep0_open`, `nng_pub0_open`, `nng_sub0_open`, `nng_push0_open`,
  `nng_pull0_open`, `nng_pair1_open`, `nng_bus0_open`,
  `nng_surveyor0_open`, `nng_respondent0_open` -- all ten ship in v0
  because each costs three lines.
- **Subscriptions** use `nng_sub0_socket_subscribe` /
  `nng_sub0_socket_unsubscribe` rather than `nng_socket_set` with
  `NNG_OPT_SUB_SUBSCRIBE`: the typed entry points exist in the 1.12 line
  and say what they do.
- **recv ownership:** `nng_recv` with `NNG_FLAG_ALLOC` hands back an
  nng-owned buffer; the wrapper copies into a malloc'd cstr / owned
  Payload and calls `nng_free` before returning, so no nng allocator
  ownership ever escapes into Turmeric code.
- **Threads:** nng runs its own worker pool; a blocking `recv` parks
  only the calling thread. Concurrent patterns (a rep server thread +
  req client on main) use `stdlib/thread.tur`.
- **Slow joiner** is real for pub/sub, bus, and survey -- a broadcast
  sent before a peer's pipe is up is dropped, not queued. Those tests
  re-send and re-try on a short receive timeout rather than sending once
  and asserting. req/rep and push/pull need no such thing.
- **`inproc` URLs in tests** get unique names per test (`inproc://<test>`)
  so suites can share a process without cross-talk; a second `listen` on
  a live name is `NNG_EADDRINUSE`, and a test asserts exactly that.
- **Link flags.** nng's `INTERFACE_LINK_LIBRARIES` carries
  `$<LINK_ONLY:Threads::Threads>`, which `file(GENERATE)` leaves in the
  manifest as `Threads::Threads` and a run of `::@(0x...)` genex
  markers. `append_link_flag_token` (`src/compiler/pkg.c`) already skips
  any token containing `::`, so what reaches the linker is the
  `libnng.a` path plus `-lpthread`. Nothing to fix -- recorded because
  a reader of `cmake/spice-deps-manifest.json` will see the markers and
  wonder.

---

## Found on the way

Both were filed with a minimal repro and are now **fixed and archived** -- each
turned out to be a shared-helper problem rather than a one-site patch, which is
why the fixes are worth reading even though the spice shipped around them first.
Both spice-side follow-ups are settled (turmeric-spices#75), and they settled
differently, which is the part worth carrying forward:

- **`Ack` is gone.** `dial` / `listen` / `sub-subscribe` and both timeout
  setters return `(Result nil int)`, the signature this plan specified from the
  start.
- **`send-until-received?` keeps its delegation.** It could inline the receive
  now, and it should not: `recv-str=?` has twelve call sites, and re-proving a
  fixed defect that a compiler fixture already pins is not worth duplicating a
  twelve-caller helper. Only the comment claiming the delegation was
  load-bearing was stale.

"Undo the workaround" is the obvious reading of a fixed report and it is right
about half the time.

- **[`tail-recursive-let-drops-carrier-bridge`](../archive/tail-recursive-let-drops-carrier-bridge.md)**
  (medium). A `let` that binds a carrier-returning producer inside a
  **self-tail-recursive** body is emitted as `struct x = <int64_t>;` --
  `emit_tail`'s inline `EX_LET` arm assigns `emit_value`'s result
  straight into a by-value-typed local instead of routing it through
  `emit_carrier_bridge`, which the non-TCO path does. Hit writing the
  pub/sub retry helper, which is the obvious spelling of a poll. Hard
  `cc` error, so loud; worked around by delegating the receive to a
  non-recursive helper.
- **[`result-nil-ok-payload-emits-void-field`](../archive/result-nil-ok-payload-emits-void-field.md)**
  (low-medium). `(Result nil E)` type-checks and then emits
  `struct { void _0; } Ok;`. This is why `Ack` briefly existed.

---

## Resolved Decisions

- **Pin v1.12.4, not v1.10.x.** The draft guessed at the 1.10 line; 1.12
  is current stable and 2.0 is still beta (`v2.0.0-beta.2` at NG0). Every
  entry point this spice uses is present in 1.12.4, including
  `NNG_FLAG_ALLOC`, which 2.0 removes in favour of `nng_recvmsg` -- so a
  2.0 move is an NG5 item with real work in it, not a version bump.
- **`(Result nil int)`, as drafted -- after a detour.** `nil` is the right
  type for "worked, carries nothing" and the draft specified it, but a `nil`
  ok payload emitted a C `void` union member and the monomorph would not
  compile, so v0 shipped a `(defopaque Ack :int)` stand-in. That compiler
  defect is fixed (report above) and `Ack` is gone. The alternative it
  avoided throughout -- `(Result int int)` with an "ok carries 0"
  convention -- is precisely the `:int` stand-in `CLAUDE.md` forbids.
- **`Payload`, not `Buf`.** An opaque's name resolves globally, so two
  spices that each define a `Buf` cannot both be loaded by one program --
  and tur-msgpack's byte carrier is `Buf`. Naming this one `Buf` (as the
  draft did) would have made msgpack-over-nng, the pairing both plans
  were written for, impossible to compile. tur-json 0.4.0 renamed its
  `Encode` / `Decode` classes for the same reason. The **layout** is
  still deliberately identical, so the crossing is one copy against a
  documented shape; the two bridge helpers live in the showcase test,
  not in either spice, and are what a stdlib `Bytes` would replace.
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
- **`timed-out?` is part of the surface.** A polling receive branches on
  exactly one error code, and making every caller learn that
  `NNG_ETIMEDOUT` is 5 is the sentinel-convention problem in miniature.

---

## NG5 -- follow-ups (explicitly out of v0)

Each graduates into its own plan if wanted:

- Per-protocol typed sockets (the typeclass-over-socket-kinds design).
- `nng_ctx` concurrent request contexts and `nng_aio` async.
- Reactor fd integration (`NNG_OPT_RECVFD` / `NNG_OPT_SENDFD` ->
  `reactor-add-fd`).
- Explicit dialer/listener handles and their option tuning.
- TLS transport (turns `NNG_ENABLE_TLS` back on and pulls in mbedTLS;
  note tur-tls already vendors mbedTLS 3.6.2, so the two builds want
  coordinating rather than duplicating).
- The nng 2.0 line, whose `nng_recvmsg`-only receive path is a real
  rewrite of `nng/msg`, not a bump.

---

## See Also

- `spices/nng/README.md` -- the shipped surface, with a
  protocol-selection table
- `spices/valkey/` -- the structural model (linear handles, cmake-deps,
  error fixtures)
- `stdlib/thread.tur`, `stdlib/chan.tur` -- v0 concurrency companions
- `stdlib/reactor.tur` -- the NG5 async integration seam
- `docs/upcoming/msgpack-spice-plan.md` -- companion codec plan; the
  cross-spice typed-messaging showcase, now real
