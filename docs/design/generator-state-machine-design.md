# Generator State Machine Design

**Phase:** GF0  
**Status:** Complete  
**Last updated:** 2026-05-21

---

## Overview

This document specifies the IR and C emission strategy for `gen`/`yield` forms
before any compiler changes are made. It serves as the contract between the
elaboration pass (GF1: `elab_forms.c`) and the emission pass (GF1:
`emit_stmt.c`, `emit_module.c`). Generators have since shipped; the
user-facing documentation is
[docs/guides/generators-guide.md](../guides/generators-guide.md).

The core idea: a `gen` body compiles to a C struct (the generator state machine)
plus a `_next` function that dispatches on an integer state tag. `yield` becomes
a save-state/return-SOME pair. Resumption is a `goto` into the function body,
landing after the yield point.

---

## 1. Generator Struct Representation

Each distinct `gen` form in the source produces a unique C struct type. The
struct has two layers of fields:

1. **State tag** (`int32_t __state`) -- the resume-point discriminant.  
   - `-1` = exhausted (done)  
   - `0` = initial (never resumed)  
   - `1..N` = one entry per yield point in source order

2. **Captured fields** -- one `int64_t` (or typed) field per variable that is
   live across at least one yield point. These include:
   - Parameters/closed-over variables captured by the `gen` form
   - `let`-bound variables whose binding site precedes a yield and whose last
     use follows a yield

Variables that are only used within a single segment between consecutive yield
points remain as ordinary C locals and are **not** promoted to struct fields.

### Struct naming

The compiler generates a unique name per `gen` site based on the enclosing
function name and a monotonically increasing counter:

```
__gen_<enclosing-fn-name>_<n>_t
```

Example: the `gen` form inside `range-gen` becomes `__gen_range_gen_0_t`.

### Creation

The `gen` form evaluates to a struct value (not a pointer). The caller receives
the struct by value and is responsible for storage:

```c
/* (range-gen 0 10) emits: */
__gen_range_gen_0_t __tmp_42 = __gen_range_gen_0_create(0, 10);
```

No heap allocation occurs at creation time. The caller may place the struct on
the stack, embed it in another struct, or heap-allocate it explicitly.

---

## 2. Control-Flow Forms That Cross Yield Points

The following table classifies each control-flow form by whether a yield can
legally appear inside it in v1, and notes the compilation strategy when yields
are present.

| Form | Yields allowed (v1) | Strategy |
|---|---|---|
| `while` | Yes | Loop body segmented by yield labels; `goto` dispatch at top |
| `if` / `when` | Yes | Each branch segmented; `goto` dispatch at top |
| `cond` | Yes | Each clause body segmented; `goto` dispatch at top |
| `do` | Yes | Sequential statements; each yield adds a state |
| `let` | Yes (in body) | If bound variable is live across yield, promote to struct field |
| nested `gen` | No (v1) | Inner generator is a separate state machine; outer drives with `gen-next` |
| `match` | No (v1) | See limitation §7.1 |
| inline-C blocks | No (v1) | Compiler cannot see yield points inside `` ```c ``` `` |
| recursive `gen` | No (v1) | See limitation §7.2 |

### 2.1 While loops

A `while` loop with one or more yields inside its body is segmented at each
yield. Each yield point `n` gets:

- A goto label `__yield_<n>:` placed immediately after the yield-return in the
  C source.
- A dispatch entry `case n: goto __yield_<n>;` at the top of `_next`.

The loop structure is preserved as a C `for(;;)` with a manual break on the
exit condition. The goto jumps from the top-of-function dispatch into the middle
of the loop body.

### 2.2 Conditionals

For `if`/`cond` forms, each branch that contains a yield is split in place.
The goto dispatch at the top of `_next` can jump into any branch, bypassing the
condition test on resume. This is valid C because the condition test is a
statement (not a declaration) and the goto merely skips it.

Variables declared in one branch must not be live across a yield in the other
branch. The liveness analysis (§3) enforces this.

### 2.3 do bodies

Sequential `do` forms are the simplest case: each yield just adds a new state.
The dispatch at the top lands after the last yield.

---

## 3. Variable Liveness Across Yield Points

### Liveness rule

A variable `x` is **yield-live** if there exists a yield point `y` such that:

- `x` is bound (by `let`, function parameter, or `gen` capture) **before** `y`
  in execution order, AND
- `x` is **used** (read or assigned via `set!`) **after** `y` in at least one
  execution path.

Yield-live variables are promoted to struct fields. All others remain C locals.

### Practical consequences

- A variable bound and used entirely within a single segment (between two
  consecutive yield points) stays local.
- A `^mut` loop counter that is read in the loop condition and written after
  each yield is yield-live and must be a struct field.
- A temporary `let`-binding computed just before a yield, whose value is the
  yield argument and is not used after the yield, stays local.

### Struct field naming

Promoted variables use the Turmeric binding name directly as the C field name
(the same scheme used by closure env structs). Name collisions with `__state`
are avoided by the `__` prefix reservation: user-visible Turmeric identifiers
may not start with `__`.

---

## 4. Hand-Compiled C Examples

These examples show the exact C output the compiler should produce. The
`Option_int` type is the existing heap-based option representation from
`stdlib/option.tur`; v1 reuses it directly via `some()`/`none()`.

> **Note on `goto` validity.** The dispatch block at the top of each `_next`
> function uses `if (__state == N) goto __yield_N;`. The target labels appear
> inside loop bodies or branch bodies. C11 §6.8.6.1 allows `goto` to jump over
> plain declarations (those without initializers). All local variables inside
> loop bodies are declared without initializers for exactly this reason. The
> targets are only ever `int64_t x;` (bare declarations), never
> `int64_t x = expr;`.

---

### 4.1 Simple Loop Generator

**Turmeric source:**

```lisp
(defn range-gen [start :int end :int] : (Generator :int)
  (gen []
    (let [^mut i start]
      (while (< i end)
        (yield i)
        (set! i (+ i 1))))))
```

**Captured variables:** `start`, `end` (function params); `i` (yield-live loop var).

**Struct:**

```c
typedef struct {
    int32_t  __state;   /* -1=done, 0=init, 1=after-yield-0 */
    int64_t  start;
    int64_t  end;
    int64_t  i;
} __gen_range_gen_0_t;
```

**Creation function (emitted by `emit_module.c`):**

```c
static __gen_range_gen_0_t __gen_range_gen_0_create(int64_t start, int64_t end) {
    return (__gen_range_gen_0_t){ .__state = 0, .start = start, .end = end };
}
```

**Next function (emitted by `emit_stmt.c`):**

```c
static int64_t __gen_range_gen_0_next(__gen_range_gen_0_t *g) {
    /* dispatch on resume point */
    if (g->__state == 1)  goto __yield_0;
    if (g->__state == -1) return none();
    /* state 0: initialise */
    g->i = g->start;
    for (;;) {
        if (!(g->i < g->end)) break;
        g->__state = 1;
        return some(g->i);
        __yield_0:;
        g->i = g->i + 1;
    }
    g->__state = -1;
    return none();
}
```

**Call site** -- the `(gen-next g)` form emits:

```c
int64_t __tmp_7 = __gen_range_gen_0_next(&g);
/* __tmp_7 is Option_int (heap ptr); caller tests with some?() */
```

---

### 4.2 Nested Loops

**Turmeric source:**

```lisp
(defn grid-gen [rows :int cols :int] : (Generator :int)
  (gen []
    (let [^mut i 0]
      (while (< i rows)
        (let [^mut j 0]
          (while (< j cols)
            (yield (+ (* i cols) j))
            (set! j (+ j 1))))
        (set! i (+ i 1))))))
```

**Captured variables:** `rows`, `cols`; yield-live: `i`, `j`.

**Struct:**

```c
typedef struct {
    int32_t  __state;
    int64_t  rows;
    int64_t  cols;
    int64_t  i;
    int64_t  j;
} __gen_grid_gen_0_t;
```

**Next function:**

```c
static int64_t __gen_grid_gen_0_next(__gen_grid_gen_0_t *g) {
    if (g->__state == 1)  goto __yield_0;
    if (g->__state == -1) return none();
    g->i = 0;
    for (;;) {
        if (!(g->i < g->rows)) break;
        g->j = 0;
        for (;;) {
            if (!(g->j < g->cols)) break;
            g->__state = 1;
            return some(g->i * g->cols + g->j);
            __yield_0:;
            g->j = g->j + 1;
        }
        g->i = g->i + 1;
    }
    g->__state = -1;
    return none();
}
```

The `goto __yield_0` jumps past both loop-condition checks and both
initialization assignments (`g->i = 0`, `g->j = 0`), landing directly inside
the inner loop body. The outer loop picks up where it left off because `g->i`
and `g->j` are struct fields.

---

### 4.3 Early Return

`return` inside a `gen` body terminates the generator: it sets `__state = -1`
and returns `none()`. It does not return from the enclosing Turmeric function.

**Turmeric source:**

```lisp
(defn until-sentinel [n :int sentinel :int] : (Generator :int)
  (gen []
    (let [^mut i 0]
      (while (< i n)
        (when (= i sentinel)
          (return))
        (yield i)
        (set! i (+ i 1))))))
```

**Captured variables:** `n`, `sentinel`; yield-live: `i`.  
The `(when (= i sentinel) (return))` test is NOT live across any yield, so no
extra struct fields are needed for it.

**Struct:**

```c
typedef struct {
    int32_t  __state;
    int64_t  n;
    int64_t  sentinel;
    int64_t  i;
} __gen_until_sentinel_0_t;
```

**Next function:**

```c
static int64_t __gen_until_sentinel_0_next(__gen_until_sentinel_0_t *g) {
    if (g->__state == 1)  goto __yield_0;
    if (g->__state == -1) return none();
    g->i = 0;
    for (;;) {
        if (!(g->i < g->n)) break;
        if (g->i == g->sentinel) {
            g->__state = -1;
            return none();           /* early termination */
        }
        g->__state = 1;
        return some(g->i);
        __yield_0:;
        g->i = g->i + 1;
    }
    g->__state = -1;
    return none();
}
```

---

### 4.4 Conditional Yield

**Turmeric source:**

```lisp
(defn sign-gen [x :int] : (Generator :int)
  (gen []
    (if (> x 0)
      (do
        (yield 1)
        (yield x))
      (do
        (yield -1)
        (yield x)))))
```

Two yield points in each branch, four total (states 1--4).  
No captured variables are yield-live: `x` is a param read before both yields,
but since it doesn't change, the compiler may fold it into the struct for
correctness; here we always promote parameters to struct fields.

**Struct:**

```c
typedef struct {
    int32_t  __state;
    int64_t  x;
} __gen_sign_gen_0_t;
```

**Next function:**

```c
static int64_t __gen_sign_gen_0_next(__gen_sign_gen_0_t *g) {
    if (g->__state == 1) goto __yield_0;
    if (g->__state == 2) goto __yield_1;
    if (g->__state == 3) goto __yield_2;
    if (g->__state == 4) goto __yield_3;
    if (g->__state == -1) return none();
    if (g->x > 0) {
        g->__state = 1;
        return some(1);
        __yield_0:;
        g->__state = 2;
        return some(g->x);
        __yield_1:;
    } else {
        g->__state = 3;
        return some(-1);
        __yield_2:;
        g->__state = 4;
        return some(g->x);
        __yield_3:;
    }
    g->__state = -1;
    return none();
}
```

The gotos for states 1 and 2 jump into the true branch; gotos for states 3 and
4 jump into the false branch. Both are valid C.

---

## 5. `(Generator a)` Surface Type and Operations

### Type definition

`(Generator a)` is not a single concrete struct -- each `gen` form produces a
distinct monomorphic struct. At the type-system level, `Generator` is a
parameterised nominal type (similar to `Option` or `Vec`) whose concrete
representation is resolved per-call-site.

In `types.h` this will be represented as `TY_GENERATOR`:

```c
TY_GENERATOR,  /* (Generator a) -- resumable iterator */
/* as.generator_.element_type : TypeKind of yielded values   */
/* as.generator_.struct_def   : StructDef* for the C struct  */
/* as.generator_.next_fn      : char* name of _next function */
```

### Core operations

| Operation | Signature | Behaviour |
|---|---|---|
| `(gen-next g)` | `(Generator a) -> (option a)` | Advance; return `some(v)` or `none` |
| `(gen-done? g)` | `(Generator a) -> :bool` | True when `__state == -1` |

These are not regular function calls; the elaborator special-cases them and
emits the appropriate C directly (calling the specific `_next` function for the
generator's concrete type).

### Construction

`(gen [capture-list] body)` evaluates to a `(Generator a)` value. The capture
list `[]` is currently reserved for future use; all free variables in `body`
are captured automatically. The form is an expression that produces a struct
value.

---

## 6. Struct Sizing Decision

**Decision: statically sized struct per `gen` instance.**

Rationale:

- The compiler knows all captured variables and all yield points at compile
  time; struct size is fixed.
- No dynamic allocation at creation time: creating a generator is a struct
  literal initialisation.
- The struct is typically short-lived (used in a single loop at the call site)
  and fits comfortably on the stack.
- Matches the plan's stated goal: "zero-overhead, resumable iterators with no
  continuation capture and no heap allocation per yield."

The caller owns the storage. For generators used inline (e.g., inside a
`while` loop at the call site), the struct lives on the stack of the caller
function. For generators stored in a `vec` or passed to another function, the
caller is responsible for ensuring the struct outlives its use -- the same
lifetime discipline that applies to any struct in Turmeric.

**Not chosen: arena/heap-allocated per-creation.** This would add allocation
overhead and defeat the zero-cost goal for the common case. It remains an
option for callers who need to store generators in data structures that outlive
the creating stack frame.

---

## 7. Limitations (v1)

### 7.1 No `gen` inside `match` arms that span a yield

Turmeric `match` compiles to a C `switch` statement. C does not permit `goto`
to jump from outside a `switch` into a case body of that `switch`. A yield
inside a `match` arm would require a resume-goto to land inside a `switch` case,
which is invalid C.

**v1 rule:** The elaborator rejects `(yield ...)` that appears (directly or
transitively) inside a `match` arm. `match` may appear in a generator body as
long as no arm contains a yield. The shipped diagnostic (TUR-E0702, emitted
from `src/compiler/elab_forms.c`) is:

```
error: 'yield' is not supported inside a 'match' arm (1.0 limitation); this requires the post-1.0 CPS pass.
```

### 7.2 No recursive generators

A generator body that calls itself (directly or indirectly) would require either
a heap-allocated call stack per recursion level, or a CPS transform. Neither
fits the zero-cost state-machine model.

**v1 rule:** The elaborator detects self-calls inside `gen` bodies and rejects
a yield in such a body with the shipped diagnostic (TUR-E0703):

```
error: 'yield' is not supported inside a recursive generator (1.0 limitation); this requires the post-1.0 CPS pass.
```

### 7.3 No `yield` inside inline-C blocks

The compiler cannot analyse yield points inside `` ```c ... ``` `` blocks. Any
attempt to call `yield` from inline C is silently undefined; the elaborator
does not detect it.

**v1 rule:** Document this limitation. A future lint pass may warn on inline-C
blocks inside `gen` bodies.

### 7.4 No `yield` inside nested `gen` forms

A `gen` form appearing inside another `gen` body is a separate, independent
generator. Its yields belong to the inner generator, not the outer one. To
delegate to an inner generator, use `yield*` (GF2) or a manual loop with
`gen-next`.

### 7.5 Mutable references to outer scope

A `gen` body may not capture `^mut ref<T>` or `^mut rc<T>` variables from an
enclosing scope. Immutable borrows and copy-type captures are fine. This
restriction avoids aliasing between the generator struct fields and the
caller's mutable state.

---

## 8. Supported and Unsupported Control-Flow Patterns

### Supported

| Pattern | Notes |
|---|---|
| `while` with yield inside body | State tag loops back; see §4.1 and §4.2 |
| `while` with early `return` | Sets `__state = -1`; see §4.3 |
| `if`/`when`/`cond` with yield in any branch | `goto` dispatch into branch; see §4.4 |
| `do` sequence with multiple yields | Each yield adds one state |
| `let` bindings live across a yield | Promoted to struct field; see §3 |
| Nested `while` loops with yield in inner loop | One yield label serves both loops; see §4.2 |
| Parameters captured by `gen []` | Always promoted to struct fields |
| Multiple yield points in one function | Each adds one state entry to dispatch |
| Calling `gen-next` on an inner generator | Normal function call; not a yield |

### Not supported (v1)

| Pattern | Reason | Workaround |
|---|---|---|
| `yield` inside `match` arm | goto into C switch is invalid | Bind match result, yield outside |
| Recursive `gen` body | No per-level stack model | Explicit stack via `vec` |
| `yield` inside inline-C | Compiler can't see yield points | Refactor to Turmeric |
| `gen` inside `gen` (same state machine) | Separate state machines | Drive inner with `gen-next` |
| Mutable outer-scope refs captured by `gen` | Aliasing hazard | Pass by value or copy |
| `gen` with a variable-length array field | VLA in struct is non-standard | Use `vec` for dynamic storage |

---

## 9. Relationship to Existing Compiler Infrastructure

| Generator concept | Analogous existing mechanism |
|---|---|
| Struct field for captured var | Closure env struct (`EX_CLOSURE`, `emit_expr.c` §1326) |
| State-tag dispatch (`goto`) | Defer thunk registration (`register_defer_thunk`) |
| `_next` function emitted at file scope | Closure thunk emitted at file scope (`emit_module.c`) |
| `return none()` for exhaustion | `EX_RETURN` with `TY_NIL` result |
| `some(v)` return | `EX_CALL` to `some` from `stdlib/option.tur` |
| Struct creation by value | `EX_MAKE_STRUCT` |

The generator implementation reuses `fresh_tmp`, `name_for_binding`,
`type_c_name`, and `indent_buf` from `emit_core.c` without modification.

---

## 10. Open Questions for GF1

1. **Type unification for `(Generator a)`.** How does `elab_core.c` unify
   two different generator types? Each `gen` site is nominally distinct.
   The element type `a` should still unify normally; the struct type should
   not unify across sites.

2. **`yield*` desugaring.** The plan defers `yield*` to GF2. Confirm that
   the GF1 AST does not need to reserve a node kind for it, or add
   `EX_YIELD_STAR` as a forward-declared no-op.

3. **Effect rows on `_next`.** If the generator body performs effects (e.g.,
   I/O), should those propagate to the `_next` function's effect row? v1
   answer: yes, conservatively propagate all effects from the body to `_next`.

4. **`gen-done?` implementation.** Since `__state` is not exposed at the
   Turmeric level, `gen-done?` must be a compiler-known intrinsic rather than
   a stdlib function. The elaborator will lower `(gen-done? g)` to an inline
   `g.__state == -1` test.
