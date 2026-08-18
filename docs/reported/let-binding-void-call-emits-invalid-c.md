# `let`-binding a `:void` call emits `void x = ...;` instead of a diagnostic

**Severity:** medium (easy to hit, trivial to work around once understood,
but the failure surfaces as a `cc` error with no `.tur` attribution)
**Found:** 2026-08-18, writing `spices/secret`'s test suite against
`tur` v0.35.0 (`2748f5e8a`).

## Summary

Binding the result of a `:void`-returning function in a `let` is accepted by
the type checker and lowered to an ill-formed C declaration:

```c
void _un_1443 = secret__tests__hygiene__fill_hybuf_ex(...);
```

```
error: variable has incomplete type 'void'
```

Using `_` as the binding name makes no difference -- the emitter still
declares a variable.

Sequencing side effects in a binding list is a natural thing to reach for
when the `let` body has to end in a particular value (here, `it` requires its
body to evaluate to a `bool`, so the buffer setup wanted to happen in the
bindings). There is no hint at the `.tur` level that it is not allowed.

## Minimal repro

```turmeric
(defmodule b2

(defn noop [] : void
  ```c
  return;
  ```)

(defn main [] : int
  (let [_ (noop)]
    0)))
```

```
$ tur run b2.tur
.../b2_tur.c:7198:18: error: variable has incomplete type 'void'
1 error generated.
```

## Expected

Either of:

1. **Reject it** in the elaborator -- `TUR-E....: cannot bind a :void
   expression; use `do` to sequence it` -- which is the smaller change and
   points straight at the fix; or
2. **Accept it** and emit the call as a bare statement, discarding the
   binding (it can never be legally referenced anyway).

(1) seems right: a named binding that can never be read is almost always a
mistake rather than deliberate sequencing.

## Workaround

Sequence with `do` instead:

```turmeric
(let [buf (alloc-buf 32)]
  (do
    (fill-buf! buf 32 255)      ;; :void, sequenced not bound
    (secure-wipe-ptr! buf 32)
    (check buf)))
```

Or give the helper a non-void return. `spices/secret/tests/hygiene_test.tur`
does both, and carries a note so the pattern is not "cleaned up" back into a
binding list.
