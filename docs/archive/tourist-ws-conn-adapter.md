# tur-tourist handlers cannot host WebSocket endpoints (spice repo)

**RESOLVED (already fixed at filing time; docs updated 2026-09-04).** The
`tourist-ws` spice (`turmeric-spices/spices/tourist-ws`, `ws-route!`) has
shipped since 2026-06-23 (TOUR-V0, commit `95eae7a4`) -- before the
2026-08-20 docs audit that filed this report. The audit checked the guide
prose, not the sibling spice checkout, and the guide had drifted: it still
said tourist could not host WebSocket endpoints and pointed at
`tourist-ws` as unbuilt future work. Fixed the guides:
[websocket-guide.md](../guides/websocket-guide.md#composing-with-httpd--tourist)'s
"Composing with httpd/tourist" section now shows `ws-route!` +
`tourist-conn`, and [web-stack-guide.md](../guides/web-stack-guide.md) links
to it. No spice-side code change was needed.

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
