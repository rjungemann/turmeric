---
title: Web Continuations Guide
category: Advanced Control Flow
description: Compact reference: `send-form-and-wait`, continuation store, routing model
---

# Web Continuations Guide

A compact reference for building multi-page web applications in Turmeric using serializable continuations. This guide assumes you understand `serial-reset` / `serial-shift` / `serial-resume` -- see [serializable-continuations-guide.md](serializable-continuations-guide.md) for that background.

For a full step-by-step walkthrough with a complete guestbook example, see [web-continuations-tutorial.md](web-continuations-tutorial.md).

---

## The `send-form-and-wait` Pattern

The canonical idiom for a page in a multi-step flow:

```turmeric no-check
(defn send-form-and-wait [render-fn : (fn [cstr] cstr)] : cstr
  (serial-shift (fn [k : serial-cont] : cstr
                  (let [token  (store-continuation k)
                        action (str "/submit?k=" token)
                        html   (render-fn action)]
                    (perform HttpEffect (send-html html))))
                0))
```

`(serial-shift handler default)` hands the rest of the enclosing `serial-reset`
to `handler` as a `serial-cont`; the handler here never resumes it, so the
request ends with the rendered form and the continuation lives on only as
the stored bytes.

**Parameters:**
- `render-fn` -- a function that takes the form `action` URL and returns an HTML string

**Returns:**
- The raw POST body that the browser submitted when the form was filled in

**When to use it:**
- Any time the flow needs to pause, show a page to the user, and resume when the user submits a form
- Works for GET-style "click to continue" pages too -- just render a form with no visible fields and a single submit button

**When not to use it:**
- For pages that do not need to resume a continuation (e.g., a static about page or a 404 error) -- just send the HTML response directly

---

## Continuation Store Contract

The continuation store (`conts.tur`) must satisfy:

```turmeric no-check
;; Store k and return a token that can be used to resume it.
(store-continuation k)  : cstr

;; Look up k by token. Returns None if missing or expired.
(load-continuation token) : (Option serial-cont)
```

```sweet-exp
;; Store k and return a token that can be used to resume it.
store-continuation(k)  : cstr

;; Look up k by token. Returns None if missing or expired.
load-continuation(token) : (Option serial-cont)
```

### Token Format

Tokens are 64 hex characters (256 bits of randomness). When HMAC signing is enabled (see [web-continuations-tutorial.md](web-continuations-tutorial.md) Step 9), tokens have the form `<64-hex-chars>.<64-hex-chars>` (raw token + HMAC-SHA256 signature).

### Storage Interface

The tutorial uses file-per-token storage in `data/conts/`:

| Operation | Path | Notes |
|-----------|------|-------|
| `store-continuation k` | Writes `data/conts/<token>.bin` | Atomic write via temp-then-rename |
| `load-continuation token` | Reads `data/conts/<token>.bin` | Returns `None` on missing file or schema mismatch |

### Eviction Policy

The tutorial implementation does not evict tokens automatically. For a production deployment, add a background sweep that deletes `.bin` files older than the TTL:

```turmeric
(defn evict-expired-conts [] : unit
  (def dir-entries (dir-list "data/conts/"))
  (Vec.for-each dir-entries
                (fn [entry]
                  (when (> (- (unix-now) (file-mtime entry)) CONT-TTL-SECONDS)
                    (file-delete entry)))))
```

```sweet-exp
defn evict-expired-conts [] : unit
  def dir-entries dir-list("data/conts/")
  Vec.for-each(dir-entries fn([entry]
    when(>(-(unix-now() file-mtime(entry)) CONT-TTL-SECONDS)
      file-delete(entry))))
```

Alternatively use the `StoredCont` struct (tutorial Step 9) to embed the creation timestamp inside the file itself, making expiry checks independent of filesystem mtime.

---

## Serializable Structs for Flow State

Any value captured inside a `serial-reset` boundary must implement `Serializable`. For custom structs, write a `definstance`:

```turmeric
;; Pattern: serialize as a Vec cstr (one field per element).
(definstance Serializable MyStruct
  (serialize [s]
    (serialize (Vec.of [s.field-a
                        s.field-b
                        (int64->cstr s.field-c)])))
  (deserialize [b]
    (match (deserialize b : (Result (Vec cstr) cstr))
      (Err msg) -> (Err msg)
      (Ok parts) ->
        (if (< (Vec.len parts) 3)
          (Err "MyStruct: not enough fields")
          (Ok (MyStruct
                :field-a (Vec.get parts 0)
                :field-b (Vec.get parts 1)
                :field-c (cstr->int64 (Vec.get parts 2))))))))
```

```sweet-exp
;; Pattern: serialize as a Vec cstr (one field per element).
definstance Serializable MyStruct
  serialize [s]
    serialize(Vec.of([s.field-a
                      s.field-b
                      int64->cstr(s.field-c)]))
  deserialize [b]
    match(deserialize(b : (Result (Vec cstr) cstr))
      (Err msg) -> Err(msg)
      (Ok parts) ->
        if({Vec.len(parts) < 3}
          Err("MyStruct: not enough fields")
          Ok((MyStruct
               :field-a Vec.get(parts 0)
               :field-b Vec.get(parts 1)
               :field-c cstr->int64(Vec.get(parts 2))))))
```

**Rules:**
- All fields must themselves implement `Serializable`
- Resource types (file handles, sockets) cannot be captured -- restructure to move them outside the `serial-reset` boundary
- The elaborator will produce a clear error if you accidentally capture a non-serializable value

---

## Routing Model

The router maps `POST /submit?k=TOKEN` to `serial-resume`:

```turmeric no-check
POST /submit?k=TOKEN
  -> parse TOKEN from query string
  -> verify HMAC signature (if signing enabled)
  -> load continuation from store -> k
  -> serial-resume k req.body
  -> flow resumes where send-form-and-wait was called
  -> eventually calls perform HttpEffect (send-html ...)
  -> HTTP response sent
```

```sweet-exp
POST /submit?k=TOKEN
  -> parse TOKEN from query string
  -> verify HMAC signature (if signing enabled)
  -> load continuation from store -> k
  -> serial-resume(k req.body)
  -> flow resumes where send-form-and-wait was called
  -> eventually calls perform HttpEffect send-html(...)
  -> HTTP response sent
```

The flow never explicitly returns a response. All output goes through `HttpEffect`. The effect handler in the main loop sends the response and loops back to `httpd-next-request`.

### Routing Table

| Method | Path | Handler |
|--------|------|---------|
| GET | `/` | Start a new flow (`run-guestbook-flow`) |
| POST | `/submit` | Resume a continuation (`resume-handler`) |
| GET | `/entries` | List all entries (no continuation needed) |
| anything else | any | 404 |

---

## Composing Flows

### Sequential Sub-Flows

A helper function that calls `send-form-and-wait` internally can be called from within any `serial-reset` boundary:

```turmeric
;; Sub-flow: collect a name and return it.
(defn collect-name [] : cstr
  (def body (send-form-and-wait (fn [a] (render-name-form a))))
  (or (parse-form-field body "name") "Anonymous"))

;; Main flow: call sub-flows in sequence.
(defn checkout-flow [req : HttpRequest] : unit
  (serial-reset
    (def name    (collect-name))
    (def address (collect-address))
    (def card    (collect-payment))
    (finalize-order name address card)))
```

```sweet-exp
;; Sub-flow: collect a name and return it.
defn collect-name [] : cstr
  def body send-form-and-wait(fn([a] render-name-form(a)))
  or(parse-form-field(body "name") "Anonymous")

;; Main flow: call sub-flows in sequence.
defn checkout-flow [req : HttpRequest] : unit
  serial-reset
    def name    collect-name()
    def address collect-address()
    def card    collect-payment()
    finalize-order(name address card)
```

Each helper is just a function -- `serial-shift` works correctly across call boundaries because the entire continuation stack within the `serial-reset` region is serialized.

### Nested Boundaries

Do not nest `serial-reset` inside another `serial-reset`. The inner boundary creates a separate continuation scope, and `serial-shift` inside it captures only up to the inner boundary. Use a single `serial-reset` per logical flow.

### Passing Continuations Across Handler Boundaries

If a continuation token is stored in one HTTP request and resumed in a completely different code path (e.g., an admin approval endpoint), the continuation still works -- the serialized bytes carry the full execution context independently of the handler that resumes them.

---

## Comparison to Racket's `send/suspend`

| Concept | Racket | Turmeric |
|---------|--------|----------|
| Boundary | `(send/suspend proc)` | `(serial-reset ...)` |
| Pause and hand URL to renderer | `send/suspend` calls `proc` with the resume URL | `(serial-shift handler 0)` hands `k` to the handler, which serializes it, builds the URL, calls `render-fn` |
| Resume URL token | Racket generates a URL using an in-memory store | `store-continuation` generates a hex token backed by files |
| Resume | Browser follows URL -> Racket resumes heap closure | Browser POSTs token -> router calls `serial-resume k body` |
| Persistence | Continuations lost on server restart (default) | Continuations persist across restarts (files on disk) |
| Type safety | Dynamic | `k : serial-cont` -- an opaque, typed handle; `bytes->serial-cont` validates before resuming |
| Back navigation | Re-using an earlier URL | Re-using an earlier token (same mechanism) |

---

## Limitations

**No true call/cc semantics.** `serial-shift` is a *delimited* shift -- it captures only up to the enclosing `serial-reset`. You cannot capture the continuation of the entire program.

**Deep-copy cost.** Serialization performs a full deep copy of all values in the continuation. Large data structures captured inside `serial-reset` will produce large `.bin` files. Move bulky data outside the boundary (e.g., store it in the guestbook store and capture only an ID).

**Token size.** A typical serialized continuation for the guestbook flow is a few kilobytes. Deeply nested flows with large intermediate values can grow larger. Monitor file sizes in `data/conts/` during development.

**No sharing.** If the same value is reachable via two different paths in a captured continuation (e.g., two bindings pointing to the same `ref`), serialization produces two independent copies. On resume, mutations to one copy do not affect the other.

**Single-threaded listener.** The tutorial HTTP listener handles one request at a time. Two simultaneous form submissions are serialized at the socket level. For concurrent flows, run multiple server processes or switch to a multi-threaded listener with per-request effect handlers.

**Schema versioning.** If you change the code between writing a token and resuming it, the frame names in the stored bytes may no longer exist in the running program; `bytes->serial-cont` then returns `Err "bytes->serial-cont: unknown frame (written by a different program?)"` rather than resuming garbage. Either drain all pending continuations before deploying, or keep a version beside each token and reject stale ones up front. See [serializable-continuations-guide.md](serializable-continuations-guide.md) -- Error Handling.
