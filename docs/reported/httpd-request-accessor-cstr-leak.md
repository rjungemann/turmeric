# httpd request accessors leak their returned cstr (false "per-call ownership" contract)

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
