# WebSocket Server Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-22
> **Type:** Networking / spice

---

## Overview

The `httpd` spice (`turmeric-spices/spices/httpd/`) handles HTTP/1.1
requests. WebSocket connections start as an HTTP Upgrade -- the server
receives a regular HTTP request, responds with `101 Switching Protocols`,
and then the connection enters WebSocket frame mode for the lifetime of
the session.

This plan adds **`ws-server`** as a new spice at
`turmeric-spices/spices/ws-server/` (sibling to the existing `ws-client`
spice). It plugs into the `httpd` spice's worker loop and lifts a
connection from HTTP to WebSocket. The same worker-thread model used by
`httpd` is reused; no new thread infrastructure is required.

The companion `ws-client` spice already exists at
`turmeric-spices/spices/ws-client/` (v0.1.0). `ws-server` mirrors its
layout, opcode constants, and TLS-detection pattern.

---

## Why a spice, not stdlib

The previous draft of this plan placed the module under `stdlib/ws-server`.
We are moving v1 networking out of stdlib and into spices alongside the
rest of the HTTP stack (`httpd`, `ws-client`, `http`, `json`,
`thread-pool` are all spices). Keeping `ws-server` in the same workspace
as `httpd` lets it depend on `httpd` directly via a `:path` dep and lets
both evolve together without churning the stdlib's stability surface.

---

## Goals / Non-Goals

### Goals (v0)

- `(ws-upgrade conn handler)` validates the HTTP Upgrade request on
  `conn`, sends the 101 response, and calls `handler` with a `WsConn`
  handle. Returns `:ok` or `:error`.
- `(ws-server-send conn msg)` and `(ws-server-send-bytes conn ptr len)`
  -- send text / binary frames to the connected client.
- `(ws-server-recv conn)` -- receive the next frame (blocks on the
  worker thread; same `WsFrame` shape as `ws-client`).
- `(ws-server-close conn)` -- initiate a close handshake.
- Automatic Ping -> Pong response inside `ws-server-recv`.
- Server frames are sent **unmasked** (RFC 6455 §5.1 server-to-client
  frames MUST NOT be masked).
- `ws-set-server-timeout conn ms` -- idle receive timeout.

### Non-Goals (v0)

- **TLS (`wss://`).** The current `httpd` spice has no TLS variant
  (there is no `server-start-tls`). `wss://` is deferred until httpd
  grows TLS support. Users who need `wss://` today should terminate TLS
  in front of httpd (e.g. a reverse proxy) or use `ws-client` for
  outbound `wss://` only.
- No broadcast / pub-sub hub (composable from multiple connections in
  user code using `chan` and `Mutex`).
- No per-message deflate (RFC 7692).
- No subprotocol negotiation.
- No HTTP/2 upgrade path.
- No reactor-integrated async receive (blocking only, v0).

---

## Prerequisite: `httpd` needs a `Conn` handle

The previous draft assumed httpd exposed `(httpd-conn-fd conn)` and an
"upgraded" flag on the conn. **Neither exists today.** Httpd's worker
loop only passes the parsed `Request` to the handler; the handler returns
a `Response`, which the worker serializes, writes, and then closes the
socket. There is no place for `ws-upgrade` to reach in and take over the
fd.

See `spices/httpd/src/httpd/server.tur:283-305` (`srv-worker-loop`):

```turmeric
(let [warg-int  (srv-warg->int arg)
      client-fd (srv-warg-fd warg-int)
      handler   (srv-warg-handler warg-int)]
  ...
  (let [resp (:: (srv-call-handler handler req) :Response)
        wbuf (serialize-response resp)]
    (srv-write-all client-fd (wbuf-bytes wbuf) (wbuf-len wbuf))
    (free-wbuf wbuf)
    (free-response resp)
    (free-request (:: req :Request))
    (srv-raw-free raw)
    (srv-close-fd client-fd)))
```

The fd is local to the worker; the handler signature is
`(fn [req : Request] : Response)` and never sees it.

### What httpd must add

Phase WS0 of this plan lands these additions to the `httpd` spice
**before** `ws-server` code begins:

1. **`Conn` opaque handle** (in `httpd/types`) wrapping `{int fd; int
   upgraded; ptr<void> tls;}`. `tls` is reserved for future TLS support
   and stays null in v0.
2. **`conn-fd`** and **`conn-mark-upgraded!`** accessors exported from
   `httpd/server` (or a new `httpd/conn` module).
3. **A new handler shape** that receives both `Conn` and `Request`:
   `(fn [conn : Conn req : Request] : Response)`. The existing
   single-arg `(fn [Request] Response)` handler shape is kept for
   compatibility -- `server-start` continues to accept it; the new
   two-arg shape is opted into via a new `server-start-conn` entry
   point (and `serve-conn` / `serve-pool-conn` companions in
   `httpd/handler`).
4. **Upgrade-aware worker loop**: after the handler returns, check
   `(conn-upgraded? conn)`. If set, skip `serialize-response`,
   `srv-write-all`, and `srv-close-fd` -- ownership of the fd has
   transferred to the WebSocket handler.

These are additive -- existing httpd consumers (`tur-tourist`, the
fixtures, the `serve` macros) keep working unchanged. The new entry
points and `Conn` type are only required for code that wants to call
`ws-upgrade`.

---

## API Surface

```turmeric
;; spices/ws-server/src/ws-server/server.tur

;;; ws-upgrade -- upgrade an HTTP connection to WebSocket.
;;;
;;; Must be called from inside an httpd handler that was registered with
;;; server-start-conn (so the handler receives a Conn alongside the
;;; Request). Validates the Upgrade headers, sends "101 Switching
;;; Protocols", marks the Conn as upgraded so the httpd worker loop will
;;; not flush a second response, then calls handler with a WsConn.
;;;
;;; Parameters:
;;;   conn    -- httpd Conn handle (from the two-arg handler signature)
;;;   req     -- httpd Request (used to read Sec-WebSocket-Key etc.)
;;;   handler -- (fn [ws : WsConn] : void) called after upgrade succeeds
;;;
;;; Returns:
;;;   :ok on a successful upgrade + handler return.
;;;   :error if the request was not a valid WebSocket Upgrade
;;;   (the caller should then return a normal HTTP error Response).
;;;
;;; Example:
;;;   (defn ws-handler [conn : Conn req : Request] : Response
;;;     (if (= (ws-upgrade conn req
;;;              (fn [ws : WsConn] : void
;;;                (let [f (ws-server-recv ws)]
;;;                  (ws-server-send ws (ws-frame-text f))
;;;                  (ws-server-close ws))))
;;;            :ok)
;;;       (resp-ok "")            ;; placeholder; worker skips it
;;;       (bad-request "not a websocket request")))
;;;
;;; Since: Phase WS1
(defn ws-upgrade [conn : Conn req : Request handler : fn] : int ...)

;;; ws-server-send -- send a UTF-8 text frame to the client.
;;;
;;; Parameters:
;;;   ws  -- WsConn handle (server side)
;;;   msg -- null-terminated UTF-8 string
;;;
;;; Returns:
;;;   Bytes written, or negative on error.
;;;
;;; Since: Phase WS1
(defn ws-server-send [ws : WsConn msg : cstr] : int ...)

;;; ws-server-send-bytes -- send a binary frame to the client.
;;;
;;; Parameters:
;;;   ws  -- WsConn handle (server side)
;;;   ptr -- payload buffer
;;;   len -- byte length
;;;
;;; Returns:
;;;   Bytes written, or negative on error.
;;;
;;; Since: Phase WS1
(defn ws-server-send-bytes [ws : WsConn ptr : ptr<void> len : int] : int ...)

;;; ws-server-recv -- receive the next frame from the client.
;;;
;;; Blocks until a frame arrives. Ping frames are answered automatically.
;;;
;;; Parameters:
;;;   ws -- WsConn handle (server side)
;;;
;;; Returns:
;;;   A WsFrame value.
;;;
;;; Since: Phase WS1
(defn ws-server-recv [ws : WsConn] : WsFrame ...)

;;; ws-server-close -- initiate the WebSocket closing handshake.
;;;
;;; Parameters:
;;;   ws -- WsConn handle (server side)
;;;
;;; Since: Phase WS1
(defn ws-server-close [ws : WsConn] : void ...)

;;; ws-set-server-timeout -- set receive timeout in milliseconds.
;;;
;;; Parameters:
;;;   ws -- WsConn handle (server side)
;;;   ms -- timeout in milliseconds (0 = no timeout)
;;;
;;; Since: Phase WS1
(defn ws-set-server-timeout [ws : WsConn ms : int] : void ...)
```

`WsFrame` and the opcode constants (`WS_KIND_TEXT`, `WS_KIND_BINARY`,
`WS_KIND_CLOSE`, `WS_KIND_PING`, `WS_KIND_PONG`) are defined in
`ws-client/src/ws-client/client.tur`. `ws-server` re-declares the same
C struct layout (or re-exports from a future shared `ws-types` spice --
see "Shared frame codec" below).

---

## Spice manifest

```turmeric
;; spices/ws-server/build.tur

(defpackage tur-ws-server
  :name        "tur-ws-server"
  :version     "0.1.0"
  :description "RFC 6455 WebSocket server upgrade for tur-httpd"
  :license     "MIT"
  :spices #{
    "test"  #{:path "../test"}
    "httpd" #{:path "../httpd"}
    ;; ws-client is a dev/test dep -- used by the echo fixture to drive
    ;; the server. ws-server itself does not depend on it at runtime.
    "ws-client" #{:path "../ws-client" :optional true}
  }
  :exports #{
    "ws-server/server" ["WsConn" "WsFrame"
                        "ws-upgrade"
                        "ws-server-send" "ws-server-send-bytes"
                        "ws-server-recv" "ws-server-close"
                        "ws-set-server-timeout"
                        "ws-frame-kind" "ws-frame-data" "ws-frame-len"
                        "ws-frame-text"
                        "ws-text?" "ws-binary?" "ws-ping?" "ws-pong?"
                        "ws-closed?" "ws-timeout?" "ws-error?"]
  })
```

---

## Integration with `httpd`

`ws-upgrade` is designed to be called from any httpd handler registered
with the new `server-start-conn` entry point (or `serve-conn` /
`serve-pool-conn`). The key invariant is that the handler has **not yet
returned a flushable Response** when `ws-upgrade` is called; the 101
response is written by `ws-upgrade` itself directly to the fd, and
`(conn-mark-upgraded! conn)` tells the worker to skip its
serialize-and-write step.

```turmeric
;; A minimal echo server mixing REST and WebSocket on the same port.

(import httpd/types    :refer [Request Response Conn])
(import httpd/server   :refer [server-start-conn server-stop])
(import httpd/request  :refer [req-method req-path])
(import httpd/response :refer [resp-ok not-found bad-request])
(import ws-server/server
  :refer [ws-upgrade ws-server-recv ws-server-send ws-server-close
          ws-frame-kind ws-frame-text])

(defn handler [conn : Conn req : Request] : Response
  (cond
    (and (= (req-method req) "GET") (= (req-path req) "/health"))
    (resp-ok "ok")

    (and (= (req-method req) "GET") (= (req-path req) "/ws"))
    (do
      (ws-upgrade conn req
        (fn [ws : WsConn] : void
          (let loop []
            (let [f (ws-server-recv ws)]
              (if (= (ws-frame-kind f) WS_KIND_CLOSE)
                (ws-server-close ws)
                (do
                  (ws-server-send ws (ws-frame-text f))
                  (loop)))))))
      (resp-ok ""))     ;; placeholder; worker skips it once upgraded

    :else (not-found "")))

(defn main [] : int
  (let [srv (server-start-conn 8080 handler)]
    ;; ... wait / signal ...
    (server-stop srv)
    0))
```

### How `ws-upgrade` plugs into the httpd worker

1. Reads `(req-header req "Upgrade")` -- must be `"websocket"`.
2. Reads `Sec-WebSocket-Key`, computes `Sec-WebSocket-Accept` (reusing
   the SHA-1 + base64 helpers already in `ws-client/client.tur`).
3. Calls `(conn-fd conn)` to get the raw socket and writes the 101
   response directly to it (bypassing `serialize-response`, which would
   otherwise add a `Content-Length`).
4. Calls `(conn-mark-upgraded! conn)` so the httpd worker loop's
   post-handler branch skips `serialize-response`, `srv-write-all`, and
   `srv-close-fd`. The fd is now owned by the `WsConn`.
5. Wraps the fd into a server-side `WsConn` and invokes the user's
   `ws-handler`.
6. On `ws-handler` return, `ws-server-close` is responsible for any
   final close frame and `close(fd)`. The httpd worker has already
   exited the `serialize-response` path so it will not double-close.

---

## Shared frame codec

Both `ws-client` and `ws-server` need:

- The `WsFrame` struct + opcode constants.
- Frame read/write routines parameterised on the `masked` flag (1 for
  client->server, 0 for server->client).

Today `ws-client` inlines its codec as static C helpers in
`client.tur` (`ws__send_frame`, `ws__read_one`). **v0 of `ws-server`
duplicates the same C helpers locally**, hard-coded to the unmasked
server-frame variant. This is deliberately the smallest path -- it
mirrors the pattern `ws-client` already uses and avoids designing a
shared-codec spice before we have two real consumers.

**Phase WS5 (post-v0)** factors the codec out into a new
`ws-codec` spice (or `ws-types`) once both spices are stable and we
have evidence the shared surface is worth a separate package.

---

## Phases

### Phase WS0 -- `httpd` Conn + upgrade hook

In `turmeric-spices/spices/httpd/`:

- Add `Conn` opaque to `httpd/types` -- `{int fd; int upgraded;
  ptr<void> tls;}`.
- Add `conn-fd`, `conn-mark-upgraded!`, `conn-upgraded?` to
  `httpd/server` (or a new `httpd/conn` module).
- Add `server-start-conn`, `serve-conn`, `serve-pool-conn` entry
  points that accept a `(fn [Conn Request] Response)` handler. Existing
  single-arg entry points are kept and unchanged.
- Modify `srv-worker-loop` (server.tur:283) to construct a `Conn` from
  the worker's `client-fd`, pass it to two-arg handlers, and skip the
  `serialize-response` / `srv-write-all` / `srv-close-fd` block when
  `(conn-upgraded? conn)` is true.
- Fixture: a no-op `server-start-conn` handler that returns `resp-ok
  "ok"` and verifies the existing tests still pass.
- Fixture: a handler that calls `(conn-mark-upgraded! conn)`, writes
  `"raw bytes"` directly via the fd, returns a placeholder Response,
  and verifies the worker neither double-writes nor double-closes.

### Phase WS1 -- Core server upgrade + echo (in `ws-server` spice)

- Scaffold `spices/ws-server/` with `build.tur`, `src/ws-server/server.tur`,
  `tests/`.
- Implement `ws-upgrade` (handshake validation, 101 write, mark-upgraded).
- Implement `ws-server-send`, `ws-server-send-bytes`, `ws-server-recv`,
  `ws-server-close`, `ws-set-server-timeout`.
- Implement automatic Ping -> Pong inside `ws-server-recv`.
- Fixture `spices/ws-server/tests/echo/`: spin up `server-start-conn`,
  drive it from a `ws-client` connection in a sibling thread on a
  kernel-assigned port, send five messages, verify echoes, close cleanly.

### Phase WS2 -- Broadcast pattern + multi-client fixture

- Demonstrate a `Mutex<vec<WsConn>>` broadcast hub in a fixture
  (no framework, just user code + spice surface).
- Fixture `spices/ws-server/tests/broadcast/`: three concurrent client
  threads; a message from one is echoed to all three.

### Phase WS3 -- Documentation

- `docs/guides/websocket-server-guide.md` -- quick-start, API
  reference, echo server, broadcast hub, mixed REST+WS example.
- Update `httpd-guide.md` (in the httpd spice or stdlib docs)
  "See also" section.
- Update `docs/upcoming/v1/websocket-client-plan.md` to point at the
  shipped `ws-server` spice.

### Phase WS4 (deferred) -- TLS (`wss://`)

Blocked on httpd growing a TLS variant. The `Conn` struct already has a
reserved `tls` slot; once httpd ships TLS, `ws-upgrade` reads the slot
and switches its handshake-write path to `mbedtls_ssl_write`, mirroring
the `__has_include(<mbedtls/ssl.h>)` pattern used by `ws-client`.

### Phase WS5 (deferred) -- Shared `ws-codec` / `ws-types` spice

Factor the duplicated frame codec out of `ws-client` and `ws-server`
into a shared spice once both are stable.

---

## Resolved Decisions

1. **Spice, not stdlib.** Joins the existing `httpd` / `ws-client` /
   `http` / `json` / `thread-pool` cluster in `turmeric-spices/`.
2. **httpd grows a `Conn` handle.** The previous draft assumed
   `httpd-conn-fd` and an "upgraded" flag already existed; they did
   not. Adding them in Phase WS0 is the prerequisite for any WebSocket
   upgrade path.
3. **Two-arg handler is opt-in.** The existing
   `(fn [Request] Response)` handler shape stays. The new
   `(fn [Conn Request] Response)` shape is reached via
   `server-start-conn` / `serve-conn` so existing httpd consumers
   (`tur-tourist`, fixtures, the `serve` macros) are not disturbed.
4. **TLS deferred.** `httpd` has no TLS variant today; `wss://` waits
   until it does. The `Conn.tls` slot is reserved so the future patch
   is local.
5. **Codec duplicated, not shared, in v0.** Mirrors `ws-client`'s own
   inline-codec layout. A shared `ws-codec` spice waits until both
   sides are stable.
6. **Upgrade response ownership.** `ws-upgrade` owns the 101 response
   to avoid the caller accidentally sending a second response. The
   `Conn` is marked upgraded and the worker's normal
   serialize-and-write step is skipped.
7. **No hub built-in.** A broadcast hub is ~10 lines of Turmeric
   (`Mutex<vec>` + `for` loop); baking it in would constrain the API
   without adding much value.

---

## See Also

- [websocket-client-plan.md](websocket-client-plan.md) -- client-side WebSocket (shipped as `ws-client` spice)
- [httpd-guide.md](../../guides/httpd-guide.md) -- HTTP/1.1 server
- `turmeric-spices/spices/httpd/` -- httpd spice source
- `turmeric-spices/spices/ws-client/` -- ws-client spice source
