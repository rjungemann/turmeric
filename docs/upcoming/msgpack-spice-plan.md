# MessagePack Spice Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-07-26
> **Type:** Serialization / spice (turmeric-spices)

---

## Overview

A new `spices/msgpack` spice providing MessagePack binary serialization,
deliberately architected as the binary twin of the `json` spice: the same
typeclass-driven serde surface (`Encode`/`Decode`-style classes with
return-type-directed decode dispatch), the same derive-macro family for
`defstruct` products, `defdata` sums, and `defopaque` newtypes, and the
same accumulate-all-errors checked-decode layer whose primitive vocabulary
mirrors `stdlib/schema.tur`.

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

- `EncodeMp` / `DecodeMp` / `DecodeMpChecked` typeclasses (named to avoid
  colliding with json's globally-resolving `Encode`/`Decode` classes).
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
- **Class-name collision:** typeclasses resolve globally, and a program
  importing both json and msgpack must be able to derive both for the
  same struct. Hence `EncodeMp`/`DecodeMp`, not a second `Encode`. The
  long-term fix (shared serde classes in stdlib that both spices
  instantiate) is blocked on the same load-reentrancy bug that forced
  json's self-contained `ownstr` mirror; note it, don't solve it here.
- **DecodeErrors:** reimplement json's U3 kernel (a spice cannot depend
  on another spice's private module, and cannot extend stdlib): opaque
  growable `{path, expected, got}` buffer, `decode-errors-count` /
  `-path` / `-expected` / `-got` / `-free`, `__mp-type-name` using
  schema.tur's names (string/int/float/bool/null/array/object -- map
  msgpack `map` to "object" so error text matches the json spice's on
  identical struct shapes).

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
round-trip to equal values.

### MP4 -- Checked decode (schema-vocabulary validator)

`DecodeMpChecked`, `DecodeErrors`, `derive-mp-decoder`. Tests assert
multi-field error accumulation (two wrong-typed fields -> two errors with
correct paths) and vocabulary parity with the json spice's messages.

### MP5 -- Documentation

Docstrings to the house standard on every export, README for the spice,
a paragraph in the spices developing guide's serialization section
positioning msgpack next to json (when to pick which), and register the
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
  shape is deliberate debt; a stdlib-level `Encode`/`Decode` pair that
  both spices instantiate is the clean end-state once the typeclass
  load-reentrancy bug is fixed. Track there, not here.

---

## See Also

- `spices/json/src/json/encode.tur` -- the architecture this mirrors
- `stdlib/schema.tur` -- error-vocabulary source of truth
- `stdlib/serial.tur` -- binary `Serializable` class; `Buf` layout peer
- `docs/upcoming/nng-spice-plan.md` -- companion plan; msgpack-over-nng
  typed messaging is the intended cross-spice showcase
