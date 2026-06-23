---
title: Tourist Session Followups Plan
category: Planning
description: Sign cookies, add `session-rotate!` for fixation defense, ship the Valkey-backed store wrapper, and (stretch) wire CSRF synchronizer tokens on top of the existing tourist-session middleware.
---

# `tur-tourist-session` Followups -- Plan

## Context

`tourist-session` v0.1.0 shipped as part of the v0.24.0 release. It
covers the load-bearing path: cookie-backed session IDs, swappable
storage backends behind a vtable (`Store` opaque + `store-new`),
lazy-load on first `session-*` call, after-middleware `session-flush`,
in-memory and file stores. The user-facing guide is
[`docs/guides/tourist-session-guide.md`](../../guides/tourist-session-guide.md).

Three deliberate gaps are documented in that guide:

1. **The session cookie is an unsigned opaque bearer token.** A 256-bit
   random ID is server-issued and only the server's store maps it to
   data, so a tampered or guessed cookie just fails to look up -- but
   there is no integrity check telling us *which* failure mode we hit,
   and a downstream `tourist`-style component cannot rely on the cookie
   being "this server's signature."
2. **There is no `session-rotate!`.** Best-practice login flows rotate
   the session ID at the privilege boundary (fixation defense). The
   shipped workaround is `(session-destroy!)` + `(session-set! ...)`,
   which discards the data the user just keyed in.
3. **The Valkey-backed store is not shipped.** The `valkey` spice ships
   (`client-connect` / `cmd-get` / `cmd-set` / `cmd-del` / `cmd-expire`
   are live) and the session `Store` vtable accepts arbitrary
   `load/save/delete` C-ABI fn pointers, so the wrapper is the only
   missing piece -- but it would land as a new sibling spice
   (`tourist-session-valkey`), not as part of `tourist-session` itself,
   so we keep the valkey C dependency out of the core spice.

This plan closes those three gaps. A fourth, **CSRF synchronizer-token
helpers**, naturally rides along but is a stretch -- shipping a CSRF
API independently is fine if the session work moves first.

## Goals

1. **Signed cookies (S0).** Optional HMAC over the cookie value so a
   tampered or forged cookie is rejected at parse time, not at store
   lookup. Backward-compatible: unsigned cookies still parse when
   signing is off; mismatched signature is rejected the same way an
   unknown ID is.
2. **`session-rotate!` (R0).** A single API that issues a fresh session
   ID, copies the live session map to it in the store, and marks the
   old ID for deletion at flush. No data loss across the rotate. Idiom
   becomes `(session-rotate! ctx)` immediately after successful login.
3. **`tourist-session-valkey` sibling spice (V0).** A
   `valkey-store-new` constructor that returns a `Store`. Reuses the
   existing `valkey` client; nothing in the core spice changes.
4. **CSRF synchronizer-token helpers (C0; stretch).** `csrf-token`,
   `csrf-check`, and a small middleware that auto-rejects state-changing
   methods (`POST`/`PUT`/`PATCH`/`DELETE`) lacking a valid token. Stored
   in the session map under a reserved `__csrf` key.

## Non-goals

- Encrypted cookies (cookie-as-storage, à la Rails / itsdangerous
  signed-then-encrypted blobs). The shipped model is server-side
  storage with an opaque bearer token; encrypting the bearer adds
  complexity without obviating the store. Defer indefinitely.
- A `session-flash` API (one-shot message channel between requests).
  Build it on top of `session-set!` / `session-del!` after S0/R0/V0
  land; not part of this plan.
- Distributed session locking (avoiding "lost update" when two requests
  for the same session race). The single-store baseline serializes
  enough; revisit if a real consumer hits it.
- Replacing the `Store` vtable with a typeclass-instance store. The
  vtable shape is load-bearing because tourist middleware registers via
  a bare C-ABI fn pointer with no captured environment; that constraint
  has not changed.

## Design

### S0 -- Signed cookies (HMAC-SHA256)

Add an optional `signing-key : option<bytes>` field to `SessionConfig`.
When set:

- **Issue.** The cookie value sent to the client is
  `<session-id>.<hmac-sha256(signing-key, session-id) base64url>`.
- **Parse.** Before lookup, split on `.`, recompute the MAC,
  constant-time compare. Mismatch -> treat exactly like an unknown ID
  (issue a fresh session next call to `session-set!`).
- **Off.** Existing unsigned cookies parse unchanged.

**Implementation site.** `src/session/cookie.tur` already handles
parse/serialize. The hash itself can go through the existing `tur-hash`
spice (if its API is stable enough) or an inline-C SHA-256 with the
HMAC construction written in Turmeric. Prefer inline-C SHA-256 only if
adding `tur-hash` as a `:spices` dep is undesirable; HMAC is 30 lines
either way.

**Key rotation.** `signing-key` is a single key, not a key set. Real
rotation needs a "current + N previous" list (accept any, sign with
current). Reserve `:signing-keys : list<bytes>` for that, but ship S0
with the single-key form; multi-key is its own follow-up if a real
deployment asks.

**Migration.** Apps that turn signing on after running unsigned cookies
will reject every old cookie on first read. That's the desired
fail-closed behavior; document it in the guide and ship.

### R0 -- `session-rotate!`

```turmeric
;;; session-rotate! -- issue a fresh session ID; copy current data
;;; into the new ID; mark the old ID for store-delete at flush.
;;; Idempotent within a single request.
(defn session-rotate! [ctx : Ctx] : void ...)
```

Implementation steps:

1. Force-load the current session if not already loaded (so the data is
   in the per-request `SessionMap` buffer).
2. Allocate a fresh ID via `session-id-new`.
3. Stash the OLD id in a new `__rotate-from` slot on the per-request
   session state.
4. Replace `__session-id` with the new ID; mark dirty.
5. At `session-flush` (after-middleware): if `__rotate-from` is set,
   `store-save` under the new ID, `store-delete` under the old ID, then
   `Set-Cookie` the new ID. If the old store-delete fails, log and
   continue -- the worst case is an orphan record that will expire on
   its own.

**No effect when called twice.** Second call rotates again only if the
ID actually changed; otherwise no-op.

**Cookie-only rotation.** When `signing-key` is set (S0), the cookie
HMAC changes too because it's over the new ID. No extra code needed.

### V0 -- `tourist-session-valkey` sibling spice

New spice under `../turmeric-spices/spices/tourist-session-valkey/`,
parallel to `tourist-session` itself. Single module
`tourist-session-valkey/store` exporting:

```turmeric
;;; valkey-store-new -- a Valkey-backed Store.
;;;
;;; Keys are namespaced under `<prefix>:<session-id>`. TTL is set to
;;; the session's `max-age` so expired sessions vacate on their own.
;;;
;;; Parameters:
;;;   client   -- borrowed Valkey client (caller owns lifecycle)
;;;   prefix   -- key namespace, e.g. "sess"
;;;   max-age  -- session lifetime in seconds (matches SessionConfig)
;;;
;;; Returns: a Store the application installs via session-new.
(defn valkey-store-new [client  : ^borrow Client
                        prefix  : cstr
                        max-age : int] : Store ...)
```

Internals route the vtable's three slots through `cmd-get`, `cmd-set`
+ `cmd-expire`, and `cmd-del`. Serialization of the per-session map is
the same JSON shape `file-store` already uses, factored into a shared
helper module in core `tourist-session` if needed.

**Why a sibling spice, not part of core.** Core `tourist-session` has
no native deps. Pulling in `valkey` would force every consumer to link
against it, even those using the memory or file store. Sibling-spice
keeps the dep optional and matches how the other store backends would
likely be sliced (e.g. a future `tourist-session-sqlite`).

**Manifest:**

```turmeric
:spices [{:name "tourist-session" :ref "v0.1.0"}
         {:name "valkey"          :ref "v0.x.y"}]
```

### C0 -- CSRF synchronizer tokens (stretch)

```turmeric
(defn csrf-token [ctx : Ctx] : cstr)             ;; current per-session token
(defn csrf-check [ctx : Ctx submitted : cstr] : bool)
(defn csrf-mw    [opts : CsrfOpts]    : MiddlewareFn)
```

`csrf-token` lazily mints a 256-bit hex token, stashes it in the
session map under `__csrf`, and returns it. `csrf-check` constant-time
compares. `csrf-mw` is an `after`-style middleware that, for any
request whose method is in
`#{"POST" "PUT" "PATCH" "DELETE"}`, requires a matching
`X-Csrf-Token` header OR a form field whose name is configured in
`CsrfOpts`. No match -> 403 with a documented response body.

Lives in core `tourist-session` (no new deps; reuses the session
machinery). Stretch because the API is small but the user-facing
contract (which methods are protected, where the token comes from,
SPA vs. classic-form workflows) deserves its own short discussion.

## Work items

| # | Item | File(s) |
|---|------|---------|
| S0.1 | Add `signing-key : option<bytes>` to `SessionConfig`. | `tourist-session/src/session/config.tur` |
| S0.2 | Implement HMAC-SHA256 (inline-C; constant-time compare). | `tourist-session/src/session/hmac.tur` (new) |
| S0.3 | In `cookie.tur`, sign on serialize / verify on parse when key is set. Reject mismatch silently (= unknown-ID path). | `tourist-session/src/session/cookie.tur` |
| S0.4 | Tests: signed round-trip, tampered-cookie rejection, off-by-default parses unchanged, hmac empty/short-key edge. | `tourist-session/tests/cookie-signing.tur` |
| S0.5 | Guide update: add "Signed cookies" subsection with config snippet and rotation note. | `docs/guides/tourist-session-guide.md` |
| R0.1 | Add `__rotate-from` slot to per-request session state. | `tourist-session/src/session/ctx.tur` |
| R0.2 | Implement `session-rotate!`; export from `session/ctx`. | `tourist-session/src/session/ctx.tur` |
| R0.3 | `session-flush` handles the rotate path: save-new + delete-old + Set-Cookie new. | `tourist-session/src/session/mw.tur` |
| R0.4 | Tests: rotate preserves data, second rotate is no-op-after-no-change, store-delete failure logs but doesn't fail the request. | `tourist-session/tests/rotate.tur` |
| R0.5 | Guide update: add `session-rotate!` to the API table and the worked login example. | `docs/guides/tourist-session-guide.md` |
| V0.1 | Scaffold `tourist-session-valkey` sibling spice (`build.tur`, `src/tourist-session-valkey/store.tur`). | new dir |
| V0.2 | Implement `valkey-store-new` over `cmd-get` / `cmd-set` + `cmd-expire` / `cmd-del`. | `tourist-session-valkey/src/tourist-session-valkey/store.tur` |
| V0.3 | Factor the per-session JSON serializer out of `file-store.tur` into a shared helper in core. | `tourist-session/src/session/serde.tur` (new) |
| V0.4 | Tests: round-trip through a local Valkey (or `requires.valkey` skip marker). | `tourist-session-valkey/tests/store-roundtrip.tur` |
| V0.5 | Guide update: add a "Valkey store" section pointing at the sibling spice. | `docs/guides/tourist-session-guide.md` |
| C0.1 (stretch) | `csrf-token`, `csrf-check`, `csrf-mw` with `CsrfOpts`. | `tourist-session/src/session/csrf.tur` (new) |
| C0.2 (stretch) | Tests: token mints once per session, header + form-field paths, missing-token -> 403. | `tourist-session/tests/csrf.tur` |
| C0.3 (stretch) | Guide update: new "CSRF" section with SPA + classic-form snippets. | `docs/guides/tourist-session-guide.md` |

S0, R0, V0 can land in any order; they don't touch each other. C0
depends on the session machinery only, so it can also slot in
independently.

## Testing

The existing `tourist-session/tests/` harness exercises an in-process
HTTP cycle against a tourist app. New tests use the same scaffold:

- **S0.4** exercises only the cookie helper; no HTTP needed.
- **R0.4** uses the existing scaffold + an in-memory store; assertions
  on the cookie value across requests.
- **V0.4** needs a real Valkey. Match the `:cmake-deps` /
  `requires.valkey` pattern that other Valkey-touching spices use so a
  developer without Valkey installed gets a PASS-skip, not a FAIL.

## Risk

- **HMAC implementation is security-sensitive.** Constant-time
  compare must be primitives-level (no early return on first mismatched
  byte). The test suite catches functional correctness but not timing;
  document the algorithm choice (HMAC-SHA256), reuse a known-good
  implementation if `tur-hash` is acceptable, and call it out in
  `Out of scope` that we are not making formal timing claims beyond
  "constant-time compare on the MAC."
- **Rotate-during-rotate.** If the application calls `session-rotate!`
  twice in one request, the second call should not orphan an
  intermediate ID -- the rotate-from slot tracks the *original* ID
  only. The test in R0.4 must cover this.
- **Valkey store + `max-age = 0`** (session expires "at browser close")
  has no natural TTL. Use a documented default (24 h) when `max-age` is
  0 and a Valkey backend is in use. Mention in the guide.

## Out of scope

- Encrypted cookies (see Non-goals).
- Multi-key signing rotation (single key for S0; multi-key is a later
  follow-up).
- Flash-message API.
- A SQLite store wrapper (would be a third sibling spice,
  `tourist-session-sqlite`; same shape as V0, separate plan).
- Cross-process session locking.
- Formal timing-attack analysis of the HMAC compare beyond
  constant-time compare on the MAC bytes.
