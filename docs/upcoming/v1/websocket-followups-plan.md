---
title: WebSocket Followups Plan
category: Planning
description: Close the security gap (TLS cert verification), unify the colliding WsConn/WsFrame names between client and server, tighten ws-upgrade's return shape, add a tour-tourist adapter, and ship subprotocol negotiation + outbound fragmentation as the next API surface.
---

# `ws-client` / `ws-server` Followups -- Plan

## Context

`ws-client` and `ws-server` shipped with v0.24.0. The user-facing guide
is [`docs/guides/websocket-guide.md`](../../guides/websocket-guide.md).
End-to-end smoke fixtures pass. Five gaps surfaced during the guide
writeup that we should not carry into v1:

1. **The TLS client does not verify certificates.**
   `ws-client/src/ws-client/client.tur:516` calls
   `mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE)`
   unconditionally. `wss://` happily connects to anything serving a
   self-signed cert. This is the single most security-relevant gap.
2. **`WsConn` and `WsFrame` collide between the two spices.**
   Both `ws-client` and `ws-server` export `(defopaque WsConn :int)`
   and `(defopaque WsFrame :int)`. A program that wants both (a relay,
   a test harness) cannot import them; today the answer is "split it
   into two binaries."
3. **`ws-upgrade` returns `:int`** -- 1 for success, 0 for failure
   (`ws-server/src/ws-server/server.tur`). Per CLAUDE.md "No lazy `:int`
   stand-ins", this should be a `result<unit, UpgradeError>` (or the
   established analogue in this codebase) so the caller knows *why* the
   upgrade failed.
4. **No tourist-side WebSocket adapter.** `tur-tourist` handlers are
   `(fn [Ctx] Response)`; there is no path from inside a handler to the
   raw `Conn` that `ws-upgrade` needs. A WebSocket route in a tourist
   app today bypasses tourist and uses `httpd` directly.
5. **No `Sec-WebSocket-Protocol` subprotocol negotiation, no outbound
   fragmentation, no `permessage-deflate`.** All three are RFC 6455 /
   7692 surface that real WebSocket clients expect. The first two are
   small; deflate is an extension and can defer further.

This plan closes items 1-4 in a single minor cycle and frames item 5
as two independent follow-ons that can land any time after.

## Goals

1. **TLS-V0 -- verified TLS by default.** Real cert verification with
   the system CA bundle; documented opt-out for self-signed dev
   workflows; no behavior change for `ws://`.
2. **NAME-V0 -- collision-free names.** Both spices ship the same
   logical type with the same Turmeric name so a single program can
   import both.
3. **UPG-V0 -- `ws-upgrade` returns a `result`.** The handler still
   sees an `int` short-circuit if it doesn't care; new callers get
   typed error info.
4. **TOUR-V0 -- `tourist-ws` adapter spice.** New sibling spice
   exposing a tourist-side `ws-route!` that hands the handler a
   `WsConn` once the upgrade has succeeded.
5. **SUB-V0 (independent) -- subprotocol negotiation.** Both sides.
6. **FRAG-V0 (independent) -- outbound fragmentation API.** Client
   and server.

## Non-goals

- `permessage-deflate` (RFC 7692). Compression on a hot WebSocket loop
  has measurable engineering surface (dictionary state, sliding
  window, takeover semantics). Defer to its own plan; the typical use
  cases (chat, telemetry, control planes) live without it.
- Reconnection / message-replay primitives. Application concern, not
  a transport concern.
- A higher-level "WebSocket actor" or pub/sub layer. Build on top of
  this surface in user code or in a separate spice.

## Design

### TLS-V0 -- verified TLS by default

Replace the `MBEDTLS_SSL_VERIFY_NONE` line with `_REQUIRED`, install
mbedTLS's bundled CA crt chain (or the system bundle when present),
and add an opt-out knob -- not a "trust everything" knob, but a
"trust this specific cert" hook for dev workflows.

```turmeric
;;; ws-connect with default verified TLS:
(ws-connect "wss://api.example.com/socket")

;;; opt-in to a custom CA bundle file:
(ws-connect-with-ca "wss://localhost:8443/socket" "/path/to/dev-ca.pem")

;;; opt-out for dev (must be explicit; documented as unsafe):
(ws-connect-insecure "wss://localhost:8443/socket")
```

Three entry points instead of one flag prevents the
"`tls-verify? false` parked in a config file then forgotten in prod"
failure mode.

**CA bundle source.**

- Linux: `/etc/ssl/certs/ca-certificates.crt`, fall back to
  `/etc/pki/tls/certs/ca-bundle.crt`.
- macOS: `SecTrustEvaluateWithError` against the system trust store via
  inline Objective-C, or shell out to `security
  export -k /Library/Keychains/SystemRootCertificates.keychain` at
  install time and cache the PEM. **Pick one in the design subphase**;
  the macOS path is the trickiest piece of this work.
- WASM: TLS is the browser's job, this code path doesn't fire.

**Hostname check** is part of verification when verify is on
(mbedTLS already wires `mbedtls_ssl_set_hostname` -- TLS-V0 is
about flipping the authmode, not adding the hostname call).

### NAME-V0 -- collision-free names

Three options ranked by preference:

| Option | Shape | Pros | Cons |
|---|---|---|---|
| A. Factor a shared `ws-core` spice | `ws-core` exports `WsConn`, `WsFrame`, frame predicates; client and server depend on it and re-export. | One canonical type; small change for callers (still see `WsConn` after re-export). | New spice to vendor; transitive dep graph. |
| B. Rename to `WsClientConn` / `WsServerConn` | Each spice keeps its own type; names diverge by side. | No new spice. | Loud rename in every user app. |
| C. Module-qualify | `WsConn` stays in both, callers use `ws-client/WsConn` vs `ws-server/WsConn`. | Smallest source change. | Importing both `:refer`-style is impossible; awkward at call sites. |

**Recommendation: A.** A `ws-core` spice with the protocol-level
types (frame layout, close codes, opcode enum, base `WsConn`
opaque) and pure helpers (`ws-accept-key`, frame parse) matches how
the codebase already factors shared core out of larger spices.

`ws-client` and `ws-server` keep their public names by re-exporting,
so the migration is "add a `:spices` dep + recompile" for most
consumers; a tiny minority that constructed a `WsConn` via inline-C
(should be none in the wild) needs to switch construction.

### UPG-V0 -- `ws-upgrade` returns `result`

```turmeric
(defdata UpgradeError
  (UpgradeMissingHeader     ; e.g. Sec-WebSocket-Key absent
    [name : cstr])
  (UpgradeBadVersion        ; client asked for a version we don't speak
    [requested : int])
  (UpgradeUnsupportedProto  ; subprotocol negotiation failed (post-SUB-V0)
    [requested : cstr])
  (UpgradeWriteFailed))     ; socket error mid-handshake

(defn ws-upgrade [conn : Conn req : Request handler : WsHandler]
  : result<unit UpgradeError> ...)
```

Backward compatibility: ship a thin `ws-upgrade-or-zero` for one
release that returns `:int` (1/0) and routes through the `result`-shaped
core. Document the deprecation; delete in the next minor.

**Handler signature also tightens** (a real type instead of `fn`):

```turmeric
(deftype WsHandler (fn [^linear WsConn] : void))
```

`^linear` because the handler owns the WsConn lifecycle and must
`ws-server-close` (or pass it to something that does) exactly once.

### TOUR-V0 -- `tourist-ws` adapter spice

New sibling at `../turmeric-spices/spices/tourist-ws/`, single module
`tourist-ws/route`. Surface:

```turmeric
;;; ws-route! -- register a route that upgrades to a WebSocket and
;;; hands the upgraded WsConn to `handler`. Internally the route
;;; intercepts the request before the normal handler chain, calls
;;; ws-upgrade with the underlying Conn, and on success drops out of
;;; the response pipeline (the framework returns "no response" because
;;; the connection has been hijacked).
(defn ws-route! [router : Router
                 path   : cstr
                 handler : WsHandler] : void ...)
```

Needs **one change in core tourist**: a `request-hijack` escape hatch
that returns "no response, do not run remaining middleware" so the
upgraded socket is not double-closed. Track as a sub-task; if the
tourist core change is contentious, fall back to a "tourist-ws routes
must be registered before tourist sees the request" guarded path that
sidesteps the middleware chain.

Subprotocol negotiation (SUB-V0) integrates naturally:

```turmeric
(ws-route! app "/ws/v1"
  :subprotocols ["json.v1" "msgpack.v1"]
  handler)
```

### SUB-V0 -- `Sec-WebSocket-Protocol` negotiation

- **Client:** `ws-connect-opts` takes an `:offered` list of subprotocol
  strings, sends `Sec-WebSocket-Protocol: <comma-list>` in the
  upgrade, exposes the server-selected one via `ws-subprotocol`.
- **Server:** `ws-upgrade-opts` accepts `:accepted ["json.v1" ...]`,
  picks the first request-offered protocol that intersects, sets the
  response header. No intersection -> `UpgradeUnsupportedProto` (uses
  UPG-V0).

API surface is small (one new opts struct each side). The
`Sec-WebSocket-Protocol` parse is comma-and-whitespace splitting.

### FRAG-V0 -- outbound fragmentation

Today every `ws-send` writes a single frame with `FIN=1`. The RFC
allows splitting a logical message across multiple frames. For large
payloads, fragmenting lets us interleave control frames (`ping`) and
bound per-frame buffer use. Add:

```turmeric
(defn ws-send-fragmented [^borrow ws : WsConn
                          payload    : ^borrow bytes
                          frame-size : int] : int)
```

Calls `ws-send-bytes` repeatedly with the `FIN` bit cleared on all but
the last frame. Pure additive API; existing `ws-send` is the
unfragmented one-frame case.

Receive-side fragmentation reassembly is already handled by
`ws-recv` (RFC requires it, otherwise existing tests would have
caught it).

## Work items

| # | Item | File(s) |
|---|------|---------|
| TLS-V0.1 | Add `ws-connect-with-ca` and `ws-connect-insecure` entry points; default `ws-connect` to `MBEDTLS_SSL_VERIFY_REQUIRED`. | `ws-client/src/ws-client/client.tur` |
| TLS-V0.2 | CA bundle discovery: Linux ssl-certs paths, macOS keychain export. | `ws-client/src/ws-client/ca-bundle.tur` (new) |
| TLS-V0.3 | Tests: connect against a known-bad cert fails by default; `-insecure` accepts; `-with-ca` accepts when matching, rejects when not. | `ws-client/tests/tls-verify.tur` |
| TLS-V0.4 | Guide update: TLS section gets a "Cert verification" subsection; the insecure variant is documented and clearly labeled. | `docs/guides/websocket-guide.md` |
| NAME-V0.1 | Scaffold `ws-core` spice (`build.tur`, `src/ws-core/{conn,frame,handshake}.tur`). | new dir |
| NAME-V0.2 | Move `WsConn`, `WsFrame`, opcode/close-code enums, `ws-accept-key` from both spices into `ws-core`; client and server re-export. | `ws-core/`, `ws-client/`, `ws-server/` |
| NAME-V0.3 | Test: a fixture imports both `ws-client` and `ws-server` and uses `WsConn` from both without `:as` aliasing. | new fixture |
| UPG-V0.1 | Define `UpgradeError` ADT; rewrite `ws-upgrade` body to return `result<unit UpgradeError>`. | `ws-server/src/ws-server/server.tur` |
| UPG-V0.2 | Ship deprecated `ws-upgrade-or-zero : int`; route through the new core. | `ws-server/src/ws-server/server.tur` |
| UPG-V0.3 | Tighten the handler param to `^linear WsConn`; introduce `WsHandler` type alias. | `ws-server/src/ws-server/server.tur` |
| UPG-V0.4 | Tests: each `UpgradeError` variant fires from a synthesized bad request; double-close on the linear handle is a compile error. | `ws-server/tests/upgrade-error.tur` |
| UPG-V0.5 | Guide update: replace the integer-status example; deprecation note for `-or-zero`. | `docs/guides/websocket-guide.md` |
| TOUR-V0.1 | Add `request-hijack` to core tourist (or, if rejected, document the alternative path). | `tourist/src/tourist/middleware.tur` |
| TOUR-V0.2 | Scaffold `tourist-ws` spice; implement `ws-route!`. | new dir + `tourist-ws/src/tourist-ws/route.tur` |
| TOUR-V0.3 | Test: a tourist app with a normal `/api` route and a `/ws` `ws-route!` both work in the same process. | `tourist-ws/tests/route-coexist.tur` |
| TOUR-V0.4 | Guide update: "WebSockets in a tourist app" subsection; remove the "use httpd directly" note. | `docs/guides/websocket-guide.md` |
| SUB-V0.1 | Add `:offered` to client `ws-connect-opts`; `ws-subprotocol` accessor. | `ws-client/` |
| SUB-V0.2 | Add `:accepted` to server `ws-upgrade-opts`; intersection logic; `UpgradeUnsupportedProto` on miss. | `ws-server/` |
| SUB-V0.3 | Tests: matching subprotocol negotiates; non-matching errors; absent header is acceptable. | both spices |
| FRAG-V0.1 | Implement `ws-send-fragmented` in both client and server. | both spices |
| FRAG-V0.2 | Tests: 1 MiB payload at 16 KiB frame size round-trips intact; ping interleaved between frames is observed. | both spices |

TLS-V0, NAME-V0, UPG-V0, TOUR-V0 are roughly independent; only
NAME-V0 has a soft ordering relationship with UPG-V0 (a single
`UpgradeError` shared via `ws-core` is cleaner than two parallel
definitions). Land NAME-V0 first when both are queued.

SUB-V0 and FRAG-V0 are pure additive surface; ship whenever.

## Testing

Existing `ws-{client,server}/tests/` have an end-to-end harness that
spins up a `httpd` server in the test process and connects to itself.
New tests reuse it. TLS-V0.3 needs a self-signed cert generated in
the test setup; the existing test harness already handles
`pem`-on-disk in the `tls` httpd tests, follow that pattern.

## Risk

- **Cert verification flip is a behavior change for `wss://`.** Apps
  that today rely on accidentally trusting self-signed certs will
  start failing. This is the *desired* failure mode but should be
  called out in the changelog.
- **`ws-core` migration is a coordinated change across three spices.**
  Cut a single release tag that bumps all three together; downstream
  pinning to old `ws-client` without bumping `ws-core` will diverge.
- **macOS CA bundle.** mbedTLS does not ship a macOS-native trust
  store integration. Pick keychain-export-at-install vs. linking
  Security.framework in the design subphase; both are real engineering.
- **`request-hijack` core change.** If tourist maintainers object,
  fall back to the registration-time guard. Don't block TOUR-V0 on a
  contentious core PR.

## Out of scope

- `permessage-deflate` (RFC 7692); separate plan.
- Reconnection/back-off helpers.
- A higher-level pub/sub layer.
- Per-message backpressure (RFC has no facility; this is socket-level).
- HTTP/2 WebSocket transport (RFC 8441); not on the v1 roadmap.
