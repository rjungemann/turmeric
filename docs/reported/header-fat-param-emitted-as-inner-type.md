---
title: Header emits `^fat` (and `TY_FN`) params as inner type, conflicting with `.c` `int64_t`
severity: hard error -- blocks separate-compilation build of any module exporting a `^fat`-param fn
status: fixed
discovered: 2026-06-06
fixed: 2026-06-10
discovered-in: spices/signal (turmeric-spices)
---

# Header emits `^fat` params as inner type, conflicting with `.c`'s `int64_t`

## Summary

In separate-compilation/project mode (`tur build .`), `emit_header` declares
exported function prototypes using the parameter's **inner declared type**
(e.g. `double` for a `^fat sig : (fn [float] float)`). The corresponding
`emit_implementation` path correctly emits the param as `int64_t` per #286
(`fat-param-emitted-as-void-ptr-warns-in-inline-c`). Result: `.c` and `.h`
disagree on the prototype, and the C compiler rejects the file with
`conflicting types for '<fn>'`.

Same bug applies to params whose `param_types[j].kind == TY_FN`: the `.c`
emits `int64_t`, the header emits the inner fn signature's `type_c_name`.

## Repro

```sh
cd ../turmeric-spices/spices/signal
/path/to/turmeric/build/tur build .
```

Observed:

```
signal__core.c:13:8: error: conflicting types for 'signal__core__sample'
   13 | double signal__core__sample(int64_t, double);
./signal__core.h:13:8: note: previous declaration is here
   13 | double signal__core__sample(double, double);
```

The Turmeric signature is
`(defn sample [^fat sig : (fn [float] float) t : float] : float ...)`.

## Root cause

`src/compiler/emit_module.c::emit_header` at lines 5519--5531 walks
`fd->n_params` and emits each with `type_c_name(_hdr_pty)` -- it never checks
`fd->params[j]->is_fat`, `fd->params[j]->is_poly_fn`, or
`fd->param_types[j].kind == TY_FN`.

The forward-declaration emitter inside `emit_implementation` (lines
1495--1525) has the correct three-way logic:

```c
if (fd->params[j]->is_poly_fn) {
    buf_puts(out, "tur_poly_fn_t");
} else if (fd->param_types[j].kind == TY_FN) {
    buf_puts(out, "int64_t");
} else if (fd->params[j]->is_fat &&
           fd->body && fd->body->kind == EX_INLINE_C) {
    buf_puts(out, "int64_t");
} else {
    /* normal carrier-aware emission */
}
```

The same logic must also be applied in `emit_header`, but additionally:
in the header the `is_fat` carrier should always be `int64_t` regardless of
whether the body is inline-C, because the **caller** sees the carrier ABI
and must use `int64_t` to match the implementation's signature.

## Proposed fix

In `emit_header`'s param-emission loop (around line 5519), insert the same
poly-fn / TY_FN / `^fat` carrier handling as `emit_implementation`'s
forward-decl loop. For `is_fat`, emit `int64_t` unconditionally (no inline-C
gate -- the carrier ABI applies to the prototype regardless of body kind).

## Validation

- `cd ../turmeric-spices/spices/signal && tur build .` -- compiles clean.
- `bash tests/run.sh` -- no fixture regressions.
- Sweep `tests/fixtures/` for any project-mode fixture exporting a
  `^fat`-param or `TY_FN`-param fn and verify generated headers.

## Resolution

Fixed in `src/compiler/emit_module.c::emit_header`'s param-emission loop: the
three-way carrier logic from `emit_implementation`'s forward-decl loop is now
mirrored -- `is_poly_fn` -> `tur_poly_fn_t`, `param_types[j].kind == TY_FN` ->
`int64_t`, and `is_fat` -> `int64_t` unconditionally (no inline-C gate, since
the carrier ABI applies to the prototype regardless of body kind). Normal
params still fall through to the existing pass-by-ptr emission.

Validated with a minimal project-mode repro exporting
`(defn sample [^fat sig : (fn [float] float) t : float] : float ...)`: both
the generated `.h` and `.c` now emit `double sig__core__sample(int64_t, double);`
and the build compiles clean. Full fixture suite passes with no regressions.
