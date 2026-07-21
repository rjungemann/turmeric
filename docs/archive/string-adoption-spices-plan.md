# `String` Adoption Audit -- spices

> **Status:** Executed 2026-07-21. All ordered items landed in
> `../turmeric-spices/` (tourist-session, regex, frame, json). See the
> **Execution status** section below for what shipped, the two toolchain
> blockers surfaced (and their resolutions), and the audit corrections found
> when verifying each body.
>
> **Prerequisite:** the owned `String` type has landed --
> [owned-string-type-plan.md](./owned-string-type-plan.md),
> [stdlib/string.tur](../../stdlib/string.tur),
> [strings-guide.md](../guides/strings-guide.md).
>
> **Companion audits:** [string-adoption-stdlib-plan.md](../upcoming/v2/string-adoption-stdlib-plan.md)
> (stdlib code) and [string-adoption-docs-plan.md](./string-adoption-docs-plan.md)
> (guides/tutorials). This is the third of the three adoption audits the owned-`String`
> plan spun off; it was deferred until `../turmeric-spices/` was checked out, which it
> now is (at `/Users/rjungemann/Projects/turmeric-spices`).

## What this is

`cstr` is a borrowed raw `const char*` with no length or ownership. The owned
`String` type is the type to reach for when a string outlives its source buffer
(a returned/stored value, a Map/Set key, server-lifetime state). This audit
sweeps the spice ecosystem (`../turmeric-spices/spices/**`) and classifies each
notable `cstr` site so migrations land **incrementally**, not as a big-bang
`cstr -> String` rewrite -- which would pessimize the many sites where a borrowed
literal, an FFI seam, or a consume-immediately formatter is exactly right.

Because spices are a **public API surface**, this audit weights signatures
(parameters/returns) and stored struct fields over internals.

Classification key (same as the stdlib audit):

- **should-be-String** -- returns / stores a **freshly allocated** buffer typed
  as a borrowed `cstr` (the "caller frees the result" ownership/leak hazard), or
  stores a borrowed `cstr` in a heap/long-lived struct that can outlive the
  buffer, or uses a computed `cstr` as a Map/Set key.
- **keep-cstr** -- borrows into an argument, a driver-owned row, a request
  structure, interned/static memory, or an FFI boundary. The borrow is
  intentional; `String` would force a needless copy.
- **needs-judgment** -- a by-value `:copy` field with no destructor, or a
  structural (non-return) capture hazard where the fix is *not* a field-type
  change.

## Scope of the sweep

43 spices swept. `cstr`-returning `defn`s number in the hundreds, but the large
majority are internal borrows, immediately-consumed formatters, or codegen
source-text threading. The genuine ownership hazards concentrate in **one
spice**: `tourist-session`. The rest are either low-urgency fresh-alloc
formatters or intentional borrows.

## Priority 1 -- `tourist-session` (the real hazard; mirrors the stdlib httpd finding)

`tourist-session` is signed-cookie session middleware: it mints session IDs,
signs/verifies cookies, and holds config in **server-lifetime** middleware state.
This is exactly the lifetime profile (`httpd` `CorsOpts` / `CookieOpts`) that the
stdlib audit flagged as the strongest dangling hazard -- and here it is also
**security-sensitive** (a signing key / session id dangling or torn is a
correctness *and* safety bug).

| Site | Body / lifetime | Verdict |
|---|---|---|
| `session/id.tur:33 session-id-new [] : cstr` | fresh `malloc` of a 64-hex random ID from `getentropy`; the ID is then used as a **store key** (`store-load`/`store-save`), HMAC-signed, and persisted across the request lifetime | **should-be-String** -- owned return; it is a long-lived key, the canonical `String` case |
| `session/cookie.tur:236 cookie-sign [signing-key : cstr id : cstr] : cstr` | fresh joined `key.mac` buffer ("caller frees"); stored into the outgoing `Set-Cookie` | **should-be-String** |
| `session/cookie.tur:261 cookie-unsign [...] : (Result cstr cstr)` | docstring: "ok(id) with a freshly allocated ID (caller frees)"; the recovered id is stored / used as a store key | **should-be-String** -- owned `(Result String cstr)` (or `(Result String String)`) |
| `session/config.tur:42 defstruct SessionConfig :copy [... signing-key path domain cookie-name same-site : cstr]` | by-value `:copy` struct with **no destructor**, captured into server-lifetime state (`session/mw.tur:31` closes over `cfg-signing-key`, invoked every request at `mw.tur:83` `(cookie-sign (cfg-signing-key cfgp) id)`). `signing-key` is set from a caller `key : cstr` via `with-signing-key` (`config.tur`) -- a computed/freed key **dangles across all later requests**. | **needs-judgment** -- do **not** migrate the `:copy` field types (no free hook -> guaranteed leak or fragile single-consumption contract). Instead **own a heap copy at the server-lifetime capture point**, mirroring the stdlib `httpd-cors-own-str` fix (a NULL-preserving, process-lifetime copy is the correct lifetime for server-lifetime middleware state). |
| `session/csrf.tur:37 defstruct CsrfOpts :copy [field-name : cstr ...]` | `field-name` is typically a static literal | **keep-cstr** (optional light note: a computed field name should be owned) |

The `SessionConfig` capture is the analogue of the stdlib `CorsOpts` /
`mw-cors-with` finding, and it takes the **same resolution**: fix the capture,
not the field type. See the stdlib audit's "needs-judgment -- stored borrowed
fields" section and the landed `httpd-cors-own-str` precedent.

## Priority 2 -- fresh-alloc formatters / serializers (low urgency)

Owned returns remove the free-obligation, but these mostly feed `println`/`show`
or a write immediately, so the hazard is a leak-if-stored, not a dangle. Migrate
via the mechanical `string/adopt-cstr` recipe when the owning module is touched
anyway. **Verify each body** (fresh-alloc vs borrow) before migrating -- the
signature alone does not decide it.

| Spice | Site(s) | Verdict |
|---|---|---|
| `frame` | `frame/print.tur:218 frame->str`; `frame/type.tur:60 type-name`, `:79 type-arrow-fmt` | **should-be-String** (low) -- fresh-built display strings |
| `json` | `json/src/json/encode.tur __json-obj-build : cstr` (serialized document) | **should-be-String** (low) -- same shape as the stdlib `json/encode` batch; serialized output is routinely returned/stored |
| `regex` | capture-extraction returns (`regex__capture` and siblings) | **should-be-String** -- captures are routinely stored past the match; verify the body per fn (some return a borrow into the subject) |

## keep-cstr categories (intentional borrows -- do NOT migrate)

Summarized rather than enumerated -- these are the bulk of the raw `cstr` count
and are all correct:

- **Driver row / reply / response accessors** returning a pointer into
  driver-owned memory valid until the next step: `postgres/*`, `sqlite/*`,
  `valkey/reply`, `http/*` and `httpd` response accessors, `frame`'s
  `tcol-utf8-at` (borrow into column storage). Same class as the stdlib
  `json/get-string` "owned by the node" accessors.
- **Per-request route / param / router accessors** in `tourist`
  (`tourist__*`) -- borrows into the request / router.
- **All `__`-prefixed internal helpers** (parsers, borrows, int-formatters) in
  `frame` and elsewhere.
- **Static / name-lookup returns** across `osc`, `rtmidi`, `wav`, `png`,
  `plutovg`, `linalg` labels, `ws-*`, `plot` -- interned or static text.
- **All `cstr` parameters** at FFI / call boundaries (the borrow is the whole
  point).

## Deliberately deferred -- codegen spices (biggest raw counts, lowest value)

- **`c-dsl`** (~115 `cstr` returns) and **`glsl`** (~89) are **source-text code
  generators**: every builder (`c-defstruct`, `codegen`, the GLSL emit pipeline)
  threads `cstr` fragments that are immediately concatenated or written out.
  Migrating is massive churn against a consume-immediately design with no
  ownership hazard. **keep-cstr as a whole category**; revisit only if generated
  fragments start getting **cached or stored** long-term.

## Spices with no `cstr` ownership concerns

Numeric / handle / graphics APIs with no string-return surface worth migrating:
`math`, `linalg` (numeric ops), `opengl`, `raylib`, `raygui`, `sdf-raylib`,
`signal`, `tidal`, `mesh`, `stats`, `ecs`, `ecs-raylib`, `zlib`, `rtaudio`.

## Ordered migration list

Each item lands as its own small change (owned-return sibling + adopt at callers,
or the capture-copy fix), with fixtures, in the spice's own repo
(`../turmeric-spices/`). None of these blocks v1 -- they are public-API hardening
on top of a landed `String`.

1. **`tourist-session` capture fix (highest value, security-sensitive).**
   `SessionConfig` server-lifetime signing-key / cookie-name capture: own a heap
   copy at the `with-signing-key` / middleware-construction seam (mirror
   `httpd-cors-own-str`). This closes the one real dangling hazard.
2. **`tourist-session` owned returns.** `session-id-new`, `cookie-sign`,
   `cookie-unsign` -> owned `String` / `(Result String _)`, with callers adopting
   and the store-key path taking a `String` key (`MapKey[String]`, `mk-owned? =
   1`). Sequence after (1) so the key type settles once.
3. **`regex` captures.** Verify each capture-return body; add an owned sibling
   where the capture is a fresh copy that callers store.
4. **`frame` / `json` formatters.** Owned `*-string` siblings via the
   `string/adopt-cstr` recipe -- mechanical, low urgency; do them when the module
   is open for other work.
5. **Leave** `c-dsl` / `glsl` codegen and all driver-borrow / request accessors
   as `cstr`.

### Migration recipe (per site)

Identical to the stdlib audit's recipe:

- **Fresh-alloc return:** add a `String`-returning sibling (build via
  `StringBuilder`, or wrap the existing `cstr` form in `string/adopt-cstr` --
  which takes ownership of a freshly heap-allocated `cstr`, copies into a
  `String`, and frees the original). Migrate callers that store/return the
  result; keep the `cstr` form where the result is consumed immediately (straight
  into `println`/a write). Do not silently change a signature FFI or literal
  callers depend on.
- **Server-lifetime capture (the `SessionConfig` case):** do **not** change the
  `:copy` field type. At the capture point, copy the incoming `cstr` into
  process-lifetime storage (a `httpd-cors-own-str`-style NULL-preserving copy),
  so the middleware state owns bytes that outlive any caller buffer. Behavior is
  preserved; the copy is paid once at server construction, not per request.
- **Computed Map/Set key:** type the key `String` and rely on `MapKey[String]`
  (`mk-owned? = 1`) so the collection owns and frees the key bytes -- exactly the
  dangling-`cstr`-key hazard the whole `String` effort removes.

## Execution status (2026-07-21)

All ordered items landed in `../turmeric-spices/`. Each owned-`String` return is
verified compiling **and** running under AddressSanitizer (no use-after-free /
double-free); the tourist-session suite is 10/10 green.

### Two toolchain blockers surfaced (the audit assumed a working foundation)

The audit's premise -- "the owned `String` type has landed" -- held for the
*interpreted* stdlib path but not for *compiled* spice consumption. Every spice
is AOT-compiled, which surfaced two gaps, both filed under
[docs/reported/compiled-string-return-int-conversion.md](../reported/compiled-string-return-int-conversion.md):

1. **Compiled `String` did not build under a modern clang.** A `String`-returning
   `defn` (String lowers to `void*`) emits an `int64_t` C return slot but returns
   the raw `void*` from the `tur_string_*` runtime without the `(int64_t)(intptr_t)`
   bridge -- `-Wint-conversion`, a hard error. No `String` fixture exercised the
   compiled path (all run in the interpreter), so it went untested. **Resolution:**
   restored the `-Wno-error=int-conversion` / `-Wno-error=incompatible-pointer-types`
   downgrades in `src/main.c` (the straddle is width-safe on LP64), documented as
   the still-open front; the proper codegen-bridge fix is tracked in the report.
2. **stdlib `String` cannot be pulled into a shared spice module's signatures.**
   `(load "stdlib/string.tur")` in a module that another module imports trips a
   reentrant typeclass-show load-ordering bug (`vec-show-loop` unresolved).
   **Resolution / pattern for spices:** each spice ships a tiny self-contained
   `<spice>/ownstr` module -- an ABI-identical `(defopaque String :ptr<void>)` with
   `adopt-cstr` / `to-cstr` / `release` / `str-len` extern wrappers over the same
   `tur_string_*` runtime and the `tur_string.c` autolink marker. No `(load)`, so
   no reentrancy; links cleanly. The bytes are the real refcounted runtime payload;
   only the nominal type is spice-local. When blocker 2 is fixed these collapse
   onto stdlib `String`.

### Per-item outcome

1. **`tourist-session` capture fix -- already resolved in-code; hardened.** The
   audit read `SessionConfig`'s server-lifetime signing-key as a dangling capture,
   but `session/state.tur:__cfg-new` already `strdup`s every string field into
   process-lifetime storage before serving -- the server never borrows a caller
   buffer, so the "dangles across all later requests" hazard did not exist. The
   one real gap vs. the `httpd-cors-own-str` precedent (NULL-preservation) is now
   closed: the `strdup`s are NULL-preserving.
2. **`tourist-session` owned returns -- landed.** `session-id-new -> String`,
   `cookie-sign -> String`, `cookie-unsign -> (Result String cstr)`. The
   session-state struct owns the id as a `String` (released in `sess-free`);
   store keys / cookie-build / hmac stay `cstr` borrows via `to-cstr` (they copy
   internally -- keep-cstr at those seams). The audit's "store-key path takes a
   `String` key (`MapKey[String]`)" did **not** apply: the stores are hand-rolled
   (linked list / files / redis), not `Map`s, so there is no `MapKey` to own the
   key; a `cstr` borrow into the backend's own copy is correct. Consumers updated:
   `session/csrf` (mint token), `tourist-session-valkey` store round-trip test.
3. **`regex` captures -- landed, plus a real bug fixed.** Added
   `capture-at-string` / `capture-named-string` (owned `String`). Verifying the
   bodies found a **mixed-ownership footgun**: `capture-at` / `capture-named`
   returned a *static* `""` for non-participating groups but a *malloc'd* copy
   otherwise, so a caller freeing the result per the documented "malloc'd copy"
   contract crashed on empty groups. Fixed: the empty case now malloc's too, so
   the `cstr` return is uniformly owned.
4. **`frame` / `json` formatters -- landed, with an audit correction.**
   `frame->string` (owned sibling of `frame->str`) landed. But the audit
   misclassified `frame/type.tur`'s `type-name` and `type-arrow-fmt` as
   "fresh-built display strings" -- their bodies return **static string
   literals** (a `switch` over `return "int32"`), so they are keep-cstr
   name-lookups; converting them would force a needless copy of a literal. Left
   as `cstr`. For `json`, rather than wrap the internal `__json-obj-build`
   (which would be dead code -- the `Encode` method returns `cstr` by contract),
   added a public polymorphic `encode-string [^Encode A] [x : A] : String` -- the
   owned-return entry point for any Encodable value. (Aside: a polymorphic wrapper
   over a typeclass method must be defined *after* at least one `definstance`, or
   method resolution fails with "no typeclass method found".)
5. **Codegen / driver-borrow spices -- left as `cstr`** per the audit.

## Related

- `docs/archive/owned-string-type-plan.md` -- the owned `String` this builds on,
  and the parent that spun off this audit as the third of three.
- `docs/upcoming/v2/string-adoption-stdlib-plan.md` -- the stdlib audit; the
  `tourist-session` `SessionConfig` finding is the spice mirror of its
  `httpd` `CorsOpts` / `mw-cors-with` capture finding, and takes the same
  `httpd-cors-own-str`-style resolution.
- `docs/archive/string-adoption-docs-plan.md` -- the guides/tutorials audit (executed/archived).
- `../turmeric-spices/spices/tourist-session/` -- the Priority-1 spice.
