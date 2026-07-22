# httpd-new-pool / httpd-new: ^fat handler leaked on the constructor's failure paths

**Summary:** `httpd-new-pool` (and the `httpd-new` wrappers that delegate to it)
takes `^fat handler` -- i.e. owns the heap fat-closure box -- but every early
failure-return path returns NULL WITHOUT releasing it, so a failed server
construction leaks the handler box (and, for a compose-middleware onion, the
whole chain).

**Severity:** Low (only on construction failure -- bind/listen/calloc/malloc
error paths, which the happy-path corpus does not hit). Found while closing R1;
NOT currently exercised by any fixture.

**Repro (by inspection):** `stdlib/httpd.tur` `httpd-new-pool` (~line 675):
`hb->handler = (int64_t)handler;` is set only after several `return NULL` points
(`bind` fail ~691, `listen` fail ~692, `calloc hb` fail ~694, `malloc fdq` fail
~701, `tur_reactor_new` fail ~706, ...). Each returns NULL and frees `hb`/`fd`
but never the `handler` box. The analogous early-refusal leak in `httpd-new-tls`
(ctx==0 / TLS ops unregistered) was fixed in R1; this is the deeper
`httpd-new-pool` layer.

**Root cause:** `^fat handler` transfers ownership into the constructor; on
success `hb->handler` is freed by `httpd-free` / `httpd-async-free` (now via
`TUR_CLOSURE_DROP`). On the failure paths there is no owner, so the box leaks.

**Fix directions:** on each `httpd-new-pool` (and `httpd-new-async`) failure
`return NULL`, `TUR_CLOSURE_DROP(handler)` first -- but ONLY on paths BEFORE the
handler is handed to something else that will free it. Audit each path: those
that free `hb` after `hb->handler` was set must also drop the handler (or route
`hb` teardown through `httpd-free`). Byte-identical flag-off (`TUR_CLOSURE_DROP`
-> plain free). Add a fixture that forces a construction failure (e.g. an
already-bound port) under LSan to lock it in.

---

**RESOLVED (2026-07-22):** Every failure `return NULL` in `httpd-new-pool`
and `httpd-new-async-with-limit` now calls `TUR_CLOSURE_DROP(handler)` before
returning, dropping the owned `^fat handler` box on the bind/listen/calloc/
malloc/reactor error paths. Audited to confirm no double-drop with the R1
`httpd-new-tls` early-refusal fix or the success-path `httpd-free`/
`httpd-async-free` owner. Locked in by the leak-checked fixture
`tests/fixtures/httpd-new-pool-fail-drops-handler/`, which occupies an
ephemeral loopback port so `httpd-new-pool` bind-fails and must drop the
capturing handler ("refused", LSan clean). Full suite green (2265/0).
