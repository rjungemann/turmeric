# Cloneable-continuation thunk casts a borrow-rc env to `int64_t`, not its pointer type -- RESOLVED

**Severity:** low (codegen hygiene; benign on LP64, correct at runtime and
leak-clean, but a `-Wint-conversion` warning that would break under `-Werror`).

**Status:** RESOLVED 2026-07-21. Fixed in `cc_cast_for_kind`
(`src/compiler/emit_cps_ir.c`).

## Summary

When an owning `rc` captured `^borrow` crossed a `cloneable-reset` (the E3 /
owning-autodrop-crossing path), the emitted continuation glue thunk cast the
captured env to `int64_t` instead of the borrow callee's real pointer parameter
type. GCC/Clang emitted `-Wint-conversion` (passing an integer where a pointer
is expected). The program still ran correctly because on LP64 `int64_t` and the
pointer are the same width, but the emitted C was not `-Werror`-clean.

## Minimal repro (pre-fix)

`tests/fixtures/cloneable-owning-autodrop-crossing/input.tur` (runs, output
`32`, leak-clean):

```sh
./build/tur emit-c tests/fixtures/cloneable-owning-autodrop-crossing/input.tur \
  | gcc -x c - -c -o /dev/null   # -> warning: passing argument 2 ... -Wint-conversion
```

The offending emitted lines (before the fix):

```c
static int64_t read_hycombine(int64_t v, RcControlBlock * r) { ... }
static intptr_t run_ccctx0_0(intptr_t env, intptr_t value) {
    return (intptr_t)read_hycombine((int64_t)value, (int64_t)env);
}                                                    /* ^^^ int passed to a pointer param */
```

## Root cause

The cloneable/serial continuation thunk emitter forwards its `intptr_t`
`env`/`value` slot into the top-level callee's parameters, casting each with
`cc_cast_for_kind(TypeKind)`. That helper special-cased only `TY_CSTR`
(`const char *`) and cast every other kind -- including the pointer-represented
`TY_RC`/`TY_WEAK` (`RcControlBlock *`), `TY_REF`/`TY_LREF`/`TY_PTR_VOID`
(`void *`) -- to `(int64_t)`. Passing an `int64_t` into a pointer parameter is
the `-Wint-conversion`.

## Fix

`cc_cast_for_kind` now returns the callee param's pointer cast for the
pointer-representation kinds: `RcControlBlock *` for rc/weak, `void *` for
ref/lref/ptr/ref-mut (a `void *` converts implicitly to any object pointer in
C, so a typed `ptr<T>` param `T *` is satisfied too), `const void *` for the
immutable borrow, `tur_cloneable_cont *` for a continuation handle, and
`const char *` for cstr as before. Every other kind keeps the `(int64_t)`
carrier cast, so scalar behavior is unchanged. The same helper feeds every
`_ccctx*` / `_skcall*` 2-arg and 1-arg call-frame thunk, so the fix is uniform.

## Verification

- The emitted thunk now reads
  `read_hycombine((int64_t)value, (RcControlBlock *)env)`; the emitted C
  compiles clean under `gcc -Wint-conversion`.
- Fixture output unchanged (`32`), leak-clean.
- Full suite: 2240 passed, 0 failed. No snapshot churn (no `expected.c`
  captures a `_ccctx`/`_skcall` thunk).
