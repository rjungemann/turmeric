# Web Continuations Tutorial -- Multi-Page Forms Without Session State

A **step-by-step guide** to building a multi-page web form application in Turmeric using serializable continuations. Each step introduces one new concept. By the end you will have a working guestbook server that captures, stores, and resumes continuations across HTTP round-trips.

> **Prerequisites**: Read [serializable-continuations-guide.md](serializable-continuations-guide.md) first. You should understand `serial-reset`, `serial-shift`, and `serial-resume` before starting.

> **Related guides**: [effects-system-guide.md](effects-system-guide.md), [c-integration-guide.md](c-integration-guide.md), [checkpointing-guide.md](checkpointing-guide.md)

---

## Tutorial Overview

| Step | Title | Key Concepts |
|------|-------|--------------|
| 0 | Project Layout | Directory structure, `CMakeLists.txt`, build targets |
| 1 | Minimal HTTP Listener | C FFI, `defeffect HttpEffect`, request/response types |
| 2 | Hello World Handler | Rendering HTML strings, routing, `HttpEffect` |
| 3 | Single-Page Form | Parsing form bodies, `defstruct`, returning responses |
| 4 | Introducing Continuations | `serial-reset`, `serial-shift`, `serial-resume` |
| 5 | Two-Page Flow | Name form -> message form, continuation token in URL |
| 6 | Three-Page Preview | Preview page, "back" link re-using an older continuation |
| 7 | Persisting the Store | `Serializable` typeclass, guestbook entries, file-based store |
| 8 | Confirmation and Posting | Confirm page, writing entry, thank-you redirect |
| 9 | Security Hardening | HMAC token signing, expiry, input sanitization |

---

## Background: The Web-Programming Problem

### The Stateless Mismatch

HTTP is stateless. A web form spanning multiple pages is inherently stateful. The standard solutions all require the programmer to manually manage state:

| Approach | Mechanism | Drawbacks |
|----------|-----------|-----------|
| Hidden form fields | Encode state in `<input type="hidden">` | Tedious, error-prone, exposed to tampering |
| Server-side sessions | Cookie holds session ID, server stores map | Memory leak risk, sticky sessions, scaling issues |
| URL query params | State embedded in URL | URL length limits, bookmarking problems |
| Explicit state machine | Each page is a named step in a DB table | Lots of boilerplate, business logic fragmented |

### The Continuation Solution

The continuation approach, popularized by PLT Scheme's `web-server/servlet` and Racket's `send/suspend`, threads the control-flow problem away entirely:

```turmeric
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

### Racket as the Reference Point

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

## Step 0: Project Layout

### 0.1 Directory Structure

The guestbook lives at `examples/guestbook/`:

```
examples/guestbook/
  CMakeLists.txt       -- builds the guestbook binary
  httpd.c              -- tiny raw-socket HTTP listener (~150 lines)
  src/
    main.tur           -- entry point, starts listener, runs router loop
    handlers.tur       -- run-guestbook-flow and GET / entry point
    router.tur         -- dispatches POST /submit?k=TOKEN to continuations
    conts.tur          -- continuation store: token generation, file load/save
    store.tur          -- guestbook entry store: load/append/list
    templates.tur      -- HTML rendering helpers for each page
    security.tur       -- HMAC signing, token verification, html-escape
  data/
    entries.bin        -- serialized guestbook entries (auto-created at runtime)
    conts/             -- one file per continuation blob (auto-created at runtime)
```text

The `data/` directory is created at runtime by the server on first start. You do not need to create it manually.

### 0.2 Adding the Build Target

In the root `CMakeLists.txt`, the guestbook is included when `TUR_EXAMPLES=ON`:

```
if(TUR_EXAMPLES)
  add_subdirectory(examples/guestbook)
endif()
```

The guestbook `CMakeLists.txt` links only against the Turmeric runtime and the C standard library -- no external HTTP library is required.

### 0.3 Justfile Recipe

After following this tutorial, the root `Justfile` gains:

```just
run-guestbook: configure-examples
    cmake --build build --target guestbook
    ./build/examples/guestbook/guestbook
```

---

## Step 1: Minimal HTTP Listener

### 1.1 The C Shim

Rather than pulling in a third-party library, the tutorial ships a tiny `httpd.c` (around 150 lines) that uses only POSIX sockets. This keeps dependencies at zero and makes the networking layer fully transparent.

Three C functions are exposed to Turmeric:

```c
/* httpd.c -- minimal raw-socket HTTP/1.1 listener.
 * Handles one request at a time (single-threaded by design).
 * Production use: replace with libmicrohttpd or similar.
 */

/* Bind a TCP socket and start listening on port.
 * Returns 0 on success, -1 on error. */
int httpd_start(int port);

/* Block until the next HTTP request arrives.
 * Writes method, path, query, and body into the out-params.
 * Caller must free the returned strings. */
void httpd_next_request(char **out_method, char **out_path,
                        char **out_query, char **out_body);

/* Send an HTTP response.
 * status: e.g. 200, 302, 404
 * content_type: e.g. "text/html"
 * body: response body string */
void httpd_send_response(int status, const char *content_type,
                         const char *body);
```

### 1.2 Turmeric Bindings

Declare the three C functions using `extern-c`:

```turmeric
;;; httpd-start -- bind an HTTP listener on the given port.
;;;
;;; Parameters:
;;;   port -- TCP port number (1-65535)
;;;
;;; Returns:
;;;   0 on success, -1 on error.
;;;
;;; Example:
;;;   (httpd-start 8080)
;;;
;;; Since: Guestbook example
(extern-c httpd-start [(port : int64)] : int64
  "httpd_start")

;;; httpd-next-request -- block until the next HTTP request arrives.
;;;
;;; Parameters:
;;;   out-method -- pointer to receive the HTTP method string
;;;   out-path   -- pointer to receive the URL path
;;;   out-query  -- pointer to receive the raw query string
;;;   out-body   -- pointer to receive the POST body
;;;
;;; Returns:
;;;   unit
;;;
;;; Example:
;;;   (httpd-next-request method path query body)
;;;
;;; Since: Guestbook example
(extern-c httpd-next-request
  [(out-method : ptr<cstr>)
   (out-path   : ptr<cstr>)
   (out-query  : ptr<cstr>)
   (out-body   : ptr<cstr>)] : unit
  "httpd_next_request")

;;; httpd-send-response -- write an HTTP response to the current connection.
;;;
;;; Parameters:
;;;   status       -- HTTP status code (200, 302, 404, etc.)
;;;   content-type -- MIME type string
;;;   body         -- response body
;;;
;;; Returns:
;;;   unit
;;;
;;; Example:
;;;   (httpd-send-response 200 "text/html" "<h1>Hello</h1>")
;;;
;;; Since: Guestbook example
(extern-c httpd-send-response
  [(status       : int64)
   (content-type : cstr)
   (body         : cstr)] : unit
  "httpd_send_response")
```

### 1.3 Request and Response Structs

```turmeric
;;; HttpRequest -- parsed HTTP request from the listener.
;;;
;;; Since: Guestbook example
(defstruct HttpRequest
  [method  : cstr   ; "GET" or "POST"
   path    : cstr   ; URL path, e.g. "/submit"
   query   : cstr   ; raw query string, e.g. "k=abc123"
   body    : cstr]) ; raw POST body, e.g. "name=Alice"
```

`HttpResponse` is implicit -- the C shim owns the socket, so the Turmeric side just calls `httpd-send-response` directly rather than constructing a value.

---

## Step 2: Hello World Handler

### 2.1 Defining `HttpEffect`

Algebraic effects decouple handler logic from transport. Define one effect operation:

```turmeric
;;; HttpEffect -- algebraic effect for sending HTTP responses.
;;;
;;; Since: Guestbook example
(defeffect HttpEffect
  (send-html [body : cstr] : unit))
```

### 2.2 A Trivial Handler

```turmeric
;;; hello-handler -- send a minimal HTML page.
;;;
;;; Parameters:
;;;   req -- the incoming HTTP request (unused)
;;;
;;; Returns:
;;;   unit
;;;
;;; Example:
;;;   (hello-handler req)
;;;
;;; Since: Guestbook example
(defn hello-handler [req : HttpRequest] : unit
  (perform HttpEffect (send-html "<h1>Hello from Turmeric!</h1>")))
```

### 2.3 The Effect Handler in `main.tur`

The top-level loop in `main.tur` interprets `HttpEffect` by calling the C shim:

```turmeric
(defn run-loop [] : unit
  (loop
    (def method (ptr-alloc cstr))
    (def path   (ptr-alloc cstr))
    (def query  (ptr-alloc cstr))
    (def body   (ptr-alloc cstr))
    (httpd-next-request method path query body)
    (def req (HttpRequest
               :method (ptr-deref method)
               :path   (ptr-deref path)
               :query  (ptr-deref query)
               :body   (ptr-deref body)))
    (handle
      (dispatch req)
      (HttpEffect.send-html [html] ->
        (httpd-send-response 200 "text/html" html)
        (resume unit)))))

(defn main [] : unit
  (httpd-start 8080)
  (run-loop))
```

The `dispatch` function (defined in `router.tur`) routes requests to the appropriate handler.

---

## Step 3: Single-Page Form

Before introducing continuations, build a plain single-page form to establish the baseline.

### 3.1 Rendering the Name Form

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
       "<h2>Guestbook -- Enter Your Name</h2>"
       "<form method='POST' action='" action "'>"
       "<label>Your name: <input name='name' autofocus/></label>"
       "<br/><button type='submit'>Next &rarr;</button>"
       "</form></body></html>"))
```

### 3.2 Parsing Form Fields

URL-encoded POST bodies have the form `name=Alice&message=Hello+World`. Extract a single named field:

```turmeric
;;; parse-form-field -- extract a named field from a URL-encoded body.
;;;
;;; Parameters:
;;;   body  -- raw POST body string
;;;   field -- field name to extract
;;;
;;; Returns:
;;;   (Option cstr) -- Some(value) if found, None if absent.
;;;
;;; Example:
;;;   (parse-form-field "name=Alice&msg=Hello" "name")  ; => (Some "Alice")
;;;
;;; Since: Guestbook example
(defn parse-form-field [body : cstr, field : cstr] : (Option cstr)
  (def prefix (str field "="))
  (def start  (cstr-find body prefix))
  (match start
    (None) -> (None)
    (Some idx) ->
      (def val-start (+ idx (cstr-len prefix)))
      (def rest      (cstr-drop body val-start))
      (def end       (or (cstr-find rest "&") (cstr-len rest)))
      (Some (percent-decode (cstr-take rest end)))))
```

The `percent-decode` helper converts `%XX` sequences and replaces `+` with space:

```turmeric
;;; percent-decode -- decode a URL percent-encoded string.
;;;
;;; Parameters:
;;;   s -- percent-encoded input
;;;
;;; Returns:
;;;   Decoded cstr. Invalid sequences are passed through unchanged.
;;;
;;; Example:
;;;   (percent-decode "Hello%20World")  ; => "Hello World"
;;;
;;; Since: Guestbook example
(defn percent-decode [s : cstr] : cstr
  ...)
```

> **Note**: A complete `percent-decode` implementation is provided in `src/security.tur`. For ASCII-only names and messages you can skip percent-decoding during early development, but include it before exposing the server to real users.

---

## Step 4: Introducing Continuations

### 4.1 The Core Idea

`serial-reset` marks the outer boundary of a serializable computation. `serial-shift` pauses the computation and hands a serialized continuation to the body:

```turmeric
;; Toy example: add two numbers entered on separate pages.
(defn add-flow [req : HttpRequest] : unit
  (serial-reset
    ;; Page 1: ask for first number
    (def body-a (send-form-and-wait (fn [action]
      (str "<form method='POST' action='" action "'>"
           "<input name='a'/><button>Next</button></form>"))))
    (def a (cstr->int64 (or (parse-form-field body-a "a") "0")))

    ;; Page 2: ask for second number
    (def body-b (send-form-and-wait (fn [action]
      (str "<form method='POST' action='" action "'>"
           "<input name='b'/><button>Add</button></form>"))))
    (def b (cstr->int64 (or (parse-form-field body-b "b") "0")))

    ;; Done: show result
    (perform HttpEffect
      (send-html (str "<p>" a " + " b " = " (int64->cstr (+ a b)) "</p>")))))
```

The two `send-form-and-wait` calls look like blocking reads but each one:
1. Serializes the continuation ("everything left to do after this point")
2. Stores it under a random token
3. Renders an HTML page whose form `action` contains that token
4. Returns control to the HTTP listener

When the browser submits the form, the router loads the token, deserializes the continuation, and calls `serial-resume`.

### 4.2 The `send-form-and-wait` Helper

This helper encapsulates the pattern:

```turmeric
;;; send-form-and-wait -- render a form page and suspend until it is submitted.
;;;
;;; Parameters:
;;;   render-fn -- a function from action URL to HTML string
;;;
;;; Returns:
;;;   The raw POST body submitted by the browser.
;;;
;;; Example:
;;;   (def body (send-form-and-wait (fn [action] (render-name-form action))))
;;;
;;; Since: Guestbook example
(defn send-form-and-wait [render-fn : (-> cstr cstr)] : cstr
  (serial-shift [k]
    (def token  (store-continuation k))
    (def action (str "/submit?k=" token))
    (def html   (render-fn action))
    (perform HttpEffect (send-html html))))
```

After `perform HttpEffect (send-html html)`, the current fiber is done -- the HTTP loop will call `serial-resume k body` the next time that token is POSTed.

### 4.3 Token Generation and Storage

`conts.tur` provides two operations:

```turmeric
;;; store-continuation -- serialize k and save to data/conts/<token>.bin.
;;;
;;; Parameters:
;;;   k -- a serial-continuation to persist
;;;
;;; Returns:
;;;   A random hex token string (64 hex characters).
;;;
;;; Since: Guestbook example
(defn store-continuation [k : (serial-continuation cstr)] : cstr
  (def token (random-hex-64))
  (def bytes (serial-cont->bytes k))
  (file-write (str "data/conts/" token ".bin") bytes)
  token)

;;; load-continuation -- deserialize a continuation from disk.
;;;
;;; Parameters:
;;;   token -- hex token returned by store-continuation
;;;
;;; Returns:
;;;   (Option (serial-continuation cstr)) -- Some(k) if found, None if missing.
;;;
;;; Since: Guestbook example
(defn load-continuation [token : cstr] : (Option (serial-continuation cstr))
  (def path (str "data/conts/" token ".bin"))
  (match (file-read path)
    (Err _) -> (None)
    (Ok bytes) ->
      (match (bytes->serial-cont bytes)
        (Err _)  -> (None)
        (Ok k)   -> (Some k))))
```

### 4.4 The Router

```turmeric
;;; dispatch -- route an HTTP request to the appropriate handler.
;;;
;;; Parameters:
;;;   req -- the incoming request
;;;
;;; Returns:
;;;   unit (side effect: sends an HTTP response)
;;;
;;; Since: Guestbook example
(defn dispatch [req : HttpRequest] : unit
  (cond
    ;; GET / -> start a new guestbook flow
    (and (cstr-eq? req.method "GET") (cstr-eq? req.path "/"))
    (run-guestbook-flow req)

    ;; GET /entries -> list all entries
    (and (cstr-eq? req.method "GET") (cstr-eq? req.path "/entries"))
    (show-entries-handler req)

    ;; POST /submit?k=TOKEN -> resume a stored continuation
    (and (cstr-eq? req.method "POST") (cstr-eq? req.path "/submit"))
    (resume-handler req)

    ;; 404 for everything else
    :else
    (perform HttpEffect
      (send-html "<h1>404 Not Found</h1>"))))

;;; resume-handler -- extract the token from the query string and resume.
;;;
;;; Since: Guestbook example
(defn resume-handler [req : HttpRequest] : unit
  (def token (parse-form-field req.query "k"))
  (match token
    (None) ->
      (perform HttpEffect (send-html "<p>Missing token.</p>"))
    (Some t) ->
      (match (load-continuation t)
        (None) ->
          (perform HttpEffect (send-html "<p>Unknown or expired token.</p>"))
        (Some k) ->
          (serial-resume k req.body))))
```

---

## Step 5: Two-Page Flow

Now wire together two `send-form-and-wait` calls for the name and message pages.

```turmeric
;;; run-guestbook-flow -- the main multi-page flow.
;;;
;;; Parameters:
;;;   req -- the initial GET / request (used only to start the flow)
;;;
;;; Returns:
;;;   unit (side effect: sends HTML pages, eventually writes an entry)
;;;
;;; Since: Guestbook example
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

### 5.1 What Happens at Runtime

Walk through the sequence for `name = "Alice"` and `message = "Hello"`:

```
Browser                 Server
  |                       |
  |-- GET / ------------>|  run-guestbook-flow starts
  |                       |  serial-reset begins
  |                       |  send-form-and-wait:
  |                       |    serial-shift captures k1
  |                       |    k1 serialized -> data/conts/abc123.bin
  |                       |    token = "abc123"
  |                       |    render-name-form called with action="/submit?k=abc123"
  |<-- HTML (name form) --|
  |                       |
  |-- POST /submit?k=abc123  body: name=Alice
  |                       |  router: load-continuation "abc123" -> k1
  |                       |  serial-resume k1 "name=Alice"
  |                       |  flow resumes: name = "Alice"
  |                       |  send-form-and-wait:
  |                       |    serial-shift captures k2
  |                       |    k2 serialized -> data/conts/def456.bin
  |                       |    render-message-form called with action="/submit?k=def456"
  |<-- HTML (msg form) ---|
  |                       |
  |-- POST /submit?k=def456  body: message=Hello
  |                       |  router: load-continuation "def456" -> k2
  |                       |  serial-resume k2 "message=Hello"
  |                       |  flow resumes: message = "Hello"
  |                       |  (more steps follow in Step 8)
```text

Each `serial-resume` re-enters the flow at exactly the point after `send-form-and-wait` returned, with all local variables (`name`, etc.) intact in the deserialized continuation.

---

## Step 6: Three-Page Preview with Back Navigation

Add a preview page between message entry and posting. The user can click "Edit" to go back.

```
    ;; Page 3: preview
    (def preview-body
      (serial-shift [k-back]
        (serial-shift [k-confirm]
          (def t-back    (store-continuation k-back))
          (def t-confirm (store-continuation k-confirm))
          (perform HttpEffect
            (send-html (render-preview
                         (html-escape name)
                         (html-escape message)
                         t-back
                         t-confirm))))))
```

### 6.1 How Back Navigation Works

Two continuations are stored on the preview page:

- `k-confirm` resumes the outer `serial-shift` -- i.e., the code *after* the preview, which writes the entry.
- `k-back` resumes the inner `serial-shift` -- i.e., re-entering the `serial-shift` that generated the message form.

When the user clicks "Edit", the browser POSTs to `/submit?k=<t-back>`. The router deserializes `k-back` and calls `serial-resume k-back ""`. This re-runs the message-form `send-form-and-wait`, presenting the form again with a fresh token.

Back navigation is completely free -- the server already serialized both continuations. No extra state tracking is needed.

### 6.2 The Preview Template

```turmeric
;;; render-preview -- render the preview page with confirm and back links.
;;;
;;; Parameters:
;;;   name      -- HTML-escaped author name
;;;   message   -- HTML-escaped message text
;;;   t-back    -- token for the "Edit" continuation
;;;   t-confirm -- token for the "Confirm" continuation
;;;
;;; Returns:
;;;   HTML string.
;;;
;;; Since: Guestbook example
(defn render-preview [name    : cstr,
                      message : cstr,
                      t-back  : cstr,
                      t-confirm : cstr] : cstr
  (str "<html><body>"
       "<h2>Preview Your Entry</h2>"
       "<p><strong>Name:</strong> " name "</p>"
       "<p><strong>Message:</strong> " message "</p>"
       "<form method='POST' action='/submit?k=" t-confirm "'>"
       "  <button type='submit' name='action' value='confirm'>"
       "    Confirm and Post</button>"
       "</form>"
       "<form method='POST' action='/submit?k=" t-back "'>"
       "  <button type='submit'>&#8592; Edit Message</button>"
       "</form>"
       "</body></html>"))
```

---

## Step 7: Persisting the Guestbook Store

### 7.1 The `GuestEntry` Struct

```turmeric
;;; GuestEntry -- a single guestbook post.
;;;
;;; Since: Guestbook example
(defstruct GuestEntry
  [name      : cstr
   message   : cstr
   posted-at : int64])  ; Unix timestamp (seconds since epoch)
```

### 7.2 The `Serializable` Instance

Entries are serialized as a `Vec cstr` (name, message, timestamp-string):

```turmeric
;;; Serializable instance for GuestEntry.
;;; Serializes as a Vec of three cstr values.
(definstance Serializable GuestEntry
  (serialize [e]
    (serialize (Vec.of [e.name e.message (int64->cstr e.posted-at)])))
  (deserialize [b]
    (match (deserialize b : (Result (Vec cstr) cstr))
      (Err msg) -> (Err msg)
      (Ok parts) ->
        (if (< (Vec.len parts) 3)
          (Err "GuestEntry: not enough fields")
          (Ok (GuestEntry
                :name      (Vec.get parts 0)
                :message   (Vec.get parts 1)
                :posted-at (cstr->int64 (Vec.get parts 2))))))))
```

### 7.3 The Store API

`store.tur` exposes three functions:

```turmeric
;;; store-load -- load all guestbook entries from data/entries.bin.
;;;
;;; Returns:
;;;   (Vec GuestEntry) -- empty Vec if the file does not exist yet.
;;;
;;; Since: Guestbook example
(defn store-load [] : (Vec GuestEntry) ...)

;;; store-append -- append a new entry and flush to disk.
;;;
;;; Parameters:
;;;   entry -- the entry to append
;;;
;;; Returns:
;;;   unit
;;;
;;; Since: Guestbook example
(defn store-append [entry : GuestEntry] : unit ...)

;;; store-all -- return a snapshot of all current entries.
;;;
;;; Returns:
;;;   (Vec GuestEntry) -- most-recent entry last.
;;;
;;; Since: Guestbook example
(defn store-all [] : (Vec GuestEntry) ...)
```

The file format is a serialized `(Vec GuestEntry)` written atomically: write to a temp file, then rename over the live file. This prevents corruption on crash.

---

## Step 8: Confirmation, Posting, and Thank-You

Complete the flow. After the preview confirmation, write the entry and show the guestbook.

```turmeric
(defn run-guestbook-flow [req : HttpRequest] : unit
  (serial-reset

    ;; Page 1: name
    (def name-body (send-form-and-wait
                     (fn [a] (render-name-form a))))
    (def name (or (parse-form-field name-body "name") "Anonymous"))

    ;; Page 2: message
    (def msg-body  (send-form-and-wait
                     (fn [a] (render-message-form a name))))
    (def message (or (parse-form-field msg-body "message") ""))

    ;; Page 3: preview (stores two continuations: k-back and k-confirm)
    (serial-shift [k-back]
      (serial-shift [k-confirm]
        (def t-back    (store-continuation k-back))
        (def t-confirm (store-continuation k-confirm))
        (perform HttpEffect
          (send-html (render-preview
                       (html-escape name)
                       (html-escape message)
                       t-back
                       t-confirm)))))

    ;; Page 4: confirmed -- write entry and show thank-you
    (store-append (GuestEntry
                    :name      (html-escape name)
                    :message   (html-escape message)
                    :posted-at (unix-now)))
    (perform HttpEffect
      (send-html (render-thankyou (store-all))))))
```

> **How confirm vs. back is handled:** The preview page has two forms pointing to two different tokens. The router dispatches entirely based on which token was POSTed. No `action` field inspection is needed in the flow itself.

### 8.1 The Thank-You Template

```turmeric
;;; render-thankyou -- render the thank-you page with the full guestbook.
;;;
;;; Parameters:
;;;   entries -- all guestbook entries to display
;;;
;;; Returns:
;;;   HTML string.
;;;
;;; Since: Guestbook example
(defn render-thankyou [entries : (Vec GuestEntry)] : cstr
  (def rows
    (Vec.map entries (fn [e]
      (str "<li><strong>" e.name "</strong>: " e.message "</li>"))))
  (str "<html><body>"
       "<h2>Thank you! Your entry has been posted.</h2>"
       "<h3>Guestbook</h3>"
       "<ul>" (cstr-join rows "") "</ul>"
       "<p><a href='/'>Add another entry</a></p>"
       "</body></html>"))
```

---

## Step 9: Security Hardening

### 9.1 HMAC Token Signing

Random tokens prevent guessing, but they do not prevent token forgery if an attacker observes a valid token. Sign each token with a server secret using HMAC-SHA256:

```turmeric
;;; sign-token -- append an HMAC-SHA256 signature to a token.
;;;
;;; Parameters:
;;;   token  -- raw hex token string
;;;   secret -- server secret bytes
;;;
;;; Returns:
;;;   "token.signature" cstr.
;;;
;;; Since: Guestbook example
(defn sign-token [token : cstr, secret : bytes] : cstr
  (def sig (hex-encode (hmac-sha256 secret (cstr->bytes token))))
  (str token "." sig))

;;; verify-token -- check the signature and return the raw token.
;;;
;;; Parameters:
;;;   signed -- "token.signature" cstr
;;;   secret -- server secret bytes
;;;
;;; Returns:
;;;   (Option cstr) -- Some(token) if valid, None if tampered or malformed.
;;;
;;; Since: Guestbook example
(defn verify-token [signed : cstr, secret : bytes] : (Option cstr)
  (def parts (cstr-split signed "."))
  (match parts
    [token sig] ->
      (def expected (hex-encode (hmac-sha256 secret (cstr->bytes token))))
      (if (cstr-eq? sig expected)
        (Some token)
        (None))
    _ -> (None)))
```

Load the secret from an environment variable at startup:

```turmeric
(def SERVER-SECRET
  (cstr->bytes (or (getenv "GUESTBOOK_SECRET") "dev-insecure-secret")))
```

Replace `store-continuation` and `load-continuation` with signed variants that call `sign-token` / `verify-token` before returning or looking up a token.

### 9.2 Token Expiry

Store a creation timestamp alongside the continuation bytes. Reject tokens older than 30 minutes:

```turmeric
(def CONT-TTL-SECONDS 1800)

;;; StoredCont -- a continuation blob with a creation timestamp.
;;; Since: Guestbook example
(defstruct StoredCont
  [bytes      : bytes
   created-at : int64])  ; unix-now at time of storage

;;; continuation-expired? -- true if the stored continuation is too old.
;;; Since: Guestbook example
(defn continuation-expired? [sc : StoredCont] : bool
  (> (- (unix-now) sc.created-at) CONT-TTL-SECONDS))
```

`load-continuation` checks `continuation-expired?` and returns `None` for stale tokens.

### 9.3 Input Sanitization

Escape `<`, `>`, `&`, and `"` before inserting user input into HTML:

```turmeric
;;; html-escape -- escape HTML special characters in a string.
;;;
;;; Parameters:
;;;   s -- raw user input
;;;
;;; Returns:
;;;   HTML-safe cstr.
;;;
;;; Example:
;;;   (html-escape "<script>") ; => "&lt;script&gt;"
;;;
;;; Since: Guestbook example
(defn html-escape [s : cstr] : cstr
  (def s1 (cstr-replace-all s "&"  "&amp;"))
  (def s2 (cstr-replace-all s1 "<"  "&lt;"))
  (def s3 (cstr-replace-all s2 ">"  "&gt;"))
  (cstr-replace-all s3 "\"" "&quot;"))
```

Call `html-escape` on every user-supplied string before embedding it in a template. The preview page and thank-you page in Step 8 already do this.

### 9.4 Single-Threaded Note

This tutorial server handles one request at a time. If a reader opens two browser tabs simultaneously, the second `GET /` starts a new flow concurrently -- but because the socket listener is single-threaded, requests are serialized at the OS level. For a multi-threaded extension, see the [threading-guide.md](threading-guide.md) and [stm-guide.md](stm-guide.md).

---

## Running the Guestbook

```bash
# First-time setup (from repo root)
just configure-examples
cmake --build build --target guestbook

# Run the server
GUESTBOOK_SECRET="my-real-secret" ./build/examples/guestbook/guestbook
# Server listens on http://localhost:8080

# Or use the Justfile recipe
just run-guestbook
```

Visit `http://localhost:8080` in a browser. Fill in your name, a message, preview the entry, and confirm. The entry appears in the thank-you page and persists in `data/entries.bin` across restarts.

---

## What to Read Next

- [web-continuations-guide.md](web-continuations-guide.md) -- Compact reference: `send-form-and-wait` pattern, continuation store contract, composing flows
- [serializable-continuations-guide.md](serializable-continuations-guide.md) -- In-depth coverage of `serial-reset`, `serial-shift`, `Serializable`, and error handling
- [effects-system-guide.md](effects-system-guide.md) -- `defeffect`, `perform`, `handle` -- the mechanics behind `HttpEffect`
- [checkpointing-guide.md](checkpointing-guide.md) -- A sibling use case: long-running business workflows that survive crashes
- [c-integration-guide.md](c-integration-guide.md) -- Deeper coverage of `extern-c` and the C FFI
