# `stdlib/random.tur` `Real-Random` / `Seeded-Random` use `static` nested functions -- won't compile

**Status:** RESOLVED 2026-07-21. The four `next_int` / `next_float` slot
implementations are hoisted to file-scope Turmeric defns
(`random-next-int!`, `random-next-float!`, `seeded-next-int!`,
`seeded-next-float!`) and referenced from the two constructor bodies via
`__TUR_CNAME_...__`, so the emitted C carries no nested functions and compiles
on a standard/clang/GNU toolchain. The capability struct's function-pointer
fields were widened to `int64_t (*)(int64_t,int64_t)` / `int64_t (*)(void)` to
match the emitted Turmeric signatures. A latent second blocker was fixed in the
same pass: the `(extern-c time [^ptr] :ptr)` binding emitted
`extern void *time(void *)`, conflicting with the real `time_t time(time_t *)`
from `<time.h>` (already in the codegen preamble) -- since `time` is only ever
called from inline-C as `time(NULL)`, the redundant binding was removed. The two
`-free` functions were tightened from an untyped (`:int`) handle to `rng : ptr`,
clearing a `free()`-of-integer warning. Regression fixture:
`tests/fixtures/random-capability` (constructs both capabilities, exercises the
slots through the struct, asserts seed reproducibility + in-range draws --
portable properties, never exact `rand()` values).

**Severity:** medium (the two capability-struct constructors `Real-Random`
and `Seeded-Random` were uncompilable on a standard/clang/GNU C toolchain; no
fixture exercised them, so the suite was green despite this). This was the same
defect class already fixed for the digest hex API -- see
[docs/archive/history/digest-hex-nested-static-fn.md](digest-hex-nested-static-fn.md).

## Summary

`Real-Random` and `Seeded-Random` built their `Random` capability struct
(two function-pointer fields, `next_int` / `next_float`) by defining the
implementation functions as **`static` nested functions** inside the `defn`'s
inline-C body -- a function defined inside another function body, carrying the
`static` storage class. The inline-C body is spliced *inside* the emitted C
function, so those helper definitions became C nested functions:

- ISO C forbids nested functions outright.
- GNU C allows nested functions as an extension but rejects the `static`
  qualifier on them (`error: invalid storage class for function ...`).
- Clang (the default macOS `cc`) does not support nested functions at all:
  `error: function definition is not allowed here`, followed by
  `use of undeclared identifier 'random_next_int'` at the
  `rng->next_int = random_next_int;` assignment.

## Repro

```turmeric
(load "stdlib/random.tur")
(defn main [] : int
  (do (Real-Random) (Seeded-Random 42) 0))
```

```sh
tur emit-c repro.tur > repro.c        # succeeds -- the bug was downstream
cc -std=c11 -c repro.c -o /dev/null   # FAILED (nested-fn errors); now compiles clean
```

Emitted C (abbreviated), showing the nested definition inside `Real_hyRandom`:

```c
static void Real_hyRandom() {
  ...
  struct Random { int (*next_int)(int, int); int (*next_float)(void); };
  static int random_next_int(int min, int max) { ... }   /* nested + static */
  static int random_next_float(void) { ... }             /* nested + static */
  Random* rng = (Random*)malloc(sizeof(Random));
  rng->next_int   = random_next_int;
  rng->next_float = random_next_float;
  return (void*)rng;
}
```

`cc -std=c11` reported (4 nested-fn errors -- 2 per constructor):

```
error: function definition is not allowed here     (random_next_int)
error: function definition is not allowed here     (random_next_float)
error: use of undeclared identifier 'random_next_int'
error: use of undeclared identifier 'random_next_float'
... and the same 4 for seeded_next_int / seeded_next_float
```

Verified on Apple clang 21.0.0 against `tur` v0.26.6 (`emit-c` + direct `cc`).

## Root cause

`stdlib/random.tur`:

- `Real-Random`: nested `static int random_next_int(...)` and
  `static int random_next_float(void)` inside the body.
- `Seeded-Random`: nested `static int seeded_next_int(...)` and
  `static int seeded_next_float(void)` inside the body.

Each was a function-local (nested) definition carrying `static` -- the illegal
combination.

**Not affected:** `rand-int`, `rand-float`, `rand-die`, `rand-bool` use only
`static int seeded = 0;` -- a legal *static local variable*, not a nested
function. Those constructors compiled fine and the fix did not touch them.

## Fix

Hoisted the four `next_int` / `next_float` bodies to **file scope** as ordinary
sibling `defn`s (the digest `sha256-transform!` / `md5-transform!` template),
referenced from the inline-C by address via the `__TUR_CNAME_<name>__`
placeholder. The struct field function-pointer types were updated to match the
emitted Turmeric signature (`int64_t (*)(int64_t,int64_t)`), since the old
`int (*)(int,int)` fields assumed the nested-fn signature. The `seeded-*`
helpers deliberately do **not** auto-seed -- `Seeded-Random` calls
`srand(seed)` once at construction and the slots draw the shared libc stream --
whereas the `random-*` helpers auto-seed with `time(NULL)` on first use, exactly
as the original nested bodies did.

## See also

- [docs/archive/history/digest-hex-nested-static-fn.md](digest-hex-nested-static-fn.md)
  -- identical defect, the template for this fix.
- [docs/guides/c-integration-guide.md](../../guides/c-integration-guide.md)
  -- the "do not define a helper *function* inside a `defn`'s inline-C body"
  guidance and pitfalls-table row.
