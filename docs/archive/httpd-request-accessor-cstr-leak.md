# httpd request accessors leak their returned cstr (false "per-call ownership" contract)

> **RESOLVED (2026-07-23).** Implemented the report's preferred fix -- a
> per-request arena. `HttpdConn` gained an `owned_cstrs` field (a
> `HttpdOwnedStr {char *s; next}` free-list) and a file-scope helper
> `httpd_conn_own_cstr(conn, s)`; `httpd-req-cookie` and `httpd-req-form` now
> route their fresh `malloc` through it, and BOTH `httpd-handle` teardown paths
> (`__cleanup_iteration` blocking + `__async_cleanup` async) drain the list at
> request end. The ergonomic "do not free" docstring contract is now true.
>
> Verified on Linux with LSan: pre-fix, `httpd-mw-cookie` leaked exactly
> `12 byte(s) in 2 allocation(s)` in `httpd_hyreq_hycookie` (matching this
> report); post-fix both `httpd-mw-cookie` and `httpd-mw-form` are LSan-clean
> under default flags (no `--enable=closure-drop-glue` needed -- these fixtures
> carry no residual closure-env leak), so their `requires.no-leak-check` markers
> were dropped. All 33 `httpd-*` fixtures pass; full `bash tests/run.sh` green
> (2269 passed). Original finding below.

**Severity:** low (bounded per-request-lookup leak; not a crash or miscompile).
httpd stdlib request-string ownership -- **not** a closure / drop-glue issue.

## Summary

`httpd-req-cookie` and `httpd-req-form` return a **freshly `malloc`'d cstr** for
a found value, and their docstrings state the returned string is "a fresh malloc
that the runtime treats as **per-call ownership**; do not free." That contract is
**not implemented** -- no per-request cleanup ever frees these allocations, so
each successful cookie/form lookup leaks one heap string for the life of the
process.

Surfaced while flag-on-testing the httpd middleware fixtures under Model R
(`--enable=closure-drop-glue`, `docs/upcoming/closure-drop-glue-plan.md`): with
the closure envs now reclaimed, the residual leaks in `httpd-mw-cookie` (12 B / 2
allocs) and `httpd-mw-form` (26 B / 3 allocs) are these accessor strings, NOT
closures -- so they are out of scope for the closure drop-glue work and tracked
here instead. Both fixtures keep `requires.no-leak-check`.

## Minimal repro

Any handler that reads a present cookie/form field leaks:

```turmeric
;; inside a handler:
(let [sid (httpd-req-cookie c "session")]   ; fresh malloc when "session" is set
  ... )                                     ; never freed -> 1 leak per request
```

LSan (via the fixtures): the leak's alloc frame is `httpd_hyreq_hycookie`
(`httpd-req-cookie`) / `httpd_hyreq_hyform` (`httpd-req-form`).

## Root cause

`stdlib/httpd.tur`:

- `httpd-req-cookie` (~`:1306`): on a match, `char *out = malloc(vlen + 1); ...;
  return out;` -- a fresh owned cstr.
- `httpd-req-form` (~`:1498`): same shape for a URL-encoded form field.

The docstrings promise the runtime owns and frees these per request, but the
`HttpdConn` teardown (`httpd-handle` cleanup) frees the request buffers/headers/
json cache, not these ad-hoc accessor allocations, and no per-request arena
tracks them. The "do not free" guidance then guarantees the leak: the handler is
told not to free, and nothing else does.

## Fix directions

1. **Per-request arena (preferred):** register each accessor allocation on a
   per-`HttpdConn` list (like `attr_list` / `route_params`) that
   `httpd-handle`'s cleanup drains at request end. Keeps the ergonomic "do not
   free" contract and makes it true.
2. **Return a borrowed view:** carve the value out of the already-owned header
   buffer (a `{ptr,len}` view or a caller-provided output buffer) so no
   allocation happens -- matches how several other `httpd-req-*` reads work.
3. **Flip the contract to caller-frees:** document "caller frees the result" and
   free it in the handlers (and the fixtures). Least ergonomic; most explicit.

## Notes

- Orthogonal to Model R closure drop-glue: freeing closure envs does not touch
  these. When this is fixed, `httpd-mw-cookie` / `httpd-mw-form` can drop
  `requires.no-leak-check` (once also built `--enable=closure-drop-glue` if their
  handlers capture closures -- but their residual leak is purely these strings).
