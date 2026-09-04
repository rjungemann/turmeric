# #json-str?<T> and #json-file<T> readers unimplemented

**Severity: low** (RESOLVED 2026-09-02; the guide already flagged them as future work);
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


## Partial resolution (2026-08-21): `#json-str?<T>` landed

`#json-str?<T>(expr)` is implemented and expands to the panicking form behind a
catch boundary -- the report's second route, taken after its blocker cleared:

```
#json-str?<T>(e)  ==>  (:: (catch-unwind (fn [] : T (:: (decode! (json/decode e)) T)))
                          (Result T int))
```

Blocker 1 is gone in two steps, and the report was right that neither half was
reader work:

- `catch-unwind-aggregate-return-miscompiled` is fixed (archived), so a thunk
  returning a struct no longer boxes its return register as `ok_val`. A typed
  decode lands in a struct by definition, which is why every SUCCESS hit it.
- **A schema violation was an `abort()`, not a panic**, in both engines --
  `schema-decode-abort` in `stdlib/schema.tur` and `native_schema_decode_abort`
  in `src/turi/interpreter_natives.c`. Nothing catchable was ever raised, so
  even a correct catch-unwind expansion could not have recovered. Both now
  `panic` (turi's through `turi_runtime_panic`), printing the failing paths
  first so the diagnostic detail survives on the recovered path too. With no
  catch boundary in scope a `schema-decode!` failure still prints and dies, so
  the `!` form's contract is unchanged.

The `decode?` default-method route was NOT taken and is now moot for this
purpose: it would have needed a second, parallel failure path through every
`HasSchema` instance, where the catch boundary reuses the one that exists.

Pinned by `tests/fixtures/schema-reader-json-str-result/`. That fixture carries
`requires.compiled`: the interpreter recovers correctly (its ERR path passes)
but hands back the struct HANDLE on the OK path -- filed as
[turi-catch-unwind-aggregate-payload](turi-catch-unwind-aggregate-payload.md),
which is now the one thing between `--interpret` and a working `#json-str?<T>`.

### Still open: `#json-file<T>`

Blocker 2 is untouched -- `read-file` returns `ptr<void>` (NULL on any error),
so the expansion still needs the autoload, the `ptr<void>` -> `cstr` bridge, a
defined answer for an unreadable file (a panic of its own for the `!` form, an
err for a future `?` form), and an owner for the malloc'd buffer. Those are the
parts worth designing rather than bolting on, and nothing above decides them.

`docs/guides/schema-guide.md` documents the `?` form, including the two
consequences of the catch-boundary design: violation detail still goes to
stderr on the recovered path, and the err payload is the panic handle carried
as `:int`.

## Resolution (2026-09-02): `#json-file<T>` and `#json-file?<T>` landed

Blocker 2's four parts, decided rather than bolted on:

1. **No autoload change and no `ptr<void` bridge.** The expansion does not go
   through `read-file` at all. `json/decode-file!` (stdlib/json.tur, already
   in the reader family's preload) reads the file into a private buffer,
   parses it with the same C parser `json/decode` uses, frees the buffer and
   returns the nodes -- a `:cstr` in, a node out.
2. **An unreadable file is its own panic**, `json/decode-file!: cannot read
   <path>`, raised through `tur_panic` (compiled) / `turi_runtime_panic`
   (interpreter), so it is catchable and distinct from a schema violation.
3. **Ownership:** the buffer never leaves `json/decode-file!`; the caller
   owns the node tree exactly as after `json/decode`.
4. **The `?` form** reuses the `#json-str?` catch boundary unchanged --
   `try_read_json_str` (src/compiler/reader.c) now parses one grammar for
   both prefixes and swaps the decoder name, so "no such file" and "wrong
   shape" are both `(err? r)`.

The interpreter has a matching native (`native_json_decode_file`), so
`--interpret` agrees with the compiled path. Pinned by
`tests/fixtures/schema-reader-json-file` (the `!` form, the `?` form on a
good file, on a file that fails the schema, and on a missing file). Guide
section added under `#json-str`.
