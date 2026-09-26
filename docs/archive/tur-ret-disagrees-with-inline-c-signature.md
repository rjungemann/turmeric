# `__TUR_RET__` names a type the inline-C function does not return

**Resolved** (2026-09-25). When the signature emitter writes the carrier
`int64_t` for an inline-C body, it now sets the function's return C type to
match (src/compiler/emit_fns.c), and `__TUR_RET__` reads that. It is not
specific to maps: a non-generic `(Vec int)` result drew the same warning.
Fixture: `tests/fixtures/inline-c-tur-ret-heap-result`, which covers both;
without the fix it draws two -Wint-conversion warnings. The
r7rs-threads-lifecycle fixture uses `(__TUR_RET__)` again. What follows is
the report as filed.

**Severity:** low. The documented cast `return (__TUR_RET__)(intptr_t)v;`
draws a `-Wint-conversion` warning in one shape. `tests/run.sh` fails a
fixture on that warning, and `-Werror` would make it a hard error. Found
while writing `tests/fixtures/r7rs-threads-lifecycle`.

## Repro

A module function, not generic, whose declared result is a concrete map
type and whose body is inline C that uses `__TUR_RET__`:

```turmeric
(defmodule lifec
  (export kept-map)
  (defn kept-map [] : (Map int any)
    ```c
    return (__TUR_RET__)(intptr_t)pthread_getspecific(some_key);
    ```))
```

The emitted C, from a `#lang r7rs` program that imports the module:

```c
static int64_t lifec__kept_hymap() {
        return (tur_adt_Map__int__any *)(intptr_t)pthread_getspecific(lifec_key);
}
```

cc then warns: returning `tur_adt_Map__int__any *` from a function with
return type `int64_t` makes integer from pointer without a cast
[-Wint-conversion].

## Root cause

src/compiler/emit_core.c:4575, in the `__TUR_RET__` splice. With no ABI
specialization active, the splice falls back to `ctx->current_fn_ret_ctype`,
which here is the map's pointer type. The function itself was declared with
the carrier type, `int64_t`. The comment above the splice says the no-spec
case is "`int64_t` for the carrier base", so the two paths disagree about
which C type this function returns.

## Workaround

Cast to the type the signature actually has, as the fixture does:
`return (int64_t)(intptr_t)...;`

## Fix directions

- Make `current_fn_ret_ctype` agree with the signature the emitter writes
  for a carrier-ABI function, or have the splice ask the same question the
  signature emitter asks.
- Add a fixture that uses `(__TUR_RET__)` in a non-generic inline-C body
  returning `(Map K V)`, `(Vec T)` and an ADT, so the run.sh check guards the
  splice.
