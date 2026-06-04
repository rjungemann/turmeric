# WebSocket Server Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-01
> **Type:** Networking / stdlib

---

## Overview

`stdlib/httpd` handles HTTP/1.1 requests. WebSocket connections start as
an HTTP Upgrade -- the server receives a regular HTTP request, responds
with `101 Switching Protocols`, and then the connection enters WebSocket
frame mode for the lifetime of the session.

This plan adds `stdlib/ws-server`, a module that plugs into the existing
`httpd-new` / router / middleware stack and lifts a connection from HTTP
to WebSocket. The same worker-pool model used by `stdlib/httpd` is
reused; no new thread infrastructure is required.

A companion plan, `websocket-client-plan.md`, covers `stdlib/ws-client`.

---

## Goals / Non-Goals

### Goals (v0)

- `(ws-upgrade conn handler)` validates the HTTP Upgrade request on
  `conn`, sends the 101 response, and calls `handler` with a `WsConn`
  handle. Returns `:ok` or `:error`.
- `(ws-server-send conn msg)` and `(ws-server-send-bytes conn ptr len)`
  -- send text / binary frames to the connected client.
- `(ws-server-recv conn)` -- receive the next frame (blocks on the
  worker thread; same `WsFrame` struct as the client).
- `(ws-server-close conn)` -- initiate a close handshake.
- Automatic Ping -> Pong response inside `ws-server-recv`.
- Server frames are sent *unmasked* (RFC 6455 §5.1 server-to-client
  frames MUST NOT be masked).
- TLS: `wss://` works transparently because `httpd-new-tls` already
  wraps the socket in TLS before handing it to the handler; `ws-upgrade`
  detects the TLS context on `conn` and uses `tls-write`/`tls-read`.
- `ws-set-server-timeout conn ms` -- idle receive timeout.

### Non-Goals (v0)

- No broadcast / pub-sub hub (composable from multiple connections in
  user code using `chan` and `Mutex`).
- No per-message deflate (RFC 7692).
- No subprotocol negotiation.
- No HTTP/2 upgrade path.
- No reactor-integrated async receive (blocking only, v0).

---

## API Surface

```turmeric
;; stdlib/ws-server.tur

;;; ws-upgrade -- upgrade an HTTP connection to WebSocket.
;;;
;;; Must be called from inside an httpd handler, on the conn that
;;; arrived from the HTTP server. Validates Upgrade headers, sends
;;; "101 Switching Protocols", then calls handler with a WsConn handle.
;;;
;;; Parameters:
;;;   conn    -- httpd connection pointer (:ptr<void>)
;;;   handler -- (fn [ws :int] :void)  called after upgrade succeeds
;;;
;;; Returns:
;;;   :ok on a successful upgrade + handler return.
;;;   :error if the request was not a valid WebSocket Upgrade
;;;   (the caller may then send a normal HTTP error response).
;;;
;;; Example:
;;;   (defroute r "GET" "/ws"
;;;     (fn [conn :ptr<void>] :nil
;;;       (ws-upgrade conn
;;;         (fn [ws :int] :void
;;;           (let [frame (ws-server-recv ws)]
;;;             (ws-server-send ws (ws-frame-text frame))
;;;             (ws-server-close ws))))))
;;;
;;; Since: Phase WS1
(defn ws-upgrade [conn : ptr<void> handler : int] : int ...)

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
(defn ws-server-send [ws : int msg : cstr] : int ...)

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
(defn ws-server-send-bytes [ws : int ptr : ptr<void> len : int] : int ...)

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
(defn ws-server-recv [ws : int] : WsFrame ...)

;;; ws-server-close -- initiate the WebSocket closing handshake.
;;;
;;; Parameters:
;;;   ws -- WsConn handle (server side)
;;;
;;; Since: Phase WS1
(defn ws-server-close [ws : int] : void ...)

;;; ws-set-server-timeout -- set receive timeout in milliseconds.
;;;
;;; Parameters:
;;;   ws -- WsConn handle (server side)
;;;   ms -- timeout in milliseconds (0 = no timeout)
;;;
;;; Since: Phase WS1
(defn ws-set-server-timeout [ws : int ms : int] : void ...)
```

`WsFrame` is shared with `stdlib/ws-client` (defined in
`stdlib/ws-types.tur`).

---

## Integration with `stdlib/httpd`

`ws-upgrade` is designed to be called from any httpd handler -- a
bare handler, a route handler, or a middleware chain. The key invariant
is that the HTTP response has **not yet been flushed** when `ws-upgrade`
is called; the 101 response is written by `ws-upgrade` itself.

```turmeric
;; A minimal echo server mixing REST and WebSocket on the same port.

(import stdlib/httpd :refer [httpd-new httpd-run httpd-free
                              router-new defroute router-mw])
(import stdlib/ws-server :refer [ws-upgrade ws-server-recv
                                  ws-server-send ws-server-close])

(defn main [] : int
  (let [r (router-new)]
    (defroute r "GET" "/health"
      (fn [conn : ptr<void>] : nil
        (httpd-resp-status! conn 200)
        (httpd-resp-body!   conn "ok")))
    (defroute r "GET" "/ws"
      (fn [conn : ptr<void>] : nil
        (ws-upgrade conn
          (fn [ws : int] : void
            (let loop []
              (let [f (ws-server-recv ws)]
                (if (= (ws-frame-kind f) :close)
                  (ws-server-close ws)
                  (do
                    (ws-server-send ws (ws-frame-text f))
                    (loop)))))))))
    (let [h (httpd-new 8080 (router-mw r))]
      (httpd-run h)
      (router-free r)
      (httpd-free h)
      0)))
```

### How `ws-upgrade` plugs into the httpd conn

`httpd` exposes the raw file descriptor via `(httpd-conn-fd conn)`.
`ws-upgrade`:

1. Reads `httpd-req-header conn "Upgrade"` -- must be `"websocket"`.
2. Reads `Sec-WebSocket-Key`, computes `Sec-WebSocket-Accept`.
3. Writes the 101 response directly to `httpd-conn-fd conn` (bypassing
   the normal `httpd-resp-*` helpers, which buffer and add
   `Content-Length`).
4. Marks the `conn` as upgraded (via an internal flag on the conn struct)
   so `httpd` does **not** flush a second response on handler return.
5. Wraps the raw fd (plus any TLS context) into a server-side `WsConn`
   and calls the user handler.

---

## Shared `WsFrame` / codec

Both `stdlib/ws-client` and `stdlib/ws-server` share:

- `stdlib/ws-types.tur` -- `WsFrame` struct + opcode constants.
- `src/stdlib/ws_codec.c` -- frame read/write routines parameterised on
  the `masked` flag (1 for client->server, 0 for server->client).

This avoids duplication of the RFC 6455 codec.

---

## Phases

### Phase WS0 -- Design audit

- Verify `httpd-conn-fd` accessor exists or add it to `stdlib/httpd`.
- Confirm that returning from a handler without calling `httpd-resp-*`
  does not double-flush. Add a `HTTPD_CONN_FLAG_UPGRADED` guard if needed.

### Phase WS1 -- Core server upgrade + echo

- `src/stdlib/ws_types.h`, `src/stdlib/ws_codec.c`.
- `src/stdlib/ws_server.c` + `stdlib/ws-server.tur`.
- Shared `stdlib/ws-types.tur`.
- `tests/fixtures/ws-server-echo/` -- spin up httpd, connect a
  `ws-client` from the same process on a kernel-assigned port, send
  five messages, verify echoes, close cleanly.

### Phase WS2 -- Broadcast pattern + multi-client fixture

- Demonstrate a `Mutex<vec<WsConn>>` broadcast hub in a fixture
  (no framework, just user code + stdlib).
- `tests/fixtures/ws-server-broadcast/` -- three concurrent client
  threads; a message from one is echoed to all three.

### Phase WS3 -- TLS (`wss://`)

- Validate that `httpd-new-tls` + `ws-upgrade` composes correctly.
- Fixture: `wss://127.0.0.1:<port>/ws` round-trip.

### Phase WS4 -- Documentation

- `docs/guides/websocket-server-guide.md` -- quick-start, API reference,
  echo server, broadcast hub, mixed REST+WS server example.
- Update `httpd-guide.md` "See also" section.

---

## Resolved Decisions

1. **Shared codec.** `ws_codec.c` is parameterised on `masked`; both
   client and server call the same read/write path with the appropriate
   flag.
2. **No hub built-in.** A broadcast hub is ~10 lines of Turmeric
   (`Mutex<vec>` + `for` loop); baking it in would constrain the API
   without adding much value.
3. **Upgrade response ownership.** `ws-upgrade` owns the 101 response
   to avoid the caller accidentally sending a second response. The httpd
   conn is marked upgraded and the normal post-handler flush is skipped.

---

## See Also

- [websocket-client-plan.md](websocket-client-plan.md) -- client-side WebSocket
- [httpd-guide.md](../guides/httpd-guide.md) -- HTTP/1.1 server
- [httpd-tls-guide.md](../guides/httpd-tls-guide.md) -- TLS layer
- [reactor-guide.md](../guides/reactor-guide.md) -- event loop
