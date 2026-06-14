# `(c-fn ...)` lowers `int` to `int64_t` and drops `const`, losing C-ABI precision

**Status:** Partially fixed -- ptr<T> precision shipped 2026-06-14
(turmeric `6cd02e2a`).  Residual gap: integer-width (`int` ->
`int64_t` instead of `size_t` / `unsigned int`) and `const`
qualification on pointers.
**Severity:** Medium. Still surfaces as a `-Wincompatible-function-pointer-types`
error whenever a typed `(c-fn ...)` is passed to a real C callback whose
signature uses non-int64 integer slots (e.g. `size_t messageSize`) or
const-qualified pointers (e.g. `const unsigned char *message`). The C
compiler rejects the function-pointer cast even though every parameter
is int64-wide at runtime, so the call would actually work.
**Discovered:** 2026-06-14, while wiring rtmidi's
`rtmidi_in_set_callback` through the S1 callback migration
(`docs/reported/spices-int-stand-in-audit-2026-06-14.md` / branch
`migrate-cfn-s1-2026-06-14` in `../turmeric-spices`).
**Scope:** the c-fn type-lowering pass in `src/compiler/emit_*` (the
side that turns `(c-fn [A...] R)` into a C function-pointer type).

## Summary

The `(c-fn ...)` type, introduced in PR #354 / commit `cf29bdb8` and
shaped per `docs/archive/history/typed-c-abi-function-pointers.md`,
lowers parameter and result types to their *carrier* C types rather
than to a precise C type. In particular:

- ~~`ptr<u8>` lowers to `void *` (instead of `unsigned char *`).~~
  **Fixed 2026-06-14, turmeric commit `6cd02e2a`.** cfnptrs now
  preserve per-arg full Type in the typedef registry and emit
  precise element-pointer types.  A regression fixture at
  `tests/fixtures/c-fn-ptr-element-precise/` exercises this.
- ~~`ptr<T>` for any non-`void` `T` lowers to `void *`.~~ **Fixed
  alongside the above.**
- `int` lowers to `int64_t` (`long long`), so a callback whose real C
  signature takes `size_t` / `unsigned int` / `int` (machine word) is
  type-incompatible at the function-pointer level.  **Residual.**
- `const` qualifiers are also lost (a `const T *` parameter ends up
  spelled `T *`).  **Residual.**

At runtime everything is int64-wide and the call would dispatch
correctly, but the C front-end rejects the assignment with
`-Wincompatible-function-pointer-types` (an error under
`-Werror`-grade configurations and a hard error under recent clang
defaults).

## Minimal repro -- rtmidi callback

The typed signature shipped in the S1 migration:

```turmeric
(defn midi-in-set-callback
  [mi : int callback : (c-fn [float ptr<u8> int ptr<void>] void)] : void
  ```c
  #include <rtmidi/rtmidi_c.h>
  rtmidi_in_set_callback((RtMidiInPtr)(intptr_t)mi, callback, NULL);
  ```)
```

Generated C function-pointer typedef (from `rtmidi__in.c`):

```c
typedef void (*tur_fnptr_void_double_void___int64_t_void___t)
    (double, void *, int64_t, void *);
```

What `<rtmidi/rtmidi_c.h>` actually declares:

```c
typedef void (*RtMidiCCallback)
    (double timeStamp, const unsigned char *message, size_t messageSize,
     void *userData);
```

`gcc -Wincompatible-function-pointer-types` (default clang behaviour
on recent macOS toolchains) refuses the implicit conversion. Workaround
in the spice is an explicit `(RtMidiCCallback)callback` cast inside
the inline-C body -- documented in
`spices/rtmidi/src/rtmidi/in.tur:midi-in-set-callback` as of branch
`migrate-cfn-s1-2026-06-14` -- which trades the language-level
type safety the c-fn was supposed to provide for a hand-written cast.

## Observed vs expected

- **Observed:** the typed c-fn rejects flagrantly wrong handler shapes
  at the Turmeric layer (which is the original S1 win), but at the C
  layer the function-pointer cast at the registration site is
  rejected for any callback whose real signature uses element-typed
  pointers or non-int64 widths -- forcing a hand-written
  `(RealSignature)callback` cast in every inline-C body. The cast hides
  any future ABI mismatch and undoes part of the safety the typed c-fn
  was supposed to deliver.
- **Expected:** the c-fn type lowers to a *precise* C signature so the
  registration site can pass the function pointer straight through with
  no cast. `ptr<u8>` should emit `unsigned char *`; `ptr<T>` should
  emit the element-typed pointer; the `int` slot should accept a
  `:size` (or distinct integer width) annotation so a `size_t` /
  `unsigned int` slot lowers correctly.

## Root-cause pointers

- `src/compiler/emit_*` lowers `c-fn` parameter types through the same
  carrier-ABI helpers as ordinary `defn` parameters, which collapse
  every pointer to `void *` and every integer to `int64_t`.
- The c-fn AST node does carry per-parameter `Type` values, so the
  information is present -- the lowering just doesn't consult element
  types when emitting the C signature.

## Proposed fix directions

### Option A -- precise lowering for `c-fn` parameters

Walk each `(c-fn [A...] R)` parameter `Type` when emitting the C
function-pointer typedef and:

1. ~~If `A` is `ptr<T>` with a known `T` other than `void`, emit
   `T_c_name *` instead of `void *`. For `ptr<u8>` specifically, emit
   `unsigned char *` (since `u8` already has a precise C name).~~
   **Done 2026-06-14, turmeric `6cd02e2a`.**
2. Preserve a `const` discipline if the type carries one -- this needs
   a tiny type-level annotation in `ptr<T>` (e.g. `ptr<T :const>`).
   **Residual.**
3. Add a `:size` (or `:usize`) primitive that lowers to `size_t`.
   **Residual.**

Pros: removes every inline-C cast at C-callback registration sites.
Each typed c-fn really *is* a sound C-level type.

Cons: requires new surface (`ptr<T :const>`, `:size` / `:usize`)
plus a type-level `const`/size tracking pass.

### Option B -- emit a per-signature reinterpret wrapper

Auto-generate a thin static C wrapper that performs the bit-precise
cast from the int64-wide carrier ABI to the real C signature, and pass
*that* to the registration site.

Pros: doesn't require new surface; the typed c-fn keeps its current
lowering.

Cons: per-distinct-real-signature wrapper churn; consumer still has to
*declare* the real signature somewhere (probably as an `(extern-c
typedef ...)` form), so the surface change is similar.

**Recommendation:** Option A. The point of c-fn is to give the C
function-pointer slot a real type; if the lowering still erases the
element type, every consumer has to spell the real C signature in an
inline-C cast anyway. The new primitives (`:size`, `ptr<T :const>`)
are small additions, and they fix a class of off-by-one ABI bugs
beyond just c-fn callbacks (struct fields, return values, ...).

## Validation

- A negative fixture: a defn that registers a typed c-fn whose
  element-pointer type does NOT match the C signature should fail
  with a clear Turmeric-level diagnostic, not a C compile error.
- Rebuild `../turmeric-spices/spices/rtmidi` against the fixed c-fn
  lowering. The cast workaround in
  `midi-in-set-callback` should be removable.

## Cross-references

- `docs/archive/history/typed-c-abi-function-pointers.md` -- the
  shipped c-fn design; carrier ABI was always documented but the
  precision gap wasn't called out.
- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- audit;
  the S1 callback migration is the load-bearing consumer.
- `../turmeric-spices` branch `migrate-cfn-s1-2026-06-14`,
  `spices/rtmidi/src/rtmidi/in.tur:midi-in-set-callback` -- carries
  the `(RtMidiCCallback)callback` workaround documented inline with a
  pointer to this report.
