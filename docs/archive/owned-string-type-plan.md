---
title: An owned String type (v2)
category: stdlib / runtime -- strings
description: cstr is a borrowed raw char* with no length or ownership; as a Map/Set key it is inserted borrowed (mk-owned? = 0), so a computed or later-freed string dangles. stdlib/str.tur is an inert borrowed view (ptr<void>, Eq only). This plan adds an owned, immutable, refcounted String with the full typeclass set (Eq/Show/Hash/Ord/MapKey/Clone) that owns its bytes and is safe as a collection key/element. It does not replace cstr; it is the type you reach for when a string outlives its source. Depends on the v1 element-dispatch fix for container participation.
status: ARCHIVED (core complete) -- the owned String type + StringSlice + #s"..." reader all landed and tested; the three adoption follow-ups are now their own tracked plans (stdlib: batches 1-2 landed, formatter batches remain; docs: executed + archived; spices: executed + archived)
---

# An owned `String`

> **Status.** The `String` type itself is implemented and tested:
> `src/runtime/tur_string.c` (refcounted immutable payload + StringBuilder),
> `stdlib/string.tur` (the `String` / `StringBuilder` newtypes, core ops, and the
> `Eq`/`Ord`/`Show`/`Hash`/`Clone`/`MapKey` instances), interpreter parity via
> `src/turi/string_native.c`, and fixtures under `tests/fixtures/string-*`
> (`string-basic`, `string-map-key`, `string-builder`, `string-ord-hash`). The
> `cstr`/`str`/`String` tiering is written up in
> `docs/guides/strings-guide.md`. The deferred borrowed-view tier has also
> landed as an owned, safe slice: `StringSlice` (`stdlib/string-slice.tur`,
> runtime in `tur_string.c`, fixture `tests/fixtures/string-slice`) -- a
> zero-copy, bounds-checked view that retains its immutable parent String, so it
> supersedes the inert `str` for the safe/owned case, plus the `#s"..."`
> owned-literal reader macro (`stdlib/string-reader.tur`). Two of the three
> **adoption audits** in the
> "Follow-up" section below are now written up:
> [string-adoption-stdlib-plan.md](../upcoming/v2/string-adoption-stdlib-plan.md) and
> [string-adoption-docs-plan.md](./string-adoption-docs-plan.md) (each with a
> per-site keep/migrate classification and an ordered migration list). The
> **spices** audit is now also written up (spices are checked out at
> `../turmeric-spices/`):
> [string-adoption-spices-plan.md](./string-adoption-spices-plan.md). The docs
> and spices audits have since been **executed and archived**; the stdlib audit
> has batches 1-2 landed with formatter batches remaining.

## Why cstr is not enough

- `cstr` is a bare `const char*` (`TY_CSTR`) -- **no length, no ownership.**
  `Eq[cstr]` is `strcmp` (content) and `Show[cstr]` is identity, so at the
  scalar level cstr already behaves like a value. But it borrows.
- As a **Map/Set key**, `MapKey[cstr]` boxes the pointer with `mk-owned? = 0`
  (`stdlib/map.tur`): the collection **borrows** the caller's `char*`. Insert a
  computed string (or one that is later freed) and the map holds a dangling
  pointer. Correct today only because the common key is a static string literal
  that never moves.
- `stdlib/str.tur`'s `str` is a **borrowed** pointer+len view (`ptr<void>`),
  has **only `Eq[str]`** (no `Show`/`Hash`/`MapKey`), and being raw `ptr<void>`
  it collides with `Show [ptr<void>]`. It is not a usable first-class string.

There is no owned string that can safely be a long-lived key, a returned value
that outlives its buffer, or an element a collection is responsible for.

## Goal

An **owned, immutable, refcounted `String`** with the full typeclass set, safe
as a collection key/element, that copies (or retains) on insert so lifetimes are
sound. `cstr` stays as the borrowed/FFI/literal type; `String` is what you reach
for when ownership matters.

## Representation

A refcounted immutable heap payload, exposed as a nominal type:

```
;; conceptually: defopaque String :ptr<void>
;; heap payload: struct { int64_t rc; size_t len; char bytes[len]; }  (NUL-terminated for cheap cstr views)
```

- **Immutable + refcounted** so `Clone` is a cheap retain, structural sharing in
  a persistent Map/Set is free, and there is no torn-read hazard (mirrors the
  STM/persistent-structure "publish an immutable payload" discipline).
- **Byte buffer with a stored length**, NUL-terminated so `String->cstr` can
  hand back a borrowed `const char*` with no copy. UTF-8 semantics live in
  helpers (len is bytes; char/grapheme iteration is a separate op), matching how
  `str` framed itself.
- A `defopaque` newtype (not raw `ptr<void>`) so it is a distinct nominal type
  the typeclass machinery dispatches on -- avoiding the `str`-vs-`ptr<void>`
  collision.

## Typeclass instances

| Class | Behavior |
|---|---|
| `Eq[String]` | content: length then `memcmp` (reuse `str-eq?`-style compare) |
| `Ord[String]` | lexicographic byte compare (`memcmp` + length tiebreak) |
| `Show[String]` | the string content (the bytes) |
| `Hash[String]` | content hash over `bytes[0..len]` (reuse `tur_hamt_hash_str`, length-aware) |
| `MapKey[String]` | **`mk-owned? = 1`** -- box via the WKC2 owned-key path (`tur_hamt_box_key` / `tur_hamt_set_eq_o owned=1`) so the map **owns and frees** the key bytes; `mk-cmp` is the content comparator |
| `Clone[String]` | retain (refcount++) -- O(1) |

The `MapKey` owned path is the crux: it is exactly why `String` keys do not
dangle where `cstr` keys do. The machinery already exists (multi-word struct
keys use it); `String` slots straight in.

## Core operations (stdlib + native)

- Construct / convert: `string/from-cstr` (copy bytes into a fresh box),
  `string/to-cstr` (borrow the NUL-terminated payload; O(1)),
  `string/from-str` (copy a borrowed view).
- Query: `string/len` (bytes), `string/empty?`, `string/char-at` / byte-at,
  `string/starts-with?` / `ends-with?` / `contains?`, `string/compare`.
- Transform (return fresh immutable strings): `string/concat`,
  `string/substring` / `slice`, `string/to-upper` / `to-lower`, `string/trim`,
  `string/split` (-> `Vec[String]`), `string/join` (`Vec[String]` -> String).
- **`StringBuilder`** for efficient incremental construction (mutable growable
  byte buffer -> `builder/finish` freezes into an immutable `String`), so
  repeated `concat` is not quadratic. This is the mutable escape hatch; `String`
  itself stays immutable.

Every native-backed op needs an interpreter override in
`src/turi/collections_native.c` (or a new `string_native.c`) registered at
`turi_env_new`, so `String` works under `--interpret` and the REPL exactly as in
compiled code -- the same discipline the collections follow.

## `cstr` / `str` / `String` -- the tiering decision

Recommended three-tier model (make it explicit in the strings guide):

- **`cstr`** -- borrowed raw `const char*`. String literals, FFI boundaries,
  static text. Zero-cost, no ownership. **Stays the type of string literals.**
- **`str`** -- borrowed pointer+len *view* (zero-copy substring over an existing
  buffer). Promote it to a real nominal type with `Eq`/`Show`/`Ord` (still
  borrowed, still not a safe owning key). Optional; could be folded into
  `String` slices if the view case is rare.
- **`String`** -- owned, immutable, the safe long-lived / collection-key type.

Decision point for the maintainer: **two-tier (`cstr` + `String`)** is simpler
and probably sufficient; the borrowed-view `str` only earns its keep if
zero-copy substrings over a foreign buffer are a real need. Default
recommendation: **two-tier**, add `str` later only if a concrete zero-copy case
appears.

Literal ergonomics: keep bare `"..."` = `cstr` (borrowed, matches today). Offer
an owned-string literal form only if churn warrants -- e.g. a reader prefix
`#s"..."` or a `str->String` at use sites. Default: explicit `string/from-cstr`
/ a small macro; do not silently change literal typing.

## Dependencies and sequencing

- **v2, additive, non-blocking.** `cstr` becomes container-worthy for Eq/Show as
  soon as the v1 element-dispatch fix lands; `String` is the *ownership* upgrade,
  not a prerequisite for Eq/Show-ability. Nothing in v1 waits on this.
- **Depends on v1 element-dispatch** for `String` to participate in
  `Vec`/`Set`/`Map` element Eq/Show (same reason every element type does).
- Composes with the **Set generalization plan** (a `String` set member is a
  `MapKey[String]` owned key) and the **multi-word element plan** (both lean on
  the same WKC2 owned-box refcount infra).

## Tests (deliverable)

- `Eq`/`Ord`/`Show`/`Hash` over `String`; content equality of independently
  constructed equal strings.
- **Ownership**: a `String` key inserted into a `Map`, then the *source* freed /
  mutated, still reads back correctly (the dangling-`cstr`-key case that
  motivates this) -- and the map frees its key boxes exactly once (LSan clean).
- `StringBuilder` builds a large string in linear time; `builder/finish`
  produces an immutable `String`.
- `cstr <-> String` round-trips; `string/to-cstr` borrow is O(1).
- Compiled vs `--interpret` parity for every op.

## Follow-up: adoption audit (after `String` lands)

Introducing `String` does not by itself fix any existing code -- the value comes
from *adopting* it where a borrowed `cstr` is currently a latent
lifetime/correctness bug (a stored/returned/collection-key string that outlives
its buffer) or where owned semantics simply read better. Do **not** attempt a
big-bang `cstr -> String` sweep; that would be churn, would pessimize the many
sites where a borrowed literal is exactly right, and would break FFI/literal
ergonomics. Instead, once `String` is implemented and stable, write three
separate adoption-audit plans -- each surveys a corpus, classifies each `cstr`
site as *keep-cstr* / *should-be-String* / *needs-judgment*, and proposes the
concrete migrations:

1. **Audit stdlib.** Sweep `stdlib/**` for `cstr` in stored fields, returned
   values that outlive their source, and Map/Set key positions. Flag every
   borrowed-key site (`MapKey[cstr]`, `mk-owned? = 0`) and every API that hands
   back a `cstr` into a longer-lived structure. Output: a per-module
   keep/migrate table and an ordered migration list (start with the
   collection-key and returned-string sites, which are the real dangling
   hazards).

2. **Audit spices.** The same sweep over the spice ecosystem
   (`../turmeric-spices/` and any in-tree spice sources), where third-party-style
   API surface is most likely to leak a borrowed `cstr` across a lifetime
   boundary. Because spices are a public surface, weight the audit toward API
   signatures (parameters/returns) over internals. Output: per-spice
   keep/migrate findings plus any `String`-shaped API additions worth proposing
   upstream.

3. **Audit guides / docs.** Sweep `docs/guides/**` (and tutorials/examples) for
   places that teach `cstr` where `String` is now the better default -- string
   handling, map/set keys, "returning a string" idioms, the strings/tiering
   guidance itself. Output: doc edits and example rewrites so new code reaches
   for `String` when ownership matters, plus the canonical "cstr vs str vs
   String -- which do I use?" decision section.

Each of the three is its own plan file under `docs/upcoming/`; they can run in
parallel once `String` is stable, and each should land its migrations
incrementally (audit table first, then batched edits) rather than as one
sweeping change.

## Related

- `stdlib/str.tur` -- the inert borrowed view to supersede or promote.
- `stdlib/typeclass-hash.tur` / `stdlib/map.tur` -- `Hash[cstr]` /
  `MapKey[cstr]` (the borrowed-key baseline `String` improves on).
- `src/runtime/hamt.h` (WKC2) -- `tur_hamt_box_key` owned-key refcount path
  that `MapKey[String]` uses.
- `docs/upcoming/v1/containers-eq-show-element-dispatch-plan.md` -- element
  dispatch (prerequisite for `String` in containers).
- `docs/upcoming/v1/set-element-api-generalization-plan.md`,
  `docs/upcoming/v2/collection-multiword-element-boxing-plan.md` -- siblings
  sharing the owned-box infra.
