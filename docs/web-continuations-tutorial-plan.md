# Multi-Page Web Forms with Serializable Continuations -- Design & Example Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-14
> **Type:** Tutorial + Example Project
> **Inspiration:** [Racket "Continue: Web Applications in Racket"](https://docs.racket-lang.org/continue/)

---

## Executive Summary

This document outlines a **tutorial, example project, and guide** for building **multi-page web form applications** in Turmeric using **serializable continuations**. The goal is to demonstrate how the web-continuation pattern -- pioneered in PLT Scheme and carried forward in Racket -- can be expressed naturally in Turmeric, and how it eliminates the "web-programming problem" of managing explicit state across HTTP round-trips.

**The core idea:** Instead of manually threading form state through sessions, cookies, or databases, each handler captures a serializable continuation -- "everything the server needs to do next" -- and encodes it as an opaque URL token. When the browser submits the next form, the server deserializes the token and resumes right where it left off.

**What we will build:** A multi-page guestbook application that lets a visitor:

1. Enter their name
2. Enter a message
3. Preview the entry
4. Either go back and edit, or confirm and post

The four pages require no explicit session state. The entire pending post travels as an opaque continuation token in hidden form fields and links.

**Turmeric features exercised:**
- `serial-reset` / `serial-shift` / `serial-resume` (Phase 18)
- `Serializable` typeclass instances for structs
- HTTP handler effects (`defeffect HttpEffect`)
- Record types (`defstruct`)
- Pattern matching, option/result types
- Minimal C FFI for a simple HTTP listener

**Target audience:** Intermediate Turmeric developers who have read the [Serializable Continuations Guide](guides/serializable-continuations-guide.md) and want a concrete, end-to-end web example.

---

## 1. Background: The Web-Programming Problem

### 1.1 The Stateless Mismatch

HTTP is stateless. A web form spanning multiple pages is inherently stateful. The standard solutions all require the programmer to manually manage state:

| Approach | Mechanism | Drawbacks |
|----------|-----------|-----------|
| Hidden form fields | Encode state in `<input type="hidden">` | Tedious, error-prone, exposed to tampering |
| Server-side sessions | Cookie holds session ID, server stores map | Memory leak risk, sticky sessions, scaling issues |
| URL query params | State embedded in URL | URL length limits, bookmarking problems |
| Explicit state machine | Each page is a named step in a DB table | Lots of boilerplate, business logic fragmented across handlers |

### 1.2 The Continuation Solution

The continuation approach, popularized by PLT Scheme's `web-server/servlet` and Racket's `send/suspend`, threads the control-flow problem away entirely:

```
handler-1 runs to a "send this form, then resume" point
  -> captures continuation k
  -> stashes k under token T
  -> sends HTML page with action="/?k=T"

browser submits form
  -> server looks up T, loads k
  -> resumes k with form data
  -> k is now inside handler-1 again, with form data in hand
  -> proceeds naturally to the next step
```

From the programmer's perspective, writing a multi-page flow reads like a single straight-line function. No state machine, no session table, no hidden fields to maintain.

### 1.3 Racket as the Reference Point

Racket's `send/suspend` API looks like this (for comparison):

```racket
(define (enter-name)
  (send/suspend
    (lambda (k-url)
      (response/xexpr
        `(html (body
          (form ([action ,k-url])
            (input ([name "name"]))
            (input ([type "submit"] [value "Next"])))))))))
```

Turmeric's `serial-shift` plays the same role as `send/suspend`, with the difference that the continuation is serialized to bytes and stored in a lookup table rather than living in a heap closure.

---

## 2. Tutorial Structure

The tutorial is broken into ten steps, each introducing one new concept. A reader who completes every step will have a working guestbook server.

| Step | Title | Key Concepts |
|------|-------|--------------|
| 0 | Project Layout | Directory structure, `CMakeLists.txt`, build targets |
| 1 | Minimal HTTP Listener | C FFI, `defeffect HttpEffect`, request/response types |
| 2 | Hello World Handler | Rendering HTML strings, routing, `HttpEffect` |
| 3 | Single-Page Form | Parsing form bodies, `defstruct`, returning responses |
| 4 | Introducing Continuations | `serial-reset`, `serial-shift`, `serial-resume` |
| 5 | Two-Page Flow | Name form -> message form, continuation token in URL |
| 6 | Three-Page Preview | Preview page, "back" link re-using older continuation |
| 7 | Persisting the Store | `Serializable` typeclass, guestbook entries, file-based store |
| 8 | Confirmation & Posting | Confirm page, writing entry, thank-you redirect |
| 9 | Security Hardening | HMAC token signing, expiry, input sanitization |

---

## 3. Step-by-Step Content Outline

### Step 0: Project Layout

Explain the directory structure:

```
examples/guestbook/
  CMakeLists.txt       -- builds the guestbook binary
  src/
    main.tur           -- entry point, starts HTTP listener
    handlers.tur       -- page handler functions
    store.tur          -- guestbook entry store
    templates.tur      -- HTML rendering helpers
    conts.tur          -- continuation store (token -> bytes)
  data/
    entries.bin        -- serialized guestbook entries (auto-created)
    conts/             -- continuation blobs (auto-created)
```

Show how to add `guestbook` as a subdirectory target in the root `CMakeLists.txt`. Show the `just run-guestbook` recipe.

---

### Step 1: Minimal HTTP Listener

Introduce the C FFI shim. The tutorial ships with a tiny `httpd.c` (around 150 lines) that wraps `libmicrohttpd` or a simple `recv`/`send` loop. Expose three C functions to Turmeric:

```c
// httpd.c (shipped with the example, not part of stdlib)
int  httpd_start(int port);
void httpd_dispatch(const char *path, const char *body,
                    char **out_response, int *out_status);
void httpd_run_forever(void);
```

Define the Turmeric side:

```turmeric
;;; httpd-start -- bind an HTTP listener on the given port.
;;;
;;; Parameters:
;;;   port -- TCP port number (1-65535)
;;;
;;; Returns:
;;;   0 on success, non-zero on error.
;;;
;;; Example:
;;;   (httpd-start 8080)
;;;
;;; Since: Guestbook example
(extern-c httpd-start [(port : int64)] : int64
  "httpd_start")
```

Define `HttpRequest` and `HttpResponse` structs:

```turmeric
(defstruct HttpRequest
  [method  : cstr
   path    : cstr
   query   : cstr        ; raw query string, e.g. "k=abc123"
   body    : cstr])      ; raw POST body

(defstruct HttpResponse
  [status  : int64
   headers : (Vec (Pair cstr cstr))
   body    : cstr])
```

---

### Step 2: Hello World Handler

Define the `HttpEffect` algebraic effect:

```turmeric
(defeffect HttpEffect
  (send-response [resp : HttpResponse] : unit))
```

Write a trivial handler and wire it to the dispatcher:

```turmeric
(defn hello-handler [req : HttpRequest] : unit
  (perform HttpEffect (send-response
    (HttpResponse
      :status  200
      :headers [(Pair "Content-Type" "text/html")]
      :body    "<h1>Hello from Turmeric!</h1>"))))
```

Explain the `handle` block that runs at the top of `main.tur`, bridging `HttpEffect` to the actual C response buffer.

---

### Step 3: Single-Page Form

Show how to build a plain HTML form without continuations first, so the reader understands the baseline:

```turmeric
;;; render-name-form -- render the name-entry page.
;;;
;;; Parameters:
;;;   action -- form action URL
;;;
;;; Returns:
;;;   HTML string for the name-entry page.
;;;
;;; Example:
;;;   (render-name-form "/step2")
;;;
;;; Since: Guestbook example
(defn render-name-form [action : cstr] : cstr
  (str "<html><body>"
       "<form method='POST' action='" action "'>"
       "<label>Your name: <input name='name'/></label>"
       "<button type='submit'>Next</button>"
       "</form></body></html>"))
```

Show how to parse `name=Alice` out of a POST body using a small helper:

```turmeric
;;; parse-form-field -- extract a single named field from a URL-encoded body.
;;;
;;; Parameters:
;;;   body  -- raw POST body string
;;;   field -- field name to extract
;;;
;;; Returns:
;;;   (Option cstr) -- Some(value) if found, None if missing.
;;;
;;; Example:
;;;   (parse-form-field "name=Alice&msg=Hello" "name")  ; => (Some "Alice")
;;;
;;; Since: Guestbook example
(defn parse-form-field [body : cstr, field : cstr] : (Option cstr)
  ...)
```

---

### Step 4: Introducing Continuations

Pivot to the central concept. Explain `serial-reset` as the "boundary" and `serial-shift` as the "pause here, hand control to the web server":

```turmeric
;; The entire multi-page flow lives inside one serial-reset.
;; serial-shift [k] means: "serialize k, return a URL token, wait
;; for the browser to POST to that token, then resume here."
(defn run-guestbook-flow [req : HttpRequest] : unit
  (serial-reset
    ...))
```

Show the `send-form-and-wait` helper that encapsulates the pattern of capturing a continuation, storing it, and returning the HTML page:

```turmeric
;;; send-form-and-wait -- render a form page and suspend until it is submitted.
;;;
;;; Parameters:
;;;   render-fn -- a function from token URL to HTML string
;;;
;;; Returns:
;;;   The parsed form body submitted by the browser.
;;;
;;; Example:
;;;   (def body (send-form-and-wait (fn [action] (render-name-form action))))
;;;
;;; Since: Guestbook example
(defn send-form-and-wait [render-fn : (-> cstr cstr)] : cstr
  (serial-shift [k]
    (def token (store-continuation k))
    (def action (str "/submit?k=" token))
    (def html   (render-fn action))
    (send-html-response html)))
```

Explain token generation (`random-hex-64`), the continuation store (`conts.tur`), and the POST-to-resume dispatcher.

---

### Step 5: Two-Page Flow

Use `send-form-and-wait` twice to build a name -> message flow:

```turmeric
(defn run-guestbook-flow [req : HttpRequest] : unit
  (serial-reset
    ;; Page 1: name
    (def name-body (send-form-and-wait
                     (fn [action] (render-name-form action))))
    (def name (or (parse-form-field name-body "name") "Anonymous"))

    ;; Page 2: message
    (def msg-body (send-form-and-wait
                    (fn [action] (render-message-form action name))))
    (def message (or (parse-form-field msg-body "message") ""))))
```

Walk through what happens step-by-step at runtime:

1. Browser GET `/` -> `run-guestbook-flow` starts, hits first `serial-shift`
2. Continuation `k1` serialized to `conts/abc123.bin`
3. Browser receives name-entry HTML with `action="/submit?k=abc123"`
4. Browser POST `/submit?k=abc123` with body `name=Alice`
5. Server loads `k1`, calls `(serial-resume k1 "name=Alice")`
6. Flow resumes after first `send-form-and-wait`, binds `name = "Alice"`
7. Hits second `serial-shift`, same cycle with `k2`

Include a sequence diagram in ASCII:

```
Browser          Server
  |--GET /------->|  start flow, hit shift-1, store k1 -> token T1
  |<--HTML(T1)---|
  |--POST T1----->|  resume k1 with body, hit shift-2, store k2 -> T2
  |<--HTML(T2)---|
  |--POST T2----->|  resume k2, flow completes
  |<--Thank You--|
```

---

### Step 6: Three-Page Preview with Back Navigation

Add a preview page. The user can click "Edit" to go back. Demonstrate that the "back" link simply encodes an older continuation token -- the server has already stored it:

```turmeric
    ;; Page 3: preview
    (def preview-html
      (render-preview name message
        :confirm-action (str "/submit?k=" confirm-token)
        :back-action    (str "/submit?k=" back-token)))
```

Explain that `back-token` points to `k2` (the message-entry continuation) and `confirm-token` points to a new continuation `k3` ready to write the entry. The server stores both. Back navigation is free.

Discuss token expiry briefly: for a tutorial, tokens persist until the process restarts. Step 9 covers expiry.

---

### Step 7: Persisting the Guestbook Store

Introduce `GuestEntry` and its `Serializable` instance:

```turmeric
(defstruct GuestEntry
  [name      : cstr
   message   : cstr
   posted-at : int64])  ; Unix timestamp

(definstance Serializable GuestEntry
  (serialize [e]
    (serialize (Vec.of [e.name e.message (int64->cstr e.posted-at)])))
  (deserialize [b]
    (let? [parts (deserialize b : (Vec cstr))]
      (GuestEntry
        :name      (Vec.get parts 0)
        :message   (Vec.get parts 1)
        :posted-at (cstr->int64 (Vec.get parts 2))))))
```

Show the store API in `store.tur`:

```turmeric
;;; store-load -- load all guestbook entries from disk.
(defn store-load [] : (Vec GuestEntry) ...)

;;; store-append -- append a new entry and flush to disk.
(defn store-append [entry : GuestEntry] : unit ...)

;;; store-all -- return a snapshot of current entries.
(defn store-all [] : (Vec GuestEntry) ...)
```

Show how the entries file is written as a serialized `Vec GuestEntry`.

---

### Step 8: Confirmation, Posting, and Thank-You

Complete the flow. The confirm continuation writes the entry:

```turmeric
(defn run-guestbook-flow [req : HttpRequest] : unit
  (serial-reset
    (def name-body (send-form-and-wait (fn [a] (render-name-form a))))
    (def name      (or (parse-form-field name-body "name") "Anonymous"))

    (def msg-body  (send-form-and-wait (fn [a] (render-message-form a name))))
    (def message   (or (parse-form-field msg-body "message") ""))

    ;; Page 3: preview with back link
    (def preview-body
      (serial-shift [k-back]
        (serial-shift [k-confirm]
          (def t-back    (store-continuation k-back))
          (def t-confirm (store-continuation k-confirm))
          (send-html-response
            (render-preview name message t-back t-confirm)))))

    ;; Distinguish "back" vs "confirm" by which token was POSTed
    (match (parse-form-field preview-body "action")
      (Some "confirm") ->
        (do
          (store-append (GuestEntry :name name :message message
                                   :posted-at (unix-now)))
          (send-html-response (render-thankyou (store-all))))
      _ ->
        ;; "back" branch -- the back token resumes from the message form
        ;; No code needed here; routing handles it
        (unit))))
```

Explain that the back/confirm branch is handled at the router level by which token is looked up.

---

### Step 9: Security Hardening

Cover three practical concerns:

**HMAC signing** -- Tokens are opaque random strings, but an attacker could guess or brute-force them. Sign tokens with a server secret:

```turmeric
(defn sign-token [token : cstr, secret : bytes] : cstr
  (str token "." (hex-encode (hmac-sha256 secret (cstr->bytes token)))))

(defn verify-token [signed : cstr, secret : bytes] : (Option cstr)
  (match (cstr-split signed ".")
    [token sig] ->
      (when (bytes-eq? (hmac-sha256 secret (cstr->bytes token))
                       (hex-decode sig))
        (Some token))
    _ -> (None)))
```

**Token expiry** -- Store a creation timestamp alongside the continuation bytes. Reject tokens older than N minutes:

```turmeric
(defstruct StoredCont
  [bytes      : bytes
   created-at : int64])

(defn continuation-expired? [sc : StoredCont] : bool
  (> (- (unix-now) sc.created-at) CONT_TTL_SECONDS))
```

**Input sanitization** -- Strip or escape `<` and `>` from all user-supplied strings before rendering into HTML:

```turmeric
;;; html-escape -- escape HTML special characters in a string.
(defn html-escape [s : cstr] : cstr
  (cstr-replace-all
    (cstr-replace-all s "&" "&amp;")
    "<" "&lt;"))
```

---

## 4. Example Project Specification

### 4.1 File-by-File Summary

| File | Purpose |
|------|---------|
| `src/main.tur` | Entry point; starts listener, runs router loop |
| `src/handlers.tur` | `run-guestbook-flow` and `GET /` entry point |
| `src/router.tur` | Dispatches `POST /submit?k=TOKEN` to the right continuation |
| `src/conts.tur` | Continuation store: token generation, file-based load/save |
| `src/store.tur` | Guestbook entry store: load/append/list |
| `src/templates.tur` | HTML rendering functions for each page |
| `src/security.tur` | HMAC signing, token verification, `html-escape` |
| `httpd.c` | Tiny C HTTP listener shim (included verbatim with explanation) |
| `CMakeLists.txt` | Builds the binary; links `libmicrohttpd` via CPM |

### 4.2 Pages

| Page | Route | Description |
|------|-------|-------------|
| Name form | `GET /` | Enter your name |
| Message form | `POST /submit?k=T1` | Enter your message |
| Preview | `POST /submit?k=T2` | Review name + message, confirm or edit |
| Back to message | `POST /submit?k=T2b` | Re-enter the message form |
| Thank you | `POST /submit?k=T3` | Entry saved; show guestbook |
| Guestbook list | `GET /entries` | List all entries (no continuation needed) |

### 4.3 Data Flow Diagram

```
GET /
  |
  +-> run-guestbook-flow starts
       serial-reset boundary begins
         serial-shift -> T1 stored, name-form HTML sent

POST /submit?k=T1  body: name=Alice
  |
  +-> resume k1 with body
       name = "Alice"
         serial-shift -> T2 stored, message-form HTML sent

POST /submit?k=T2  body: message=Hello
  |
  +-> resume k2 with body
       message = "Hello"
         serial-shift (k-back) -> T2b stored
           serial-shift (k-confirm) -> T3 stored
             preview HTML sent (contains links to T2b and T3)

POST /submit?k=T3  (user clicked Confirm)
  |
  +-> resume k3
       GuestEntry saved to disk
         thank-you HTML sent

POST /submit?k=T2b  (user clicked Edit)
  |
  +-> resume k-back
       message-form HTML sent again (same as T2 step above)
```

---

## 5. Guide Structure (Companion Reference)

Alongside the step-by-step tutorial, provide a shorter **reference guide** (`guides/web-continuations-guide.md`) covering:

1. **The `send-form-and-wait` pattern** -- canonical idiom, parameters, when to use it
2. **Continuation store contract** -- token format, storage interface, eviction policy
3. **Serializable structs** -- required typeclass instances, how to derive them
4. **Routing model** -- how `POST /submit?k=TOKEN` maps to `serial-resume`
5. **Composing flows** -- nesting flows, passing continuations across handler boundaries
6. **Comparison to Racket's `send/suspend`** -- side-by-side API table
7. **Limitations** -- no true call/cc semantics, deep-copy cost, token size, no sharing

The guide should be self-contained for readers who already understand continuations and just want the Turmeric-specific API.

---

## 6. Supporting Materials

### 6.1 Inline Runnable Examples

Each code block in the tutorial should be a complete, compilable excerpt. A `just run-guestbook` target in the root `Justfile` should build and launch the server so a reader can follow along live.

### 6.2 Tests

Add a `tests/guestbook/` directory with:

- `flow_test.tur` -- unit-tests for `parse-form-field`, `html-escape`, `sign-token`, `verify-token`
- `store_test.tur` -- round-trip test for `GuestEntry` serialization, `store-append` / `store-load`
- `integration_test.tur` -- drives the full flow programmatically without HTTP by calling `serial-resume` directly

### 6.3 HTML Templates

The templates use plain string concatenation -- no macro DSL -- so Step 2 does not require readers to learn anything beyond `str`. A stretch goal for an appendix: introduce an `html` macro that produces a Turmeric S-expression DSL for composing HTML, similar to Racket's `response/xexpr`.

---

## 7. Relationship to Existing Docs

| Existing doc | Relationship |
|---|---|
| [serializable-continuations-guide.md](guides/serializable-continuations-guide.md) | Prerequisite; already contains the web-continuation example as a short snippet -- this tutorial expands it into a full walkthrough |
| [checkpointing-guide.md](guides/checkpointing-guide.md) | Sibling use case (persistence); cross-link from Step 7 |
| [effects-system-guide.md](guides/effects-system-guide.md) | `HttpEffect` is an algebraic effect -- cross-link from Step 2 |
| [c-integration-guide.md](guides/c-integration-guide.md) | `httpd.c` shim; cross-link from Step 1 |
| [snake-game-tutorial.md](guides/snake-game-tutorial.md) | Model for step-by-step format |

---

## 8. Open Questions

1. **HTTP library choice** -- Should the shim use `libmicrohttpd` (easy, permissive license) or a raw `socket`/`accept` loop (zero deps, more code to explain)? A raw loop is more transparent for beginners and avoids a CPM dependency. Recommendation: raw loop for the tutorial, note `libmicrohttpd` as an upgrade path.

2. **Token storage** -- File-per-token in `data/conts/` vs. a single `conts.bin` append-log. File-per-token is simpler to explain and naturally evicted by mtime; the append-log is faster. Recommendation: file-per-token for the tutorial.

3. **URL encoding** -- Should `parse-form-field` handle `%XX` percent-decoding? Technically required; practically harmless to skip for ASCII-only names. Recommendation: include a minimal percent-decoder with a note about the edge case.

4. **Concurrent requests** -- The tutorial is single-threaded. If a reader opens two tabs, the second GET `/` overwrites the first flow. Add a one-paragraph note explaining the limitation and pointing to the STM guide for a multi-threaded extension.

5. **Stretch goal: HTML macro DSL** -- Racket's `response/xexpr` makes templates much more readable. An appendix showing how to write a Turmeric `html` macro that compiles to string concatenation would reinforce the macro system and make a natural follow-on exercise.

---

## 9. Deliverables

| Deliverable | Path |
|-------------|------|
| Tutorial (step-by-step) | `docs/guides/web-continuations-tutorial.md` |
| Reference guide | `docs/guides/web-continuations-guide.md` |
| Example project | `examples/guestbook/` |
| Unit tests | `tests/guestbook/` |
| Justfile recipe | `just run-guestbook` in root `Justfile` |
| README entry | Add to `docs/guides/README.md` under "Tutorials and Examples" |
