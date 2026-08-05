# `cons` cstr arg uncast in the CPS-IR emit path (macOS -Wint-conversion build failure)

> **RESOLVED (2026-07-23).** The CPS-IR `BS_FUNC_CALL` emitter now casts `cons`
> arguments through `(int64_t)(intptr_t)` -- mirroring the non-CPS path -- so a
> `cstr` head/tail no longer trips clang's `-Wint-conversion`. `bash
> tests/run.sh` green (2278/0).

**Severity:** Medium -- a real `tur build` failure, but only on macOS clang
(GCC/Linux treats the implicit `char* -> int64_t` conversion as a warning, so
the suite was green there while the macOS CI job failed to compile the
fixture).

## Symptom

`tests/fixtures/re-string` failed to build on the macOS CI runner:

```
error: incompatible pointer to integer conversion passing 'char[7]' to
       parameter of type 'int64_t' (aka 'long long') [-Wint-conversion]
 11209 |     __t11 = cons(("[0-9]+"), (INT64_C(0)));
```

## Repro

Any `#lang`/effectful file that routes a `(cons <cstr> ...)` through the
CPS-IR emit path, e.g. `re/union-patterns-string`:

```turmeric
(re/union-patterns-string (cons "[A-Za-z]+" (cons "[0-9]+" 0)))
```

## Root cause

`cons` is emitted as `static int64_t cons(int64_t h, int64_t t)`. The non-CPS
call emitter (`emit_core.c`, `BS_FUNC_CALL`) already casts every `cons` arg
through `intptr_t` (keyed on the `c_op == "cons"` identity) precisely so a
pointer-sized non-int head (cstr, opaque handle) crosses into the `int64_t`
parameter without an incompatible-conversion diagnostic. The **CPS-IR**
emitter (`emit_cps_ir.c`, `prim_expr`, `BS_FUNC_CALL`) emitted each arg as a
bare `(%s)` with no such cast, so a fixture whose `cons` call was lowered
through CPS emitted `cons(("[0-9]+"), ...)` -- an implicit `char* -> int64_t`
that GCC warns on but macOS clang rejects.

## Fix

`emit_cps_ir.c` `BS_FUNC_CALL` now applies the same `cons`-keyed
`(int64_t)(intptr_t)(...)` arg cast as the non-CPS path. Regression guard: the
existing `re-string` fixture (which exercises the CPS lowering of a cstr cons
list).
