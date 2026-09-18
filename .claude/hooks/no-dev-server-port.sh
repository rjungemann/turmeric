#!/usr/bin/env bash
# PreToolUse (Bash): keep Claude off the developer's Try Turmeric dev server.
#
# Port 3000 is the developer's own `just web-dev` server. Denied here:
#   - anything addressed to port 3000 on the local host;
#   - a server started with --port 3000 or PORT=3000;
#   - the web-dev recipe itself (`just web-dev`, `tur run web-dev`), which
#     takes 3000;
#   - starting vite (`npm run dev`, `npx vite`) without an explicit --port.
# Browser tests need none of these: web/playwright.config.js starts its own
# server on a free port. See CLAUDE.md, "Dev server port -- STRICT RULE".
cmd=$(jq -r '.tool_input.command // empty')

deny() {
    jq -n --arg r "$1" \
        '{hookSpecificOutput: {hookEventName: "PreToolUse", permissionDecision: "deny", permissionDecisionReason: $r}}'
    exit 0
}

reason="Port 3000 is the developer's own Try Turmeric dev server. Never use it, start one on it, run web-dev, or point anything at it. Browser tests (npx playwright test) start their own server on a free port; for a manual server run 'npx vite --port <free port> --strictPort' from web/ and stop it when done. See CLAUDE.md, 'Dev server port -- STRICT RULE'."

if grep -Eq '(localhost|127\.0\.0\.1|0\.0\.0\.0|\[::1?\]):3000([^0-9]|$)' <<<"$cmd"; then deny "$reason"; fi
if grep -Eq -- '--port[= ]+3000([^0-9]|$)|(^|[^A-Za-z_])PORT=3000([^0-9]|$)' <<<"$cmd"; then deny "$reason"; fi
if grep -Eq '(^|[^A-Za-z_-])(just|tur run)[[:space:]]+web-dev([^A-Za-z_-]|$)' <<<"$cmd"; then deny "$reason"; fi
if grep -Eq '(^|[^A-Za-z_-])(npm run dev|npx vite|npm exec vite)([^A-Za-z_-]|$)' <<<"$cmd" &&
   ! grep -Eq -- '--port[= ]+[0-9]+' <<<"$cmd" &&
   ! grep -Eq 'vite[[:space:]]+(build|optimize)' <<<"$cmd"; then
    deny "$reason"
fi
exit 0
