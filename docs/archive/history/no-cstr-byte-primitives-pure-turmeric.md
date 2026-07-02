---
title: No pure-Turmeric byte primitives on cstr (cstr-len, cstr-nth)
severity: LOW. Documentation/tutorial gap; workaround exists.
status: RESOLVED 2026-07-01. Added `stdlib/cstr.tur` exposing `cstr-len` and `cstr-nth` as an importable (not autoloaded) module: `(import cstr :refer [cstr-len cstr-nth])`. Keeping it out of `g_stdlib_autoload_files[]` sidesteps the Vec element-type inference perturbation the earlier autoload attempt hit, and adds zero fixture failures. Tutorials that want raw byte access on `:cstr` literals can now add one import line instead of hand-rolling a `(list 49 43 ...)` fallback.
---

# No pure-Turmeric byte primitives on cstr

## Summary

Turmeric has no autoloaded / natively-callable way to read the length of a
`:cstr` or the byte at a given index without dropping into inline C.
`stdlib/str.tur` exposes `str-len` and `str-eq?` on the `str` view struct,
but there is no equivalent for raw `:cstr` values, and `str.tur` is not
in the autoload list so fixtures cannot use it without an `import`.

This blocks the natural pure-Turmeric way to write text-processing
tutorials -- the parser-combinators tutorial has to hardcode its input
as a `(list 49 43 ...)` of ASCII codes because there is no
`(cstr-nth "1+2*(3+4)" i)` primitive.

## Repro

```turmeric
(defn main [] : int
  (println (cstr-len "hello"))          ;; error: unknown function 'cstr-len'
  (println (cstr-nth "hello" 0))        ;; error: unknown function 'cstr-nth'
  0)
```

Neither name resolves in a single-file compile.

## Proposed fix

Add a small `stdlib/cstr.tur` with two definitions and autoload it
alongside `list.tur` etc.:

```turmeric
(defn cstr-len [s : cstr] : int
  ```c return (int64_t)strlen((const char *)(intptr_t)s); ```)

(defn cstr-nth [s : cstr i : int] : int
  ```c return (int64_t)(unsigned char)((const char *)(intptr_t)s)[i]; ```)
```

Attempted this on 2026-07-01 by:

- creating `stdlib/cstr.tur` with the two definitions
- adding `"cstr.tur"` to the three autoload arrays in `src/main.c`
  (around lines 638, 5527, 11088)

Result: adding the new module perturbs codegen enough to break **9
inference-sensitive fixtures** (mostly `vec-*` inference cases) and
regenerates ~172 codegen snapshots. The 172 snapshot updates are
expected mechanical churn, but the 9 real regressions look like
elaboration is picking up a differently-ordered function table and
misresolving a Vec element type. Root cause not investigated further
this pass.

Failing fixtures observed after the autoload change:

- `letrec-self-recursive-closure`
- `vec-captureless-fat-closure-readback`
- `vec-eq-ascribed-multi`
- `vec-get-exists-element`
- `vec-new-fresh-arg-concrete-unify`
- `vec-new-push-infer`
- `vec-push-byvalue-aggregate-escapes-frame`
- `vec-typed-fat-closure-readback`
- `w3-letrec-open-capture`

Each fails with a C-level `-Wint-conversion` error where a `Vec`
pointer is being handed to a function expecting `int64_t` (or vice
versa), suggesting inference is choosing the wrong overload after the
autoload-table shift.

Workaround (in use by
`tests/fixtures/parsec-tutorial/input.tur`): pre-tokenize the input as
a `(list 49 43 ...)` of ASCII codes and walk it with `list-head` /
`list-tail`.

## Fix directions

1. **Preferred**: fix the elaborator so autoload ordering doesn't affect
   Vec element-type inference, then add `stdlib/cstr.tur` to the autoload
   list. Regen the 172 snapshots in the same commit.
2. **Alternative**: register `cstr-len` / `cstr-nth` as native builtins
   directly in `src/main.c` (like `list-head`) rather than as an
   autoloaded module -- avoids the codegen-snapshot churn but doubles
   the registration surface.
