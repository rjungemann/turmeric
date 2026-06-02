# WebSocket Client Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-01
> **Type:** Networking / stdlib

---

## Overview

Turmeric has `stdlib/httpd` for HTTP/1.1 servers and `tur-tls` for TLS
termination, but no WebSocket support. This plan covers a
`stdlib/ws-client` module implementing the client side of RFC 6455
(WebSocket). A companion plan, `websocket-server-plan.md`, covers
`stdlib/ws-server`.

The client lets Turmeric programs connect to any WebSocket endpoint --
local servers, third-party APIs, browser dev-tools, real-time services --
and exchange text and binary frames. It sits atop plain POSIX sockets
and optionally threads through `tur-tls` for `wss://` URIs.

---

## Goals / Non-Goals

### Goals (v0)

- `(ws-connect uri)` returns an opaque `WsConn` handle; throws on failure.
- `(ws-send conn "hello")` sends a UTF-8 text frame.
- `(ws-send-bytes conn ptr len)` sends a binary frame.
- `(ws-recv conn)` blocks until a frame arrives; returns a `WsFrame`
  struct with kind (`:text` / `:binary` / `:ping` / `:pong` / `:close`),
  payload pointer, and length.
- `(ws-close conn)` sends a Close frame, reads the echo, closes the socket.
- `(ws-free conn)` unconditionally releases memory and the fd.
- TLS (`wss://`) via `tur-tls`: if the URI scheme is `wss`, the handshake
  is wrapped in a `tls-ctx` automatically.
- Ping/pong: `ws-recv` responds to Ping frames automatically; exposes Pong
  frames to the caller.
- Masking: all client-to-server frames are masked per RFC 6455 §5.3.
- Non-blocking mode: `(ws-set-timeout conn ms)` sets `SO_RCVTIMEO` so
  `ws-recv` returns a `:timeout` frame kind instead of blocking forever.

### Non-Goals (v0)

- No async / reactor integration (blocking I/O only, v0).
- No per-message deflate (RFC 7692 permessage-deflate).
- No HTTP proxy tunnelling (`CONNECT`).
- No subprotocol negotiation (`Sec-WebSocket-Protocol`).
- No extension negotiation beyond the above.
- No streaming / fragmented sends > a single continuation frame.

---

## API Surface

```turmeric
;; stdlib/ws-client.tur

;;; ws-connect -- open a WebSocket connection to uri.
;;;
;;; Parameters:
;;;   uri -- a ws:// or wss:// URI string
;;;
;;; Returns:
;;;   An opaque WsConn handle on success.
;;;   Throws an error string on connection or handshake failure.
;;;
;;; Example:
;;;   (let [c (ws-connect "ws://localhost:9000/feed")]
;;;     ...)
;;;
;;; Since: Phase WC1
(defn ws-connect [uri :cstr] :int ...)

;;; ws-send -- send a UTF-8 text frame.
;;;
;;; Parameters:
;;;   conn -- WsConn handle
;;;   msg  -- null-terminated UTF-8 string
;;;
;;; Returns:
;;;   Number of bytes written to the socket, or negative on error.
;;;
;;; Example:
;;;   (ws-send c "ping")
;;;
;;; Since: Phase WC1
(defn ws-send [conn :int msg :cstr] :int ...)

;;; ws-send-bytes -- send a binary frame.
;;;
;;; Parameters:
;;;   conn -- WsConn handle
;;;   ptr  -- pointer to payload buffer
;;;   len  -- byte length of payload
;;;
;;; Returns:
;;;   Number of bytes written, or negative on error.
;;;
;;; Since: Phase WC1
(defn ws-send-bytes [conn :int ptr :ptr<void> len :int] :int ...)

;;; ws-recv -- receive the next WebSocket frame.
;;;
;;; Blocks until a complete frame arrives (or the timeout fires).
;;; Responds to Ping frames automatically; Pong frames are surfaced.
;;;
;;; Parameters:
;;;   conn -- WsConn handle
;;;
;;; Returns:
;;;   A WsFrame value.
;;;
;;; Since: Phase WC1
(defn ws-recv [conn :int] :WsFrame ...)

;;; ws-close -- initiate the WebSocket closing handshake.
;;;
;;; Parameters:
;;;   conn -- WsConn handle
;;;
;;; Since: Phase WC1
(defn ws-close [conn :int] :void ...)

;;; ws-free -- unconditionally release all resources for conn.
;;;
;;; Parameters:
;;;   conn -- WsConn handle
;;;
;;; Since: Phase WC1
(defn ws-free [conn :int] :void ...)

;;; ws-set-timeout -- set receive timeout in milliseconds.
;;;
;;; A value of 0 disables the timeout (default: blocks indefinitely).
;;; When the timeout fires, ws-recv returns a WsFrame with kind :timeout.
;;;
;;; Parameters:
;;;   conn -- WsConn handle
;;;   ms   -- timeout in milliseconds
;;;
;;; Since: Phase WC1
(defn ws-set-timeout [conn :int ms :int] :void ...)
```

### `WsFrame` struct

```turmeric
(defstruct WsFrame
  [kind    :int   ;; :text | :binary | :ping | :pong | :close | :timeout | :error
   data    :ptr<void>
   len     :int
  ])
```

`data` points into an internal buffer that is valid until the next call
to `ws-recv` or `ws-free`. Callers that need to keep the payload past
the next receive should memcpy it.

---

## Implementation Notes

### Handshake (RFC 6455 §4)

1. Parse the URI for host, port, path, and scheme.
2. Open a TCP socket; for `wss://` wrap it with `tls-connect` from
   `tur-tls` before step 3.
3. Send an HTTP/1.1 Upgrade request with a random 16-byte base64-encoded
   `Sec-WebSocket-Key`.
4. Read the HTTP/1.1 101 Switching Protocols response and verify the
   `Sec-WebSocket-Accept` header (SHA-1 of key + GUID, base64).
5. Socket is now in frame mode.

Base64 and SHA-1 are needed only at handshake time. Use inline-C
wrappers over `mbedtls_sha1` (already available via `tur-tls`) and a
small `base64_encode` / `base64_decode` added to `src/util/base64.c`.

### Frame codec (RFC 6455 §5)

Each frame:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)    |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+-------------------------------+
|     Extended payload length continued, if payload len == 127  |
+- - - - - - - - - - - - - - -+-------------------------------+
|                               |Masking-key, if MASK set to 1  |
+-------------------------------+-------------------------------+
| Masking-key (continued)       |          Payload Data         |
+-------------------------------- - - - - - - - - - - - - - - - +
:                     Payload Data continued ...                :
+---------------------------------------------------------------+
```

- Opcodes: `0x0` continuation, `0x1` text, `0x2` binary, `0x8` close,
  `0x9` ping, `0xA` pong.
- Client frames: MASK bit always 1; 4-byte key XOR'd over payload.
- Server frames: MASK bit always 0.
- Fragmented messages are reassembled before surfacing to the caller.
- Payload > 2^16 bytes uses the 64-bit length field; no artificial cap.

### TLS integration

`ws-connect` checks the URI scheme. For `wss://`:

```turmeric
(let [ctx (tls-client-ctx-new)
      fd  (tcp-connect host port)]
  (tls-handshake ctx fd host)
  (ws-handshake-over-tls ctx ...))
```

The `WsConn` object stores a `is_tls :int` flag; `ws-send` and `ws-recv`
dispatch to `tls-write`/`tls-read` vs. plain `send`/`recv` accordingly.

---

## Phases

### Phase WC0 -- Research and fixtures

- Survey RFC 6455 edge cases (masking, fragmentation, close codes).
- Write fixture programs that connect to `ws://127.0.0.1:<port>` using
  the planned API.

### Phase WC1 -- Plain `ws://` client

- `src/stdlib/ws_client.c` + `stdlib/ws-client.tur`.
- `base64_encode` / `base64_decode` in `src/util/base64.c`.
- SHA-1 via `mbedtls_sha1` (already pulled in by `tur-tls`; link
  unconditionally for stdlib builds).
- Frame codec: send (with masking) and receive (no masking).
- Automatic Ping -> Pong response inside `ws-recv`.
- Graceful close handshake.
- `tests/fixtures/ws-client-text/` and `tests/fixtures/ws-client-binary/`
  using a minimal in-process echo server.

### Phase WC2 -- `wss://` support

- Thread TLS through `ws-connect` for `wss://` URIs.
- Fixture: connect to a self-signed `wss://` echo server spun up by the
  test harness.

### Phase WC3 -- Timeout + non-blocking improvements

- `ws-set-timeout`, `:timeout` frame kind.
- Fixture: timeout fires when no message arrives within the window.

### Phase WC4 -- Documentation

- `docs/guides/websocket-client-guide.md` with quick-start, full API
  reference, and common patterns (chat bot, market data feed, echo test).
- Cross-link from `httpd-guide.md` "See also" section.

---

## Resolved Decisions

1. **No async v0.** Keeping the API synchronous/blocking matches the
   existing httpd and tls patterns and keeps the implementation small.
   A reactor-integrated `ws-recv-async` can be added in a follow-up.
2. **SHA-1 source.** `mbedtls_sha1` is already present via `tur-tls`;
   no new dependency.
3. **Buffer ownership.** `WsFrame.data` borrows from an internal ring
   buffer inside `WsConn`. This avoids per-frame heap allocation; callers
   copy on demand.

---

## See Also

- [websocket-server-plan.md](websocket-server-plan.md) -- server-side WebSocket upgrade
- [httpd-guide.md](../guides/httpd-guide.md) -- HTTP/1.1 server
- [httpd-tls-guide.md](../guides/httpd-tls-guide.md) -- TLS layer
