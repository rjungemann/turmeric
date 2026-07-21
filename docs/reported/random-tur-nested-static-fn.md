# `stdlib/random.tur` `Real-Random` / `Seeded-Random` use `static` nested functions -- won't compile

**Severity:** medium (the two capability-struct constructors `Real-Random`
and `Seeded-Random` are uncompilable on a standard/clang/GNU C toolchain; no
fixture exercises them, so the suite is green despite this). This is the same
defect class already fixed for the digest hex API -- see the archived
[docs/archive/history/digest-hex-nested-static-fn.md](../archive/history/digest-hex-nested-static-fn.md).

## Summary

`Real-Random` and `Seeded-Random` build their `Random` capability struct
(two function-pointer fields, `next_int` / `next_float`) by defining the
implementation functions as **`static` nested functions** inside the `defn`'s
inline-C body -- a function defined inside another function body, carrying the
`static` storage class. The inline-C body is spliced *inside* the emitted C
function, so those helper definitions become C nested functions:

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
tur emit-c repro.tur > repro.c        # succeeds -- the bug is downstream
cc -std=c11 -c repro.c -o /dev/null   # FAILS
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

`cc -std=c11` reports (4 nested-fn errors -- 2 per constructor):

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

- `Real-Random` (defn at line 39): nested `static int random_next_int(...)`
  at line 49 and `static int random_next_float(void)` at line 64.
- `Seeded-Random` (defn at line 115): nested `static int seeded_next_int(...)`
  at line 127 and `static int seeded_next_float(void)` at line 134.

Each is a function-local (nested) definition carrying `static` -- the illegal
combination.

**Not affected:** `rand-int` (176), `rand-float` (202), `rand-die` (227),
`rand-bool` (252) use only `static int seeded = 0;` -- a legal *static local
variable*, not a nested function. Those constructors compile fine, and this
report does not touch them.

## Fix directions

Hoist the four `next_int` / `next_float` bodies to **file scope** as ordinary
sibling `defn`s, exactly as `digest/sha256-transform!` /
`digest/md5-transform!` were factored out of the digest bodies, then reference
them from the inline-C body by address via the `__TUR_CNAME_<name>__`
placeholder (never a hand-written mangled name):

```turmeric
(defn random-next-int [min : int max : int] : int
  ```c
  static int seeded = 0;
  if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
  int range = (int)max - (int)min + 1;
  return (int64_t)((int)min + (rand() % range));
  ```)

(defn Real-Random [] : ptr
  ```c
  typedef struct Random Random;
  struct Random { int64_t (*next_int)(int64_t,int64_t); int64_t (*next_float)(void); };
  Random* rng = (Random*)malloc(sizeof(Random));
  rng->next_int   = __TUR_CNAME_random-next-int__;
  rng->next_float = __TUR_CNAME_random-next-float__;
  return (void*)rng;
  ```)
```

Note the struct field's function-pointer type must match the emitted
Turmeric signature (`int64_t (*)(int64_t, int64_t)`), or carry an explicit
cast; the current `int (*)(int, int)` fields assume the old nested-fn
signature. Removing the `static` keyword alone is **not** enough -- nested
functions are still non-standard; the definitions must move out of the
enclosing `defn` body.

## Why it went unnoticed

No fixture under `tests/fixtures/` constructs a `Real-Random` /
`Seeded-Random` capability (only the scalar `rand-*` helpers are exercised
indirectly, and those are unaffected), so `bash tests/run.sh` never compiles
the broken code. Same blind spot the digest hex API had before its regression
fixture was added; a `tests/fixtures/random-capability/` fixture that calls
both constructors would guard the fix.

## See also

- [docs/archive/history/digest-hex-nested-static-fn.md](../archive/history/digest-hex-nested-static-fn.md)
  -- identical defect, already resolved (the template for the fix).
- [docs/guides/c-integration-guide.md](../guides/c-integration-guide.md)
  -- the "do not define a helper *function* inside a `defn`'s inline-C body"
  guidance and pitfalls-table row.
