# httpd Middleware Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-01
> **Type:** Networking / stdlib

---

## Overview

`stdlib/httpd` already documents a middleware pattern (see
`docs/guides/httpd-guide.md` §Middleware H7): a middleware is a function
that takes the `next` handler and returns a new handler that wraps it.
The `httpd-call` primitive invokes a stored handler closure.

This plan formalises and expands that foundation into a small
`stdlib/httpd-mw` module: a collection of production-ready middleware
and a `compose-mw` helper that threads a stack of middlewares together
without boilerplate.

The primitives (`httpd-call`, `router-mw`, handler calling convention)
are **not changed**. Everything added here is pure Turmeric library code
on top of the existing C layer.

---

## Goals / Non-Goals

### Goals (v0)

- `(compose-mw mw1 mw2 ... handler)` -- compose an ordered stack of
  middleware functions around a final handler.
- **Logging middleware** -- structured request/response logging (method,
  path, status, elapsed ms) to stdout or a user-supplied writer.
- **CORS middleware** -- add `Access-Control-Allow-*` headers; configure
  allowed origins, methods, and headers; handle preflight `OPTIONS`
  requests.
- **Basic-auth middleware** -- `Authorization: Basic` header check
  against a static credential map; returns 401 + `WWW-Authenticate` on
  failure.
- **Rate-limit middleware** -- sliding-window token bucket per IP (stored
  in a `Mutex<hamt>` of counters); returns 429 on exhaustion.
- **Static-file middleware** -- serve files from a root directory; sets
  correct `Content-Type` from extension; returns 304 on matching
  `If-None-Match` / `ETag`.
- **Body-size middleware** -- reject requests whose `Content-Length`
  exceeds a configured limit with 413.
- **Timeout middleware** -- enforce a per-request wall-clock budget;
  triggers a 503 if the downstream handler has not written a status
  after N ms.
- **Panic-recovery middleware** -- catch a downstream panic / error
  signal, log it, and return a 500 rather than crashing the worker.
- `(httpd-req-attr conn key)` and `(httpd-set-attr! conn key val)` --
  attach arbitrary key/value context to a request (used by auth
  middleware to pass the authenticated user name downstream).

### Non-Goals (v0)

- No streaming response body; body is still buffered in the existing
  `httpd-resp-body!` string.
- No gzip / brotli response compression middleware (out of scope without
  a compression spice).
- No session / cookie store (build on top of basic-auth + request attrs).
- No JWT validation (composable in user code using inline-C + mbedtls).
- No HTTP/2 push.

---

## API Surface

### `compose-mw`

```turmeric
;;; compose-mw -- thread a list of middlewares around a final handler.
;;;
;;; Applies middlewares right-to-left so the first argument wraps
;;; outermost (i.e., runs first on request, last on response).
;;;
;;; Parameters:
;;;   & mws :int -- middleware functions followed by the final handler
;;;                 as the last argument
;;;
;;; Returns:
;;;   A single handler closure.
;;;
;;; Example:
;;;   (let [h (compose-mw log-mw cors-mw auth-mw (router-mw r))]
;;;     (httpd-new 8080 h))
;;;
;;; Since: Phase MW1
(defn compose-mw [& mws :int] :int ...)
```

### Logging middleware

```turmeric
;;; log-mw -- structured request/response logging.
;;;
;;; Logs "<method> <path> -> <status> (<ms>ms)" to stdout.
;;; Pass a custom writer with log-mw-with-writer.
;;;
;;; Parameters:
;;;   next -- downstream handler
;;;
;;; Since: Phase MW1
(defn log-mw [next :int] :int ...)

;;; log-mw-with-writer -- logging middleware with a custom writer fn.
;;;
;;; Parameters:
;;;   writer -- (fn [line :cstr] :void)
;;;   next   -- downstream handler
;;;
;;; Since: Phase MW1
(defn log-mw-with-writer [writer :int next :int] :int ...)
```

### CORS middleware

```turmeric
(defstruct CorsOpts
  [origins :cstr   ;; "*" or comma-separated list
   methods :cstr   ;; e.g. "GET,POST,OPTIONS"
   headers :cstr   ;; allowed request headers
   max-age :int    ;; preflight cache seconds (0 = omit header)
  ])

;;; cors-mw -- add CORS headers and handle OPTIONS preflight.
;;;
;;; Parameters:
;;;   opts -- CorsOpts
;;;   next -- downstream handler
;;;
;;; Since: Phase MW1
(defn cors-mw [opts :CorsOpts next :int] :int ...)
```

### Basic-auth middleware

```turmeric
;;; basic-auth-mw -- HTTP Basic authentication check.
;;;
;;; On success, stores the username via (httpd-set-attr! conn "user" ...).
;;; On failure, responds 401 and does not call next.
;;;
;;; Parameters:
;;;   credentials -- hamt mapping username :cstr -> password-hash :cstr
;;;                  (bcrypt hash; use basic-auth-hash to generate)
;;;   realm       -- realm string for WWW-Authenticate header
;;;   next        -- downstream handler
;;;
;;; Since: Phase MW2
(defn basic-auth-mw [credentials :int realm :cstr next :int] :int ...)

;;; basic-auth-hash -- hash a plaintext password for use in credentials map.
;;;
;;; Since: Phase MW2
(defn basic-auth-hash [password :cstr] :cstr ...)
```

### Rate-limit middleware

```turmeric
(defstruct RateLimitOpts
  [requests :int   ;; max requests per window
   window-s :int   ;; window size in seconds
  ])

;;; rate-limit-mw -- sliding-window IP-based rate limiter.
;;;
;;; Returns 429 with a Retry-After header when the limit is exceeded.
;;;
;;; Parameters:
;;;   opts -- RateLimitOpts
;;;   next -- downstream handler
;;;
;;; Since: Phase MW2
(defn rate-limit-mw [opts :RateLimitOpts next :int] :int ...)
```

### Static-file middleware

```turmeric
;;; static-mw -- serve files from root-dir for requests not handled by next.
;;;
;;; Checks next first; only serves a file if next returns 404.
;;; Sets Content-Type based on file extension. Returns 304 on ETag match.
;;;
;;; Parameters:
;;;   root-dir -- filesystem path to serve files from
;;;   next     -- downstream handler (usually a router)
;;;
;;; Since: Phase MW2
(defn static-mw [root-dir :cstr next :int] :int ...)
```

### Body-size middleware

```turmeric
;;; body-size-mw -- reject requests with Content-Length > max-bytes with 413.
;;;
;;; Parameters:
;;;   max-bytes -- maximum accepted body size
;;;   next      -- downstream handler
;;;
;;; Since: Phase MW1
(defn body-size-mw [max-bytes :int next :int] :int ...)
```

### Panic-recovery middleware

```turmeric
;;; recover-mw -- catch panics from downstream; respond 500 and log.
;;;
;;; Parameters:
;;;   next -- downstream handler
;;;
;;; Since: Phase MW1
(defn recover-mw [next :int] :int ...)
```

### Request attributes

Added to `stdlib/httpd`:

```turmeric
;;; httpd-set-attr! -- attach a key/value pair to a request.
;;;
;;; Parameters:
;;;   conn -- httpd connection pointer
;;;   key  -- attribute name
;;;   val  -- attribute value (cstr)
;;;
;;; Since: Phase MW2
(defn httpd-set-attr! [conn :ptr<void> key :cstr val :cstr] :void ...)

;;; httpd-req-attr -- retrieve a previously set request attribute.
;;;
;;; Returns nil-value if the key has not been set.
;;;
;;; Parameters:
;;;   conn -- httpd connection pointer
;;;   key  -- attribute name
;;;
;;; Since: Phase MW2
(defn httpd-req-attr [conn :ptr<void> key :cstr] :cstr ...)
```

---

## `compose-mw` implementation

`compose-mw` takes a variadic list (last element is the innermost
handler, first is outermost) and folds right:

```turmeric
(defn compose-mw [& mws :int] :int
  (let [lst (cons-list->vec mws)]
    (let [inner (vec-last lst)]
      (vec-fold-right
        (fn [mw acc :int] :int (httpd-apply mw acc))
        inner
        (vec-drop-last lst)))))
```

`httpd-apply mw next` calls `(mw next)` -- it is a two-argument closure
application. Middleware authors write:

```turmeric
(defn my-mw [next :int] :int
  (fn [conn :ptr<void>] :nil
    ...
    (httpd-call next conn)))
```

This is already the established pattern; `compose-mw` just eliminates
the nesting at the call site.

---

## Request attribute storage

Request attributes are stored in a small singly-linked list of
`(key . val)` pairs pinned to the `httpd_conn` struct. The conn struct
gains one `int64_t attr_list` field (a `cons` list, or 0). Field is
zeroed on connection accept and freed (shallow) after the handler
returns.

This avoids a heap allocation for the common case (zero or one
attribute); deeper attribute maps are rare and the linear scan cost is
negligible for single-digit key counts.

---

## Phases

### Phase MW0 -- Audit existing middleware examples

- Review `tests/fixtures/httpd-h*` for patterns already in use.
- Confirm `httpd-call` signature; add `httpd-apply` helper if missing.

### Phase MW1 -- Core helpers + simple middleware

- `stdlib/httpd-mw.tur`: `compose-mw`, `log-mw`, `log-mw-with-writer`,
  `body-size-mw`, `recover-mw`.
- Fixtures: `tests/fixtures/httpd-mw-log/`,
  `tests/fixtures/httpd-mw-body-size/`,
  `tests/fixtures/httpd-mw-recover/`.

### Phase MW2 -- Auth + CORS + rate-limit + static

- `basic-auth-mw` (bcrypt via `mbedtls_pkcs5_pbkdf2_hmac` or
  `bcrypt` wrapper in inline-C).
- `cors-mw` + preflight handling.
- `rate-limit-mw` with `Mutex<hamt>` sliding window.
- `static-mw` with ETag (`mtime` + size as hex string).
- Request attrs (`httpd-set-attr!` / `httpd-req-attr`) added to
  `stdlib/httpd`.
- Fixtures for each.

### Phase MW3 -- Documentation

- `docs/guides/httpd-middleware-guide.md` -- overview, each middleware
  with example, `compose-mw` usage, writing custom middleware.
- Update `httpd-guide.md` §Middleware to reference the new guide and
  `stdlib/httpd-mw`.

---

## Resolved Decisions

1. **No new framework.** Everything is plain Turmeric function
   composition over the existing `httpd-call` primitive. The only new
   C-layer addition is the `attr_list` field on `httpd_conn`.
2. **`compose-mw` argument order.** First argument = outermost (wraps
   last, runs first). This matches the common convention in Ring
   (Clojure) and Rack (Ruby): `(compose-mw log-mw cors-mw handler)` logs
   first on the way in and last on the way out.
3. **Static-file fallback.** `static-mw` defers to `next` first and only
   serves a file if `next` returned 404. This makes it safe to compose
   after a router without having to list every route.
4. **Rate-limit state.** The `Mutex<hamt>` lives in the closure captured
   by `rate-limit-mw`. Multiple server instances (or multiple calls to
   `rate-limit-mw`) get independent counters; sharing state is left to
   the caller.

---

## See Also

- [httpd-guide.md](../guides/httpd-guide.md) -- HTTP/1.1 server primitives
- [httpd-tls-guide.md](../guides/httpd-tls-guide.md) -- TLS layer
- [websocket-server-plan.md](websocket-server-plan.md) -- WebSocket upgrade (compatible with middleware)
- [threading-guide.md](../guides/threading-guide.md) -- `Mutex` and worker pool details
