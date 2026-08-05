# `derive-show` / `derive-debug` / `derive-display` reject the `[label .accessor]` alias form

**Status:** RESOLVED (2026-07-20). Fixed in `stdlib/macros.tur`: every derive
`*-rest__` / `*-body__` macro now binds the vec-field label as
`(symbol-name (first field))` instead of `(first field)`, so the compile-time
`str-append` receives a string. `derive-show` with the alias form now compiles
and renders correctly (regression fixture `tests/fixtures/derive-show-alias/`;
the structs-guide example compiles). NOTE: `derive-debug` / `derive-display`
still fail on `cstr`/`bool`/... fields for a SEPARATE reason (no `Debug`/`Display`
instances for those primitives + a wrong dispatch fallback) -- tracked in
`docs/reported/derive-debug-display-missing-primitive-instances.md`. Kept for the
paper trail.

**Severity:** low (documented feature errors at compile time; simple fix)

## Summary

The `[label .accessor]` field-alias form documented for the derive macros in
`docs/guides/structs-guide.md` fails to compile. For a vec field descriptor the
macros bind the label as `(first field)` -- the label *symbol* -- and then pass
it to the compile-time `str-append`, which expects strings:

```
label (if is-vec (first field) (symbol-name field))
...
prefix (str-append ", " label " = ")   ; str-append gets a symbol -> error
```

## Repro

```turmeric
(load "stdlib/typeclass.tur")
(load "stdlib/str.tur")
(defstruct Named [title : cstr internal-label : cstr count : int])
(derive-show Named title [display-name .internal-label] count)
(defn main [] : int
  (do (println (show (make-struct Named "T" "L" 3))) 0))
```

```
stdlib/macros.tur:139:27: error: compile-time str-append expects string arguments
139 |             prefix        (str-append ", " label " = ")]
```

The bare-symbol field form (`title`, `count`) works; only the `[label
.accessor]` alias form errors. The structs-guide's own example
(`derive-show MyStruct name [display-name .internal-label] count`) therefore
does not compile.

## Root cause

`stdlib/macros.tur`, in `derive-show-rest__`, `derive-show-body__`,
`derive-debug-rest__`, `derive-debug-body__`, `derive-display-rest__`, and
`derive-display-body__`: the vec-field label is `(first field)` (a symbol)
rather than `(symbol-name (first field))` (its string). `str-append` /
`str-concat` construction then receives a non-string.

## Fix directions

Change each `label (if is-vec (first field) (symbol-name field))` to
`label (if is-vec (symbol-name (first field)) (symbol-name field))`. The
show-owned-result-plan stage-2 `derive-show-string` macros (added alongside)
already use the corrected form, so this is the two-line-per-macro change to
bring derive-show/debug/display into line and make the structs-guide example
compile.
