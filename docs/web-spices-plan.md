# Plan: Web Spices -- Template, HTTP Server, Scotty Micro-Framework

> **Status:** Draft Plan
> **Last Updated:** 2026-05-28
> **Type:** Spice Design + Architecture + Implementation Roadmap
> **Related:**
> - `docs/guides/developing-spices-guide.md` (spice authoring conventions)
> - `docs/guides/threading-guide.md` (OS threads / Mutex / Arc)
> - `../turmeric-spices/spices/http/` (existing HTTP client spice)

---

## Overview

Three new spices form a composable web stack for Turmeric:

| Spice | Analogue | Depends on |
|---|---|---|
| `tur-template` | ERB / EJS | (none -- pure Turmeric) |
| `tur-httpd` | Mongoose / Civetweb | (none -- POSIX sockets + pthreads) |
| `tur-turist` | Haskell's scotty | `tur-httpd`, `tur-template` |

`tur-template` is a standalone string-templating engine; `tur-httpd` is a
threaded HTTP/1.1 server; `tur-turist` layers routing and response helpers
on top of both. They are deliberately separate so any layer can be used
independently -- you can use `tur-template` in a CLI tool, or `tur-httpd`
without any routing framework.

---

## Spice 1: `tur-template`

### Motivation

Turmeric already has first-class string operations but no composable way to
mix control flow and literal text. A minimal template engine eliminates the
`str-concat` soup that arises in code generators, email renderers, and
HTTP response bodies.

ERB/EJS were chosen as the model because:
- Syntax is widely known and tooling-friendly (syntax highlighters exist).
- The template engine itself requires no external C library.
- Semantics are simple enough to specify completely in one file.

### Syntax

```
<% ... %>    code block (result discarded; use for if/let/for)
<%= ... %>   expression block (result coerced to :cstr and emitted)
<%# ... %>   comment (removed entirely; never emitted)
<%%          literal <%  (escape)
```

Everything outside a tag is emitted verbatim.

### Example

```tur
(import template/render :refer [render render-file])

(let [tmpl "<h1><%= title %></h1>\n<% for [item items] %><li><%= item %></li>\n<% end %>"]
  (render tmpl {"title" "List" "items" (list "a" "b" "c")}))
; => "<h1>List</h1>\n<li>a</li>\n<li>b</li>\n<li>c</li>\n"
```

### Design

The engine is three passes over the template string, all in pure Turmeric
with one thin inline-C helper for the inner scan:

```
1. Lex:    cstr -> list<Token>   (TextChunk | CodeBlock | ExprBlock | Comment)
2. Parse:  list<Token> -> list<Node>  (Literal | Emit | IfNode | ForNode | LetNode)
3. Render: list<Node> x Env -> :cstr
```

`Env` is a `Map<:cstr :cstr>` from `tur/map`. All interpolated values are
strings; callers must convert before passing. This keeps the engine small and
avoids pulling in a reflection or dynamic-dispatch system.

#### Supported control forms inside `<% ... %>`

| Form | Syntax |
|---|---|
| `if` | `<% if cond %>` ... `<% else %>` ... `<% end %>` |
| `for` | `<% for [x xs] %>` ... `<% end %>` |
| `let` | `<% let [x val] %>` ... `<% end %>` |

These forms mirror Turmeric's own syntax and are parsed by the template engine
as a small DSL (not eval'd as Turmeric code). This keeps `tur-template`
dependency-free and sandboxable.

#### `render-file`

```tur
(render-file "/path/to/template.html.tur" env)
```

Reads the file, passes to `render`. File extension `.tur` is conventional but
not enforced.

### Module Layout

```
tur-template/
  build.tur
  src/
    template/
      token.tur       -- Token type; lex :: :cstr -> list<Token>
      parse.tur       -- Node type; parse :: list<Token> -> list<Node>
      render.tur      -- render :: list<Node> x Env -> :cstr
                         render-file :: :cstr x Env -> result<:cstr>
      env.tur         -- Env type alias; make-env, env-get, env-set
  tests/
    fixtures/
      basic/
        template.html.tur
        expected.txt
        main.tur
      if-else/
      for-loop/
      escaping/
```

### `build.tur`

```turmeric
(defpackage tur-template
  :name        "tur-template"
  :version     "0.1.0"
  :description "ERB/EJS-style string templating engine"
  :license     "MIT"
  :spices #{
    "test" #{:url "https://github.com/turmeric-lang/turmeric-spices"
            :ref "test-v0.1.0"
            :subdir "spices/test"
            :optional true}
  }
  :exports #{
    "template/render" ["render" "render-file"]
    "template/env"    ["make-env" "env-get" "env-set"]
  })
```

---

## Spice 2: `tur-httpd`

### Motivation

The existing `tur-http` spice is an HTTP *client*. There is no server-side
counterpart. `tur-httpd` fills that gap with a minimal threaded HTTP/1.1
server built on POSIX `socket`/`accept`/`pthreads` -- no additional C library
required.

Why POSIX sockets rather than wrapping Mongoose or Civetweb?

- Zero additional CMake deps; compiles anywhere Turmeric compiles.
- The server is simple enough (~300 lines of inline-C) that a wrapper adds
  more indirection than value.
- Civetweb/Mongoose both have their own event loops that conflict with
  Turmeric's threading model.

For production TLS, a future `tur-httpd-tls` variant can swap the plain
socket layer for mbedTLS (reusing the pattern from `tur-http`).

### Threading Model

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

This is intentionally simple. A thread-pool variant is out of scope for
the initial milestone.

### API

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

#### Request accessors

| Function | Returns | Notes |
|---|---|---|
| `req-method` | `:cstr` | `"GET"`, `"POST"`, ... |
| `req-path` | `:cstr` | path component only (`"/foo/bar"`) |
| `req-query` | `:cstr` | raw query string after `?`, or `""` |
| `req-header` | `result<:cstr>` | `(req-header req "Content-Type")` |
| `req-body` | `:cstr` | request body, `""` if none |

#### Response constructors

| Function | Signature | HTTP status |
|---|---|---|
| `ok` | `content-type body -> Response` | 200 |
| `not-found` | `body -> Response` | 404 |
| `bad-request` | `body -> Response` | 400 |
| `response` | `status content-type body -> Response` | caller-supplied |
| `with-header` | `Response key val -> Response` | adds a header |
| `with-body` | `Response body -> Response` | replaces body |
| `with-status` | `Response status -> Response` | replaces status |

### Request Parsing

HTTP/1.1 only; `Transfer-Encoding: chunked` is not supported in the initial
milestone. The inline-C parser reads the request line, headers, and body
(up to `Content-Length` bytes) into a heap-allocated struct.

### Module Layout

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

### `build.tur`

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

## Spice 3: `tur-turist`

### Motivation

`tur-httpd` delivers raw requests to a single handler function. That forces
every application to write its own dispatch table. `tur-turist` provides
the idiom from Haskell's
[scotty](https://hackage.haskell.org/package/scotty):

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
(import turist/app   :refer [turist])
(import turist/dsl   :refer [get! post! put! delete! text json-body redirect])
(import turist/param :refer [param capture])

(turist 3000
  (get! "/hello/:name"
    (fn [req]
      (text (str-concat "Hello " (capture req "name")))))
  (post! "/echo"
    (fn [req]
      (text (req-body req)))))
```

### Route Syntax

Route patterns follow Express/Sinatra conventions:

| Pattern | Matches | Captures |
|---|---|---|
| `"/hello"` | `/hello` exactly | (none) |
| `"/user/:id"` | `/user/123` | `id = "123"` |
| `"/files/*"` | `/files/a/b/c` | `* = "a/b/c"` |

Patterns are matched in declaration order; first match wins.

### DSL Functions

| Function | HTTP method | Example |
|---|---|---|
| `get!` | GET | `(get! "/path" handler)` |
| `post!` | POST | `(post! "/items" handler)` |
| `put!` | PUT | `(put! "/items/:id" handler)` |
| `delete!` | DELETE | `(delete! "/items/:id" handler)` |
| `any!` | any | `(any! "/catch-all" handler)` |

Each returns a `Route` value. `turist` collects routes, builds a dispatch
table, starts `tur-httpd`, and routes incoming requests.

### Handler Helpers

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

### Template Integration

When `tur-template` is present as a dependency, `tur-turist` exposes a
convenience helper:

```turmeric
(import turist/template :refer [render-template])

(get! "/greet/:name"
  (fn [req]
    (let [name (ok-val (capture req "name"))
          env  (make-env {"name" name})]
      (html (render-template "views/greet.html.tur" env)))))
```

`render-template` resolves paths relative to the working directory. It is
only available if `tur-template` is declared as a `:spices` dep in the
consumer's `build.tur`.

### Middleware

Middleware is a function `Request -> option<Response>`:

- Return `(none)` to continue to the next handler.
- Return `(some resp)` to short-circuit and respond immediately.

```turmeric
(import turist/middleware :refer [use!])

;; Log every request
(def logger
  (fn [req]
    (println (str-concat (req-method req) " " (req-path req)))
    (none)))

(turist 3000
  (use! logger)
  (get! "/" (fn [_] (text "ok"))))
```

Middleware runs in declaration order before routes.

### Module Layout

```
tur-turist/
  build.tur
  src/
    turist/
      app.tur        -- turist entry point; server lifecycle
      dsl.tur        -- get!, post!, put!, delete!, any!, Route type
      router.tur     -- pattern compilation and dispatch
      param.tur      -- param, capture; query-string parser
      helpers.tur    -- text, html, json-body, redirect, status
      middleware.tur -- use!, Middleware type, middleware chain
      template.tur   -- render-template (conditional on tur-template dep)
  tests/
    fixtures/
      hello-world/   -- GET / -> 200 "Hello"
      path-capture/  -- GET /user/:id; capture "id"
      query-param/   -- GET /search?q=foo; param "q"
      middleware/    -- logging middleware short-circuit
      post-echo/     -- POST body round-trip
      template/      -- render-template integration (requires.spices)
```

### `build.tur`

```turmeric
(defpackage tur-turist
  :name        "tur-turist"
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
    "turist/app"        ["turist"]
    "turist/dsl"        ["get!" "post!" "put!" "delete!" "any!"]
    "turist/param"      ["param" "capture"]
    "turist/helpers"    ["text" "html" "json-body" "redirect" "status"]
    "turist/middleware" ["use!"]
    "turist/template"   ["render-template"]
  })
```

---

## Implementation Phases

### Phase 1 -- `tur-template` (no deps, standalone)

| Step | Task |
|---|---|
| T1 | Scaffold `tur-template` in `../turmeric-spices/spices/template/` |
| T2 | Implement `token.tur`: lexer that produces `list<Token>` |
| T3 | Implement `parse.tur`: parser for `if/else/end`, `for/end`, `let/end`, emit |
| T4 | Implement `render.tur`: `render` and `render-file` with `Env = Map<:cstr :cstr>` |
| T5 | Add `env.tur`: thin `make-env`, `env-get`, `env-set` wrappers |
| T6 | Write fixture tests: basic, if-else, for-loop, escaping |
| T7 | Add docstrings (`;;;`) to all exported functions |
| T8 | Run `just docs` to include `tur-template` in the API reference |

### Phase 2 -- `tur-httpd` (POSIX sockets)

| Step | Task |
|---|---|
| H1 | Scaffold `tur-httpd` in `../turmeric-spices/spices/httpd/` |
| H2 | `parse.tur`: inline-C request parser (method, path, headers, body) |
| H3 | `request.tur`: `Request` struct; public accessors |
| H4 | `response.tur`: `Response` struct; constructors + modifier functions |
| H5 | `write.tur`: inline-C response serializer |
| H6 | `server.tur`: POSIX `socket`/`bind`/`listen`/`accept` loop in a background thread; worker-per-connection |
| H7 | Fixture tests: echo, headers, post-body (using `tur-http` client for assertions) |
| H8 | Docstrings + `just docs` |

### Phase 3 -- `tur-turist` (routing + middleware)

| Step | Task |
|---|---|
| S1 | Scaffold `tur-turist` in `../turmeric-spices/spices/turist/` |
| S2 | `router.tur`: compile route patterns to a match function; `:name` and `*` segments |
| S3 | `dsl.tur`: `Route` type; `get!`, `post!`, `put!`, `delete!`, `any!` |
| S4 | `param.tur`: query-string parser; `param`, `capture` |
| S5 | `helpers.tur`: `text`, `html`, `json-body`, `redirect`, `status` |
| S6 | `middleware.tur`: `use!`, middleware chain runner |
| S7 | `app.tur`: `turist` entry point -- collects routes + middleware, starts `tur-httpd` |
| S8 | `template.tur`: `render-template` wrapper (compile-time optional dep) |
| S9 | Fixture tests: hello-world, path-capture, query-param, middleware, post-echo, template |
| S10 | Docstrings + `just docs` |

---

## End-to-End Example

A complete application using all three spices:

```turmeric
(import turist/app        :refer [turist])
(import turist/dsl        :refer [get! post!])
(import turist/param      :refer [param capture])
(import turist/helpers    :refer [text html json-body])
(import turist/middleware :refer [use!])
(import turist/template   :refer [render-template])
(import template/env      :refer [make-env])

;; Middleware: simple request logger
(def logger
  (fn [req]
    (println (str-concat (req-method req) " " (req-path req)))
    (none)))

(turist 3000
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

- **`tur-httpd` keep-alive:** HTTP/1.1 persistent connections are out of scope
  for the initial milestone. Responses always include `Connection: close`.
- **`tur-template` eval mode:** A future `render-eval` variant could evaluate
  `<% %>` blocks as real Turmeric expressions (using the `eval` API). This is
  intentionally deferred -- the sandboxed DSL approach is safer and simpler.
- **`tur-turist` async handlers:** All handlers are synchronous. An async
  variant that returns `future<Response>` is a natural follow-on once
  `tur-httpd` gains a thread-pool mode.
- **Content negotiation:** `tur-turist` does not inspect `Accept` headers.
  Handlers must call the correct response helper themselves.
- **Static file serving:** A `serve-static!` route helper is a common need;
  deferred to a follow-on milestone.
