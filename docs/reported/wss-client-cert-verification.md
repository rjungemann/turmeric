# ws-client wss:// does no certificate verification (spice repo)

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
