---
title: C Integration Guide
category: Language Features
description: Foreign function interface (FFI) and C interop
---

# Turmeric ↔ C Integration Guide

Turmeric compiles to C99. This means C integration is not a plugin API -- it is
the compilation target itself. There is no runtime library to link against and
no interpreter to embed. Instead, you write Turmeric code that reaches into C
(and vice-versa) by making the generated C source do what you need.

This guide covers the two directions:

1. **Calling C from Turmeric** -- importing symbols with `extern-c`, writing
   inline C blocks, and understanding how the generated code interacts with
   your C headers.
2. **Calling Turmeric from C** -- using the generated C output as a static
   library or including the emitted source directly in a larger C project.

---

## 1. The Compilation Model

Running `./build/tur build path/to/file.tur` internally does:

```text
Source → Reader → Elaborator → Effect-lower → CPS transform
       → Borrow-checker → Emit C99 → cc → executable
```

The emitter (`src/emit.c`) writes a self-contained `.c` file. For multi-file
builds it also emits a `_main.c` that `#include`s the generated modules.
No Turmeric runtime shared library is produced; the only runtime artifact is
`src/runtime.{c,h}` (the defer/continuation frame structs), which gets compiled
in via `build/runtime.o`.

To inspect the emitted C without building, use:

```sh
./build/tur emit-c path/to/file.tur
```

This makes debugging integration problems much easier because you can see
exactly what the C side of the equation looks like.

### 1.1 Interpreter-only natives are not available to compiled code

A handful of list primitives -- `cons`, `head`, `tail`, `nil-value`, and
`cstr->parse-int` -- are **interpreter natives** registered by `tur run` / the
REPL (in `src/main.c`), *not* compiled stdlib functions. They are unbound when
you `tur build` / `tur emit-c` a file: you will see

```text
error: unknown function or operator 'cons'
```

even though the same program runs fine under `tur run`. This is why docstring
examples that read `(cons "/bin/ls" (cons "-l" 0))` work at the REPL but fail to
compile verbatim.

In compiled programs, build raw cons lists by one of:

- **Define local inline-C stubs** at the top of the file (the interpreter will
  shadow them with its natives automatically, so the file still runs both ways):

  ```turmeric
  (defn cons [value : int next : int] : int
    ```c typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
    __tur_cons_cell *c = malloc(sizeof(*c));
    c->head = (int64_t)value; c->tail = (int64_t)next;
    return (int64_t)(intptr_t)c;
    ```)
  ```

- **Use a stdlib helper** that *is* compiled -- e.g. `stdlib/args.tur`'s
  `args/parse` linearises the pre-declared `*args*` cons list for you, and
  `stdlib/list.tur` provides typed `Cons`/`tcons` cells.
- **Pass `0` (the empty list)** where an API accepts an empty cons list. For
  example `(process/spawn "/bin/true" 0)` spawns with an empty argv -- the
  idiomatic way to write a *compiled* `process/spawn` call without a list
  builder. (See `stdlib/process.tur`.)

See the "CLI Argument Parsing" rule in `CLAUDE.md` for the `*args*` conventions.

---

## 2. Calling C from Turmeric

### 2.1 `extern-c` -- Importing a C symbol

```turmeric
(extern-c function-name [arg-types...] return-type)
```

```sweet-exp
extern-c function-name [arg-types...] return-type
```

`extern-c` declares that a C function (or global) with the given name is
available at link time. The elaborator trusts the signature entirely -- there
is no validation against an actual header file.

**Examples from the stdlib:**

```turmeric
;; libc file I/O (stdlib/io.tur)
(extern-c fopen  [^cstr ^cstr] :ptr)
(extern-c fclose [^ptr]        :int)
(extern-c fread  [^ptr ^int ^int ^ptr] :int)

;; libc memory (stdlib/io.tur)
(extern-c malloc [^ptr size]  :ptr)
(extern-c free   [^ptr p]     :void)

;; libc RNG (stdlib/random.tur)
(extern-c rand  [^]       :int)
(extern-c srand [^int]    :void)
(extern-c time  [^ptr]    :ptr)
```

```sweet-exp
;; libc file I/O (stdlib/io.tur)
extern-c fopen  [^cstr ^cstr] :ptr
extern-c fclose [^ptr]        :int
extern-c fread  [^ptr ^int ^int ^ptr] :int

;; libc memory (stdlib/io.tur)
extern-c malloc [^ptr size]  :ptr
extern-c free   [^ptr p]     :void

;; libc RNG (stdlib/random.tur)
extern-c rand  [^]       :int
extern-c srand [^int]    :void
extern-c time  [^ptr]    :ptr
```

**Type annotation reference:**

| Turmeric type | Generated C type | Notes |
|---------------|-----------------|-------|
| `:int`        | `int64_t`       | Alias for `int64` |
| `:int8`       | `int8_t`        | −128 … 127 |
| `:int16`      | `int16_t`       | −32 768 … 32 767 |
| `:int32`      | `int32_t`       | −2 147 483 648 … 2 147 483 647 |
| `:int64`      | `int64_t`       | Alias for `int` |
| `:uint8`      | `uint8_t`       | 0 … 255 |
| `:uint16`     | `uint16_t`      | 0 … 65 535 |
| `:uint32`     | `uint32_t`      | 0 … 4 294 967 295 |
| `:uint64`     | `uint64_t`      | 0 … 18 446 744 073 709 551 615 |
| `:float`      | `double`        | Alias for `float64` |
| `:float32`    | `float`         | IEEE 754 single-precision |
| `:float64`    | `double`        | IEEE 754 double-precision |
| `:bool`       | `bool`          | `<stdbool.h>` |
| `:cstr`       | `const char *`  | Null-terminated, borrowed |
| `:ptr`        | `void *`        | Untyped pointer |
| `:void`       | `void`          | For return types only |

When a C function is variadic (e.g. `printf`), declare it with just the fixed
arguments. The elaborator does not validate the variadic portion:

```turmeric
(extern-c printf [^cstr] :int)
(printf "count=%lld\n" count)   ;; extra args pass through unchecked
```

```sweet-exp
extern-c printf [^cstr] :int
printf("count=%lld\n" count)   ;; extra args pass through unchecked
```

**Globals and zero-argument functions:**

Use an empty arg list `[^]` for globals that are accessed as function calls
or zero-argument functions:

```turmeric
(extern-c stderr [^] :ptr)   ;; FILE* stderr -- accessed as (stderr)
(extern-c rand   [^] :int)   ;; int rand(void)
```

```sweet-exp
extern-c stderr [^] :ptr   ;; FILE* stderr -- accessed as stderr()
extern-c rand   [^] :int   ;; int rand(void)
```

### 2.2 Inline C blocks -- Arbitrary C inside a Turmeric expression

Surround C source with triple backticks and a `c` tag:

```turmeric
(defn file-size [f]
  ```c
  FILE* file = (FILE*)f;
  long pos = ftell(file);
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, pos, SEEK_SET);
  return (int)size;
  ```)
```

```sweet-exp
defn file-size [f]
  ```c
  FILE* file = (FILE*)f;
  long pos = ftell(file);
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, pos, SEEK_SET);
  return (int)size;
  ```
```

The block is pasted verbatim into the generated function body. Turmeric
parameters are available by name as C local variables with their translated
types. You **must** provide an explicit `return` if the function has a
non-`void` return type.

Inline C is the escape hatch for anything the type system cannot yet express:
struct definitions, platform intrinsics, `#include`s for system headers inside
a function scope, etc.

**Style rule**: always place the closing ` ``` ` and the enclosing `)` on the
same line (` ```) `). Putting ` ``` ` on its own line causes Markdown renderers
to interpret it as the end of any surrounding code fence, breaking rendered
documentation.

**Important constraints:**

- The generated code is `c99 -pedantic`. Avoid GCC/Clang extensions unless
  you know the target will always use an extension-compatible compiler.
- Local `typedef`s and `struct` definitions inside inline C are fine (see
  `stdlib/random.tur` -- it defines `typedef struct Random Random` inside the
  inline block). They are scoped to that function.
- Do not rely on identifier names that look like Turmeric-mangled names
  (e.g. `tur__0`) -- these are unstable implementation details.
- `static` helpers defined inside an inline block work, but be aware of ODR
  if the same function name is used in multiple inline blocks across files.

**Calling a sibling `defn` from inline C -- `__TUR_CNAME_<name>__`:**

When an inline-C body needs to call (or take the address of) another Turmeric
`defn`, do **not** hand-write that defn's mangled C identifier -- the mangling
scheme (`mangle.c`) is an internal detail that can change, and a stale spelling
fails silently at the C-compile stage (`implicit declaration of function ...`)
with no Turmeric-level warning. Instead, splice the name with the
`__TUR_CNAME_<source-name>__` placeholder. The emitter expands it through the
same mangler the rest of the compiler uses, so the reference always tracks the
current scheme:

```turmeric
(defn tur-int-carrier-eq? [a : int b : int] : bool (= a b))

(definstance MapKey [int] (mk-box [x] x)
  ;; expands to the current mangled spelling of `tur-int-carrier-eq?`
  (mk-cmp [x] : int ```c return (int64_t)(intptr_t)__TUR_CNAME_tur-int-carrier-eq?__; ```)
  (mk-owned? [x] 0))
```

The source name between `__TUR_CNAME_` and the trailing `__` may contain sigils
(`-`, `?`, `!`, `=`, ...); it is terminated by the first `__`. The result
carries no module prefix, so it resolves to the unprefixed C name of a
module-local global (the common case for stdlib internal helpers).

### 2.3 Capability structs -- The idiomatic pattern for C APIs

The stdlib uses **capability structs** to wrap C APIs behind a Turmeric-visible
interface. This pattern keeps the unsafe pointer juggling isolated:

```turmeric
;; stdlib/random.tur -- capability struct wrapping libc rand()
(defn Real-Random []
  ```c
  typedef struct Random Random;
  struct Random {
      int (*next_int)(int min, int max);
      int (*next_float)(void);
  };

  static int random_next_int(int min, int max) {
      static int seeded = 0;
      if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
      return min + rand() % (max - min + 1);
  }

  static int random_next_float(void) {
      static int seeded = 0;
      if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
      return rand() % 10000;
  }

  Random* rng = (Random*)malloc(sizeof(Random));
  rng->next_int  = random_next_int;
  rng->next_float = random_next_float;
  return (void*)rng;
  ```)

(defn Real-Random-free [rng]
  ```c free(rng); ```)
```

```sweet-exp
;; stdlib/random.tur -- capability struct wrapping libc rand()
defn Real-Random []
  ```c
  typedef struct Random Random;
  struct Random {
      int (*next_int)(int min, int max);
      int (*next_float)(void);
  };

  static int random_next_int(int min, int max) {
      static int seeded = 0;
      if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
      return min + rand() % (max - min + 1);
  }

  static int random_next_float(void) {
      static int seeded = 0;
      if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
      return rand() % 10000;
  }

  Random* rng = (Random*)malloc(sizeof(Random));
  rng->next_int  = random_next_int;
  rng->next_float = random_next_float;
  return (void*)rng;
  ```

defn Real-Random-free [rng]
  ```c free(rng); ```
```

The struct is returned as `:ptr` (opaque `void *`) and freed explicitly. This
is intentionally manual -- `rc<T>` and `weak<T>` cannot track arbitrary C heap
memory yet, so the caller is responsible for cleanup.

---

## 3. Calling Turmeric from C

Turmeric does not yet produce a linkable `.a` or `.so`. However, there are two
practical ways to use compiled Turmeric code inside a larger C project:

### 3.1 Include the emitted `.c` directly

```
./build/tur emit-c mylib.tur > generated/mylib.c
```

Then add `generated/mylib.c` (and `src/runtime.c`) to your C build. Declare
the Turmeric-emitted top-level `defn` functions with `extern` in a hand-written
header, and call them from your C code.

Name mangling is minimal: a top-level `(defn my-function ...)` becomes
`my_function` in C (hyphens → underscores). Closures and anonymous functions
get mangled names like `tur__closure_N`.

### 3.2 Subprocess / build-step integration

Use `./build/tur build` as a build step that produces an executable, then have
your C application invoke it as a subprocess. This is the zero-coupling option:
the Turmeric binary handles I/O independently.

### 3.3 Linking `runtime.c`

Whichever approach you use, if the generated code uses `defer` you must compile
and link `src/runtime.c`. Its public surface is small:

```c
/* src/runtime.h */

typedef void (*defer_fn_t)(void *env);

#define TUR_FRAME_MAX_DEFERS 32

typedef struct tur_frame {
    defer_fn_t defers[TUR_FRAME_MAX_DEFERS];
    void *envs[TUR_FRAME_MAX_DEFERS];
    int n;
    struct tur_frame *parent;
    bool may_capture;          /* unused in v1 */
    struct EffectRow *effect_row; /* unused in v1 */
} tur_frame;

void tur_frame_init(tur_frame *f, tur_frame *parent);
int  tur_frame_push_defer(tur_frame *f, defer_fn_t thunk, void *env);
void tur_frame_fire_lifo(tur_frame *f);
void tur_frame_fire_chain(tur_frame *f);
```

---

## 4. Memory Management

Turmeric has three memory tiers. Understanding which tier a value lives in is
essential when crossing the C boundary.

### 4.1 Arena (compile-time only)

The compiler itself uses a bump-allocator arena (`src/arena.h`). This is
**compiler-internal only** -- generated programs do not use it.

### 4.2 Reference counting -- `rc<T>`

`rc<T>` is Turmeric's primary heap type. In generated C it is represented as a
pointer to an `RcControlBlock` followed immediately by the value. The control
block holds a strong count and a weak count.

```c
/* src/rc.h */
struct RcControlBlock {
    uint64_t strong_count;
    uint64_t weak_count;
    void    *value;
    RcDropFn drop_fn;      /* NULL → use free() */
    TypeKind value_type_kind;
    GcColor  color;        /* Bacon-Rajan cycle collector */
    bool     may_contain_cycles;
};
```

**Pitfall:** If you receive an `rc<T>` across the C boundary (as a `void *`),
you are holding a raw pointer into Turmeric's reference-counting machinery.
Calling `free()` on it directly will corrupt the control block. Always let
Turmeric code manage `rc<T>` lifetimes; pass scalars or opaque `void *`
capability structs across the boundary instead.

**Pitfall:** Cycles in `rc<T>` graphs are broken by the Bacon-Rajan cycle
collector, but only Turmeric-managed `rc<T>` nodes are tracked. If you create
a cycle that involves a raw C pointer (e.g. a C struct that holds a `void *`
back to an `rc<T>`), the cycle collector will not see it and memory will leak.

### 4.3 Weak pointers -- `weak<T>`

A `weak<T>` holds only the control block pointer (strong count = 0 is allowed).
`upgrade` returns a value wrapped in `Option`; if the strong count has reached
zero it returns `nil`. Weak pointers crossing the C boundary have the same
concern as `rc<T>` -- do not `free()` them directly.

### 4.4 Manual heap (`malloc`/`free` via `extern-c`)

When an inline C block or `extern-c` call allocates memory with `malloc`, that
memory is invisible to the cycle collector and the borrow checker. You must
`free()` it manually, typically with a matching `extern-c free` call or an
inline block. The stdlib consistently pairs allocating functions with a
corresponding `*-free` function (see `Real-Random-free` above).

**`defer` is the right tool here:**

```turmeric
(let [buf (malloc 1024)]
  (defer (free buf))
  ;; ... use buf ...
  )  ;; free fires here, even if an exception is thrown
```

```sweet-exp
let [buf malloc(1024)]
  defer free(buf)
  ;; ... use buf ...
  ;; free fires here, even if an exception is thrown
```

---

## 5. The `defer` System

`defer` registers a cleanup thunk that fires in LIFO order at scope exit,
including on exception unwind. This maps directly to `tur_frame_fire_lifo` in
the runtime.

```turmeric
(let [f (fopen "data.bin" "rb")]
  (defer (fclose f))
  ;; ... read from f ...
  )   ;; fclose(f) called here
```

```sweet-exp
let [f fopen("data.bin" "rb")]
  defer fclose(f)
  ;; ... read from f ...
  ;; fclose(f) called here
```

**Maximum defers per frame:** `TUR_FRAME_MAX_DEFERS` = 32. Exceeding this at
runtime returns `-1` from `tur_frame_push_defer` (the generated code silently
ignores the error in v1). Keep the number of defers per lexical scope under 32.

**`defer` and exceptions:** Turmeric uses `setjmp`/`longjmp` for exceptions
(`src/exn.h`). The exception machinery calls `tur_frame_fire_chain` before
jumping to the nearest handler, so defers do fire on exception unwind. However,
if a `defer` itself throws an exception the behavior is undefined in v1.

**`defer` and `return`:** Defers fire before `return` via `tur_frame_fire_chain`.
This means you can safely return from the middle of a scope that has registered
defers.

---

## 6. Exception Handling

Exceptions are non-resumable and use `setjmp`/`longjmp`:

```turmeric
(try
  (throw 42)
  (catch [e :int]
    (println e))
  (finally
    (println "always")))
```

```sweet-exp
try
  throw(42)
  catch [e :int]
    println(e)
  finally
    println("always")
```

Generated C for the `try` block calls `setjmp`. The `throw` form calls
`tur_throw`, which fires defers then `longjmp`s to the nearest handler. If
there is no handler, `tur_throw` prints the exception and calls `exit(1)`.

**From C:** If your inline C block or `extern-c` function needs to signal an
error, the safest approach in v1 is to return a sentinel value (e.g. `NULL` or
`-1`) and check it in Turmeric with `if`/`when`. Calling `tur_throw` directly
from C code that was called from inside a `try` block would work mechanically
(it is just a C function), but the exception type system would not know the
payload type at compile time. Use sentinel returns instead.

**Exception payloads** are typed by `TypeKind`. In v1, payloads are always
scalar values (int, bool, cstr) or `void *`. You cannot throw an `rc<T>` as an
exception payload yet.

---

## 7. Type System Boundary Rules

| Turmeric concept | Safe to pass to C? | Notes |
|-----------------|--------------------|-------|
| `int`, `float`, `bool` | Yes | Map to `int64_t`, `double`, `bool` |
| `cstr` | Yes (read-only) | `const char *`; Turmeric owns the string data |
| `ptr` | Yes | `void *`; you manage the lifetime |
| `ref<T>` | No | Borrow-checker-managed; do not store across call |
| `rc<T>` | No | Contains control block; use `ptr` wrappers instead |
| `weak<T>` | No | Same issue as `rc<T>` |
| closures | No | Env struct pointer; layout is unstable |
| structs (copy) | Yes (by value) | Passed as C value types |
| structs (move) | With care | Passing implies ownership transfer |

The golden rule: **use `ptr` (opaque `void *`) for any C-allocated resource
that crosses the boundary**, and keep `rc<T>`/`ref<T>` on the Turmeric side.

---

## 8. Inline C and the Type Checker

The elaborator (`src/elab.c`) does not parse inline C. It treats an inline
block as a black box and trusts the annotated return type. This means:

- **Type mismatches in inline C are silent.** A block annotated `:int` that
  actually returns a `double *` will compile and then corrupt memory at runtime.
- **Undefined behavior is not caught by the borrow checker.** The borrow
  checker (`src/borrow_check.c`) stops at the boundary of an inline block.
- **No `#include` is injected.** If your inline C calls `memcpy`, you need to
  either add an `extern-c memcpy` declaration or put `#include <string.h>` at
  the top of the inline block. The latter is valid C99 (an `#include` can
  appear anywhere a declaration can appear).

---

## 9. Build and Linking

### 9.1 Building the compiler

```sh
make           # debug build -- -Og, ASan+UBSan, -DTUR_DEBUG=1
make release   # -O2, -DNDEBUG
```

The compiler binary is `build/tur`.

### 9.2 Compiler flags for generated code

The compiler invokes `$(CC)` (defaulting to `cc`) with:

```sh
-Wall -Wextra -Werror -Wno-unused-parameter -std=c99 -pedantic
```

Plus, in debug mode: `-Og -g -fsanitize=address,undefined -DTUR_DEBUG=1`  
In release mode: `-O2 -DNDEBUG`

**Pitfall:** `-Werror` is on. Any warning in your inline C block or in a header
it includes will be a build error. Common sources of warnings in inline C:

- Implicit function declarations (missing `#include`).
- Signed/unsigned comparisons when mixing `int64_t` with `size_t`.
- `int` vs `long` mismatches when calling libc functions that return `size_t`.

Cast liberally and include headers explicitly.

### 9.3 Linking external libraries

`extern-c` imports must be resolvable at link time. Pass extra linker flags via
the `LDFLAGS` environment variable:

```sh
LDFLAGS="-lraylib -framework OpenGL" make release
```

For system libraries (`-lm`, `-lpthread`, etc.) add them to `LDFLAGS` in your
build script or `Makefile` wrapper.

### 9.4 Multi-file builds

```sh
./build/tur build src/main.tur   # compiles main.tur and any (require ...) deps
```

Each required module emits its own `.c` + `.h` pair. A generated `_main.c`
`#include`s all modules and defines `main()`. `extern-c` declarations in one
module are visible to C but not automatically shared between Turmeric modules --
repeat the declaration in each module that needs it, or factor them into a
shared `.tur` file.

---

## 10. Common Pitfalls Summary

| Pitfall | Consequence | Fix |
|---------|-------------|-----|
| Calling `free()` on an `rc<T>` pointer | Heap corruption | Never cross this boundary; use `:ptr` instead |
| Annotating inline C with wrong return type | Silent type confusion or memory corruption | Run with `emit-c` and inspect the generated code |
| Missing `#include` in inline C | Implicit function declaration warning → `-Werror` build failure | Add `#include` at top of inline block |
| Creating a C↔rc cycle | Memory leak (cycle collector can't see C pointers) | Keep cycles entirely on one side |
| More than 32 defers in a single scope | Silent drop of excess defers | Split scope or refactor |
| `defer` throwing an exception | Undefined behavior in v1 | Keep defer bodies simple and non-throwing |
| Inline C that calls `longjmp` unexpectedly | Skips Turmeric defer/rc cleanup | Only use `longjmp` if you know the full unwind path |
| Storing a `ref<T>` across an `extern-c` call | Borrow checker does not track C call boundaries | Use copy or `rc<T>` for data that outlives a single call |
| Varadic `extern-c` with wrong arg types | UB at runtime | Check generated C with `emit-c`; cast explicitly in callers |
| `static` name collision in multiple inline blocks | ODR violation / linker error | Prefix static helper names with a module-specific prefix |
| Using `cons`/`head`/`tail` in compiled code | `error: unknown function or operator 'cons'` (they are interpreter-only natives) | Define inline-C stubs, use a stdlib list helper, or pass `0` for an empty list (see §1.1) |

---

## 11. Worked Example -- Wrapping a C Library

This example wraps a hypothetical `libmath` C library with a Turmeric module.

**libmath.h** (your C header):

```c
typedef struct Vec2 { double x, y; } Vec2;
Vec2  vec2_add(Vec2 a, Vec2 b);
double vec2_len(Vec2 v);
Vec2 *vec2_alloc(double x, double y);  /* heap-allocated, caller frees */
void  vec2_free(Vec2 *v);
```

**math_wrap.tur** (Turmeric wrapper):

```turmeric
;; Declare the functions we need
(extern-c vec2_alloc [^float ^float] :ptr)
(extern-c vec2_free  [^ptr]          :void)

;; vec2_add and vec2_len operate on struct values, which we pass through
;; inline C since struct-by-value is not in the type system yet
(defn vec2-add [a b]
  ```c
  #include "libmath.h"
  Vec2 *pa = (Vec2 *)a;
  Vec2 *pb = (Vec2 *)b;
  Vec2 *result = (Vec2 *)malloc(sizeof(Vec2));
  *result = vec2_add(*pa, *pb);
  return (void *)result;
  ```)

(defn vec2-len [v]
  ```c
  #include "libmath.h"
  Vec2 *pv = (Vec2 *)v;
  return vec2_len(*pv);
  ```)

(defn demo []
  (let [a (vec2_alloc 3.0 4.0)]
    (defer (vec2_free a))
    (let [len (vec2-len a)]
      (println len))))   ;; prints 5.0
```

```sweet-exp
;; Declare the functions we need
extern-c vec2_alloc [^float ^float] :ptr
extern-c vec2_free  [^ptr]          :void

;; vec2_add and vec2_len operate on struct values, which we pass through
;; inline C since struct-by-value is not in the type system yet
defn vec2-add [a b]
  ```c
  #include "libmath.h"
  Vec2 *pa = (Vec2 *)a;
  Vec2 *pb = (Vec2 *)b;
  Vec2 *result = (Vec2 *)malloc(sizeof(Vec2));
  *result = vec2_add(*pa, *pb);
  return (void *)result;
  ```

defn vec2-len [v]
  ```c
  #include "libmath.h"
  Vec2 *pv = (Vec2 *)v;
  return vec2_len(*pv);
  ```

defn demo []
  let [a vec2_alloc(3.0 4.0)]
    defer vec2_free(a)
    let [len vec2-len(a)]
      println(len)   ;; prints 5.0
```

Build with:

```sh
LDFLAGS="-L. -lmath" ./build/tur build math_wrap.tur
```

---

## 12. Future Directions

These are not yet available in v1 but are planned:

- **`extern-struct`** -- import a C struct layout into the Turmeric type system,
  eliminating the need for opaque `:ptr` wrappers.
- **`rc<T>` with custom drop** -- the `RcDropFn` field in `RcControlBlock` is
  already wired; future phases will let user code register a custom destructor
  so an `rc<T>` can own a C-allocated resource directly.
- **Algebraic effects across the boundary** -- Phase 18+ effects (`perform`/
  `handle`) are implemented using delimited continuations (`tur_cont`). Crossing
  the C boundary inside a `handle` block is not yet safe.
- **Embedding API** -- a `libtur.a` with `tur_eval()` and a value API is
  described in the v2 roadmap but does not exist in v1.
