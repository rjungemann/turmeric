# Cloneable-continuation thunk casts a borrow-rc env to `int64_t`, not its pointer type

**Severity:** low (codegen hygiene; benign on LP64, correct at runtime and
leak-clean, but a `-Wint-conversion` warning that would break under `-Werror`).

## Summary

When an owning `rc` captured `^borrow` crosses a `cloneable-reset` (the E3 /
owning-autodrop-crossing path), the emitted continuation glue thunk casts the
captured env to `int64_t` instead of the borrow callee's real pointer parameter
type. GCC/Clang emit `-Wint-conversion` (passing an integer where a pointer is
expected). The program still runs correctly because on LP64 `int64_t` and the
pointer are the same width, but the emitted C is not `-Werror`-clean.

## Minimal repro

`tests/fixtures/cloneable-owning-autodrop-crossing/input.tur` (runs, output
`32`, leak-clean):

```sh
./build/tur emit-c tests/fixtures/cloneable-owning-autodrop-crossing/input.tur \
  | gcc -x c - -c -o /dev/null   # -> warning: passing argument 2 ... -Wint-conversion
```

The offending emitted lines:

```c
static int64_t read_hycombine(int64_t v, RcControlBlock * r) { ... }
static intptr_t run_ccctx0_0(intptr_t env, intptr_t value) {
    return (intptr_t)read_hycombine((int64_t)value, (int64_t)env);
}                                                    /* ^^^ should be (RcControlBlock *)env */
```

## Root cause (direction)

The cloneable-continuation thunk emitter builds the forwarded-env argument with a
blanket `(int64_t)env` cast rather than the borrow callee's declared parameter
type. It should cast the env to the callee param type (here `RcControlBlock *`),
mirroring the `(intptr_t)`/typed-cast the direct path uses when forwarding an
`rc` handle. The emit site is the `dk_frame(run_ccctx0_0, ...)` glue-thunk
generation in the CPS/DK continuation lowering (`run_ccctx0_0` in the emitted C;
see the cloneable-continuation thunk emission in `src/passes/cps_ir.c` /
`src/compiler/emit_cps_ir.c`).

## Fix directions

Emit the forwarded-env cast using the borrow callee's parameter type instead of
`int64_t`. The value argument in the same thunk (`(int64_t)value`) is fine when
the resume value is an integer; only the env forward that lands in a pointer
parameter needs the typed cast.
