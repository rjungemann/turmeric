# Plan: `tur-httpd` -- Threaded HTTP/1.1 Server

> **Status:** Draft Plan
> **Last Updated:** 2026-05-28
> **Type:** Spice Design + Architecture + Implementation Roadmap
> **Related:**
> - `docs/guides/developing-spices-guide.md` (spice authoring conventions)
> - `docs/guides/threading-guide.md` (OS threads / Mutex / Arc)
> - `../turmeric-spices/spices/http/` (existing HTTP client spice)
> - `docs/tur-template-plan.md` (template engine spice)
> - `docs/tur-turist-plan.md` (Scotty-style micro-framework that layers on top)

---

## Overview

`tur-httpd` is a minimal threaded HTTP/1.1 server built on POSIX
`socket`/`accept`/`pthreads` -- no additional C library required. It is the
server-side counterpart to the existing `tur-http` client.

It is one of three spices that together form a composable web stack for
Turmeric:

| Spice | Analogue | Depends on |
|---|---|---|
| `tur-template` | ERB / EJS | (none -- pure Turmeric) |
| `tur-httpd` | Mongoose / Civetweb | (none -- POSIX sockets + pthreads) |
| `tur-turist` | Haskell's scotty | `tur-httpd`, `tur-template` |

The three are deliberately separate so any layer can be used independently.

---

## Motivation

The existing `tur-http` spice is an HTTP *client*. There is no server-side
counterpart. `tur-httpd` fills that gap.

Why POSIX sockets rather than wrapping Mongoose or Civetweb?

- Zero additional CMake deps; compiles anywhere Turmeric compiles.
- The server is simple enough (~300 lines of inline-C) that a wrapper adds
  more indirection than value.
- Civetweb/Mongoose both have their own event loops that conflict with
  Turmeric's threading model.

For production TLS, a future `tur-httpd-tls` variant can swap the plain
socket layer for mbedTLS (reusing the pattern from `tur-http`).

---

## Threading Model

Each accepted connection is handled on a new OS thread. A global
`Mutex<accept-queue>` serializes `accept()` so only one thread calls it at
a time. The server itself runs on a background thread started by
`server-start`; `server-stop` joins it.

```
main thread
  server-start(8080, handler-fn)     -- spawns listener thread
    listener thread
      loop: accept() -> spawn worker thread
        worker thread: read request -> call handler-fn -> write response
  ... application code ...
  server-stop(server)                -- signals stop; joins listener
```

A bounded thread pool is included in the initial milestone (see H6/H7): the
listener hands accepted sockets to a fixed-size pool of worker threads via a
`Mutex<Queue<socket>>`. Thread-per-connection is kept as a fallback config
(`:mode :spawn`) but `:mode :pool :size N` is the default.

---

## API

```turmeric
(import httpd/server  :refer [server-start server-stop])
(import httpd/request :refer [req-method req-path req-header req-body req-query])
(import httpd/response :refer [response ok not-found bad-request
                                with-header with-body with-status])

;; Start a server; handler-fn :: Request -> Response
(def srv (server-start 8080 (fn [req]
  (ok "text/plain" "Hello, world!"))))

;; Gracefully stop
(server-stop srv)
```

### Request accessors

| Function | Returns | Notes |
|---|---|---|
| `req-method` | `:cstr` | `"GET"`, `"POST"`, ... |
| `req-path` | `:cstr` | path component only (`"/foo/bar"`) |
| `req-query` | `:cstr` | raw query string after `?`, or `""` |
| `req-header` | `result<:cstr>` | `(req-header req "Content-Type")` |
| `req-body` | `:cstr` | request body, `""` if none |

### Response constructors

| Function | Signature | HTTP status |
|---|---|---|
| `ok` | `content-type body -> Response` | 200 |
| `not-found` | `body -> Response` | 404 |
| `bad-request` | `body -> Response` | 400 |
| `response` | `status content-type body -> Response` | caller-supplied |
| `with-header` | `Response key val -> Response` | adds a header |
| `with-body` | `Response body -> Response` | replaces body |
| `with-status` | `Response status -> Response` | replaces status |

---

## Request Parsing

HTTP/1.1 only; `Transfer-Encoding: chunked` is not supported in the initial
milestone. The inline-C parser reads the request line, headers, and body
(up to `Content-Length` bytes) into a heap-allocated struct.

---

## Module Layout

```
tur-httpd/
  build.tur
  src/
    httpd/
      server.tur    -- server-start, server-stop; listener + worker threads
      request.tur   -- Request struct; req-method, req-path, req-header, req-body, req-query
      response.tur  -- Response struct; ok, not-found, response, with-header, ...
      parse.tur     -- inline-C: parse raw bytes into Request
      write.tur     -- inline-C: serialise Response into bytes
  tests/
    fixtures/
      echo/         -- server echoes method + path; test with tur-http client
      headers/      -- verifies header round-trip
      post-body/    -- verifies body read
```

---

## `build.tur`

```turmeric
(defpackage tur-httpd
  :name        "tur-httpd"
  :version     "0.1.0"
  :description "Minimal threaded HTTP/1.1 server (POSIX sockets)"
  :license     "MIT"
  :spices #{
    "test" #{:url "https://github.com/turmeric-lang/turmeric-spices"
            :ref "test-v0.1.0"
            :subdir "spices/test"
            :optional true}
    "http" #{:url "https://github.com/turmeric-lang/turmeric-spices"
            :ref "http-v0.1.0"
            :subdir "spices/http"
            :optional true}
  }
  :exports #{
    "httpd/server"   ["server-start" "server-stop"]
    "httpd/request"  ["req-method" "req-path" "req-header" "req-body" "req-query"]
    "httpd/response" ["response" "ok" "not-found" "bad-request"
                      "with-header" "with-body" "with-status"]
  })
```

---

## Implementation Phases

| Step | Task |
|---|---|
| H1 | Scaffold `tur-httpd` in `../turmeric-spices/spices/httpd/` |
| H2 | `parse.tur`: inline-C request parser (method, path, headers, body) |
| H3 | `request.tur`: `Request` struct; public accessors |
| H4 | `response.tur`: `Response` struct; constructors + modifier functions |
| H5 | `write.tur`: inline-C response serializer |
| H6 | `server.tur`: POSIX `socket`/`bind`/`listen`/`accept` loop in a background thread; worker dispatch trait |
| H7 | `pool.tur`: bounded thread pool (`Mutex<Queue<socket>>` + N worker threads); selectable via `:mode :pool :size N` (default) or `:mode :spawn` (thread-per-conn fallback) |
| H8 | Fixture tests: echo, headers, post-body (using `tur-http` client for assertions); include a concurrent-load fixture that saturates the pool |
| H9 | Docstrings + `just docs` |

---

## Open Questions

- **Keep-alive:** HTTP/1.1 persistent connections are out of scope for the
  initial milestone. Responses always include `Connection: close`. The worker
  loop is structured around a `handle-connection` function so keep-alive
  (a request loop guarded by an idle timeout and the client's `Connection:`
  header) can be added later without an API break. The pool worker contract
  -- "take a socket, return when done" -- does not change.
