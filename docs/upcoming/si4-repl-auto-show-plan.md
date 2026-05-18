# SI4 -- REPL Auto-Show Implementation Plan

> **Status:** Not started. Splits into three independent sub-tasks with very
> different effort profiles.
>
> **Prerequisites:** SI0--SI3 complete (all `Show` instances exist for stdlib
> types).
>
> **Last updated:** 2026-05-14

---

## Motivation

After SI0--SI3, evaluating `(pair-new 1 2)` in the REPL still prints a raw
integer (the heap pointer) rather than `"(1, 2)"`. The goal is to make
top-level REPL results display in human-readable form using the `Show` typeclass
conventions established in SI1--SI3.

---

## Architecture Reality Check

The original SI4 plan assumed type tags would need to be added to the compiler.
After reading the runtime source, the picture is better than expected in one area
and unchanged in another:

- **`TuriValue` already carries runtime type tags** (`TURI_INT`, `TURI_BOOL`,
  `TURI_FLOAT`, `TURI_CSTR`, `TURI_STRUCT`, etc.) -- no compiler changes needed
  for primitives and structs.
- **`TuriStruct` already carries the struct name** (`as_struct->name`) and
  field values (`as_struct->fields[]`). Field *names* are available in
  `StructDef->fields[i].name` at the `make-struct` call site in `eval.c` -- a
  one-line addition passes them through to `TuriStruct`.
- **Heap-allocated stdlib types** (`list`/`Cons`, `option`, `vec`, `pair`) are
  all returned as raw `int64_t` with tag `TURI_INT`. The runtime cannot
  distinguish a plain `42` from a list pointer without additional type
  information from the compiler. This remains the hard problem.

---

## Sub-tasks

### SI4-A -- Struct auto-show in the REPL

**Effort:** ~1 day  
**Compiler changes:** None  
**Files:** `src/turi/eval.c`, `src/turi/value.h`, `src/turi/value.c`,
`web/main.js` (no change needed -- already uses `turi_wasm_eval`)

**Goal:** `(make-struct Point 3 4)` in the REPL prints `Point { x = 3, y = 4 }`
instead of `#<struct Point>`.

#### Task list

- [ ] **Add `field_names` to `TuriStruct`** (`src/turi/eval.c` line ~223)

  ```c
  struct TuriStruct {
      const char  *name;
      uint32_t     n_fields;
      TuriValue   *fields;
      const char **field_names;  /* <-- add this; interned strings from StructDef */
  };
  ```

- [ ] **Populate `field_names` in `make_struct_val`** (`src/turi/eval.c` line ~229)

  Change the signature to accept `const char **field_names` and copy the
  pointer array (the strings themselves are interned and outlive the struct).

  The call site at line ~1004 already has `e->as.make_struct_.def`, which
  holds `def->fields[i].name` for each field. Pass these names through.

  ```c
  // At call site (eval.c ~line 1004):
  const char *field_names_buf[MAX_EVAL_ARGS];
  StructDef *def = e->as.make_struct_.def;
  for (uint32_t i = 0; i < n; i++)
      field_names_buf[i] = def ? def->fields[i].name : NULL;
  return make_struct_val(sname, n, fields, field_names_buf);
  ```

- [ ] **Update `turi_value_repr` for `TURI_STRUCT`** (`src/turi/eval.c` line ~1490)

  Replace the `#<struct %s>` placeholder with a proper `"Name { f1 = v1, ... }"`
  format. Recursively call `turi_value_repr` on each field value.

  ```c
  case TURI_STRUCT: {
      TuriStruct *s = v.as_struct;
      if (!s) { snprintf(buf, cap, "nil"); break; }
      int written = snprintf(buf, cap, "%s {", s->name ? s->name : "?");
      for (uint32_t i = 0; i < s->n_fields && written < (int)cap; i++) {
          char field_buf[256];
          turi_value_repr(field_buf, sizeof(field_buf), s->fields[i]);
          const char *fname = (s->field_names && s->field_names[i])
                              ? s->field_names[i] : "?";
          written += snprintf(buf + written, cap - (size_t)written,
                              "%s %s = %s",
                              i == 0 ? "" : ",", fname, field_buf);
      }
      snprintf(buf + written, cap - (size_t)written, " }");
      break;
  }
  ```

- [ ] **Update `turi_print_value` in `src/turi/value.c`** the same way (mirrors
  `turi_value_repr`; used by the native REPL). Requires exposing `field_names`
  via `value.h`.

- [ ] **Manual smoke test:** In the native REPL (`just repl`):
  - `(make-struct Point 3 4)` → `Point { x = 3, y = 4 }`
  - Multi-field structs render all fields with correct names.
  - Nested structs (struct fields that are themselves structs) render recursively.

- [ ] **Web REPL smoke test:** `web/main.js` already calls `turi_wasm_eval`
  which calls `turi_value_repr`, so the web REPL gets the improved struct
  display automatically once `turi_value_repr` is updated. Rebuild WASM with
  `just wasm` and verify in the browser.

---

### SI4-B -- Primitive auto-show (bool, float, cstr)

**Effort:** Already done (free)  
**Compiler changes:** None  

`turi_value_repr` already handles these correctly:
- `TURI_BOOL` → `"true"` / `"false"`
- `TURI_INT` → `"%lld"` (correct for plain integers)
- `TURI_FLOAT` → `"%g"` (matches `Show [float32]` convention)
- `TURI_CSTR` → `"\"%s\""` (quotes around the string, fine for REPL display)

No work needed here.

---

### SI4-C -- Heap-allocated stdlib types (list, option, vec, pair)

**Effort:** 2--3 weeks  
**Compiler changes:** Yes -- significant  
**Files:** `src/elab.c`, `src/codegen.c`, `src/turi/eval.c`, `src/turi/value.h`,
`src/wasm_glue.c`, `web/main.js`

**Goal:** `(cons 1 (cons 2 (nil-value)))` in the REPL prints `"[1, 2]"` instead
of a raw pointer integer.

**The core problem:** All heap-allocated stdlib types (`Cons`, `option`, `vec`,
`Pair`) return `int64_t` from their constructor functions, which the evaluator
stores as `TURI_INT`. The runtime cannot distinguish `42` from a list pointer
without additional type information from the static type-checker.

#### Option A -- Runtime type-tag wrapper (recommended)

Wrap all `int64_t`-returning expressions at the eval boundary with a
`(value, type_tag)` pair. The type tag is a compiler-emitted string (e.g.
`"list"`, `"option"`, `"vec"`, `"pair"`, `"int"`) derived from the static type
of the top-level expression.

**Tasks:**

- [ ] **Add `type_tag` field to the eval/REPL result protocol.**  
  In `elab.c`, after elaborating a top-level `(defn main ...)` or REPL input
  expression, record the Turmeric static type as a `const char *type_tag`
  alongside the `int64_t` result.

- [ ] **Emit type tag alongside REPL results in `codegen.c`.**  
  The code generator already knows the static type of every expression.  
  For top-level REPL expressions, emit a parallel `const char *__repl_type`
  variable alongside the result value.

- [ ] **Thread `type_tag` through `turi_eval` → `turi_wasm_eval`.**  
  Extend `TuriValue` or add a parallel out-parameter so the caller gets both
  the `int64_t` result and the type tag string.

- [ ] **Implement `turi_show_result(int64_t val, const char *type_tag)`
  in `src/wasm_glue.c`.**  
  Dispatch table over known type tags:

  ```c
  const char *turi_show_result(int64_t val, const char *type_tag) {
      if (!type_tag || strcmp(type_tag, "int") == 0)
          return show_int(val);
      if (strcmp(type_tag, "list") == 0 || strcmp(type_tag, "Cons") == 0)
          return show_list(val);      /* iterates Cons chain */
      if (strcmp(type_tag, "option") == 0)
          return show_option(val);    /* reads is_some / value */
      if (strcmp(type_tag, "vec") == 0)
          return show_vec(val);       /* reads data / len */
      if (strcmp(type_tag, "Pair") == 0)
          return show_pair(val);      /* reads first / second */
      /* fallback */
      char *buf = malloc(32);
      snprintf(buf, 32, "%lld", (long long)val);
      return buf;
  }
  ```

  Each `show_*` helper mirrors the C inline logic already in the stdlib
  `Show` instances (`stdlib/list.tur`, `stdlib/option.tur`, etc.).

- [ ] **Update `web/main.js`** to pass the type tag returned by the extended
  `turi_wasm_eval` into `turi_show_result`, and display the resulting string.

- [ ] **Update the native REPL** (`src/turi/eval.c` or equivalent) to call
  `turi_show_result` after evaluating each top-level expression.

- [ ] **Smoke tests:**
  - `(cons 1 (cons 2 (nil-value)))` → `"[1, 2]"`
  - `(pair-new 10 20)` → `"(10, 20)"`
  - `(some 99)` → `"some(99)"`
  - `(none)` → `"none"`
  - `(+ 1 2)` → `"3"` (plain int, no regression)

#### Option B -- Tagged value struct in generated C (alternative)

Instead of passing type tags at the eval boundary, change the C ABI so all
values are wrapped in `{ int64_t value; const char *type_tag; }` rather than
bare `int64_t`. This is a larger breaking change to the code generator but
eliminates the need for a separate type-tag channel.

This is the more architecturally sound long-term approach but would require
updating all generated C code and all stdlib inline C blocks. Not recommended
as an immediate next step.

---

## Prior art: how other languages handle type tags

Understanding how other languages solve this problem informs which option to
choose for SI4-C.

### GHC Haskell -- dictionary passing (most relevant)

GHC never embeds a runtime type tag in a value. Instead, the typeclass
dictionary (a record of function pointers, one per method) is passed as an
implicit extra argument at every polymorphic call site. The compiler statically
resolves which `Show` dictionary to use before evaluation; the value itself
carries no type information.

```haskell
-- show x  (x :: Int)  compiles roughly to:
showDict_Int.show x
```

For GHCi's REPL, the type inferencer resolves the type of the top-level
expression *before* evaluation, selects the right dictionary, and threads it
in. The result value is still an untagged machine word at runtime.

**Implication for Turmeric:** SI4 Option A (pass the static type tag alongside
the `int64_t` result at the REPL boundary) is the GHC-equivalent approach.
The compiler already knows the static type; it just needs to surface it once
at the eval boundary rather than embed it in every value.

### OCaml -- low-bit tag + GC block headers

OCaml uses a hybrid:
- **Immediates** (int, bool, char): the LSB is `1`. Value = `(n << 1) | 1`.
  One bit of every integer is permanently borrowed for the tag.
- **Heap pointers**: LSB is `0`. The pointed-to block begins with a header
  word: `[ size (22 bits) | GC color (2 bits) | tag (8 bits) ]`.

Structural types (tuples, records, variants) use tag byte `0`; strings `252`;
floats `253`; closures `246`. The GC reads these tags to know how to trace.

**Cost:** 63-bit integers on 64-bit hardware; one header word per heap object.

### Lua 5.x -- explicit tagged union

Lua uses a structure nearly identical to Turmeric's `TuriValue`:

```c
typedef struct TValue {
    union { /* value */ } value_;
    lu_byte tt_;   /* type tag: LUA_TNIL, LUA_TBOOLEAN, LUA_TNUMBER, ... */
} TValue;
```

No bits are stolen from values; the tag is a separate byte. LuaJIT replaces
this with NaN boxing for performance (see below).

### NaN boxing -- LuaJIT, JavaScriptCore

A 64-bit IEEE double has ~2^52 spare NaN bit-patterns. These are used to
encode non-double values:

```
Normal double:  any non-NaN 64-bit pattern
NaN-boxed value: [ 1 | 11111111111 | 1 | tag (3 bits) | payload (48 bits) ]
```

48 bits is enough for any pointer on current x86-64 hardware. Tags
distinguish: `object`, `string`, `boolean`, `null`, `undefined`, `int32`.

**Pros:** Every value fits in one 64-bit register with no separate tag word.  
**Cons:** Real doubles require a round-trip check; only ~7 distinct tags fit
without another indirection level.

### Ruby / SBCL / Scheme -- pointer tagging (low 3 bits)

These languages use the low 2--3 bits of a `uintptr_t` to encode the type of
immediates, relying on heap objects being aligned (so their low bits are always
`000`):

```
Ruby (low 3 bits):
  001  fixnum (int shifted left by 1)
  000  heap pointer -> RObject with flags word
  010  float
  000/special  false, true, nil, undef
```

SBCL (Common Lisp) uses 3 low bits similarly, with a second-level **widetag**
byte in the block header for heap objects needing finer discrimination (bignum,
simple-vector, hash-table, etc.).

### Summary table

| Language | Approach | Value overhead |
|---|---|---|
| GHC Haskell | Dictionary at call site; no tag in value | None |
| OCaml | LSB=1 for int; block header tag for heap | 1 bit/int; 1 word/heap obj |
| Lua 5.x | Explicit tagged union (like `TuriValue`) | 1 extra byte per value |
| LuaJIT | NaN boxing | None (fits in 64 bits) |
| Ruby / SBCL | Low bits of pointer + object header | 1 bit/int; 1 word/heap obj |

### Recommendation for Turmeric

The **GHC approach** (SI4-C Option A) fits Turmeric's architecture best:
- No changes to value representation or generated C.
- No overhead on ordinary (non-REPL) code paths.
- The static type is already known by the compiler at every call site.
- Only the REPL boundary needs the type tag, not every value in the program.

NaN boxing or low-bit tagging would require changing Turmeric's entire value
representation and all generated C code -- worthwhile only if Turmeric later
adopts a GC that needs to trace heap objects, at which point a block-header
approach (like OCaml's) would be more natural than NaN boxing anyway.

---

## Recommended execution order

1. **SI4-A** (struct show) -- high value, low risk, ~1 day.
2. **SI4-B** -- already done.
3. **SI4-C Option A** -- after SI4-A lands, if REPL quality for stdlib types is
   a priority. Scope it as its own plan when ready to start.

---

## Open questions

1. **Should `turi_value_repr` for `TURI_CSTR` drop the surrounding quotes?**  
   **Decision: keep quotes.** The REPL prints `"hello"` (with quotes), matching
   GHCi, Python, and Clojure conventions. This makes the type visible at a glance
   and distinguishes `"42"` (a string) from `42` (an int). `Show [cstr]` returning
   the bare string is correct for user-facing output; the REPL repr is a separate
   concern.

2. **Depth limit for recursive struct repr?**  
   **Decision: hard limit of 4, truncate with `#<struct TypeName>`.**  
   Add a `depth` parameter to `turi_value_repr` (and `turi_print_value`),
   decrementing on each recursive call. When `depth` reaches 0, emit
   `#<struct TypeName>` (using the name already stored in `TuriStruct->name`)
   rather than expanding the fields. Example output at the limit:

   ```
   Outer { inner = Inner { deep = #<struct Deep> } }
   ```

   The public signatures stay clean by wrapping in a top-level entry point
   that passes the initial depth:

   ```c
   /* Internal: depth-limited repr. */
   static void turi_value_repr_d(char *buf, size_t cap, TuriValue v, int depth);

   /* Public entry point: always starts at depth 4. */
   void turi_value_repr(char *buf, size_t cap, TuriValue v) {
       turi_value_repr_d(buf, cap, v, 4);
   }
   ```

3. **Memory ownership of `turi_show_result` return values (SI4-C only).**  
   **Decision: per-evaluation scratch arena.**  
   A bump arena is allocated at the start of each top-level REPL evaluation and
   reset (not freed) after the result string is printed. All `show_*` helpers
   allocate into this arena via an `arena_alloc(arena, size)` helper rather than
   calling `malloc` directly. No per-call `free` is needed; nested calls (e.g.
   `show_list` calling `show_int` on each element) compose naturally without
   coordination.

   The arena must be implemented before SI4-C begins. Suggested interface:

   ```c
   typedef struct ShowArena {
       char  *buf;
       size_t cap;
       size_t used;
   } ShowArena;

   ShowArena  show_arena_new(size_t cap);   /* malloc's backing buffer */
   void      *show_arena_alloc(ShowArena *a, size_t size);
   void       show_arena_reset(ShowArena *a);  /* resets used=0, keeps buffer */
   void       show_arena_free(ShowArena *a);   /* frees backing buffer */
   ```

   A single `ShowArena` lives in `TuriEnv` (or is passed as a parameter through
   the eval call stack). `turi_wasm_eval` resets the arena before each call and
   reads the result string before the next reset.

   Note: SI4-A (struct show via `turi_value_repr`) uses a stack-allocated `char`
   buffer today and does not require the arena. The arena is only needed for the
   SI4-C heap-type helpers where output size is unbounded (e.g. a list of 10,000
   elements).
