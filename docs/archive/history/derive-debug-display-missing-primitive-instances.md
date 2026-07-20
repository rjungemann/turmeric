# `derive-debug` / `derive-display` miscompile on non-int/ptr fields (missing `Debug`/`Display` primitive instances)

**Status:** RESOLVED (2026-07-20). Defect 1 (missing instances) fixed in
`stdlib/typeclass.tur`: `Display` and `Debug` now cover `cstr`, `bool`, and the
sized-numeric set (`Display` delegates to the matching `Show` body; `Debug` keeps
the `type(value)` tag with `cstr` quoted / `bool` bare). `derive-debug` /
`derive-display` over cstr/bool/float fields now compile and render. Regression
fixture: `tests/fixtures/derive-debug-display/`. Defect 2 (the dispatch
fallback) is deeper and remains open, narrowed into
`docs/reported/method-dispatch-missing-instance-falls-back-to-carrier-representative.md`.
Kept for the paper trail.

**Severity:** medium (silent miscompile / segfault; derive-debug/display are
effectively unusable for structs with `cstr`/`bool`/... fields)

## Summary

`stdlib/typeclass.tur` defines `Debug` and `Display` instances only for `int`
and `ptr<void>` -- there is no `Debug[cstr]`, `Debug[bool]`, `Display[cstr]`,
etc. (`Show`, by contrast, has instances for `cstr`, `bool`, and the full
numeric set in `stdlib/typeclass-show.tur`). So `derive-debug` / `derive-display`
over a struct with a `cstr` (or other uncovered) field emit a `.debug` / `.display`
call on that field that has no matching instance, and dispatch falls back to the
*enclosing* instance method instead of erroring cleanly.

## Repro

```turmeric
(load "stdlib/typeclass.tur")
(load "stdlib/str.tur")
(defstruct Named [title : cstr count : int])
(derive-debug Named title count)
(defn main [] : int
  (let [n (make-struct Named "T" 3)]
    (do (println (debug n)) 0)))
```

Emitted C (abridged) -- the `title` (cstr) field dispatches to
`__inst_Debug_debug_Named` (the SELF instance) rather than a `Debug[cstr]`:

```c
static const char * __inst_Debug_debug_Named(tur_adt_Named __p) {
    __auto_type __ps_52 = (__inst_Debug_debug_Named((const char *)(__p).title)); /* wrong! */
    ...
```

```
error: incompatible type for argument 1 of '__inst_Debug_debug_Named'
```

An all-`int`-field struct compiles; adding any `cstr`/`bool`/... field breaks it.
`derive-show` is unaffected because `Show[cstr]` etc. exist.

## Root cause

Two compounding defects:

1. **Missing instances.** `Debug` / `Display` cover only `int` and `ptr<void>`
   in `stdlib/typeclass.tur`. Add the same primitive coverage `Show` has
   (`cstr`, `bool`, the numeric types) -- ideally sharing the `Show` bodies.
2. **Bad dispatch fallback.** A `.debug` method call on a field type with no
   instance silently resolves to the enclosing/self instance method instead of
   emitting a "no instance" diagnostic. That fallback (`.debug` on `cstr` ->
   `__inst_Debug_debug_Named`) is a latent miscompile independent of derive-*;
   worth a hard TUR-E00xx "no instance for Debug[cstr]" at the dispatch site.

## Notes

Separate from the `[label .accessor]` alias-label bug
(`docs/archive/.../derive-show-alias-label-not-stringified.md`, now fixed):
that made str-append reject a symbol label; this is about missing typeclass
instances + a wrong dispatch fallback. `derive-debug`/`derive-display` have no
fixture coverage today, which is why both defects went unnoticed.
