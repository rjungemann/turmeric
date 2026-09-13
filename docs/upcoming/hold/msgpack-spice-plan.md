# MessagePack Spice Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-09-13
> **Type:** Serialization / spice (turmeric-spices)

---

## Overview

A new `spices/msgpack` spice providing MessagePack binary serialization,
deliberately architected as the binary twin of the `json` spice: the same
typeclass-driven serde surface (format-tagged `EncodeMp` / `DecodeMp`
classes with return-type-directed decode dispatch, mirroring the
`EncodeJson` / `DecodeJson` this plan renames json's classes to), the
same derive-macro family for `defstruct` products, `defdata` sums, and
`defopaque` newtypes, and the same accumulate-all-errors checked-decode
layer whose primitive vocabulary mirrors `stdlib/schema.tur`.

Where json traffics in malloc'd `cstr` fragments, msgpack is a binary
format with embedded NUL bytes, so the codec trafficks in an owned,
length-prefixed byte-buffer type instead. MessagePack is concatenative --
a container's encoding is its header followed by its elements' encodings
back-to-back -- so json's fragment-chaining derive strategy carries over
directly: each instance emits an owned fragment, and the struct/sum derive
macros concatenate fragments under a map header.

Native decode backing is **mpack** (ludocode/mpack), a small,
embedding-oriented C library whose *tree* reader API (`mpack_tree_t` /
`mpack_node_t`) mirrors yyjson's doc/value handle pair one-for-one, so the
json spice's decode shape transplants with almost no redesign. Encoding is
hand-rolled inline C (writing msgpack framing for ints/strings/floats is a
few dozen lines) so the write path has zero library dependency; mpack's
writer stays available as a fallback if the hand-rolled path grows warts.

The spice lives in `/Users/rjungemann/Projects/turmeric-spices/spices/msgpack/`
as a Tier-3 spice (`:cmake-deps` fetches mpack, static-only), modeled on
`spices/json/build.tur`.

---

## Goals / Non-Goals

### Goals (v0)

- `EncodeMp` / `DecodeMp` / `DecodeMpChecked` typeclasses -- explicitly
  format-tagged, because typeclasses resolve globally and one program may
  hold both spices at once.
- **json's classes renamed to match:** `Encode` -> `EncodeJson`, `Decode`
  -> `DecodeJson`, `DecodeChecked` -> `DecodeJsonChecked`, plus their
  methods. Neither spice owns the unqualified spelling. One breaking
  change, one minor release, no deprecation window. Mapping and footprint
  in [Naming](#naming-explicit-serde-classes); the work is phase MPJ.
- Owned byte-buffer type `Buf` (length-prefixed, `buf-len` / `buf-data` /
  `buf-free`) as the encode output and decode input carrier.
- Primitive instances matching json's set: `int`, `bool`, `float`, `cstr`
  (msgpack `str`), `(Option A)` (`none` -> `nil`), `(Cons A)` encode ->
  msgpack `array`, plus a standalone `decode-mp-list` helper (a
  `DecodeMp [Cons]` instance is impossible for the same shared-`:int`-carrier
  reason documented in `json/encode.tur`).
- `derive-msgpack T (field type)...` emitting both directions for
  `defstruct` products, plus `-encode` / `-decode` single-direction
  variants, `derive-msgpack-opaque T :as carrier`, and
  `derive-msgpack-sum` for `defdata` (externally-tagged: a 1-entry map
  `{"Ctor": {...fields...}}`, matching json's sum convention).
- Structs encode as msgpack `map` with string keys (interop-friendly
  default; readable by any msgpack consumer without a schema).
- `derive-mp-decoder T (field type)...` emitting a `DecodeMpChecked`
  instance that accumulates all violations (path / expected / got) into a
  `DecodeErrors`-style buffer whose type-name vocabulary mirrors
  `stdlib/schema.tur`, exactly as json's U3 layer does.
- Decoded strings are malloc'd copies that survive `mp-tree-free`
  (json's `Decode [cstr]` rule).
- Round-trip tests against golden byte fixtures (hand-computed msgpack
  bytes checked into the test dir) and cross-checked against the json
  spice's derive output on the same struct shapes.

### Non-Goals (v0)

- No streaming / incremental encode or decode (whole-value only; mpack's
  tree reader wants the full buffer).
- No msgpack `ext` types, including the timestamp extension.
- No `bin` <-> `str` configurability (Turmeric `cstr` maps to msgpack
  `str`; raw `bin` support is a follow-up alongside a byte-slice story).
- No compact array-encoded structs (positional, schema-required layout) --
  map-with-string-keys only in v0.
- No non-string map keys on the derive path (int-keyed maps decodable via
  the low-level node API only).
- No field renaming / skip attributes (same reserved-slot posture as
  json's derive macros -- the field-type slot is parsed, policy hooks come
  later, upstream of both spices ideally).
- No zero-copy decode (everything is copied out of the tree).

---

## API Surface

```turmeric
;; spices/msgpack/src/msgpack/encode.tur -- classes + derives live in ONE
;; module so emitted definstances pass the orphan-instance check.

;;; encode-mp -- serialize a value to an owned MessagePack fragment.
;;;
;;; Parameters:
;;;   x -- any value with an EncodeMp instance
;;;
;;; Returns:
;;;   An owned Buf holding the msgpack encoding; caller frees with
;;;   buf-free (or hands ownership onward).
;;;
;;; Example:
;;;   (let [b (encode-mp 42)]        ; => 0x2a, len 1
;;;     ...
;;;     (buf-free b))
;;;
;;; Since: Phase MP1
(defclass EncodeMp [a] (encode-mp [x] : Buf))

;;; decode-mp -- decode a tree node into a typed value.
;;;
;;; Return-type-directed: the instance is selected by ascription at the
;;; call site, e.g. (:: (decode-mp t n) (Result int cstr)).
;;;
;;; Parameters:
;;;   tree -- the parsed MpTree (owns all node memory)
;;;   node -- the MpNode to decode
;;;
;;; Returns:
;;;   (Result a cstr) -- err carries a malloc'd message on type mismatch.
;;;
;;; Since: Phase MP2
(defclass DecodeMp [a] (decode-mp [tree : MpTree node : MpNode] : (Result a cstr)))

;;; DecodeMpChecked -- accumulating validator counterpart of DecodeMp.
;;; Collects ALL field violations instead of failing fast.
;;; Since: Phase MP4
(defclass DecodeMpChecked [a]
  (decode-mp-checked [tree : MpTree node : MpNode] : (Result a DecodeErrors)))
```

```turmeric
;; spices/msgpack/src/msgpack/decode.tur -- low-level tree API over mpack.
;; Unlike json/decode.tur's bare :int handles, these are real opaques
;; (per the no-lazy-:int rule; json predates it).

(defopaque MpTree :ptr<void>)   ;; wraps mpack_tree_t*; owns all nodes
(defopaque MpNode :int)         ;; a node handle valid while its tree lives

;;; mp-parse -- parse an owned buffer into a node tree.
;;;
;;; Parameters:
;;;   buf -- msgpack bytes (borrowed; caller still owns it)
;;;
;;; Returns:
;;;   (Result MpTree cstr) -- err on malformed input.
;;;
;;; Example:
;;;   (let [t (ok-val (mp-parse b))]
;;;     ... (mp-tree-free t))
;;;
;;; Since: Phase MP2
(defn mp-parse [buf : Buf] : (Result MpTree cstr) ...)

;;; mp-tree-root -- the root node of a parsed tree.
;;; Since: Phase MP2
(defn mp-tree-root [tree : MpTree] : MpNode ...)

;;; mp-map-get -- look up a string key in a map node.
;;;
;;; Returns:
;;;   (Option MpNode) -- none when the key is absent or the node is not
;;;   a map (json's json-obj-get analog, but honest about absence).
;;;
;;; Since: Phase MP2
(defn mp-map-get [tree : MpTree node : MpNode key : cstr] : (Option MpNode) ...)

;;; mp-tree-free -- release the tree and every node it owns.
;;; Since: Phase MP2
(defn mp-tree-free [tree : MpTree] : nil ...)
```

```turmeric
;; spices/msgpack/src/msgpack/buf.tur -- owned length-prefixed byte buffer.
;; Self-contained (a spice cannot import stdlib modules); layout matches
;; stdlib/serial.tur's bytes value (8-byte LE length + data) so the two
;; interoperate at the pointer level if a caller needs to cross over.

(defopaque Buf :ptr<void>)

(defn buf-len  [b : Buf] : int ...)
(defn buf-data [b : Buf] : ptr<void> ...)
(defn buf-free [b : Buf] : nil ...)
(defn buf-concat [a : Buf b : Buf] : Buf ...)   ;; consumes both; derive plumbing
```

Derive usage mirrors json exactly:

```turmeric
(defstruct User [name : cstr age : int])
(derive-msgpack User (name cstr) (age int))

(let [b (encode-mp (User "ada" 36))]           ;; fixmap {"name":"ada","age":36}
  (let [t (ok-val (mp-parse b))]
    (let [u (ok-val (:: (decode-mp t (mp-tree-root t)) (Result User cstr)))]
      ...)))
```

---

## Naming: explicit serde classes

Turmeric typeclasses resolve **globally** -- a `defclass` is not scoped by
its module's `(export ...)` list, which is why `json/encode.tur` documents
`Encode` / `Decode` / `DecodeChecked` as deliberately *absent* from that
list. A program importing both spices therefore cannot have two classes
called `Encode`, and whichever spice claims the bare name makes the other
look like the special case.

So neither spice gets it. json's classes are renamed to be as explicit as
msgpack's, in **one breaking change, one minor release**. No deprecation
shims, no aliases, no transition window: the language has one consumer, and
a clean break costs less than the machinery to avoid it.

### The mapping

| json today (`tur-json` 0.3.0) | json after the rename | msgpack (new) |
| --- | --- | --- |
| `Encode` / `encode` (`encode.tur:50`) | `EncodeJson` / `encode-json` | `EncodeMp` / `encode-mp` |
| `Decode` / `decode` (`encode.tur:373`) | `DecodeJson` / `decode-json` | `DecodeMp` / `decode-mp` |
| `DecodeChecked` / `decode-checked` (`encode.tur:1037`) | `DecodeJsonChecked` / `decode-json-checked` | `DecodeMpChecked` / `decode-mp-checked` |
| `decode-list` (`encode.tur:513`) | `decode-json-list` | `decode-mp-list` |
| `derive-decoder` (`encode.tur:1074`) | `derive-json-decoder` | `derive-mp-decoder` |

The other derive macros keep their names: `derive-json`,
`derive-json-encode` / `-decode`, `derive-json-opaque`, `derive-json-sum`
are already json-explicit. `derive-decoder` is the odd one out -- it is
bare *and* collides head-on with msgpack's `derive-mp-decoder` -- so it
moves with the classes. Renaming it changes `json/build.tur`'s `:exports`
map for `json/encode`, a manifest edit in the same commit.

`encode-string` and `DecodeErrors` are *exported defns/types*, not globally
resolving classes, so `:refer` disambiguates them and nothing forces them to
move. A reader holding both spices open still sees two `DecodeErrors`;
rename them in the same commit or leave them, but decide once -- see the
open question below.

### Why the method names must differ too

Renaming only the classes and letting both declare a method named `encode`
is not an option, and not for style reasons. Two classes declaring the same
method name **compile with zero diagnostics and dispatch to whichever
instance registered last**, so reordering two unrelated `definstance` forms
silently flips which format a program serializes to:

```
$ tur check p.tur      # no output, exit 0
$ tur run p.tur
1042                   # EncodeMp
# swap the two definstance blocks, change nothing else:
$ tur run p.tur
json:42                # EncodeJson
```

Filed as
[same-method-name-in-two-classes-dispatches-by-declaration-order](../../reported/same-method-name-in-two-classes-dispatches-by-declaration-order.md).
Until that grows an ambiguity diagnostic, distinct method names are the
only thing keeping the two spices apart, which is why the mapping renames
methods as well as classes.

### The footprint

Small, and entirely in-repo. Across the spices tree: **29 `definstance`
heads and 19 constraint sites**, nearly all inside `json/encode.tur`'s own
macro templates. Outside json it is five sites total:

- `http/src/http/request.tur:76` -- `json-request [^Encode T ...]`
- `httpd/src/httpd/handler.tur:222` and `:233` -- `json-ok`, `json-resp`
- `http/errors/json-request-missing-encode-instance.tur` and
  `httpd/errors/req-decode-missing-decode-instance.tur` -- negative
  fixtures whose `expected.diag` quotes the class name, so their expected
  text moves too

Plus the macro templates that *emit* the old names: `derive-json`'s six
`definstance` templates inside json, and `req-decode`
(`httpd/src/httpd/handler.tur:208`), which splices a bare
`(decode __doc __root)` into its expansion. Those are not optional -- a
template emitting a name that no longer resolves breaks every caller -- so
they move in the same commit as everything else.

**Not** in scope: the three main-repo fixtures that declare their *own*
`Encode` / `Decode` classes --
`tests/fixtures/decode-bool-carrier-instance-ascription`,
`instance-method-return-carrier-bridge`, and
`typeclass-method-parameterized-result-decode`. They are self-contained
reductions of json-spice bugs, not consumers of the spice (none carries
`requires.spices`), so the rename does not reach them and they keep the
bare names their archived reports quote. Worth knowing before a repo-wide
grep makes them look like fallout.

### The edit

One mechanical pass, then fix what the compiler complains about:

```sh
# tur-json 0.3.0 -> 0.4.0.  Order matters: DecodeChecked before Decode.
# GNU sed assumed; on BSD/macOS sed the in-place flag takes an argument (-i '').
grep -rlE 'Encode|Decode|decode-checked|decode-list|derive-decoder' \
  --include='*.tur' . | xargs sed -i \
  -e 's/\bDecodeChecked\b/DecodeJsonChecked/g' \
  -e 's/\bdecode-checked\b/decode-json-checked/g' \
  -e 's/\bdecode-list\b/decode-json-list/g' \
  -e 's/\bderive-decoder\b/derive-json-decoder/g' \
  -e 's/\bEncode\b/EncodeJson/g' \
  -e 's/\bDecode\b/DecodeJson/g'
```

Verified to rewrite the json names while leaving `EncodeMp`,
`DecodeMpChecked`, `derive-mp-decoder` and `DecodeErrors` untouched (`\b`
does not match inside `EncodeMp`, and the lowercase rules are
case-sensitive). It does **not** touch bare method *calls* -- `(encode x)`,
`(decode doc val)` -- because those are indistinguishable from any other
identifier by regex; the compiler finds them for you as
"no typeclass method found" / TUR-E0015, which is the fast path here.

## Implementation Notes

- **mpack via `:cmake-deps`** (FetchContent, static, tests/tools off),
  exact tag pinned during MP0:

  ```turmeric
  :cmake-deps #map{
    "mpack" #map{:url     "https://github.com/ludocode/mpack"
                 :ref     "v1.1.1"                     ;; confirm at MP0
                 :options #map{:BUILD_SHARED_LIBS "OFF"}}
  }
  ```

  `:spices` carries the usual `:optional true` `test` dep; `:exports`
  maps `msgpack/encode`, `msgpack/decode`, `msgpack/buf`.
- **Hand-rolled encode:** int -> smallest-width int format (fixint /
  int8..int64), float -> float64, cstr -> fixstr/str8/str16/str32 by
  length, `none` -> nil (0xc0), bool -> 0xc2/0xc3, list -> array header +
  concatenated fragments, struct -> fixmap/map16 header + key/value
  fragment pairs. The derive plumbing (`__mp-chain` / `__mp-map-build`)
  walks alternating key/fragment pairs and frees each fragment after
  copying, exactly like `__json-chain` / `__json-obj-build`.
- **Float rule applies:** every float test probe uses a non-zero
  fractional part (`7.1`, `3.25`) per the strict rule -- msgpack has
  distinct int and float wire formats, so an integral probe would mask
  a coercion bug completely.
- **Decode unroll cap:** `__mp-decode-make-struct` is hand-unrolled for
  1-5 fields like json's `__decode-make-struct`; >5 fields means a
  hand-written instance. Inherited constraint, documented in the macro
  header.
- **Class-name collision -- fixed on both sides, not dodged.**
  Typeclasses resolve globally, and a program importing both json and
  msgpack must be able to derive both for the same struct. v0 does not buy
  that by letting json keep the bare name: json is renamed to
  `EncodeJson` / `DecodeJson` / `DecodeJsonChecked` with the bare
  spellings retired outright, alongside msgpack's `EncodeMp` / `DecodeMp` /
  `DecodeMpChecked`. Mapping and footprint are in
  [Naming](#naming-explicit-serde-classes); the work is phase MPJ.
  The long-term fix (shared serde classes in stdlib that both spices
  instantiate) is still blocked on the same load-reentrancy bug that
  forced json's self-contained `ownstr` mirror -- don't solve it here, but
  note that the rename makes that end-state *cheaper*: afterwards no caller
  spells a serde class without a format tag, so a future shared class is
  additive rather than a second breaking rename.
- **DecodeErrors:** reimplement json's U3 kernel (a spice cannot depend
  on another spice's private module, and cannot extend stdlib): opaque
  growable `{path, expected, got}` buffer, `decode-errors-count` /
  `-path` / `-expected` / `-got` / `-free`, `__mp-type-name` using
  schema.tur's names (string/int/float/bool/null/array/object -- map
  msgpack `map` to "object" so error text matches the json spice's on
  identical struct shapes). The type *name* `DecodeErrors` is exported,
  not global, so both spices may legally export one; whether to
  disambiguate them anyway is an open question below.

---

## Phases

### MP0 -- Research and fixtures

Pin the mpack tag and confirm its CMake static build works under
`:cmake-deps` FetchContent (the json/yyjson and tls/mbedtls recipes are
the models). Check in golden byte fixtures: a table of value -> expected
msgpack bytes covering every format family v0 touches (fixint boundaries,
int8/16/32/64 negative and positive edges, float64 with fractional
probes, fixstr/str8 boundary at 31/32 chars, nil, bools, small arrays,
fixmap boundary at 15/16 entries). Cross-generate the expected bytes with
a second implementation (e.g. `python3 -c "import msgpack..."` at
fixture-authoring time only -- the checked-in bytes are the artifact, the
suite has no python dependency).

### MPJ -- json spice: explicit class names (parallel track)

Not msgpack code, but a **prerequisite for MP3's cross-check test**: a
single program that derives both spices for one struct cannot exist while
json owns the bare `Encode`. Independent of MP0-MP2 and can land first.

One breaking commit. Run the sed pass from
[The edit](#the-edit), then fix what the compiler reports -- bare method
calls surface as "no typeclass method found" / TUR-E0015 and are the only
part the regex cannot do. Update the macro templates in json, http and
httpd (they emit the old names), `json/build.tur`'s `:exports`, and the two
`errors/` fixtures' expected diagnostic text. Bump `tur-json` to 0.4.0 and
note the rename in its CHANGELOG with the mapping table -- as a record of
what changed, not a migration guide.

Gate: `spices/json/tests/` green, plus the http and httpd suites -- they
are the only downstream consumers, and their negative fixtures assert
diagnostic text this rename changes.

### MP1 -- Buf + hand-rolled encode

`msgpack/buf` module; `EncodeMp` class and primitive instances (int,
bool, float, cstr, Option, Cons); `__mp-chain` fragment plumbing.
Golden-bytes tests green for every primitive.

### MP2 -- Decode via mpack tree

`msgpack/decode` module (`MpTree`/`MpNode` opaques, `mp-parse`,
`mp-tree-root`, `mp-map-get`, array iteration helpers, `mp-tree-free`);
`DecodeMp` class and primitive instances; `decode-mp-list`. Round-trip
tests: encode -> parse -> decode for every primitive.

### MP3 -- Derive macros

`derive-msgpack` (+ `-encode` / `-decode`), `derive-msgpack-opaque
:as carrier`, `derive-msgpack-sum`. Round-trip tests on 1-5 field
structs, a sum type, and an opaque newtype; a cross-check test that
json-derives and msgpack-derives the same struct and asserts both
round-trip to equal values. **That cross-check test needs MPJ landed** --
it is a single program holding both spices, which is exactly what the
bare-`Encode` collision makes impossible today.

### MP4 -- Checked decode (schema-vocabulary validator)

`DecodeMpChecked`, `DecodeErrors`, `derive-mp-decoder`. Tests assert
multi-field error accumulation (two wrong-typed fields -> two errors with
correct paths) and vocabulary parity with the json spice's messages.

### MP5 -- Documentation

Docstrings to the house standard on every export, README for the spice,
a paragraph in the spices developing guide's serialization section
positioning msgpack next to json (when to pick which) and stating the
format-tagged class-naming convention the two now share, and register the
spice in the top-level `:members` list.

---

## Risks / Open Questions

- **mpack vs hand-rolled decode.** If mpack's CMake fetch fights the
  `:cmake-deps` machinery (it is amalgamation-oriented), fallback is
  vendoring the two amalgamated files the way json carries its C
  helpers, or hand-rolling the reader too. Decide at MP0; the API above
  is independent of the choice.
- **`Buf` vs a future stdlib byte-slice type.** If stdlib grows a real
  owned-bytes type, `msgpack/buf` should become a mirror of it (the
  ownstr playbook). Layout-matching `serial.tur`'s bytes value now keeps
  that door open.
- **Shared serde classes.** `EncodeMp`/`DecodeMp` duplicating json's
  shape is deliberate debt; a stdlib-level pair that both spices
  instantiate is the clean end-state once the typeclass load-reentrancy
  bug is fixed. Track there, not here. MPJ does not deliver it and is not
  a substitute for it -- but it removes the thing that would otherwise
  make it break callers twice, since afterwards nobody spells a serde
  class without a format tag.
- **`DecodeErrors` / `encode-string` ambiguity.** Both are exported
  defns/types rather than global classes, so `:refer` disambiguates them
  and the rename does not force the issue. A reader with both spices open
  still sees two `DecodeErrors`. Rename to `JsonDecodeErrors` /
  `MpDecodeErrors` (and `encode-json-string`) in the same commit, or leave
  them? Decide at MPJ -- deciding later costs a second breaking release for
  no reason, since MPJ is already breaking.
- **Same-method-name dispatch is silent.** Distinct method names are what
  keeps the two spices apart, and nothing in the compiler enforces that:
  two classes sharing a method name dispatch by declaration order with no
  diagnostic
  ([report](../../reported/same-method-name-in-two-classes-dispatches-by-declaration-order.md)).
  The naming convention is the only guard. If a third serde spice ever
  lands, that report becomes load-bearing rather than informational.
- **A generic wrapper over a return-dispatch method needs one specific
  body.** `encode-string` and `decode-list` are this shape, and the msgpack
  side will want its own. A direct tail-forward fails codegen on the
  carrier/by-value return boundary; unwrap-and-rebuild works
  ([report](../../reported/generic-wrapper-tail-forwarding-a-return-dispatch-method.md),
  pinned by `tests/fixtures/generic-wrapper-over-return-dispatch-method`).
  Not a blocker, but budget for it rather than rediscovering it.

---

## See Also

- `spices/json/src/json/encode.tur` -- the architecture this mirrors;
  the three classes MPJ renames are at `:50`, `:373` and `:1037`
- `stdlib/schema.tur` -- error-vocabulary source of truth
- `stdlib/serial.tur` -- binary `Serializable` class; `Buf` layout peer
- `docs/upcoming/nng-spice-plan.md` -- companion plan; msgpack-over-nng
  typed messaging is the intended cross-spice showcase
