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
- **json's classes renamed to match, bare names deprecated:** `Encode` ->
  `EncodeJson`, `Decode` -> `DecodeJson`, `DecodeChecked` ->
  `DecodeJsonChecked`. Neither spice owns the unqualified spelling. The
  bare method names ship one release as `^deprecated` forwarding shims;
  the bare *class* names cannot be shimmed and are a hard rename. Full
  mapping, mechanics and staging in [Naming](#naming-explicit-serde-classes-deprecated-bare-names);
  the work is phase MPJ.
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

## Naming: explicit serde classes, deprecated bare names

Turmeric typeclasses resolve **globally** -- a `defclass` is not scoped by
its module's `(export ...)` list, which is why `json/encode.tur` documents
`Encode` / `Decode` / `DecodeChecked` as deliberately *absent* from that
list. A program importing both spices therefore cannot have two classes
called `Encode`, and whichever spice claims the bare name makes the other
look like the special case.

v0 fixes that on both sides at once: **neither** spice owns the bare name.
json's classes become as explicit as msgpack's, and the unqualified
spellings are deprecated on the way out.

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
map for `json/encode`, which is a manifest edit in the same commit.

`encode-string` and `DecodeErrors` are *exported defns/types*, not globally
resolving classes: an importing program disambiguates them with `:refer`,
so this change does not force them. A reader holding both spices open still
sees two `DecodeErrors`; whether to rename them too is an open question
below.

### What can carry a deprecation shim, and what cannot

The compiler's `^deprecated` attribute attaches to `defn` and `def` only
(`src/compiler/elab_fns.c:5573` and `:11208`). There is no `^deprecated`
on `defclass` and no typeclass-alias form, which splits the rename cleanly
in two.

**Method names CAN be shimmed.** A constrained generic `defn` forwarding to
the renamed method keeps `(encode x)` compiling for one release, and the
use-site warning fires from `src/compiler/elab_module.c:1666` carrying the
message text we choose -- promoted to a hard error under
`--Werror=deprecated`:

```turmeric
(defn ^deprecated "bare `encode` is deprecated; use `encode-json` (msgpack's is `encode-mp`)"
  encode [^EncodeJson A] [x : A] : cstr
  (encode-json x))
```

That is exactly the shape of the already-shipping `encode-string`
(`json/encode.tur:166`), so the encode side is low risk. Note the shim is a
plain `defn`, not a class method: unlike the class it replaces it *is*
scoped by `(export ...)`, so `encode` / `decode` / `decode-checked` must be
added to `json/encode`'s export list and to `json/build.tur`'s `:exports`
for the deprecation window, then removed again at 0.5.0.

**The decode side works too, but only one body shape compiles.**
`decode` is return-type-directed -- the instance is selected by an
ascription at the call site -- so a forwarding shim has to let that
ascription pin `A` and then re-dispatch through it. Spiked against
`tur` v0.47.0; the result is unambiguous. The obvious body **fails
codegen**:

```turmeric
;; DOES NOT COMPILE -- forwards the carrier straight out
(defn ^deprecated "use `decode-json`" decode [A] [(DecodeJson A)]
  [doc : int  val : int] : (Result A cstr)
  (decode-json doc val))
```

```
error: incompatible types when returning type 'int64_t'
       but 'tur_adt_Result__int__cstr' was expected
```

The shim's monomorphized spec declares a by-value `Result`, while the
forwarded method call yields the int64 carrier -- the mismatch family behind
`return-dispatch-ascription-result-wrapped-not-honored` and
`typeclass-method-parameterized-result-carrier-mismatch`. Adding an
ascription to the inner call does not help. **Destructure and rebuild does:**

```turmeric
;; COMPILES AND RUNS -- unwrap at A, rebuild at the shim's own result type
(defn ^deprecated "use `decode-json`" decode [A] [(DecodeJson A)]
  [doc : int  val : int] : (Result A cstr)
  (let [r (:: (decode-json doc val) (Result A cstr))]
    (if (ok? r) (ok (ok-val r)) (err (err-val r)))))
```

Verified end-to-end on `int`, `cstr`, and a `defstruct` payload (the
`derive-json` case), on both the ok and err arms, and through a
macro-spliced call site of the `req-decode` shape. So the decode shim is
**not** the risk this plan first assumed -- it is a known body shape. Two
things still to watch: the rebuild costs an unwrap/rewrap per call (it is a
deprecation shim, so that is acceptable), and it is only reachable because
`ok-val`/`err-val` are total at `A`; a class whose method returns something
other than a two-armed `Result` would need its own shape.

**Class names CANNOT be shimmed.** Every `(definstance Encode [T] ...)`
head and every `[^Encode T]` / `[(Decode A)]` constraint is a hard rename
with no transitional spelling. Across the spices tree that is 29
`definstance` heads and 19 constraint sites, nearly all of them inside
`json/encode.tur`'s own macro templates. Outside json it is five sites
total:

- `http/src/http/request.tur:76` -- `json-request [^Encode T ...]`
- `httpd/src/httpd/handler.tur:222` and `:233` -- `json-ok`, `json-resp`
- `http/errors/json-request-missing-encode-instance.tur` and
  `httpd/errors/req-decode-missing-decode-instance.tur` -- negative
  fixtures whose `expected.diag` quotes the class name, so their expected
  text moves too

All are in the same repo and move in the same commit.

**Not** in scope: the three main-repo fixtures that declare their *own*
`Encode` / `Decode` classes --
`tests/fixtures/decode-bool-carrier-instance-ascription`,
`instance-method-return-carrier-bridge`, and
`typeclass-method-parameterized-result-decode`. They are self-contained
reductions of json-spice bugs, not consumers of the spice (none carries
`requires.spices`), so the rename does not reach them and they should keep
the bare names their archived reports quote. Worth knowing before a
repo-wide grep makes them look like fallout.

### Macro-emitted call sites move FIRST

This is the trap. `req-decode` (`httpd/src/httpd/handler.tur:208`) splices
a bare `(decode __doc __root)` into its expansion, and `derive-json`'s six
`definstance` templates do the same inside json.

Measured behavior (spiked on v0.47.0): the warning is emitted **once per
macro expansion**, and every one of them carries the span of the *macro
template*, not of the call site. Three `(req-decode ...)` calls produce
three identical warnings all pointing at the same line inside
`httpd/handler.tur`. So a downstream user gets N copies of a warning about
a symbol they never typed, located in a file they do not own and cannot
edit -- unactionable and un-silenceable short of not calling the macro.

So: update every macro template in json, http and httpd to emit the new
names in the **same commit** that introduces them, before any shim reaches
a downstream caller. A shim should only ever warn about a name the warned
line actually contains.

### Closing the class-name gap: `^deprecated` on `defclass`

The "class names cannot be shimmed" limit above is a missing compiler
feature, not a law. Scoped out here because it is the difference between a
hard break and a clean deprecation for the half of this rename that has no
migration path at all.

**Why the bare names must go regardless.** The alternative -- let both
spices declare a method named `encode` and distinguish them by class -- is
not merely bad style, it is a **silent wrong answer** today: two classes
declaring the same method name compile with zero diagnostics and dispatch
to whichever instance registered last, so reordering two unrelated
`definstance` forms flips which format a program serializes to. Filed as
[same-method-name-in-two-classes-dispatches-by-declaration-order](../../reported/same-method-name-in-two-classes-dispatches-by-declaration-order.md).
That closes the "could we just keep `encode`?" question: no.

**Feature 1 -- `^deprecated` on `defclass` (small).** The attribute parse is
a copy of `defn`'s (`elab_fns.c:5573-5603`): `defclass` reads its name at
`items[1]` (`elab_typeclasses.c:1277`) and everything after is positional,
so the same "shift the index past `^deprecated` and an optional message
string" loop drops in. Storage is two fields on `TypeClass`
(`typeclass.h:86`), mirroring `Binding.is_deprecated` /
`deprecation_message` (`expr.h:299`).

Emission is the easy part, because every user-facing class-name site funnels
through one function -- `typeclass_env_lookup_typeclass`
(`typeclass.c:86`), a flat scan of one registry. Each caller already has the
same shape (`lookup; if (!tc) { error; return NULL; }`) and a span in hand,
so one helper called after each successful lookup covers all of them:

| Site | What the user wrote |
| --- | --- |
| `elab_typeclasses.c:2364` | `definstance Encode [T]` head |
| `elab_typeclasses.c:2903`, `:3041` | `definstance` constraint vectors |
| `elab_fns.c:5729` | `[^Encode A]` in a type-param vector |
| `elab_fns.c:5946` | `[(Encode A)]` constraint vector on `defn` |
| `elab_fns.c:6227` | `^Encode` in a params vector |
| `elab_fns.c:7507` | `where (Encode a)` clause |
| `elab_types.c:1460`, `:2722` | constraint inside a type annotation |

The other `typeclass_env_lookup_typeclass` callers look up a *hardcoded*
class the compiler needs (`Num` at `elab_call.c:2529`, `Drop`/`Clone` at
`elab_forms.c:319`, `Serializable` at `elab_effects.c:1257`). Those must
NOT warn -- the user did not spell the name -- which is the one thing a
blanket warning inside the lookup function would get wrong. Hence the
helper at call sites rather than in the lookup.

Reuse `TUR-D0001`'s deprecation band and `--Werror=deprecated`
(`main.c:10304`) so the class warning promotes exactly like the method one.

**Feature 2 -- a deprecated class *alias* (the one that actually helps).**
Feature 1 only lets you *warn* about `Encode`; it does not let `Encode`
mean `EncodeJson`, so `definstance Encode [T]` still has to be rewritten.
What closes the gap is a transparent alias:

```turmeric
(defclass-alias ^deprecated "use `EncodeJson`" Encode EncodeJson)
```

There is a direct precedent: `defalias` (`elab_types.c:240-296`) is exactly
this for *types* -- a transparent name-to-target table consulted at
resolution, where the alias never becomes a nominal type of its own. The
class version is simpler, because `typeclass_env_lookup_typeclass` is the
single chokepoint that `defalias` does not get to enjoy: an `alias_of`
pointer on `TypeClass`, followed after the deprecation warning, makes all
nine sites in the table above work with no per-site change.

**The catch, and it is a real one.** An alias fixes the class name but not
the method names inside an instance body. With `Encode` aliased to
`EncodeJson`, whose method is `encode-json`, this still fails:

```turmeric
(definstance Encode [User] (encode [x] ...))    ;; `encode` is not a method of EncodeJson
```

So a class alias makes *constraints* (`[^Encode T]`, `where (Encode a)`)
and instance *heads* source-compatible, but an instance body must still be
updated to the new method name -- unless a method-alias facility lands
alongside, which is a larger design. That is the honest boundary: the alias
converts most of the 19 constraint sites into a warning, and leaves the 29
`definstance` bodies a mechanical edit.

**Recommendation.** Neither feature is on the critical path -- MPJ ships
without them via the sed recipe, and the spices tree is small enough that
the edit is an afternoon. Build Feature 1 if the deprecation is meant to
reach out-of-tree consumers, since a warning is the only thing that finds
their sites for them. Treat Feature 2 as speculative until someone outside
the tree actually depends on `Encode`; an alias that leaves instance bodies
broken is a half-migration that may confuse more than the clean break does.

### Staging

`tur-json` is at 0.3.0. Land the rename as 0.4.0 and delete the shims in
0.5.0:

| Release | `EncodeJson` / `DecodeJson` | bare `encode` / `decode` methods | bare `Encode` / `Decode` classes |
| --- | --- | --- | --- |
| 0.3.0 (today) | -- | the only spelling | the only spelling |
| 0.4.0 | canonical | `^deprecated` shim; warns at each use site | **gone** -- hard rename |
| 0.5.0 | canonical | gone | gone |

Both halves of 0.4.0 go in the spice's CHANGELOG with the mapping table and
a mechanical recipe, since the class-name half gets no compiler assistance
at all:

```sh
# tur-json 0.3.0 -> 0.4.0, applied to a consumer's own sources.
# Order matters: DecodeChecked before Decode. GNU sed assumed; on BSD/macOS
# sed the in-place flag takes an argument (-i '').
grep -rlE 'Encode|Decode|decode-checked|decode-list|derive-decoder' \
  --include='*.tur' . | xargs sed -i \
  -e 's/\bDecodeChecked\b/DecodeJsonChecked/g' \
  -e 's/\bdecode-checked\b/decode-json-checked/g' \
  -e 's/\bdecode-list\b/decode-json-list/g' \
  -e 's/\bderive-decoder\b/derive-json-decoder/g' \
  -e 's/\bEncode\b/EncodeJson/g' \
  -e 's/\bDecode\b/DecodeJson/g'
```

Bare *method* calls are deliberately left alone by that recipe: the shims
carry them, and the compiler names each one so the migration can be driven
by `--Werror=deprecated` rather than by sed.

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
- **Class-name collision -- fixed on both sides, not dodged.**
  Typeclasses resolve globally, and a program importing both json and
  msgpack must be able to derive both for the same struct. v0 does not buy
  that by letting json keep the bare name: json is renamed to
  `EncodeJson` / `DecodeJson` / `DecodeJsonChecked` with the bare
  spellings deprecated, alongside msgpack's `EncodeMp` / `DecodeMp` /
  `DecodeMpChecked`. Mapping, shim mechanics and staging are in
  [Naming](#naming-explicit-serde-classes-deprecated-bare-names); the work
  is phase MPJ. The long-term fix (shared serde classes in stdlib that
  both spices instantiate) is still blocked on the same load-reentrancy
  bug that forced json's self-contained `ownstr` mirror -- don't solve it
  here, but note that the rename makes that end-state *cheaper*: once
  0.5.0 lands, no caller spells a serde class without a format tag, so a
  future shared class is additive rather than a third breaking rename.
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

Rename json's three classes and their methods per the mapping table above;
rename `decode-list` -> `decode-json-list` and `derive-decoder` ->
`derive-json-decoder`; update `json/build.tur`'s `:exports`. Update every
macro template in json, http and httpd to emit the new names in the same
commit (see *Macro-emitted call sites move FIRST*). Move the five
non-json sites listed above -- two of which are `errors/` fixtures whose
`expected.diag` quotes the class name and therefore moves with it. Add the `^deprecated` method shims -- the decode one using the verified
destructure-and-rebuild body, not a direct tail-forward -- and export them
for the deprecation window. Bump `tur-json` to 0.4.0 and write the
CHANGELOG migration note with the mapping table and the sed recipe.

Optional, and decided at MPJ rather than assumed: land `^deprecated` on
`defclass` first (scoped in the Naming section, ~9 call sites behind one
helper) so the class-name half of the break warns instead of simply
failing. Worth it only if an out-of-tree consumer exists by then; the
in-tree sites are a mechanical edit either way.

Gate: `spices/json/tests/` green under the new names, plus two new
fixtures per shimmed method -- one calling the bare name and asserting the
deprecation warning text, one under `--Werror=deprecated` asserting it is
an error. Also re-run the http and httpd suites: they are the only
downstream consumers, and the negative fixtures there assert diagnostic
text that this rename changes.

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
  make it break callers twice, since after 0.5.0 nobody spells a serde
  class without a format tag.
- ~~**Decode-side shim feasibility.**~~ **Resolved 2026-09-13** by a spike
  against v0.47.0: the shim works, but only with a destructure-and-rebuild
  body -- a direct tail-forward fails codegen on the carrier/by-value
  return boundary. Verified on `int`, `cstr` and a struct payload, both
  `Result` arms, and through a macro-spliced call site. See
  [Naming](#what-can-carry-a-deprecation-shim-and-what-cannot). What is
  left is narrower: the rebuild assumes a two-armed `Result`, so if a
  future class method returns some other shape its shim needs its own
  body.
- **Bare class names have no migration path until the compiler grows one.**
  The 0.4.0 break falls entirely on `definstance` heads and constraints.
  Two compiler features would close it -- `^deprecated` on `defclass`, and
  a deprecated class alias -- both scoped in
  [Naming](#closing-the-class-name-gap-deprecated-on-defclass). Neither is
  on MPJ's critical path, and the alias only half-closes the gap (instance
  *bodies* still need the new method names). Decide whether MPJ waits for
  Feature 1 based on whether an out-of-tree consumer exists by then.
- **`DecodeErrors` / `encode-string` ambiguity.** Both are exported
  defns/types rather than global classes, so `:refer` disambiguates them
  and MPJ does not force the issue. A reader with both spices open still
  sees two `DecodeErrors`. Rename to `JsonDecodeErrors` / `MpDecodeErrors`
  (and `encode-json-string`) inside the same 0.4.0 window, or leave them?
  Decide at MPJ -- deciding later costs the json spice a second breaking
  release.
- **Deprecation window length.** One minor (0.4.0 -> 0.5.0) assumes the
  only consumers are in-repo, which is true today: `http` and `httpd` are
  the only spices that import `json/encode`, and `ansi/color.tur` merely
  cites it in a comment. If the spice has picked up an out-of-tree
  consumer by then, hold the shims a release longer rather than
  shortening the window; `--Werror=deprecated` already gives such a
  consumer a way to find every site mechanically.

---

## See Also

- `spices/json/src/json/encode.tur` -- the architecture this mirrors;
  the three classes MPJ renames are at `:50`, `:373` and `:1037`
- `src/compiler/elab_fns.c:5573` -- `^deprecated` attribute parse;
  `src/compiler/elab_module.c:1666` -- its use-site warning;
  `--Werror=deprecated` is parsed in `src/main.c:10304`
- `stdlib/schema.tur` -- error-vocabulary source of truth
- `stdlib/serial.tur` -- binary `Serializable` class; `Buf` layout peer
- `docs/upcoming/nng-spice-plan.md` -- companion plan; msgpack-over-nng
  typed messaging is the intended cross-spice showcase
