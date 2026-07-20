# `String` Adoption Audit -- stdlib

> **Status:** Batches 1-2 landed 2026-07-20 (foundational bridge, path + digest
> clusters, and the httpd server-lifetime capture fix); remaining formatter
> batches proposed.

## Progress

**Batch 2 (landed) -- previously-blocked items.**

- **digest cluster (item 4) -- UNBLOCKED + done.** The blocking bug (digest/*-hex
  inline-C used `static` *nested* C functions, rejected by a standard/clang
  compiler) is fixed: the SHA-256 / MD5 per-block transforms are hoisted to
  file-scope `defn`s (`digest/sha256-transform!`, `digest/md5-transform!`) and
  called from the four digest bodies via `__TUR_CNAME_...__`. Verified against
  FIPS/RFC vectors (fixture `tests/fixtures/digest-hex`). The owned siblings
  `digest/{sha256,md5}-string` now ship in `stdlib/digest-string.tur` (fixture
  `tests/fixtures/digest-string`). Report archived:
  `docs/archive/digest-hex-nested-static-fn.md`.
- **httpd CorsOpts capture (items 2/3) -- the genuine hazard fixed.** On analysis,
  the strongest real hazard was not the struct field type but `mw-cors-with`
  capturing borrowed origin/methods/headers strings into a **server-lifetime**
  closure -- dangling if a caller passes a computed/freed buffer. `mw-cors-with`
  (the single choke point for `mw-cors`, `mw-cors-opts` too) now owns heap copies
  of those strings via `httpd-cors-own-str` (a NULL-preserving, process-lifetime
  copy -- the correct lifetime for server-lifetime middleware state). Behavior
  preserved; the 3 CORS fixtures stay green.
- **httpd CookieOpts / CorsOpts *field types* -- deliberately NOT migrated.**
  These are by-value `:copy` structs with no destructor. Migrating the fields to
  `String` would allocate per construction with no free hook (a guaranteed leak)
  or require a fragile single-consumption free-on-serialize contract
  (double-free risk). The structs are single-consumption: `httpd-set-cookie!`
  and the CORS emit helpers copy the bytes into their own buffer *at use*, so the
  borrow is valid across the construct-then-use expression. The residual capture
  hazard -- the only place the value outlived its source -- was the middleware
  closure, now fixed above. A field-type migration is therefore net-negative and
  is intentionally left undone.

**Batch 1 (landed).**

- **Foundational owned bridge (item 1).** `stdlib/string.tur` gains
  `string/adopt-cstr` -- takes ownership of a freshly heap-allocated `cstr`
  (copies into a `String`, frees the original), the generic bridge that turns any
  "caller frees the result" `cstr` function into an owned `String` at the call
  site with no leak. Plus `int->string`, the owned counterpart of `int->str`
  (built directly via the `tur_string_from_int` runtime helper, no cstr
  intermediate; full compiled/`--interpret` parity). Backed by
  `src/runtime/tur_string.c` + `src/turi/string_native.c`. Fixture:
  `tests/fixtures/string-int`.
- **Path cluster (item 7).** New opt-in module `stdlib/path-string.tur` with
  owned siblings `path/{join,basename,dirname,extension,stem,normalize}-string`,
  each a one-line `string/adopt-cstr` wrapper over the `path/*` cstr form. Kept
  in a separate module so the lean `tur/path` (no map/typeclass dependency) stays
  lean. Compiled-path only (like `tur/path` itself, whose inline-C has no
  interpreter native). Fixture: `tests/fixtures/path-string`.

(Batch 1 originally deferred the digest cluster and the httpd items; both are
now resolved in Batch 2 above -- digest unblocked and shipped, the httpd
capture hazard fixed, and the by-value field migration consciously declined.)

**Remaining batches (proposed).** The rest of the fresh-alloc clusters --
`json/encode`, `csv/emit*`, `re/*`, `term/*`, `range*` -- follow the exact
`path-string` recipe (opt-in `*-string` sibling module, one `string/adopt-cstr`
wrapper per function). They are mechanical; batch them per module as capacity
allows, in the priority order below.

---

> **Original audit:** complete 2026-07-20.
>
> **Prerequisite:** the owned `String` type has landed --
> [docs/upcoming/v2/owned-string-type-plan.md](./owned-string-type-plan.md),
> [stdlib/string.tur](../../../stdlib/string.tur),
> [docs/guides/strings-guide.md](../../guides/strings-guide.md).
>
> **Companion audits:** [string-adoption-docs-plan.md](./string-adoption-docs-plan.md)
> (guides/tutorials). A spices audit is deferred until `../turmeric-spices/` is
> in the tree.

## What this is

The `String` type does not by itself fix any existing code. This audit sweeps
`stdlib/**` and classifies every `cstr` site that could benefit, so the
migration can land **incrementally** (this table first, then batched edits) --
**not** as a big-bang `cstr -> String` rewrite, which would pessimize the many
sites where a borrowed literal is exactly right and would churn FFI/literal
ergonomics.

Classification key:

- **should-be-String** -- returns / stores a **freshly allocated** buffer typed
  as a borrowed `cstr`. Returning owned-but-typed-as-borrowed heap memory (the
  "caller frees the result" idiom) is the exact ownership/leak hazard `String`
  removes.
- **keep-cstr** -- returns / stores a **borrow** into an argument, a struct
  field, or interned/static memory that outlives the call. The borrow is
  intentional; `String` would force a needless copy.
- **needs-judgment** -- ambiguous or a structural (non-return) hazard.

## Scope of the sweep

Every `cstr`-typed `defstruct` field in `stdlib/**` (2 structs, both in
`httpd.tur`) and every `defn ... : cstr` return (~65 functions across ~20
modules) was read and classified. Map/Set key positions: the only `cstr`-keyed
collection surface in stdlib is the `MapKey[cstr]` instance itself
(`stdlib/map.tur:391`) and doc examples -- there is no stored `Map[cstr V]` /
`Set[cstr]` field in a stdlib struct, so the collection-key hazard is a
documentation concern (see the docs audit), not a stdlib-code one.

## should-be-String (fresh allocation typed `cstr`)

### Foundational builders -- migrate first (widest leverage)

| Site | Rationale |
|---|---|
| `stdlib/str-build.tur:30 str-concat` | `malloc(la+lb+1)` + memcpy of both inputs; returns the new buffer. |
| `stdlib/str-build.tur:55 int->str` | `malloc` + snprintf; "Caller owns the buffer." |
| `stdlib/cstr.tur:59 cstr-sub` | `malloc(outlen+1)` + memcpy; docstring: "the caller frees the result." |
| `stdlib/typeclass-show.tur:179 show-concat` | private copy of `str-concat`; mallocs + returns a fresh joined buffer. |

These three primitives (`str-concat`, `int->str`, `cstr-sub`) are composed by
nearly every other should-be-String site below, so migrating them first
cascades ownership correctness outward.

### Path / formatting / serialization

| Site | Rationale |
|---|---|
| `stdlib/path.tur:24 path/join` | mallocs base+sep+segment. |
| `stdlib/path.tur:49 path/basename` | `strdup(name)`. |
| `stdlib/path.tur:69 path/dirname` | `strdup` / fresh malloc'd prefix. |
| `stdlib/path.tur:95 path/extension` | `strdup("")` / `strdup(dot)`. |
| `stdlib/path.tur:118 path/stem` | `strdup(base)` / fresh truncated copy. |
| `stdlib/path.tur:164 path/normalize` | builds result in a fresh malloc'd `out`. |
| `stdlib/term.tur:138,159,180,201,222,243,264 term/bold,dim,red,green,yellow,blue,cyan` | each `strdup(s)` (non-tty) or malloc+snprintf with escapes; every path fresh. |
| `stdlib/digest.tur:94 digest/sha256-hex` | heap 65-byte hex string. |
| `stdlib/digest.tur:236 digest/md5-hex` | heap 33-byte hex string. |
| `stdlib/range-bound.tur:211 range-fmt` | composed from `str-concat`/`int->str`. |
| `stdlib/range-bound.tur:240 range->str` | delegates to `range-fmt`. |
| `stdlib/range-bound.tur:265 bound->str` | Inclusive/Exclusive branches return fresh `bound-fmt` (Unbounded returns a static literal -- the trivially-safe case `String` subsumes). |
| `stdlib/range-bound.tur:275 bound-fmt` | `str-concat` of bracket + `int->str`. |
| `stdlib/range.tur:133 bound-show-fmt` | `malloc(32)` + snprintf. |
| `stdlib/csv.tur:271 csv/emit-row-with-delim` | malloc/realloc line accumulation. |
| `stdlib/csv.tur:326 csv/emit-row` | tail-calls `emit-row-with-delim`. |
| `stdlib/csv.tur:348 csv/emit-with-delim` | malloc/realloc whole-document accumulation. |
| `stdlib/csv.tur:404 csv/emit` | tail-calls `emit-with-delim`. |
| `stdlib/re.tur:461 re/replace` | `str-concat`/`cstr-sub` spliced buffer. |
| `stdlib/re.tur:484 re/replace-all` | accumulation via `re-replace-go`. |
| `stdlib/re.tur:502 re/wrap-paren` | two `str-concat`s (internal helper). |
| `stdlib/re.tur:508 re/union-acc` | right-fold of `str-concat` (internal helper). |
| `stdlib/re.tur:539 re/union-patterns` | returns fresh `re/union-acc` result. |
| `stdlib/json.tur:502 json/encode` | `malloc(256)` growable buffer -> `b.data`. |

### HTTP accessors that decode into a fresh buffer

| Site | Rationale |
|---|---|
| `stdlib/httpd.tur:1296 httpd-req-cookie` | match path mallocs + memcpys the decoded value (non-match returns `""`). |
| `stdlib/httpd.tur:1488 httpd-req-form` | mallocs `out` and URL-decodes into it. |
| `stdlib/schema.tur:696 schema-error-message` | `malloc(total)` + sprintf of all error lines; "Caller owns the cstr." |

## needs-judgment -- stored borrowed fields (structural hazard)

These are not `cstr` returns but **stored borrows** in a heap struct, the
strongest dangling hazard: the struct can outlive the buffer whose pointer it
holds.

| Site | Rationale |
|---|---|
| `stdlib/httpd.tur:1646 struct CorsOpts` (allow-origin/allow-methods/allow-headers/expose-headers) | **strongest.** `mw-cors-with` (`:1759`) borrows caller-supplied cstrs into a **server-lifetime** middleware closure (built once, invoked every request). A dynamically-built origin/header string dangles across all later requests. Constructor borrows, never copies. |
| `stdlib/httpd.tur:1348 struct CookieOpts` (name/value/path/domain/same-site) | `cookie` (`:1380`) / `cookie-full` (`:1394`) store incoming cstrs **by borrow**. `value`/`name` are routinely caller-computed (e.g. a session id from `str-concat`) and can outlive that buffer until `httpd-set-cookie!` serializes. |

## keep-cstr (intentional borrow -- leave as `cstr`)

Per-request / per-structure accessors that return a pointer into a
longer-lived owner, and helpers that return static literals. These are correct
and must **not** be migrated:

- `stdlib/args.tur:404,481,555 args/get-str, args/subcommand, args/error-msg` -- fields of the parsed-result structure.
- `stdlib/fs.tur:472 fs/tmpfile-path` -- `^borrow TmpFile` field.
- `stdlib/json.tur:293 json/get-string` -- the node's string slot ("owned by the node; do not free").
- `stdlib/panic.tur:54,70 panic-message, panic-file` -- borrows into the Panic payload.
- `stdlib/sym.tur:33 sym->str` -- interned/static symbol name.
- `stdlib/httpd.tur` `:968 method`, `:976 path`, `:987 version`, `:1002 body`, `:1115 header`, `:2176/2189/2202 part name/filename/content-type`, `:3130 param`, `:3518 attr`, `:3552 remote-ip`, `:3755 static-mime` -- all borrows into the conn / part / static storage (`remote-ip` docstring: "valid only for the duration of the handler").
- `stdlib/schema.tur:630,645 schema-error-path, schema-error-text` -- SchemaError fields; `:784,794 sch-tyname-, sch-want-name-` -- static type-name literals.

## Ordered migration list

Each item lands as its own small change (new owned-return variant + adopt at
callers, or field-type change + constructor copy), with fixtures. Do the
foundational primitives first so downstream formatters inherit the fix.

1. **`str-concat`, `int->str`** (`str-build.tur`), **`cstr-sub`** (`cstr.tur`) --
   foundational; almost every other should-be-String site composes them.
   Provide `String`-returning forms; keep the `cstr` forms for literal/FFI use.
2. **`CorsOpts` fields + `mw-cors-with` capture** (`httpd.tur`) -- server-lifetime
   closure holding borrowed cstrs; longest-lived holder, widest blast radius.
3. **`CookieOpts` fields (esp. `value`, `name`) + `cookie`/`cookie-full`** --
   borrowed cstrs stored in a heap struct whose `value` is routinely computed.
4. **`digest/sha256-hex`, `digest/md5-hex`** -- hex digests are canonically
   retained as cache keys / content IDs / long-lived map keys.
5. **`json/encode`, `csv/emit`, `csv/emit-row`** -- serialized output is almost
   always returned, stored, or written past the call frame.
6. **`httpd-req-form`, `httpd-req-cookie`, `schema-error-message`** -- public
   fresh-alloc-as-`cstr` accessors, easy to leak/store.
7. **`path/join`, `path/normalize`** (then the rest of `path/*`) -- computed
   paths are routinely stashed in structs and passed onward.
8. **`term/*`, `range*` formatters, `re/*`** -- lower priority; mostly feed
   `println`/`show` immediately, but owned returns remove the free-obligation.

### Migration recipe (per site)

- **Fresh-alloc return:** add a `String`-returning sibling (e.g.
  `str-concat` -> a `String`-typed builder path via `StringBuilder`), migrate
  callers that store/return the result, and keep the `cstr` form where the
  result is consumed immediately (e.g. straight into `println`). Do not silently
  change an existing signature that FFI or literal callers depend on.
- **Stored field:** change the field type to `String`, make the constructor
  `string/from-cstr` the incoming `cstr` (copy on store), and `string/release`
  it when the struct is freed. This is exactly the `MapKey[String]` ownership
  discipline applied to a plain field.
