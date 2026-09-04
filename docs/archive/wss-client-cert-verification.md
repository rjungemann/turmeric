# ws-client wss:// does no certificate verification (spice repo)

**RESOLVED (already fixed at filing time; docs updated 2026-09-04).**
`ws-client`'s TLS-V0 (`turmeric-spices` commit `95eae7a4`, 2026-06-23) shipped
verified-by-default `wss://` before the 2026-08-20 docs audit that filed this
report: `ws-connect` now installs the system CA store and
`MBEDTLS_SSL_VERIFY_REQUIRED` (plus hostname checking) unless the caller opts
into `ws-connect-insecure`; `ws-connect-with-ca` verifies against a
caller-supplied bundle. The audit checked guide prose, not the sibling spice
checkout, and the guide had drifted -- it still described
`MBEDTLS_SSL_VERIFY_NONE` as current behavior. Fixed
[websocket-guide.md](../guides/websocket-guide.md#tls-wss)'s TLS section and
"Limitations and gotchas" bullet to document the three entry points
(`ws-connect` / `ws-connect-with-ca` / `ws-connect-insecure`). No spice-side
code change was needed.

**Severity: medium** (security) -- `ws-connect "wss://..."` accepts any
certificate (`MBEDTLS_SSL_VERIFY_NONE`), unsafe for arbitrary public
endpoints. Found in the 2026-08-20 docs audit. Lives in the sibling
`turmeric-spices` repo, which was absent from the audit container -- re-verify
there before starting.

## Fix direction

Opt-in CA bundle path + a `verify-peer` flag defaulting on, per the websocket
guide's own "Future work".

## Guides to update when fixed

- docs/guides/websocket-guide.md (TLS section, Limitations)
