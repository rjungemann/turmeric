# `String` Adoption Audit -- docs / guides

> **Status:** Executed 2026-07-21 -- all 7 ordered edits landed. See the
> **Execution status** section below.
>
> **Prerequisite:** the owned `String` type has landed --
> [owned-string-type-plan.md](./owned-string-type-plan.md),
> [stdlib/string.tur](../../stdlib/string.tur),
> [strings-guide.md](../guides/strings-guide.md).
>
> **Companion audits:** [string-adoption-stdlib-plan.md](../upcoming/v2/string-adoption-stdlib-plan.md)
> (stdlib code; batches 1-2 landed, formatter batches remain) and
> [string-adoption-spices-plan.md](./string-adoption-spices-plan.md) (spices;
> executed).

## What this is

Before `String` existed the guides necessarily taught `cstr` even for cases --
returning a computed string, storing a string in a field, string map/set keys --
where an owned type is now the better default. This audit sweeps
`docs/guides/**` (and tutorials/examples) and classifies each site, so example
rewrites and cross-links land **incrementally**, not as one pass.

Classification key:

- **keep-cstr** -- correctly teaches `cstr` as the literal / FFI / borrow /
  parameter type. Leave as-is.
- **mention-String** -- add a one-line note or an owned-return alternative and a
  cross-link to `strings-guide.md`; no full rewrite.
- **rewrite-to-String** -- the example currently teaches the *wrong default*
  (returns or stores a computed string as `cstr`); rewrite it to `String`.

All string-heavy guides below share one gap: none currently link to the new
`strings-guide.md` (only `owned-string-type-plan.md` does). Every edit should
add that cross-link.

## Do first -- index gap

- **docs/guides/README.md** -- the guides index/TOC has **no entry** for
  `strings-guide.md` (confirmed: not in the sectioned list, not in the "By
  topic" list, no "Strings" heading). Add it under **Language Basics** and the
  "Language Basics" topic line (~233), described as the `cstr` vs `str` vs
  `String` tiering guide.

## Priority 1 -- returning / building computed strings

### docs/guides/web-continuations-tutorial.md (biggest offender; no strings-guide link)

| Line | Current | Verdict |
|---|---|---|
| 1430 `html-escape [s : cstr] : cstr` | returns fresh string from chained `cstr-replace-all` (and leaks `s1/s2/s3`) | **rewrite-to-String** -- owned return; intermediates become O(1) transforms |
| 548 `percent-decode [s : cstr] : cstr` | builds + returns a decoded string | **rewrite-to-String** (or mention) |
| 445 `render-name-form`, 989-992 `render-preview`, 1249 `render-thankyou` | return computed HTML | **mention-String** -- owned return removes caller-must-free |
| 493 `parse-form-field ... : (Option cstr)` | returns a slice/copy of body outliving the call | **mention-String** (owned `Option String`) |
| 1302 `sign-token : cstr`, 1316 `verify-token : (Option cstr)` | return computed tokens | **mention-String** |
| 236, 299-312 `Request`/response structs `[method path query body : cstr]` | borrowed views handed in by the host at the FFI seam | **keep-cstr** (problem is only when copied out to persist -- see GuestEntry) |

### docs/guides/web-stack-guide.md

| Line | Current | Verdict |
|---|---|---|
| 201 `greet [name : cstr] : cstr` | builds via `render`; guide says (219) "The returned string is heap-allocated; the caller must free it" | **rewrite-to-String / mention-String** -- the manual-free footgun is exactly what `String` removes |
| 530 `render-view [name : cstr env : int] : cstr` | returns `render` output past `env-free` | **mention-String** |

### docs/guides/performance-guide.md

- **299-357 "String and text processing"** -- the primary string-building
  section teaches `str` / `str/builder` and `join` returns `:str` (a *borrowed
  view*) even though `builder/finish` produced fresh bytes; never mentions owned
  `String`. **mention-String** (point `builder/finish -> String` as the owned
  result) and cross-link.

### docs/guides/tur-logic-guide.md

- **390 `reify-term [t subs] : cstr`** -- returns strings assembled with
  `str-concat` / `int->cstr` recursively, past their source.
  **mention-String / rewrite-to-String**.

### docs/guides/cloudflare-deployment-guide.md

- **63 `http-ok`, 314 `handle-request`, 430 `greet`, 433 `tur-handle`** -- return
  computed `cstr`. **mention-String** with a caveat: the outermost handler hands
  the string back across a WASM/host boundary where `cstr` is required
  (**keep-cstr** at that seam); the internal builders (e.g. `greet` assembling
  `"Hello, name"`) are the owned-string cases.

## Priority 2 -- structs storing a `cstr` field

| Site | Current | Verdict |
|---|---|---|
| `web-continuations-tutorial.md:1047 GuestEntry [name message : cstr posted-at : int64]` | stored in a `Vec`, serialized, outlives the request; `name`/`message` are parsed form input | **rewrite-to-String** |
| `schema-guide.md:258 defstruct User :copy [name : cstr age : int]` | `name` populated by `schema-decode!` from a JSON node; should outlive the node | **mention-String / rewrite-to-String** |
| `effects-vs-monads.md:88 defstruct Cfg-Error [what : cstr where : cstr]` | carried past its source via `Throw`; dangles if computed | **mention-String** |
| `serializable-continuations-guide.md:174/181 schema-id : cstr` | stored + serialized ("stable hash of frame chain shape") | **mention-String** (keep-cstr only if always a static literal) |
| `structs-guide.md:186/211/276/537 Person/MyStruct [name : cstr ...]` | canonical examples with *literal* names -- borrow is fine | **keep-cstr**, optional light note "a field that must own a computed/stored string should be `String`" + cross-link |

## Priority 3 -- Map/Set key guidance / examples

| Site | Current | Verdict |
|---|---|---|
| `data-literals-guide.md:218/225 #set{}:cstr` (empty `Set[cstr]`) | teaches the typed-empty-literal `:T` suffix; `cstr` incidental | **mention-String** -- a set of *computed* keys is the dangling hazard the new guide warns about; use `Set[String]` in the example or add a note. Low priority |
| `hamt-guide.md:46,99` | low-level HAMT keys are hashes; "the caller owns those lifetimes" | **keep** -- but cross-link `strings-guide.md` for the owned-key story at the typed `Map`/`Set` layer. Low priority |
| `json-guide.md` object keys via `json/object-put` | JSON owns them internally | **keep-cstr** |

## Explicitly NOT flagged (correct `cstr`)

Literal / FFI / borrow / parameter uses stay `cstr`:

- `symbols-guide.md:81 label [tag : Sym] : cstr` -- `sym->str` returns the interned static pointer.
- All `path`/`sql`/`uri`/`msg : cstr` **parameters**, `extern-c` / `defeffect` /
  websocket / db-handle signatures (typing-handles, websocket-guide,
  dynamic-vars, datalog, httpd-middleware, substructural, uniqueness, snake-game
  `Draw-Text`, `httpd-send-response`) -- borrowed at the call boundary.
- `show [x] : cstr`, `describe`, `serialize` examples (union-intersection,
  existential, gadts, `derive-show`) -- the `Show`/dispatch method type is `cstr`
  by design.

## Ordered edit list

1. **README.md** -- add the missing `strings-guide.md` index + topic entry.
2. **web-continuations-tutorial.md** -- `html-escape` (1430) and `GuestEntry`
   (1047): the clearest return-computed-string and store-computed-string
   hazards; rewrite to `String`, link the guide.
3. **web-stack-guide.md:201/530** -- `greet`/`render-view` return heap `cstr`
   with an explicit "caller must free" note; textbook `String` case.
4. **performance-guide.md:299-357** -- the string-building section must mention
   owned `String` / `builder/finish` and cross-link.
5. **schema-guide.md:258**, **effects-vs-monads.md:88** -- struct fields holding
   decoded/boundary strings; mention `String`.
6. **tur-logic-guide.md:390** -- `reify-term` returns `str-concat` output;
   mention/rewrite.
7. Low priority: **data-literals-guide.md** set-key note, **hamt-guide.md** /
   **structs-guide.md** cross-links.

Every item in 2-7 also adds the missing cross-link to
[strings-guide.md](../guides/strings-guide.md).

## Execution status (2026-07-21)

All 7 ordered edits landed and are verified in-tree:

1. **README.md** -- `strings-guide.md` now appears in the guides index and in
   the "Language Basics" topic line.
2. **web-continuations-tutorial.md** -- `html-escape` returns owned `String`;
   `GuestEntry` stores `name`/`message` as `String` (the `Serializable`
   instance borrows via `string/to-cstr` on the wire and re-owns via
   `string/from-cstr` on decode). Cross-linked.
3. **web-stack-guide.md** -- `greet` returns `String`; the "caller must free"
   note reframed as the footgun owned `String` removes. Cross-linked.
4. **performance-guide.md** -- the string-building section points
   `builder/finish -> String` as the owned result. Cross-linked.
5. **schema-guide.md**, **effects-vs-monads.md** -- decoded/boundary struct
   fields note owned `String`. Cross-linked.
6. **tur-logic-guide.md** -- `reify-term` string-assembly noted. Cross-linked.
7. Low-priority cross-links landed in **data-literals-guide.md**,
   **hamt-guide.md**, **structs-guide.md**, **serializable-continuations-guide.md**,
   and **cloudflare-deployment-guide.md** (12 guides reference `strings-guide.md`
   in total).
