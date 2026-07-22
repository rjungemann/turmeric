# httpd-mw-rate-limit: RateLimit counter table never freed

**Summary:** every `mw-rate-limit` instance allocates an `RLState` (mutex +
`RLEntry entries[1024]`) in `httpd-mw-ratelimit-new` and nothing ever frees it --
`httpd-mw-rate-limit` leaks ~24 KB / 2 allocs. NON-closure; unrelated to Model R
(the fixture's stale marker text blames the "compose-middleware closure onion",
which is wrong).

**Severity:** Low (bounded, one table per limiter, process-lifetime). The fixture
keeps `requires.no-leak-check`.

**Repro:** `tur --enable=closure-drop-glue build
tests/fixtures/httpd-mw-rate-limit/input.tur` then run under LSan -> two leaks
both `#1 ... in httpd_hymw_hyratelimit_hynew`.

**Root cause:** `stdlib/httpd.tur` `httpd-mw-ratelimit-new` (~line 3674):
`calloc` of `RLState`, then `calloc` of `st->entries` (1024 * sizeof(RLEntry)).
The state handle is captured by the `mw-rate-limit` closure and read by
`httpd-mw-ratelimit-check`, but there is no `httpd-mw-ratelimit-free` and no
teardown hook -- `httpd-free` does not know about middleware-owned side state.

**Fix directions:** give `RLState` a destructor and a way to run it at server
teardown. Options: (a) an `httpd-mw-ratelimit-free` + a middleware-teardown
registry the handler chain walks on `httpd-free`; (b) make the RL state an owned
opaque (`defopaque` + `Drop`) captured by the closure, so the closure drop-glue
releases it via the `Drop` instance (this is exactly the owned-opaque capture
path the String/`Drop` work already established -- see the plan's #1b). Option (b)
folds it into the Model R machinery; (a) is a standalone httpd-teardown fix.
