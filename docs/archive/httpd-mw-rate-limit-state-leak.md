# httpd-mw-rate-limit: RateLimit counter table never freed

> **RESOLVED (2026-07-23).** Took the report's option (a) -- a standalone
> httpd-teardown fix -- in the safest form: a process-exit registry.
> `httpd-mw-ratelimit-new` now records each RLState's two heap blocks (the state
> and its `entries` table) on a file-scope `void *`-keyed registry and installs a
> one-time `atexit` hook (`httpd_rl_cleanup_all`) that frees them.
>
> Freeing at process exit rather than at `httpd-free` is deliberate and avoids a
> use-after-free the naive "walk the chain on httpd-free" version would hit: two
> servers can each own a limiter, and `httpd-free` of one must not free the
> other's still-live counter table. The registry stores bare `void *` (not
> `RLState *`) so its file-scope typedef/globals/cleanup need no `pthread_mutex_t`
> -- the `__tur_include__` block is emitted above the preamble's `<pthread.h>`;
> the mutex lives inside the freed RLState block and needs no separate teardown at
> exit. (Option (b), folding this into the closure drop-glue via an owned opaque +
> `Drop`, remains the eventual home once that machinery lands; this standalone fix
> unblocks the leak now.)
>
> Verified on Linux with LSan: pre-fix `httpd-mw-rate-limit` leaked exactly
> `24640 byte(s) in 2 allocation(s)` in `httpd_hymw_hyratelimit_hynew` (the
> report's ~24 KB / 2 allocs); post-fix it is clean (atexit runs before LSan's
> check), the limiter still fires (`r3-status=429`), and `requires.no-leak-check`
> was dropped. All 33 `httpd-*` fixtures pass; full `bash tests/run.sh` green
> (2269 passed). Original finding below.

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
