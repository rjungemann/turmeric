# #json-str?<T> and #json-file<T> readers unimplemented

**Severity: low** (the guide already flags them as future work);
`#json-str?` emits a "not yet implemented" diagnostic. Found in the
2026-08-20 docs audit.

**Investigated 2026-08-20 and NOT landed.** The original fix direction
("implement `#json-str?<T>` as a `schema-decode`-based Result expansion;
`#json-file<T>` as read-file + decode") understates both halves. Neither is a
reader-only change. Details below so the next attempt starts from the real
blockers instead of re-deriving them.

## Root cause (of the diagnostic)

src/compiler/reader.c:1920-1928 (RD2) -- the `?` branch of
`try_read_json_str` emits the not-implemented error. `#json-str<T>` itself is
implemented immediately below it and expands to
`(:: (decode! (json/decode e)) T)`.

## Blocker 1 -- `#json-str?<T>` has no typed Result path

`HasSchema` has exactly one method, the panicking
`(defclass HasSchema [a] (decode! [node : int] : a))` (stdlib/schema.tur:1123).
There is no `decode?`. Its own docstring routes graceful handling elsewhere:
"validate first with schema-decode against the type's schema and branch on
schema-decode-ok?" -- which a reader macro cannot do, because the schema lives
*inside* each instance's `decode!` body and no `schema-of` method exposes it.

The natural expansion is therefore to wrap the panicking form:

```turmeric
(defn try-user [body : cstr] : (Result User int)
  (catch-unwind (fn [] : User (:: (decode! (json/decode body)) User))))
```

This **type-checks**, and return-type-directed dispatch does survive the
lambda (the inner ascription drives it). It then **segfaults at runtime on the
success path** -- see
[catch-unwind-aggregate-return-miscompiled.md](catch-unwind-aggregate-return-miscompiled.md).
`catch-unwind` over a thunk returning a by-value aggregate calls it through
`TUR_APPLY0`'s `int64_t (*)(void *)` cast and boxes garbage as `ok_val`, which
the consumer then casts to a struct pointer. A typed `#json-str?<T>` decodes
into a struct by definition, so it hits this every time.

Two viable routes once that is fixed:

- Add a `decode?` **default method** to `HasSchema` returning `(Result a E)`.
  `defclass` does support default method bodies
  (src/compiler/elab_typeclasses.c:1485), so this need not break the three
  in-tree `definstance HasSchema` sites.
- Or keep it reader-side and expand to the `catch-unwind` form above.

Note `catch-unwind` yields an **untyped `:int` result box** at the stdlib
surface (stdlib/panic.tur: "the `:int` result box returned by catch-unwind"),
so whichever route is taken, the reader must produce a properly typed
`(Result T E)` and not export an `:int` -- see CLAUDE.md's no-`:int`-stand-ins
rule.

## Blocker 2 -- `#json-file<T>` is not a mirror of `#json-str<T>`

`read-file` (stdlib/io.tur:229) returns `ptr<void>`, not `:cstr`, and is
`NULL` on any error. So the expansion needs, at minimum:

1. `stdlib/io.tur` added to the reader family's autoload set -- only json.tur
   and schema.tur are preloaded today (src/main.c:7161-7185), so
   `#json-file<T>` would resolve to an unknown name under `--interpret`.
2. A `ptr<void>` -> `:cstr` bridge for `json/decode`'s parameter.
3. Defined semantics for an unreadable file. `json/decode` on NULL is not a
   schema violation, so the panicking form needs its own clear panic rather
   than an unrelated crash, and a future `#json-file?<T>` needs it as an err.
4. Ownership: `read-file`'s buffer is malloc'd and the caller frees. A naive
   expansion leaks it on every call.

(3) and (4) are the parts worth designing rather than bolting on.

## Fix direction

Land in this order: the catch-unwind aggregate fix, then `#json-str?<T>`
(whichever of the two routes above), then `#json-file<T>` with its autoload,
error and ownership decisions made explicitly.

## Guides to update when fixed

- docs/guides/schema-guide.md
