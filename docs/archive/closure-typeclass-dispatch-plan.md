# Closure–Typeclass Dispatch: `(fn, env)` Pairs in Dict Structs

**Status:** Complete (implemented as Phase CCL)  
**Replaces:** "Use non-capturing closures or named helper functions" workaround  
**Affects:** `src/emit.c`, `src/typeclass.h`, stdlib typeclass implementations, test fixtures, `docs/guides/hkt-guide.md`

---

## Problem

Closures that capture variables from an enclosing scope are represented as
`void*` (a fat closure: `struct { int64_t __fn; <captured fields>... }`).
Typeclass method parameters that accept functions are typed as `int64_t`
(a bare function pointer).

When a capturing closure is passed to a typeclass method at a call site like:

```turmeric
(let [delta 5]
  (.fmap opt (fn [x] (+ x delta))))   ;; delta is captured -- currently broken
```

The closure's env pointer is silently discarded. The method receives only the
`__fn` field cast to `int64_t`, and calling it without an env pointer is
undefined behavior (the captured `delta` is never accessible).

---

## Chosen Solution: `tur_poly_fn_t` Parameters in Dictionary Structs

The `tur_poly_fn_t` struct already exists (emitted in `emit.c` as part of
Phase HRT1):

```c
typedef struct { void *env; int64_t (*fn)(void *, int64_t); } tur_poly_fn_t;
```

The plan is to use this struct for every function-typed parameter in every
typeclass method signature emitted into dictionary structs. This makes each
dictionary method slot aware of both the function pointer and its environment.

At call sites, the compiler packs a closure into a `tur_poly_fn_t` before
passing it to a method. Method implementations unpack it with
`pair.fn(pair.env, arg)`.

Non-capturing lambdas and plain named functions are wrapped in a
`tur_poly_fn_t` with `env = NULL`; implementations must handle both cases
(a `NULL` env means call without an extra env argument, or the thunk ignores
the env parameter — see calling convention below).

### Unified calling convention for `tur_poly_fn_t`

Every function stored in `tur_poly_fn_t` is emitted with the signature:

```c
int64_t thunk(void *env, int64_t arg);
```

For non-capturing functions the `env` parameter is accepted but ignored.
This lets call sites always use a single dispatch pattern:

```c
pair.fn(pair.env, arg);
```

No branch on `env == NULL` is needed.

---

## Affected Components

| Component | File | Change summary |
|-----------|------|----------------|
| Dictionary struct codegen | `src/emit.c` ~4385–4430 | Emit `tur_poly_fn_t` fields for fn-typed params |
| Dictionary singleton init | `src/emit.c` ~4433–4460 | Wrap impl fn pointers in `tur_poly_fn_t` literals |
| Closure emission | `src/emit.c` ~2661–2706 | Thunks must accept `(void *env, int64_t arg)` |
| Call sites (typeclass dispatch) | `src/emit.c` ~2402–2443 | Pack closure into `tur_poly_fn_t`; unpack at call |
| Non-capturing fn call sites | `src/emit.c` (same region) | Wrap with `{ NULL, fn_ptr }` |
| `tur_poly_fn_t` definition | `src/emit.c` ~5309–5313 | Already exists; confirm it is emitted before dict structs |
| Typeclass method signatures | `src/typeclass.h` | No structural change needed; fn-typed-ness inferred at emit time |
| `stdlib/list.tur` fmap | `stdlib/list.tur` ~170–198 | Update inline C to receive and use `tur_poly_fn_t` |
| `stdlib/typeclass.tur` (all instances) | `stdlib/typeclass.tur` | Update all inline C method bodies |
| Other stdlib instances | `stdlib/*.tur` | Any `definstance` with a fn-typed method param |
| Test fixtures | `tests/fixtures/hkt-*/` | Add capturing-closure test cases |
| HKT guide | `docs/guides/hkt-guide.md` | Remove limitation note; add closure capture section |

---

## Implementation Steps

### Step 1 — Confirm `tur_poly_fn_t` is emitted early enough

In `emit.c`, find where `tur_poly_fn_t` is emitted (~line 5313) and confirm it
appears in the output file before the first dictionary struct typedef. If not,
move the emission of `tur_poly_fn_t` to the preamble section that runs before
any typeclass codegen.

### Step 2 — Update dictionary struct codegen (`emit.c` ~4385–4430)

Currently the loop emits a raw function pointer field for each method:

```c
// current
buf_printf(ctx->file, "int64_t (*%s)(int64_t container, int64_t fn);\n", method_name);
```

Change: for each parameter whose type indicates it is a function (i.e. the
parameter is annotated as `:fn`, or is a `^f`-applied callable, or is detected
as `is_poly_fn`), emit `tur_poly_fn_t` instead of `int64_t`.

Practical heuristic for this phase: any parameter named `fn`, `f`, or matching
the method's declared fn-param position is treated as a fn-typed parameter.
A cleaner long-term approach is to mark fn-typed params in `TypeClassMethod`
at parse time (see Step 2a).

**Step 2a (optional but recommended):** Add a `bool *param_is_fn` bitfield to
`TypeClassMethod` in `src/typeclass.h` (one bit per param). Set it during
`defclass` elaboration when a parameter's declared type is a function arrow or
`^f`-applied. This removes the name-based heuristic.

### Step 3 — Update dictionary singleton init (`emit.c` ~4433–4460)

Currently the singleton initializer stores a bare function pointer:

```c
// current
.fmap = __functor_list_fmap,
```

Change to a compound literal that initializes `tur_poly_fn_t`:

```c
// new
.fmap = { .env = NULL, .fn = __functor_list_fmap },
```

The implementation function `__functor_list_fmap` must be updated in Step 5 to
have the thunk signature `int64_t(void *env, int64_t arg, ...)`.

### Step 4 — Update closure emission (`emit.c` ~2661–2706)

Currently, the thunk function generated for a fat closure has the signature:

```c
int64_t __thunk_N(void *env, int64_t arg);   // already correct for HRT1
```

Verify this is already the case (the research notes suggest it is for
continuations). If any code path emits a thunk with the old signature
`int64_t __thunk_N(int64_t arg)`, update it to add the `void *env` first
parameter.

### Step 5 — Update call sites (`emit.c` ~2402–2443)

**Passing a closure to a `tur_poly_fn_t` parameter:**

When the elaborator sees a closure (a `void*`-typed expression) being passed to
a position typed as `tur_poly_fn_t`, emit a packing expression:

```c
(tur_poly_fn_t){
  .env = (void *)(intptr_t)closure_ptr,
  .fn  = (int64_t (*)(void *, int64_t))(intptr_t)((int64_t *)closure_ptr)[0]
}
```

The `[0]` dereference reads the `__fn` field (always the first field of every
fat closure struct).

**Passing a non-capturing lambda or named function:**

```c
(tur_poly_fn_t){ .env = NULL, .fn = named_fn }
```

**Calling through a `tur_poly_fn_t` inside a method body:**

```c
pair.fn(pair.env, arg);
```

### Step 6 — Update stdlib method implementations

For each `definstance` method that receives a `fn`-typed parameter and calls it
with inline C, change the call pattern.

**`stdlib/list.tur` — `__functor_list_fmap`** (~line 183):

```c
// current
((int64_t(*)(int64_t))(intptr_t)fn)(cell->value)

// new (fn is now tur_poly_fn_t)
fn.fn(fn.env, cell->value)
```

Apply the same change to:
- Any other `fmap`, `bind`, `fold-left`, `fold-right`, `bimap` implementations
  in `stdlib/*.tur` that call their fn parameter with inline C.
- The `bind` / `do-m` implementations for `option`, `result`, `vec`, etc.

### Step 7 — Add / update test fixtures

**Update existing fixtures** (`tests/fixtures/hkt-functor-option/input.tur`,
`tests/fixtures/hkt-functor-list/`, etc.): verify they still pass.

**Add new capturing-closure fixtures:**

```
tests/fixtures/hkt-closure-capture/input.tur
tests/fixtures/hkt-closure-capture/expected_output
```

Minimum test cases:

```turmeric
;; Single captured variable through fmap
(let [delta 5]
  (let [opt (__opt_some 10)]
    (let [result (.fmap opt (fn [x] (+ x delta)))]
      (println (__opt_unwrap result)))))   ;; 15

;; Captured variable through bind (do-m)
(let [scale 3]
  (let [r (do-m x (__opt_some 4) (__opt_some (* x scale)))]
    (println (__opt_unwrap r))))           ;; 12

;; Nested capture (closure over closure)
(let [a 2]
  (let [b 3]
    (let [opt (__opt_some 10)]
      (let [result (.fmap opt (fn [x] (+ (* x a) b)))]
        (println (__opt_unwrap result)))))) ;; 23
```

### Step 8 — Update `docs/guides/hkt-guide.md`

See the [HKT Guide Changes](#hkt-guide-changes) section below.

---

## HKT Guide Changes

### Remove the limitation note (line 228–231)

Delete the blockquote:

```markdown
> **Limitation**: Closures in Turmeric that capture variables from an enclosing
> scope are represented as `void*` (opaque handles) and cannot be passed directly
> to typeclass methods expecting `int64_t fn`. For chained `do-m` with multiple
> bindings that reference each other's values, use `__bind_option` directly.
```

### Remove item 1 from Known Limitations (line 340)

Delete:

```markdown
1. **Closure capture**: Closures that capture variables (`void*` type) cannot be passed to typeclass methods expecting `int64_t fn`. Use non-capturing closures or named helper functions.
```

Renumber the remaining items (2 → 1, 3 → 2, 4 → 3).

### Expand "Closures with fmap" section (after line 247)

Replace the current non-capturing-only examples with a section that covers both:

```markdown
## Closures with fmap

Both non-capturing and capturing closures can be passed to typeclass methods.

### Non-capturing closures

```turmeric
(let [opt (__opt_some 10)]
  (let [result (.fmap opt (fn [x] (+ x 5)))]
    (println (__opt_unwrap result))))  ;; 15
```

### Capturing closures

Closures that close over variables in an enclosing scope work transparently:

```turmeric
(let [delta 5]
  (let [opt (__opt_some 10)]
    (let [result (.fmap opt (fn [x] (+ x delta)))]
      (println (__opt_unwrap result)))))  ;; 15
```

The compiler packs the closure's function pointer and captured environment into
a `tur_poly_fn_t` pair before passing it to the typeclass method. Method
implementations receive and call the pair uniformly; no manual adaptation is
needed.

### do-m with captured variables

`do-m` chains also support capture across bindings:

```turmeric
(let [scale 3]
  (let [r (do-m x (__opt_some 4) (__opt_some (* x scale)))]
    (println (__opt_unwrap r))))  ;; 12
```
```

### Update the "dictionary passing" performance section (line 268–272)

Update the generated C example to show the new `tur_poly_fn_t` field:

```markdown
```c
/* generated for (definstance Functor [option] ...) */
typedef struct {
  int64_t (*fmap)(int64_t container, tur_poly_fn_t fn);
} dict_Functor_option;
static dict_Functor_option __dict_Functor_option = {
  .fmap = { .env = NULL, .fn = __fmap_option }
};
```
```

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| ABI break: all existing `definstance` inline C must be updated | Grep for the old call pattern `((int64_t(*)(int64_t))(intptr_t)fn)` and update mechanically |
| `tur_poly_fn_t` must be defined before first dict struct in output | Move emission to preamble; add a compile-time assertion test |
| Non-capturing lambdas get slightly larger call frame (extra `NULL` arg) | Negligible; C compiler will optimise the NULL away with `-O1`+ |
| Name-based fn-param heuristic (Step 2a alternative) is fragile | Prefer the `param_is_fn` bitfield in `TypeClassMethod`; the heuristic is only acceptable as a temporary measure |

---

## Out of Scope

- Monomorphization (`-O` flag) — still planned separately; this change is
  orthogonal to it.
- `defkind` elaboration — unchanged.
- Recursive types / Free monad — unchanged.
