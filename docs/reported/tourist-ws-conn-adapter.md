# tur-tourist handlers cannot host WebSocket endpoints (spice repo)

**Severity: low** (workaround documented: separate ports or raw tur-httpd).
Found in the 2026-08-20 docs audit. Lives in the sibling `turmeric-spices`
repo, which was absent from the audit container -- re-verify there before
starting.

## Repro

No way to call `ws-upgrade` from a tourist route handler: tourist uses httpd's
one-arg handler shape and never exposes the `Conn` that `ws-upgrade` requires.

## Root cause

Spice-side: tourist builds on `server-start`, not `server-start-conn`.

## Fix direction

A `tourist-ws` adapter (or a two-arg tourist handler variant) surfacing `Conn`
to route handlers.

## Guides to update when fixed

- docs/guides/websocket-guide.md ("Composing with httpd/tourist")
- docs/guides/web-stack-guide.md
