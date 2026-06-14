# tur-tourist middleware: takes `req`, no ctx, no post-response hook

**Status:** Reported
**Severity:** Design defect / expressiveness hole -- blocks any non-trivial
middleware (sessions, auth, request-scoped logging with capture context,
response decoration, CSRF, gzip/compression, observability).
**Discovered:** 2026-06-14, while scoping the session-middleware plan at
`docs/upcoming/v1/tourist-session-middleware-plan.md`.
**Spice:** `../turmeric-spices/spices/tourist` (`tur-tourist` v0.1.0).

---

## Summary

`tourist/middleware`'s `use!` registers a function with the signature
`(fn [req : int] : int)` and runs it pre-dispatch with the raw httpd request
handle. The tourist `ctx` (the value that route handlers receive and that
carries `param`/`capture`/`req-full-path`) does not exist yet when middleware
runs, and there is no hook that runs after the route handler returns. The
context object also has a fixed C layout with no slot for user-attached
per-request data.

The practical consequence: middleware can only inspect the raw request and
either short-circuit with a `Response` or pass through. It cannot

- attach data that the route handler will see (no shared ctx),
- decorate or rewrite the outgoing response (no post-route hook),
- carry per-request state across the chain (no per-request scratch).

Sessions, signed-cookie auth, request-scoped logging that prints the matched
route, response compression, request ID injection, and CSRF all require one
of those three. None of them are expressible against the current API.

## Observed

`spices/tourist/src/tourist/middleware.tur:58`

```turmeric
(defn use! [fn : int] : int
  (route-new "__mw" 0 fn))
```

The registered `fn` is invoked at `spices/tourist/src/tourist/app.tur:228`:

```turmeric
(defn __tourist-handler [req : int] : int
  (let [opt (mw-chain-run (__tourist-state-mws) req)]
    (if (not= opt 0)
      (__tourist-opt-extract opt)
      (__tourist-route-dispatch (__tourist-state-routes) req))))
```

`req` is the httpd request handle. The tourist ctx is built only inside
`__tourist-route-dispatch` after a route matches (`app.tur:190` for sub-apps
and `app.tur:216` for regular routes) and is freed before the response is
returned. Middleware never sees it.

`mw-chain-run` (`middleware.tur:114`) is purely pre-dispatch: it returns the
first non-zero `option<Response>`, otherwise `0` (none). There is no
corresponding `after-mw-chain-run` invoked on the dispatched response.

The ctx struct (`param.tur:226`) is a fixed C layout:

```c
struct __tourist_ctx {
  char  *method; char *path; char *query; char *body;
  char **hdr_keys; char **hdr_vals; int hdr_count; int body_len;
  int64_t caps;
  char  **qp_keys; char **qp_vals; int qp_count; int qp_cap;
  char  *full_path;
};
```

There is no extension slot (e.g. a `void *user` or `int64_t attrs`
HAMT handle) where middleware could stash a session map, request ID, etc.

## Expected

Middleware should receive the ctx (so handlers and middleware share a
data channel), and there should be a way to observe / rewrite the outgoing
response. Concretely, one of:

- **A.** `use!` takes `(fn [ctx : int] : int)` returning `option<Response>`,
  ctx is built once at request entry and reused for the matched route, and
  a second hook `use-after!` takes `(fn [ctx : int resp : int] : int)`
  returning the (possibly rewritten) response. This is the cleanest fit for
  the session plan and matches Rack/Plug/Express conventions.
- **B.** Middleware returns a "next handler" wrapper (Ring-style:
  `(fn [ctx] -> Response)` where the middleware itself calls into the next
  step and is free to manipulate the response after the call returns).
- **C.** Keep the pre-dispatch shape but add ctx-on-req plus a separate
  response-decorator hook list. Less coherent than A or B.

Whichever is chosen, the ctx struct needs an extension slot for per-request
attributes (a HAMT handle from `tur-map` would be enough), so middleware can
attach session data, request IDs, parsed auth principals, etc., without
each middleware spice needing to fork the ctx layout.

## Why it matters / why this is a real bug

The middleware feature was advertised as complete (`use!` exported, README
mentions it). The signature is, in practice, only useful for filters that
either pass through silently or 403/redirect. Every realistic use case
beyond that requires either ctx sharing or response decoration. Calling this
"middleware" sets a user expectation it does not meet.

It also blocks `docs/upcoming/v1/tourist-session-middleware-plan.md` SS4 /
SS5 (`session-mw` with `(session-get ctx ...)` and save-on-response), which
is the immediate trigger for this report.

## Proposed fix directions

1. **Pick the new middleware shape** (A is recommended; matches the
   existing session plan and Rack-style precedent).
2. **Extend `tourist-ctx-new`** to allocate a `int64_t attrs` slot
   (HAMT handle, `0` = empty) plus a `int64_t resp_headers` slot
   (deferred header append list) and free both in `tourist-ctx-free`.
   Add `(ctx-attr-get ctx key)`, `(ctx-attr-set! ctx key val)`, and
   `(ctx-add-header! ctx name value)` helpers.
3. **Build ctx at request entry, before middleware**, so it is shared.
   Build with empty captures; the route-dispatch step swaps captures in
   when a route matches.
4. **Add `use-after!`** registering `(fn [ctx resp])` middleware that runs
   after the route handler in reverse declaration order. Each can return
   the same `resp` or a replacement. Apply `ctx-add-header!`-appended
   headers to the final response before returning.
5. **Update tourist tests and docs** to cover ctx-sharing and post-route
   decoration. Bump tourist to `v0.2.0` (breaking middleware change).
6. **Revise the session-middleware plan** to target the new API once it
   lands; SS4 then becomes a straight implementation rather than a workaround.

## Validation of a fix

- Tourist's existing middleware tests pass against the new signature
  (port internal handlers in the spice itself).
- A new test demonstrates: middleware attaches `request-id` to ctx,
  route handler reads it via `(ctx-attr-get ctx "request-id")`, an
  after-middleware adds `X-Request-Id` to the response.
- The session-middleware plan's SS5 integration test (cookie round-trip,
  dirty-flag flush, `session-destroy!`) compiles and passes without
  reaching for thread-locals.
