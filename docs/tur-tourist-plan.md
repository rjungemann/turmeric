# Plan: `tur-tourist` -- Scotty-style HTTP Micro-Framework

> **Status:** Draft Plan
> **Last Updated:** 2026-05-28
> **Type:** Spice Design + Implementation Roadmap
> **Related:**
> - `docs/guides/developing-spices-guide.md` (spice authoring conventions)
> - `docs/tur-template-plan.md` (template engine spice)
> - `docs/tur-httpd-plan.md` (HTTP server spice this layers on top of)

---

## Overview

`tur-tourist` layers routing, response helpers, middleware, and static file
serving on top of `tur-httpd`. It provides the idiom from Haskell's
[scotty](https://hackage.haskell.org/package/scotty) -- a small DSL for
declaring routes and writing handlers.

It is one of three spices that together form a composable web stack for
Turmeric:

| Spice | Analogue | Depends on |
|---|---|---|
| `tur-template` | ERB / EJS | (none -- pure Turmeric) |
| `tur-httpd` | Mongoose / Civetweb | (none -- POSIX sockets + pthreads) |
| `tur-tourist` | Haskell's scotty | `tur-httpd`, `tur-template` |

The three are deliberately separate so any layer can be used independently.

---

## Motivation

`tur-httpd` delivers raw requests to a single handler function. That forces
every application to write its own dispatch table. `tur-tourist` provides the
idiom from Haskell's scotty:

```haskell
-- Haskell scotty
main = scotty 3000 $ do
  get  "/hello/:name" $ do
    name <- param "name"
    text ("Hello " <> name)
  post "/echo" $ do
    b <- body
    text b
```

The Turmeric equivalent, using the same vocabulary:

```turmeric
(import tourist/app   :refer [tourist])
(import tourist/dsl   :refer [get! post! put! delete! text json-body redirect])
(import tourist/param :refer [param capture])

(tourist 3000
  (get! "/hello/:name"
    (fn [req]
      (text (str-concat "Hello " (capture req "name")))))
  (post! "/echo"
    (fn [req]
      (text (req-body req)))))
```

---

## Route Syntax

Route patterns follow Express/Sinatra conventions:

| Pattern | Matches | Captures |
|---|---|---|
| `"/hello"` | `/hello` exactly | (none) |
| `"/user/:id"` | `/user/123` | `id = "123"` |
| `"/files/*"` | `/files/a/b/c` | `* = "a/b/c"` |

Patterns are matched in declaration order; first match wins.

---

## DSL Functions

| Function | HTTP method | Example |
|---|---|---|
| `get!` | GET | `(get! "/path" handler)` |
| `post!` | POST | `(post! "/items" handler)` |
| `put!` | PUT | `(put! "/items/:id" handler)` |
| `delete!` | DELETE | `(delete! "/items/:id" handler)` |
| `any!` | any | `(any! "/catch-all" handler)` |
| `serve-static!` | GET | `(serve-static! "/assets" "./public")` |

Each returns a `Route` value. `tourist` collects routes, builds a dispatch
table, starts `tur-httpd`, and routes incoming requests.

`serve-static!` is a route that matches `GET <url-prefix>/*`. It strips
the prefix, joins onto `fs-root`, rejects any resolved path that escapes
`fs-root` (path-traversal guard), and serves the file with a `Content-Type`
inferred from the extension via a small built-in mime table. A miss falls
through to the next route, so dynamic routes can shadow static paths.

---

## Handler Helpers

```turmeric
;; Response shorthands (return a Response for tur-httpd)
(text "hello")                          ; 200 text/plain
(html "<p>hello</p>")                   ; 200 text/html
(json-body "{\"ok\":true}")             ; 200 application/json
(redirect "/new-path")                  ; 302 Location: /new-path
(status 201 (text "created"))           ; override status code

;; Request helpers
(param req "name")     ; query param  ?name=...  -> result<:cstr>
(capture req "id")     ; path capture :id        -> result<:cstr>
```

`param` and `capture` both return `result<:cstr>`. If the key is absent,
`err "missing"` is returned; handlers decide how to respond.

---

## Template Integration

When `tur-template` is present as a dependency, `tur-tourist` exposes a
convenience helper:

```turmeric
(import tourist/template :refer [render-template])

(get! "/greet/:name"
  (fn [req]
    (let [name (ok-val (capture req "name"))
          env  (make-env {"name" name})]
      (html (render-template "views/greet.html.tur" env)))))
```

`render-template` resolves paths relative to the working directory. It is
only available if `tur-template` is declared as a `:spices` dep in the
consumer's `build.tur`.

---

## Middleware

Middleware is a function `Request -> option<Response>`:

- Return `(none)` to continue to the next handler.
- Return `(some resp)` to short-circuit and respond immediately.

```turmeric
(import tourist/middleware :refer [use!])

;; Log every request
(def logger
  (fn [req]
    (println (str-concat (req-method req) " " (req-path req)))
    (none)))

(tourist 3000
  (use! logger)
  (get! "/" (fn [_] (text "ok"))))
```

Middleware runs in declaration order before routes.

---

## Module Layout

```
tur-tourist/
  build.tur
  src/
    tourist/
      app.tur        -- tourist entry point; server lifecycle
      dsl.tur        -- get!, post!, put!, delete!, any!, Route type
      router.tur     -- pattern compilation and dispatch
      param.tur      -- param, capture; query-string parser
      helpers.tur    -- text, html, json-body, redirect, status
      middleware.tur -- use!, Middleware type, middleware chain
      static.tur     -- serve-static!; mime table; path-traversal guard
      template.tur   -- render-template (conditional on tur-template dep)
  tests/
    fixtures/
      hello-world/   -- GET / -> 200 "Hello"
      path-capture/  -- GET /user/:id; capture "id"
      query-param/   -- GET /search?q=foo; param "q"
      middleware/    -- logging middleware short-circuit
      post-echo/     -- POST body round-trip
      static-files/  -- serve-static!; mime detection; path-traversal rejection
      template/      -- render-template integration (requires.spices)
```

---

## `build.tur`

```turmeric
(defpackage tur-tourist
  :name        "tur-tourist"
  :version     "0.1.0"
  :description "Sinatra/scotty-style HTTP micro-framework"
  :license     "MIT"
  :spices #{
    "httpd"    #{:url "https://github.com/turmeric-lang/turmeric-spices"
               :ref "httpd-v0.1.0"
               :subdir "spices/httpd"}
    "template" #{:url "https://github.com/turmeric-lang/turmeric-spices"
               :ref "template-v0.1.0"
               :subdir "spices/template"
               :optional true}
    "test"     #{:url "https://github.com/turmeric-lang/turmeric-spices"
               :ref "test-v0.1.0"
               :subdir "spices/test"
               :optional true}
  }
  :exports #{
    "tourist/app"        ["tourist"]
    "tourist/dsl"        ["get!" "post!" "put!" "delete!" "any!"]
    "tourist/param"      ["param" "capture"]
    "tourist/helpers"    ["text" "html" "json-body" "redirect" "status"]
    "tourist/middleware" ["use!"]
    "tourist/static"     ["serve-static!"]
    "tourist/template"   ["render-template"]
  })
```

---

## Implementation Phases

| Step | Task |
|---|---|
| S1 | Scaffold `tur-tourist` in `../turmeric-spices/spices/tourist/` |
| S2 | `router.tur`: compile route patterns to a match function; `:name` and `*` segments |
| S3 | `dsl.tur`: `Route` type; `get!`, `post!`, `put!`, `delete!`, `any!` |
| S4 | `param.tur`: query-string parser; `param`, `capture` |
| S5 | `helpers.tur`: `text`, `html`, `json-body`, `redirect`, `status` |
| S6 | `middleware.tur`: `use!`, middleware chain runner |
| S7 | `app.tur`: `tourist` entry point -- collects routes + middleware, starts `tur-httpd` |
| S8 | `static.tur`: `serve-static!`; mime table; path-traversal guard (reject `..` and symlink escapes) |
| S9 | `template.tur`: `render-template` wrapper (compile-time optional dep) |
| S10 | Fixture tests: hello-world, path-capture, query-param, middleware, post-echo, static-files, template |
| S11 | Docstrings + `just docs` |

---

## End-to-End Example

A complete application using all three spices:

```turmeric
(import tourist/app        :refer [tourist])
(import tourist/dsl        :refer [get! post!])
(import tourist/param      :refer [param capture])
(import tourist/helpers    :refer [text html json-body])
(import tourist/middleware :refer [use!])
(import tourist/template   :refer [render-template])
(import template/env      :refer [make-env])

;; Middleware: simple request logger
(def logger
  (fn [req]
    (println (str-concat (req-method req) " " (req-path req)))
    (none)))

(tourist 3000
  (use! logger)

  (get! "/"
    (fn [_]
      (html (render-template "views/index.html.tur" (make-env {})))))

  (get! "/hello/:name"
    (fn [req]
      (let [name (ok-val (capture req "name"))]
        (text (str-concat "Hello, " name "!")))))

  (post! "/echo"
    (fn [req]
      (text (req-body req)))))
```

---

## Open Questions

- **Async handlers:** v0.1 handlers are synchronous (`Request -> Response`).
  Async handlers are a planned v0.2 goal: `Request -> future<Response>`,
  dispatched on top of a per-connection reactor in `tur-httpd`. We accept
  that v0.1 handlers may need a small lift (wrapping in `future-pure` or
  similar) when v0.2 lands -- the API break is bounded and documented up
  front rather than designed around.
- **Content negotiation:** Resolved -- v0.1 does not inspect `Accept`
  headers. Handlers call the correct response helper themselves; users who
  need negotiation can read the header via `req-header` and dispatch. If a
  `respond-with` pattern emerges from real use it can land in v0.2 without
  an API break.
- **Static file serving:** Resolved -- `serve-static!` ships in v0.1.
  Signature: `(serve-static! url-prefix fs-root)`. The helper strips
  `url-prefix` from the request path, rejects any resolved path that
  escapes `fs-root` (path-traversal guard against `..` segments and
  symlink escapes), reads the file, and serves it with a `Content-Type`
  inferred from the extension (a small built-in mime table). Falls
  through to the next route on 404 so dynamic routes can shadow static
  paths.
