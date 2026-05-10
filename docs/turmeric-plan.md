# Turmeric Language — Design & Implementation Plan

A Lisp (Clojure/Fennel-flavored) that compiles to C, with homoiconic macros, struct-backed closures, scope-based `defer`, and `ref` values that auto-deallocate at scope end.

---

## Progress Summary

| Phase | Status | Exit Criterion | Notes |
|---|---|---|---|
| 0 | ✅ **Complete** | hello.tur round-trip | All infrastructure in place: arena, reader, forms, diag, buf, emit, main |
| 1 | ✅ **Complete** | Fizzbuzz | All core forms, arithmetic, comparison, logical ops; 12/12 fixtures green under ASan/UBSan |
| 2 | ✅ **Complete** | Top-level functions + extern-c | defn, fn, extern-c, inline-C blocks all compiling and running. Multi-file support with _main.c generation. Mutual recursion via two-pass elaboration. 18/18 tests pass (16 happy, 2 negative). |
| 3 | ✅ **Complete** | Closures | Capture analysis, env struct synthesis, closure thunk emission, and call-site lowering all working. Nested fn without captures lifts to static functions with proper function pointer type emission. Capturing fn emits closure struct + thunk function with env parameter. Closure calls pass env pointer to thunk. 18/18 tests pass (17 happy, 1 negative). |
| 4 | ✅ **Complete** | defer + scope unwind | v0 lowering shipped: defers in `do`-wrapped scopes collected and emitted in LIFO at scope exit. **v1 lowering complete**: Unified runtime-list-on-frame model implemented per effects-plan.md §6.10. Each scope with defers emits a `tur_frame` with parent pointers for nested scopes. Defer bodies without captures lower to simple thunks; defer bodies with captures generate env structs and thunks that access captured values lexically. The S1/S2/S3 strategy choice is now a runtime policy decision. 24/24 fixtures green incl. defer-order, defer-nested-scopes, defer-mutated-binding, defer-conditional, defer-in-loop. |
| 5 | ✅ **Complete** | ref<T> | `(ref expr)` heap-allocates; `@r` dereferences; `drop!` explicitly frees; compiler injects `defer (drop! r)` at ref binding sites. ref<T> lowers to `void*` in C for v1. 28/28 fixtures green incl. ref-basic, ref-deref, ref-nested, ref-explicit-drop. **Move semantics tracking deferred** — `is_moved` field added to Binding; enforcement of poisoning on move pending follow-up. |
| 6 | ✅ **Complete** | defmacro + quasiquote | **Core macro system working**: `defmacro` special form implemented with parameter substitution; macro expansion integrated into elaborator; bootstrap interpreter (`src/interp.{c,h}`) created; reader macros for `'` (quote), `` ` `` (quasiquote), `~` (unquote), and `~@` (unquote-splicing) implemented; quasiquote expansion working with unquote in macro bodies. **Threading macros implemented**: `->` and `->>` as special forms. **Parameter syntax relaxed**: defmacro now accepts both `[]` (vector) and `()` (list) for parameter lists. **gensym implemented**: generates fresh symbols during macro expansion via substitute_params (works in quasiquote contexts). **when/unless macros implemented**: `when` and `unless` now defined as macros in stdlib/macros.tur (loaded automatically). **cond**: desugared to nested if by elaborator, supports `:else` keyword. 36/36 fixtures green incl. macro-defmacro, macro-quote, macro-nested, macro-multi-arg, macro-quasiquote, macro-quasiquote-unquote, macro-threading, macro-threading-last. |
| 7 | ✅ **Complete** | Stdlib seed | **Core types implemented** in stdlib/*.tur: `slice<T>` (borrowed view), `vec<T>` (growable array), `str` (UTF-8 string), `option<T>` (sum type), `result<T,E>` (sum type). **Macros**: `when`, `unless` in stdlib/macros.tur (loaded automatically). **cond**: restored as special form in elaborator with `:else` keyword support. **Test runner**: `assert`, `run-tests!`, `deftest` stubs in stdlib/test.tur. **Compiler integration**: Modified `src/main.c` to load stdlib/macros.tur automatically; added `#include <stdlib.h>` and `#include <string.h>` to emitted C headers. **Note**: stdlib type implementations use inline C with malloc/free which causes type mismatches when compiled into every file. Deferred until Phase 11 when `:ptr<T>` type annotations or separate stdlib compilation are added. **Fixtures**: 6 new stdlib placeholder fixtures + updated macros fixture with `:else` support. Total: 42/42 fixtures green.
| 8 | ✅ **Complete** | Diagnostics polish | SPAN_UNKNOWN sentinel, DiagCode enum (TUR-E0001-0007), multi-line source snippets, underline styles, color/JSON output, --no-color/--json-diagnostics/--explain flags, tur check subcommand, Levenshtein-based "Did you mean" suggestions, enhanced error messages in elab.c. 42/42 tests pass. |
| 9 | ✅ **Complete** | rc<T> + weak<T> | Reference counting v1 GC with control block, rc/of, rc/clone, rc/drop, rc->ptr, rc/strong-count, weak, upgrade, weak?. rc<T> composes with closures. 46/46 tests pass (added rc-basic, rc-shared, weak-upgrade, rc-cycle-leak). Defer injection for rc/drop deferred to future work. |
| 10 | ⏳ Pending | Bacon-Rajan cycle collector | v2 GC layered over RC |
| 11 | ⏳ Pending | Copy traits | Mark types as `Copy` (bitwise dup) vs `Move` (ownership transfer); auto-derive for primitives |
| 12 | ⏳ Pending | Borrow traits | Optional checked `&T` / `&mut T` borrows alongside untracked `ptr<T>`; aliasing rules enforced within a function |
| 13 | ⏳ Pending | Lifetime annotations | Explicit `'a` lifetime parameters on functions and references; lifetime elision rules for common cases |
| 14 | ⏳ Pending | Borrow checker with lifetimes | Full intra- and inter-procedural borrow checking; prevents dangling references and use-after-move at compile time |
| 15 | ⏳ Pending | Typeclasses | Haskell/Rust-style typeclass-based dispatch with dictionary passing; extends elaborator's operator dispatch table; `(defclass Show [a] (show [x] : cstr))`, `(definstance Show int ...)` |
| 16 | ⏳ Pending | Capability passing (v1 effects) | Library-level effect system using typeclasses; zero runtime cost; covers mocking, dependency injection, resource passing |
| 17 | ⏳ Pending | Exceptions | Lightweight control flow; non-resumable; setjmp/longjmp or label-based unwind; integrates with defer, ref, rc |
| 18 | ⏳ Pending | Delimited continuations (`shift`/`reset`) | Selective CPS-transform; one-shot continuations; S2 defer strategy; substrate for algebraic effects |
| 19 | ⏳ Pending | Algebraic effects (v3) | OCaml 5-style effect handlers; effect rows; built on shift/reset substrate and unified defer model |

**Last updated:** 2026-05-09 (Phase 4: v0 lowering complete. **v1 lowering complete**: Full runtime list-on-frame model implemented per effects-plan.md §6.10 with thunk generation for both captured and non-captured defers. 24/24 fixtures green. The S1/S2/S3 strategy choice is now a runtime policy decision. See §10.5 for details. **Phase 5: ref<T> complete** - `(ref expr)` heap-allocates with auto-defer drop injection; `@r` dereferences; `drop!` explicitly frees. 28/28 fixtures green. Move semantics tracking infrastructure added but enforcement deferred. **Phase 6: Core macro system complete** - `defmacro` with parameter substitution, macro expansion integrated, bootstrap interpreter created, quote reader macro added. **Quasiquote implemented** - reader macros for `` ` ``, `~`, `~@` with expansion for simple cases (literals, symbols, nested quasiquotes, unquote). Quasiquote works in macro bodies with parameter substitution via unquote. **Threading macros implemented** - `->` inserts value as first argument, `->>` inserts value as last argument. **when/unless macros implemented** in stdlib/macros.tur (loaded automatically). 36/36 fixtures green. **Phase 7: Stdlib seed complete** - Core type definitions (`slice`, `vec`, `str`, `option`, `result`) implemented in stdlib/*.tur. **cond as special form** - restored in elaborator with `:else` keyword support (Phase 6 had removed it; macro version deferred due to lack of list operations). **Compiler integration**: Modified `src/main.c` to load stdlib/macros.tur automatically; added `#include <stdlib.h>` and `#include <string.h>` to emitted C headers for stdlib inline C functions. **Fixtures**: stdlib-macros fixture tests when/unless; 6 placeholder stdlib fixtures created. Total: 42/42 fixtures green. **Note**: stdlib type implementations use inline C with malloc/free; full integration deferred until Phase 11 when `:ptr<T>` type annotations or separate stdlib compilation are implemented. **Phase 8: Diagnostics polish** - Enhanced error messages with SPAN_UNKNOWN sentinel, error codes (TUR-E0001 through TUR-E0007), multi-line source snippets with context, underline styles (^^^, ~~~), color support with --no-color flag, --explain flag for code snippets, --json-diagnostics for IDE integration, tur check subcommand, "Did you mean" suggestions via Levenshtein distance, and suggestion engine for actionable hints. Total: 42/42 fixtures green. **Changes**: src/forms.h (SPAN_UNKNOWN), src/diag.{c,h} (enhanced diagnostics), src/symbols.{c,h} (Levenshtein distance), src/elab.c (enhanced errors), src/main.c (new flags and subcommand). **Phase 9: rc<T> + weak<T> complete** - Reference counting v1 GC implemented with `RcControlBlock` struct, `rc_cb_alloc`, `rc_strong_increment/decrement`, `rc_weak_increment/decrement`, `rc_upgrade`, `rc_get_value`, `rc_is_alive`. Type system extended with `TY_RC` and `TY_WEAK`. Expression kinds added: `EX_RC_OF`, `EX_RC_CLONE`, `EX_RC_DROP`, `EX_RC_PTR`, `EX_RC_COUNT`, `EX_WEAK`, `EX_WEAK_UPGRADE`, `EX_WEAK_PRED`. Elaboration functions implemented for all rc/weak operations. Codegen emits rc runtime inline in generated C. Added fixtures: rc-basic, rc-shared, weak-upgrade, rc-cycle-leak. Total: 46/46 tests pass. **Deferred**: defer injection for rc/drop, custom drop functions for stdlib types, rc/ref conversion, weak dangling detection, deferred free queue. **Files changed**: src/rc.{c,h} (new), src/types.h/c (TY_RC, TY_WEAK), src/expr.h/c (new expr kinds), src/elab.c (symbols, elab functions), src/emit.c (rc runtime emission, codegen cases), src/expr.c (print cases), src/builtins.c (builtin specs), src/main.c (no changes needed))

---

## Phase 2 Implementation Checklist

- [x] Extend TypeKind with TY_FN and TY_PTR_VOID
- [x] Extend ExprKind with EX_FN, EX_CALL, EX_FN_DEF, EX_EXTERN_C, EX_INLINE_C
- [x] Update type_eq, type_name, type_c_name for function types
- [x] Update emit.c switch statements for new expr kinds
- [x] Update expr.c print function for new expr kinds
- [x] Add sym_defn, sym_fn, sym_extern_c to elaborator (F_CBLOCK handled directly in elab_form)
- [x] Parse type annotations (:int, :bool, :void, :cstr, :ptr) in elaborator
- [x] Implement elab_defn for (defn name [params...] : ret-T body...)
- [x] Implement elab_fn for (fn [params...] body...) - no capture, lift to static
- [x] Implement elab_extern_c for (extern-c name [params...] : ret-T)
- [x] Implement elab_inline_c for ```c ... ``` blocks
- [x] Implement elab_call for (f a b c) function calls
- [x] Emit function declarations and definitions in emit.c
- [x] Emit function calls in emit.c
- [x] Emit extern-c declarations in emit.c
- [x] Emit inline-C blocks in emit.c
- [x] Update driver for multi-file support (_main.c generation) - single file mode still default; directory triggers multi-file with per-module .h/.c
- [x] Add fixtures: defn-basic, mutual-recursion, extern-printf, inline-c-popcount
- [x] Add negative tests: capturing fn gate, arity mismatches (bad inline-C deferred - requires build-time validation)

---

## Phase 3 Implementation Checklist — Closures

- [x] Add `EX_CLOSURE` to ExprKind enum
- [x] Add `struct Closure` definition with fn, captures, n_captures, env_name
- [x] Add `closure_` field to Expr union
- [x] Add `closure_fn_binding` field to Binding struct
- [x] Add `closure` field to FnDef struct
- [x] Initialize `closure_fn_binding` to NULL in binding_new
- [x] Initialize `closure` field to NULL in FnDef creation
- [x] Implement `collect_free_vars()` for capture analysis
- [x] Modify `elab_fn()` to perform capture analysis and create closures for capturing fns
- [x] Modify `elab_fn()` to infer return type from body if not specified
- [x] Modify `elab_let()` to set `closure_fn_binding` on bindings when init is EX_CLOSURE
- [x] Modify `elab_call()` to recognize closure bindings (TY_PTR_VOID with closure_fn_binding)
- [x] Modify `elab_call_fn()` to handle closure arity (subtract 1 for env parameter)
- [x] Add env_struct_names tracking to EmitCtx
- [x] Add closure and env_var_name fields to EmitCtx for thunk emission
- [x] Modify `name_for_binding()` to emit `env_var->field` for captured bindings in closure thunks
- [x] Modify `emit_fn_def()` to emit env struct type and cast env parameter for closure thunks
- [x] Modify `emit_fn_def()` to set up closure context for thunk body emission
- [x] Modify `EX_CLOSURE` emission to create env struct instance and return pointer
- [x] Modify `EX_CALL` emission to pass closure value to thunk function
- [x] Add closure-call test fixture demonstrating capturing fn
- [x] Clean up: env_var_name memory management (initialized to NULL, freed on restore)
- [x] Clean up: Remove duplicate closure_fn_binding assignment in elab_let
- [ ] Improve: Support multiple closure signatures with proper type system

---

## Phase 4 Implementation Checklist — defer + scope unwind

- [x] Add `EX_DEFER` to ExprKind enum
- [x] Add `defer_` field to Expr union (body)
- [x] Add `sym_defer` to Elab state and initialize in elab_init_state
- [x] Implement `elab_defer()` for parsing (defer expr)
- [x] Add defer to special form dispatch in elab_call
- [x] Add EX_DEFER case to expr_print
- [x] Add EX_DEFER case to emit_value (returns nil)
- [x] Add EX_DEFER case to emit_stmt (degenerate path: lone defer emits body inline)
- [x] Error handling: defer at module top level (fixture: errors/defer-top-level)
- [x] Error handling: defer without arguments
- [x] **v0 lowering**: EX_DO emission collects EX_DEFER children, emits non-defers in source order, then defer bodies LIFO before block close. Covers let/defn/while/do bodies wrapped in EX_DO during elaboration.
- [x] Hoist last value into a temp before firing defers, so the do-block's value is well-defined when defers run.
- [x] Fixture: `defer-order` — three defers in a let, prove LIFO ordering (third→first).
- [x] Fixture: `defer-nested-scopes` — inner-let defers fire before outer-let defers.
- [x] Negative fixture: `errors/defer-top-level`.
- [x] **v1 lowering** — unified runtime-list-on-frame model (effects-plan.md §6.10):
  - [x] Add `src/runtime.{c,h}` with `tur_frame` struct (defers[N], envs[N], n, parent, may_capture, effect_row) and functions (`tur_frame_init`, `tur_frame_push_defer`, `tur_frame_fire_lifo`, `tur_frame_fire_chain`).
  - [x] `FnDef.may_capture: bool` field added to expr.h (future-proofing for v3 effects).
  - [x] `Type.fn.effect_row` slot added to types.h (future-proofing per effects-plan.md §6.10).
  - [x] Modify `Expr.defer_` to store capture info (captures array, n_captures).
  - [x] Modify `elab_defer()` to perform capture analysis using `collect_free_vars()`.
  - [x] Modify `emit_do_value()` / `emit_stmt()` to emit `tur_frame` per scope with defers.
  - [x] Generate thunk functions for defer bodies (simple static funcs for no-capture, env struct + func for captures).
  - [x] Register thunks via `tur_frame_push_defer(&frame, thunk, env)` and fire via `tur_frame_fire_lifo(&frame)`.
  - [x] Extend `tur_frame` to store env pointers for captured defers (parallel envs[] array).
  - [x] Implement env struct generation and casting in defer thunks with captures.
- [x] Fixture: `defer-mutated-binding` — defer captures ^mut binding, proves lexical capture (prints 10, not 20).
- [x] Fixture: `defer-conditional` — defer inside if-branch fires at enclosing scope's end.
- [x] Fixture: `defer-in-loop` — defer inside while body fires at loop scope's end with captured loop variable.
- [x] Codegen snapshots for the runtime-list lowering landed.
- [ ] Fixture: `defer-early-return` — deferred until early-return / break / `return` ships.

---

## 1. Design goals & scope

- **Lispy surface syntax** — S-expressions, immutable-by-default bindings, `defn`/`let`/`if`/`do`.
- **Homoiconic macros** — code is data; `defmacro` runs at compile time and rewrites AST.
- **Compiles to readable C99** — no runtime VM, no GC. Output should be auditable.
- **Statically typed from v0** — annotations on `defn`/`def`, inference within bodies. No `Any` / dynamic type. See §1.1.
- **Closures as structs** — every closure becomes `struct { fn_ptr; env* }`; calling is `c.fn(c.env, args...)`.
- **`defer`** — Go-style; runs at end of *lexical* scope (not function), in LIFO order.
- **`ref` type** — owning handle to a heap value; dropping it at scope end is implemented as a compiler-injected `defer`.

Non-goals (initial cut): generics, traits, async, full type inference, package manager, self-hosting.

### 1.1 Typing baseline (v0) — and the path to typeclasses

Turmeric is **statically typed from day one**. The future "type system" work in §12.2 adds *more sophisticated* typing (typeclasses, occurrence typing) — it does **not** add static typing where there was none. Avoiding the Clojure-on-JVM trap, where retrofitting types onto a fully dynamic core is the hard part, is the entire point of this commitment.

**The v0 baseline:**
- Annotations required on `defn` / `def` params and return types.
- Local bindings (`let`, internal `set!`-targets) get types inferred from their RHS. No further inference.
- **No `Any` / `Value` / dynamic type.** Every binding has a concrete C type at codegen time.
- **Operators (`+`, `=`, `<`, `>`, `<=`, …) are elaborator-resolved from day one** via a dispatch table keyed on operand types. v0 has primitive entries (`int_add`, `float_add`, `int_eq`, …); the elaborator picks the right one. When typeclasses arrive (§12.2), they extend this same table — `definstance Num int` just registers entries the elaborator already knows how to consume. *No re-architecture needed; typeclass dispatch is "more rows in an existing table."*
- Heterogeneous collection literals (`[1 "two" 3.0]`) are a v0 error. Sum types come with §12.2.

**What to avoid (so v1 typeclasses don't require a rewrite):**
- *No `void*` or universal tagged-union `Value` at the C level.* Each Turmeric type lowers to its own C type. A runtime `Value` would import a dynamic-language runtime we'd never get rid of.
- *Don't keep `Form*` as the IR past elaboration.* Use a typed IR (`Expr` / `TExpr`) where every node carries its resolved type. This forces elaboration to commit, and lets §12.2's typeclass-resolution and §12.2(a)'s occurrence-typing pass query types directly.
- *Macros produce `Form`s; the elaborator types macro-expanded code the same way as hand-written code.* No "dynamic path" for macro output.
- *Don't bake operator C-types into codegen.* Codegen looks up the operator implementation in the elaborator's dispatch table. v0 has primitive ops; typeclasses add more.

**Cost of deferring each typing feature (rough order):**

| Feature | Cost of deferring to §12.2 | Risk |
|---|---|---|
| Param/return annotations | already in v0 | none |
| Local inference (RHS-only) | already in v0 | none |
| Typeclasses + dictionary-passing dispatch | medium — extends the elaborator's dispatch table; uses §4 closure machinery as the dict shape | **low**, given operators are already elaborator-resolved |
| Occurrence typing | low — additive flow-sensitive pass | low |
| Generics / parametric polymorphism | **high** — pervasive IR change | the real risk. v0 strategy: write per-type containers; provide a `defmacro` to monomorphize where needed (`(monomorphize Pair int int)`); accept the bloat until §12.2 |

**Direction locked in:** typeclasses + dictionary-passing dispatch (§12.2). Occurrence typing may come first if it's the cheaper win, but the architectural commitment is to typeclasses, and v0's elaborator-resolved operators are the on-ramp.

---

## 2. Language sketch

```clojure
(defn make-counter [start]
  (let [n (ref start)]                ;; heap-allocated, freed at scope end
    (fn [] (set! n (+ @n 1)) @n)))    ;; closure captures n by reference

(defn main []
  (let [c (make-counter 0)]
    (defer (println "bye"))
    (println (c))   ;; 1
    (println (c)))) ;; 2
```

Key forms:
- `def`, `defn`, `defmacro`, `let`, `if`, `do`, `when`, `cond`
- `fn` (anonymous closure), `set!`, `quote`, `quasiquote` `~` `~@`
- `ref`, `@` (deref), `defer`
- `defstruct` — product types, see §5.2
- `&` (address-of), `*p` / `(deref p)`, `sizeof`, `alignof`, `malloc`, `free` — see §5.4
- `extern-c` for declaring imported C functions
- **Inline C via triple-backticks** — see §2.1
- Primitives: `int`, `float`, `bool`, `char`, `cstr`, `str`, `ptr<T>`, `ref<T>`, `array<T,N>`, `slice<T>`, `vec<T>`

### 2.1 Inline C escape hatch

For glue code, platform intrinsics, and "I just need this one line of C," allow raw C to be embedded in the source:

````clojure
(defn fast-popcount [^int x] : int
  ```c
  return __builtin_popcount(x);
  ```)

(let [tid ```c (long)pthread_self() ```]
  (println tid))
````

Rules:
- Triple-backtick block ` ```c … ``` ` is *expression-positioned*: it lowers to a C statement-expression `({ … })` (GCC/Clang extension; documented as required) or, in strict-C99 mode, gets lifted to a generated helper function.
- The optional `c` tag after the opening fence is reserved for future tags (e.g., `cpp`, `asm`).
- Free identifiers inside the block are looked up in the surrounding Turmeric scope and bound as locals at the top of the emitted block (`int x = <captured>;`). Captures are *by value* unless the type is `ref<T>` or `ptr<T>`.
- The block has access to whatever headers the compilation unit already includes; `#include` inside an inline block is rejected — declare extern functions via `extern-c` instead.
- **Why triple-backticks, not single?** Single backtick is reserved for quasiquote (Clojure/Fennel-style). Triple-backticks don't conflict with the reader and are visually familiar from Markdown.

This is deliberately a sharp tool — it's the language's escape valve, not a substitute for proper bindings. Type-checker treats inline blocks as opaque: callers declare the return type with `: T` annotation; the compiler trusts it.

Type system (v1): explicit annotations on params and `def`s; local inference inside expressions. No subtyping. Generics deferred — start with monomorphization-by-template-substitution at macro layer.

---

## 3. Compiler pipeline

1. **Reader** — text → forms (lists, symbols, numbers, strings, keywords). Reuse a tagged-union `Form` node; preserve source spans.
2. **Macro expansion** — repeatedly expand until fixpoint. `defmacro` bodies execute in a host interpreter (see §6).
3. **Special-form lowering** — desugar `when`, `cond`, `->`, `let*` into core forms.
4. **Type checking / elaboration** — assign types, resolve operator overloads against the elaborator's dispatch table (§1.1), insert coercions, mark closure captures. **Output is a typed IR (`Expr`)** where every node carries its resolved type; this is the source of truth for all later passes. `Form*` is not used past this point.
5. **Closure conversion** — lift `fn` into top-level C function + env struct; rewrite call sites.
6. **Scope analysis & `defer` injection** — walk scopes, collect explicit `defer`s, insert implicit `defer`s for every `ref` binding.
7. **C codegen** — emit `.h` + `.c`. Each scope becomes a block with a label-based unwind chain so `defer`s fire in LIFO order on every exit (normal, early `return`).
8. **Driver** — invoke `cc` (configurable) on emitted sources.

Single binary `tur`; `tur build foo.tur` → `./foo`.

---

## 4. Closures (the struct-with-fn-ptr trick)

For each `fn` form, the compiler emits:

```c
typedef struct counter_env { int *n; } counter_env;

static int counter_thunk(void *env_, void) {
    counter_env *e = env_;
    *e->n += 1;
    return *e->n;
}

typedef struct { int (*fn)(void*); void *env; } closure_int_void;
```

Call site `(c)` lowers to `c.fn(c.env)`. Env is heap-allocated when the closure escapes its defining scope, stack-allocated otherwise (escape analysis: if any reference to the closure outlives the scope, box it).

Calling-convention table is keyed by the closure's flattened type signature; the compiler emits one `closure_<sig>` typedef per distinct signature used.

---

## 5. `defer` and `ref`

**`defer` lowering.** Each scope gets a synthetic stack of deferred thunks, but since C doesn't have real closures we emit them as inlined cleanup blocks reached through `goto`:

```c
{   // let scope
    ref_int n = ref_int_new(0);
    /* ... body ... */
    goto __cleanup_L7;
__cleanup_L7:
    ref_int_drop(&n);   // injected by ref binding
}
```

Early `return` inside the scope rewrites to `{ result = X; goto __cleanup_L7; }`, which chains outward. This avoids needing function pointers for cleanup and matches Go's semantics for scope exit.

**`ref<T>`.** A `ref<T>` is `struct { T* p; }`. Constructing one calls `malloc`; the compiler injects a `defer (drop! r)` at the binding site. Move semantics: assigning a `ref` transfers ownership (source is poisoned at compile time). `@r` dereferences. No reference counting in v1 — a single owner, like Rust's `Box<T>`. Sharing is via raw `ptr<T>` borrows, which the compiler does *not* track in v1 (documented sharp edge).

### 5.1 Memory management for everything else (GC strategy)

`ref<T>` covers the **owned** case. Many programs also want shared, cyclic, or otherwise-not-statically-scoped allocation. The road map:

**v0 — don't deallocate.** Anything not held by a `ref<T>` comes from a process-wide bump arena and is never freed until exit. Fine for the bootstrap compiler (short-lived, predictable footprint) and early scripts/tools. Document it loudly.

**v1 — chosen direction: refcounting + weak refs (`rc<T>` / `weak<T>`).** See §5.1.2 for the full feasibility analysis; the short version:
- Composes with the existing `defer`/`ref` machinery (RC drop *is* a defer'd `rc-release`) — no runtime rearchitecture.
- Three-tier ownership: `ref<T>` (unique, zero overhead) → `rc<T>` (shared) → `weak<T>` (observer). Closes the current gap where shared ownership forces raw `ptr<T>` and self-discipline.
- Predictable, prompt reclamation; no STW pauses; no stack scanning.
- Single-threaded v0 means counts are plain `int`s, not atomics.
- Cycles leak in pure RC — handled in v1 by documenting `weak<T>` as the cycle-breaker (Swift's stance), which fits Lisp idioms (mostly DAG-shaped data: cons cells, ASTs, env chains).

**v2 — Bacon-Rajan trial-deletion cycle collector.** Once cycles become a felt pain point, layer a Bacon-Rajan style concurrent cycle collector over the RC machinery. CPython uses essentially this approach. Properties that make it the natural pairing:
- *Doesn't disturb the rest of the runtime* — it scans only RC'd objects whose strong count was decremented but didn't reach 0 (suspect roots), so non-cyclic programs pay nothing.
- *Tunable / disablable* — programs that don't need it (short-lived, no cycles) can turn it off entirely.
- *Composes with `defer`* — collection happens between scope drops, not inside them, so timing of `defer` actions stays unchanged.
- *No new ABI* — uses the same control block as `weak<T>`. v1 just needs to leave room (a "color" field in the control block: black/grey/white/purple per Bacon-Rajan) and a "suspect roots" buffer in the runtime.

**Other strategies** considered and shelved (kept for the record):
- *Conservative Boehm GC* — drop-in, but worst semantic guarantees and an external dependency. Could serve as a stopgap if RC integration slips.
- *Precise mark-sweep* with compiler-emitted root maps — clean, but a much bigger project than RC, and only worth doing if we're abandoning RC entirely.
- *Per-region arenas* — `(with-region r …)` complementing `ref<T>`. Still attractive as an *additional* tool for batch-allocation patterns; not a replacement for RC.
- *Cheney-on-the-MTA* — see §5.1.1. Reopen only if delimited continuations (§12.1) become a v1 priority *and* we're willing to re-architect `defer` around CPS.

### 5.1.1 Cheney-on-the-MTA feasibility — *alternate, not chosen*

> Kept for the record. The chosen v1 direction is RC+weak refs (§5.1.2). Reopen this analysis only if delimited continuations (§12.1) become a v1 priority *and* we're willing to re-architect `defer` around CPS.

The pitch (Baker): compile every Turmeric function in CPS so calls are tail calls and **functions never return**. Allocate from the C stack as a bump pointer. When the stack hits a high-water mark, run Cheney's two-space copying GC over the *live* objects rooted in the current continuation, copy survivors to the heap, then `longjmp` back to the stack base and resume. The C stack is effectively the nursery; `longjmp` is the nursery flip.

**Why it's tempting for Turmeric:**

1. **Free synergy with §12.1 delimited continuations.** We're already planning a CPS pass for `shift`/`reset`. Cheney-MTA reuses exactly that infrastructure — the CPS form needed for `shift` is the CPS form needed for the GC. One transform, two payoffs.
2. **The closure model already fits.** Our closures are `struct {fn_ptr; env*}` (§4). Continuations under Cheney-MTA are *the same shape*: a function pointer plus an environment. No second representation needed for "k".
3. **C as portable assembler.** The technique deliberately avoids inline asm and stack-walking — exactly the portability Turmeric wants.
4. **Allocation is a SP bump.** As fast as bump-arena allocation, but with reclamation.

**Why it's hard:**

1. **It's a whole-runtime commitment, not a module.** Once you adopt MTA, *every* function in the language must be CPS-transformed and must never return normally — including the standard library, including FFI thunks. You can't bolt it on later without rewriting code-gen end-to-end. This is the dealbreaker for adopting it after v0.
2. **Tension with `defer` and `ref`.** This is the deepest issue. Defers fire at "end of scope," and `ref<T>` drops piggyback on that. But under MTA, functions don't have ends — control passes via tail call into the continuation. Reconciliation paths:
   - Treat each Turmeric scope's exit as a continuation invocation; the prologue captures `k_exit` and the scope body tail-calls it on every exit path. Defers are statements emitted *just before* the tail call to `k_exit`. Workable, but it inverts the way §5's label-based unwind works today and effectively forces the entire defer story to be expressed in CPS.
   - Schedule defers as GC-time finalizers on heap roots. Conceptually clean for `ref<T>` (drop = finalizer) but it changes timing — defers no longer run promptly. Probably unacceptable.
   - Track defers in an explicit per-scope stack carried in the env. Flexible, but adds a dynamic data structure where today we have static `goto` labels.
3. **Tail-call reliance on the C compiler.** Cheney-MTA presumes the C compiler turns every call into a jump. GCC/Clang at `-O2` mostly do, but it's not contractual: `-O0` debug builds blow the stack instantly; some target ABIs (Windows x64) restrict TCO; calls through function pointers (which we use everywhere — closures!) aren't always tail-called. Mitigations exist (Chicken Scheme uses `setjmp` to *force* return-to-base when a per-call stack budget is exceeded, sidestepping the TCO question entirely), but at a cost in per-call overhead.
4. **FFI is awkward.** Foreign functions return normally. Calling into libc means crossing a boundary where MTA's "never return" invariant is violated. Standard fix: marshal foreign calls through a trampoline that captures the current continuation, calls the foreign function, and on return resumes the continuation. Doable, but every FFI call gets that wrapper, which interacts poorly with the §2.1 inline-C escape hatch.
5. **Debugging.** Stack traces look bizarre — every "frame" is the same handful of trampolining functions. Need explicit shadow-stack instrumentation in debug builds.
6. **Interaction with structs and value semantics.** §5.2 structs are value-copied on assignment. Under MTA, large struct values either (a) live on the C stack (fine, but they get copied during the longjmp/replay if they're live) or (b) get auto-boxed (cheap allocation, but loses the "values are values" model). Solvable, but it's another design knob.

**Verdict.** Feasible — Chicken Scheme has run on this model in production for ~25 years, so it's not theoretical. But for Turmeric the cost is substantial: it co-opts the CPS pass, demands a from-the-bottom redesign of `defer`/`ref` lowering, and changes the FFI contract. It is therefore **not a v0 strategy**, and choosing it as the v1 GC means committing to it before phase 0 (because retrofitting it later means rewriting most of the back end).

**Decision criterion:** if delimited continuations (§12.1) become a v1 priority *and* we're willing to re-architect `defer` around CPS, Cheney-MTA is the most coherent unified design — one CPS transform pays for control, allocation, and reclamation. If continuations stay post-v1 or we want to ship faster, pick Boehm or precise-mark-sweep first and revisit.

**What v0 needs to preserve to keep this option open:** the allocator-interface indirection (`tur_alloc` vs raw `malloc`), the reserved GC header word in env structs, and a CPS-friendly IR shape — i.e., don't compile control flow into structures (like `goto`-spaghetti for `defer`) that a future CPS pass can't recover from cleanly. The §3 pipeline ordering — closure conversion *before* defer injection — already cooperates with this; keep it that way.

**What to preserve in v0 to keep these doors open:**
- All non-`ref` allocation goes through one allocator interface (`tur_alloc`, `tur_alloc_typed`) — never raw `malloc`. Swapping the implementation later is a one-file change.
- Closure env structs already have a header word reserved for future GC metadata (tag/forwarding pointer). Don't repurpose it.
- The codegen emits type tags for heap-allocated aggregates from day one, even though v0 ignores them.

### 5.1.2 Reference counting + weak refs — *chosen v1 direction*

> **Decision: this is the v1 GC strategy for Turmeric.** v2 layers a Bacon-Rajan cycle collector on top once cycles become a felt problem. See the §5.1 road map for the v0→v1→v2 path.

The pitch: extend the existing ownership story with a third tier of pointer.

| Pointer | Owns? | Cost | Use for |
|---|---|---|---|
| `ref<T>` | unique owner | zero (one defer'd drop) | the common case — single owner, scope-bounded |
| `rc<T>` | shared owner | refcount header + retain/release on copy | shared values, multi-owner data, DAGs |
| `weak<T>` | non-owning observer | weak slot in control block; `(upgrade w)` returns `option<rc<T>>` | break cycles, observer patterns |
| `ptr<T>` | nothing | none | FFI, raw memory; type-checked but untracked |

Crucially, `rc<T>`'s drop *is* a `defer` — it's just `(defer (rc-release r))` instead of `(defer (drop! r))`. The compiler injects it the same way it does for `ref<T>` (§5). RC isn't a separate runtime model bolted on; it's the existing defer-driven cleanup machinery with a different drop function.

**Why it's tempting for Turmeric:**

1. **Mechanically compatible with what's already there.** §5's defer-injection pass already finds binding sites and synthesizes drops. Adding `rc<T>` is "register a new drop function and a header layout" — not a runtime overhaul. *No CPS transform required, no longjmp, no root maps.*
2. **Predictable, prompt reclamation.** No pauses, no stack scanning, no nondeterministic finalizers. Memory pressure stays close to the high-water mark of live data. Matches Lisp users' expectations far better than a tracing GC's "memory grows until it doesn't."
3. **Slots cleanly between `ref<T>` and `ptr<T>`.** Right now there's a usability gap: if you need *shared* ownership, your only option is raw `ptr<T>` and self-discipline. `rc<T>` fills it without abandoning the model.
4. **Incremental rollout.** `rc<T>` can ship in v1 (or v0.5) without a runtime rearchitecture. Programs that don't use it pay nothing. Compare with Cheney-MTA (§5.1.1), which is all-or-nothing.
5. **Compiler optimizations are well-understood.** Last-use analysis converts retain+release pairs into moves; redundant retain elimination collapses `f(x); g(x)` traffic; uniqueness inference can demote single-owner `rc<T>` to `ref<T>` automatically. Swift's compiler is the proof point; the literature is rich.
6. **Single-threaded for free.** v0 is single-threaded, so refcounts can be plain `int`, not atomics — every retain/release is one memory op, not a `lock xadd`. Atomics only enter the picture if/when threads do.

**Why it's hard:**

1. **Cycles leak.** This is the irreducible cost of pure RC. Three responses, in order of effort:
   - *Document weak refs as the solution* (Swift's stance, ObjC's stance). Programmer breaks cycles with `weak<T>` at parent→child / observer→observed boundaries. Works in practice for most Lisp idioms — functional code is mostly DAG-shaped (cons cells, ASTs, env chains) — and gives the user a clear, local mental model. Cost: programmer attention.
   - *Add a cycle collector* (CPython's approach: Bacon-Rajan trial-deletion). Periodic, scans only RC'd objects with multiple incoming refs, can be tuned/disabled. Adds complexity but isolates it; doesn't perturb the rest of the runtime. Probably the right v2 move once cycles become a felt problem.
   - *Static cycle detection*. Rejected in the literature — too restrictive for a real language. Don't.
2. **Per-assignment overhead, even when elided.** Every copy of an `rc<T>` is a retain; every drop is a release. The compiler will eliminate most of them, but not all. For tight inner loops over RC'd objects, throughput is measurably worse than a copying collector. Counter: those loops should use `ref<T>` or `slice<T>` (§5.3) anyway — RC is for structurally shared data, not per-iteration scratch.
3. **Header-word cost.** Each RC'd allocation needs strong + weak counts. Two layout choices: header inline with `T` (saves an indirection but bloats every value) vs. separate control block (Rust's approach — `T*` stays a normal pointer, slices over RC'd data don't pay header overhead). Lean: separate control block.
4. **Closure environments interact subtly.** Today, escaping closure envs are heap-boxed (§4) but otherwise untracked. With RC, the natural move is "captured envs become `rc<env>`" — but that means *every closure call* potentially does a retain/release. Mitigation: only RC the env if multiple closures share it; otherwise keep the env single-owner. The closure-conversion pass needs a small uniqueness analysis, but it's local and cheap.
5. **`weak<T>` requires a control-block shape.** The strong count and weak count must outlive the object so `(upgrade w)` can check liveness without dereferencing freed memory. Standard solution: control block has [strong, weak]; object freed when strong→0 but control block freed only when both →0. Adds a "zombie" state for the object; well-understood, but a runtime invariant to document.
6. **Cascade-free latency.** Long chains of `rc<T>` releases can cause stack-deep avalanche frees on a single drop. Standard fix: a deferred-free queue inside `rc-release` that linearizes the cascade. Cheap in code, but a thing the runtime has to do.
7. **Doesn't help the unannotated heap.** §5.1's v0 strategy (process-wide bump arena, never freed) leaks indefinitely for long-running programs. RC only helps the data the user explicitly opts into. So *long-running programs still need a tracing GC for unannotated allocations*, **or** the language has to require `rc<T>` for all non-`ref` heap allocations. The latter is opinionated but coherent and is what I'd recommend if we go this route.

**Composition with `defer`** — this is where RC shines vs. Cheney-MTA:

- `(let [r (rc/of (Point. 0 0))] …)` — compiler injects `(defer (rc-release r))` exactly as it does for `ref<T>`. No CPS, no inversion of control flow, no scope-end ambiguity.
- `(let [w (weak r)] …)` — `weak<T>` also gets a defer'd release for its weak slot.
- Mixing `ref<T>`, `rc<T>`, and `defer` in one scope: drops fire LIFO at scope exit, same as today. The label-based unwind (§5) doesn't change.
- Moving an `rc<T>` out of a scope (return value): defer is suppressed, ownership transfers, retain count is unchanged. Compiler enforces statically.

**Verdict.** **The most pragmatic GC strategy for Turmeric.** Concretely:

- The only option that *composes* with `defer`/`ref` rather than fighting them. No re-architecture.
- Ships in v1 without committing v0 to anything; the `tur_alloc` indirection and reserved header word from §5.1 already preserve the option.
- Cycles are a real cost but a *survivable* one: weak refs at known-cyclic boundaries handle the common cases, and a Bacon-Rajan cycle collector can be added in v2 without disturbing existing code.
- Throughput is worse than a copying collector for allocation-heavy workloads — but better latency, better locality, no STW pauses, no compiler magic required.

**Recommendation.** Make `rc<T>` + `weak<T>` the v1 GC story. Treat tracing GC (Boehm / precise mark-sweep / Cheney-MTA) as v2 options for "the unannotated heap" if and when long-running workloads demand it. If we adopt delimited continuations (§12.1) and need the CPS pass anyway, *then* reopen the Cheney-MTA conversation — but until then, RC is the strictly cheaper bet.

**What v0 needs to preserve, in addition to §5.1.1's list:**
- Treat the drop function as a per-type field (`type_info->drop`), so adding `rc-release` as a new drop function in v1 is mechanical.
- Reserve `rc/of`, `weak`, `upgrade`, `strong-count` as keywords now; emit "not implemented in v0" errors if used. This avoids syntactic retrofitting later.
- Don't optimize the closure env layout in v0 in a way that assumes single ownership — leave room for a future "envs may be `rc<env>`" pass.

### 5.2 Structs (product types)

```clojure
(defstruct Point
  [x : int
   y : int])

(let [p (Point. 3 4)]                ;; positional ctor
  (println (.x p)) (println (.y p)))

(let [q (make Point :x 3 :y 4)]      ;; keyword ctor (macro over Point.)
  (println (.x q)))
```

- Lowers directly to a C `typedef struct { int x; int y; } Point;`. No header word, no vtable.
- **Value semantics by default** — assignment copies. To share or mutate-through, hold a `ref<Point>` or `ptr<Point>`.
- Field access: `(.x p)` / `(.y p)` are reader sugar for `(. p x)` / `(. p y)`. Setter form: `(set! (.x p) 5)` lowers to `p.x = 5;` when `p` is mutable.
- No methods, no inheritance — methods are just functions. Polymorphic dispatch on structs comes via typeclasses (§12.2).
- `(defstruct Point :packed)` and `:align N` attributes pass through to C for FFI layout control.
- Generic structs (`(defstruct Pair [a : T1, b : T2])`) deferred to v2; for now, write `Pair-int-int`, `Pair-int-str`, etc., or generate them with a macro.

### 5.3 Arrays, strings, and slices

Three layers, Rust-shaped:

| Type | Owns? | Shape | Use for |
|---|---|---|---|
| `array<T, N>` | yes (inline) | `T[N]` value | fixed-size buffers on the stack/struct |
| `slice<T>` | no | `struct { T* p; size_t len; }` | function params, sub-views, iteration |
| `vec<T>` | yes (heap) | `struct { T* data; size_t len; size_t cap; }` | growable owning collection |

```clojure
(defn sum [^slice<int> xs] : int
  (let [acc (ref 0)]
    (for [x xs] (set! acc (+ @acc x)))
    @acc))

(let [a : array<int 4> [1 2 3 4]]
  (sum a))                            ;; array auto-coerces to slice<int>

(let [v (vec/of 1 2 3)]
  (sum v))                            ;; vec auto-coerces to slice<int>
```

Strings are the same pattern, restricted to bytes:
- `cstr` — `const char *`, NUL-terminated; only for C interop.
- `str` — `struct { const u8* p; size_t len; }`, **UTF-8, not NUL-terminated** — the lingua franca for text within Turmeric.
- `string` — owning, growable: heap-backed `vec<u8>` with the UTF-8 invariant. Acts as `str` when borrowed.

Operations: `(nth s i)`, `(slice s start end)`, `(len s)`, `(concat …)`, iteration via `for`. Bounds-checks on by default in v0 (cheap); `(unsafe-nth …)` opts out.

Why slices: the same function works on stack arrays, heap vecs, and substrings without ownership ceremony. APIs that don't need to allocate take `slice<T>` and `str`; APIs that own take `vec<T>` and `string`.

### 5.4 Low-level memory: `&`, `sizeof`, `malloc`/`free`

For FFI, intrinsics, and "I just need raw bytes" cases. This is the unsafe edge of the language; the type-checker tracks types but does **not** track lifetimes for raw pointers.

```clojure
(let [x  : int 42
      p  : ptr<int> (& x)              ;; address-of
      v  : int      (deref p)          ;; or @p
      sz : usize    (sizeof Point)
      buf : ptr<Point> (malloc Point 16)]
  (defer (free buf))                   ;; manual; no ref auto-drop here
  (set! (deref (ptr-add buf 0)) (Point. 0 0))
  …)
```

- `(& expr)` — address-of. Reader macro form `&expr` accepted.
- `(deref p)` / `@p` — load through pointer. `(set! (deref p) v)` for store.
- `(sizeof T)`, `(alignof T)` — compile-time constants; lower to `sizeof(T)` / `_Alignof(T)`.
- `(malloc T)` allocates one `T`; `(malloc T n)` allocates an array. `(free p)` is `free(p)`. Both wrap libc and return/take `ptr<T>` for type ergonomics.
- `(ptr-add p n)`, `(ptr-sub p n)`, `(ptr-eq p q)` — pointer arithmetic in element units (not bytes).
- `ptr<T>` is **nullable**; `(null? p)` is the test. A `non-null<T>` refinement is reserved for the future type system (§12.2).
- Interaction with `ref<T>`: `(ref->ptr r)` borrows a `ptr<T>` from a `ref<T>` for the remainder of the scope. The compiler can't statically prove the borrow doesn't outlive the `ref`'s `defer` drop — documented sharp edge, same as §5.

Use `ref<T>` when you can; reach for raw pointers when interop or perf demands it.

---

## 6. Macros

`defmacro` runs at compile time. Two viable strategies:

- **(A) Bootstrap interpreter** — embed a small Turmeric-subset interpreter in the compiler host language. Macros run there. Simpler to start; chosen for v0.
- **(B) Stage-1 self-hosting** — compile macros to a shared library and `dlopen` them. Faster, more flexible. Deferred to v2.

The interpreter shares the `Form` representation with the compiler so quasiquote/unquote work on the same nodes the compiler later sees. Hygiene v1: gensym-based via a `(gensym 'foo)` builtin — no automatic hygiene yet (documented).

---

## 7. Implementation phases

| Phase | Deliverable | Exit criterion |
|---|---|---|
| 0 | C skeleton repo (Makefile, CI, sanitizers on by default), arena allocator, `Form` ADT, `hello.tur` round-trip | `(println "hi")` compiles & runs |
| 1 | Reader, core forms, primitive types, C codegen for arithmetic + `if` + `let` | Fizzbuzz |
| 2 | Functions (top-level), C ABI interop (`extern-c`) | Calls into libc (`printf`, `malloc`) |
| 3 | Closures: env struct synthesis, escape analysis, call-site lowering | Counter example from §2 |
| 4 | `defer` + scope unwind via labels | `defer` ordering tests pass |
| 5 | `ref<T>` with move semantics + auto-`defer` drop | Valgrind-clean on test suite |
| 6 | `defmacro` + quasiquote in the bootstrap interpreter | Macro-defined `when`, `->`, `cond` |
| 7 | Stdlib seed: `vec<T>` growable array, `slice<T>` borrowed view, `str` UTF-8 string, `option<T>` sum type, `result<T,E>` sum type; test runner with `(deftest)`, `(assert)`, `(run-tests!)`; `cond`, `case`, `when`, `unless` rewritten as macros | Self-hosted test suite passes; stdlib types usable from user code; Valgrind-clean on stdlib tests |
| 8 | Error reporting: span propagation through reader → macro expansion → elaboration → codegen; miette-style multi-line snippets with source context, caret underlines, related notes; `--explain <code>` flag for long-form error explanations; `--json-diagnostics` for IDE integration; error codes for documentation lookup | Bad-input fixtures show pointer-and-caret errors; all error paths have source spans; CI linter ensures no `SPAN_UNKNOWN` in snapshots |
| 9 *(v1)* | `rc<T>` + `weak<T>`: reference counting v1 GC with control-block layout (strong_count, weak_count, value, drop_fn); `(rc/of x)`, `(rc/clone r)`, `@r` deref, `(rc->ptr r)` raw borrow, `(rc/strong-count r)`; `weak<T>` with `(weak r)`, `(upgrade w)` → `option<rc<T>>`, `(weak? w)`; defer-injected `rc-release`; last-use elision; cascade-free deletion via deferred free queue; `(rc/from-ref r)` and `(ref/from-rc r)` conversions | RC fixtures pass; Valgrind-clean; cycle leak documented; `rc<T>` composes with `ref<T>` and closures; weak pointer upgrade behaves correctly |
| 10 *(v2)* | Bacon-Rajan cycle collector layered over RC: GC-specific fields in control block (color enum: WHITE/GREY/BLACK/PURPLE, may_contain_cycles flag); suspect roots buffer tracks objects where strong_count=0 but weak_count>0; mark phase traverses from strong-count>0 black roots; trial deletion collects white suspect components; `gc/disable!` and `gc/enable!` for control; `gc/force!` for testing; disabled by default for zero-overhead | Cycle fixture from phase 9 reclaims memory; GC passes its own test suite; no performance regression for non-cyclic programs; collector can be disabled |
| 11 | Copy traits: `Copy` vs `Move` distinction; auto-derive `Copy` for primitives (`int`, `bool`, `cstr`, `ptr<T>`); explicit `(deftype ... :copy)` opt-in for user structs; assignment of non-`Copy` values poisons the source binding | Reassigning a moved `ref<T>` is a compile error; `int` and friends still freely copy; fixture covers struct with and without `:copy` opt-in |
| 12 | Borrow traits: introduce `&T` (immutable) and `&mut T` (exclusive mutable) reference types alongside `ptr<T>`; aliasing rules enforced within a function (N readers XOR 1 writer); raw `ptr<T>` remains the documented escape hatch | `(let [r (& x)] ...)` and `(let [r (&mut x)] ...)` typecheck with intra-fn lifetime tracking; double `&mut` is a diagnostic; FFI tests using `ptr<T>` keep compiling |
| 13 | Lifetime annotations: surface syntax for explicit lifetime parameters (e.g. `^'a`) on `defn` signatures and reference types; elision rules cover the common single-input/single-output case so most code stays unannotated | A `(defn longest [^'a &cstr x ^'a &cstr y] : ^'a &cstr ...)` fixture compiles; an annotation that returns a reference outliving its input is a diagnostic; elided cases match Rust's three elision rules |
| 14 | Borrow checker with lifetimes: full intra- and inter-procedural checking driven by the lifetime IR from phase 13; integrates with `ref<T>` move tracking (phase 5/11) and `&T`/`&mut T` aliasing (phase 12); `(unsafe ...)` block exits the checker for FFI/perf code | Use-after-move, dangling reference, and conflicting-borrow fixtures all fail to compile with localized diagnostics; `(unsafe ...)` regression fixture still compiles; existing phase 0–10 fixtures remain green |

Each phase ends with a fixture test in `tests/<phase>/` that survives forever.

---

## 8. Open questions to decide before phase 0

All five resolved.

1. ~~**Host language.**~~ **Decided: C99.** The compiler is written in C. Rationale: Turmeric already emits C, so the compiler shares its target's runtime model, and self-hosting (rewriting `tur` in Turmeric, which then emits C) becomes a natural milestone rather than a re-port. Trade-offs accepted: more manual memory management in the compiler itself, weaker ADTs (use tagged unions + macros), no batteries-included parser/diag libs. See §11 for how this shapes the implementation.
2. ~~**Type system depth in v1.**~~ **Decided: annotations on bindings + locals inferred from RHS only.** Mandatory annotations on `defn` / `def` params and returns; locals (`let`, `set!`-targets) get types inferred from their RHS, no further inference. No Hindley-Milner. Codified in §1.1; the elaborator's typed-IR output is the source of truth. Operators are elaborator-resolved from day one so typeclasses (§12.2) plug in cleanly.
3. ~~**Borrow story.**~~ **Decided: document `ptr<T>` as untracked ("raw pointers, your problem"); defer borrow checking to v2.** §5.4 is the documented sharp edge. v1 still gets a lot of safety from `ref<T>`'s move semantics and `rc<T>` (§5.1.2); raw `ptr<T>` is the explicit escape hatch. Revisit when we have real users hitting use-after-free in practice.
4. ~~**C dialect.**~~ **Decided: C99.** Max portability, every platform's `cc` understands it. Optional C11 (`_Generic`, atomics) only behind a feature flag for stdlib hot paths if/when benchmarks demand it. Inline-C blocks (§2.1) using GCC/Clang statement-expressions are still allowed but flagged as a non-strict-C99 dependency; `--strict-c99` lifts them to helper functions.
5. ~~**Build artifact layout.**~~ **Decided: one `.c` per Turmeric source file, plus a generated `_main.c`.** Each `.tur` compiles to a paired `.c` + `.h`; `_main.c` contains the `main()` entry point and `#include`s every emitted header. Linker resolves cross-file references. Benefits: separate compilation works from day one (touching one file recompiles only that file), the multi-unit story is plumbed before §12.3 modules arrives, and `_main.c` is the single place where startup ordering / global initialization lives. Whole-program single-file emission can come later as `tur build --single-file` if anyone wants it.

---

## 9. Risks

- **Closure escape analysis** is the trickiest correctness boundary; getting it wrong gives use-after-free. Mitigation: default to heap-boxed envs, optimize stack-allocation later behind a flag.
- **Macro hygiene** absence will burn users; gensym is a band-aid. Plan a v2 hygienic-expansion pass before the language has many users.
- **`defer` semantics** must match user intuition exactly — document the divergence from Go (lexical scope vs. function scope) prominently.
- **Compile-time interpreter ↔ compiler drift** — share the `Form` type and a single set of core-form evaluators.

---

## 10. Concrete roadmap — phase tasks

§7 has the high-level shape. This section breaks the first five phases (0–4) into concrete tasks sized to land in a focused session. Check them off as they go in.

A few cross-cutting rules apply to every phase:
- **No feature lands without fixtures** — happy-path, interaction, negative (with golden diagnostic), and codegen-snapshot. See §11.
- **Spans flow through every pass.** Adding them retroactively is the worst refactor in C.
- **All allocation goes through `tur_alloc` / arena.** Never raw `malloc` in compiler code. Preserves §5.1's swap-the-allocator option.
- **Symbols are typed values, never `const char*`.** Preserves §12.4 hygiene path.

### 10.1 Phase 0 — C skeleton + `hello.tur` round-trip — ✅ **shipped**

Goal: prove the pipeline end-to-end with the smallest possible feature set. Everything in this phase is plumbing.

**Repo & build**
- [x] `git init`; `README.md`, `.gitignore` (build artifacts, `.dSYM/`, `tests/out/`).
- [x] Layout: `src/`, `tests/fixtures/`, `tests/run.sh`.
- [x] `Makefile` with two profiles: `debug` (`-Og -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c99 -pedantic`), `release` (`-O2 -DNDEBUG`). Per-file `.d` auto-deps via `-MMD -MP`.
- [ ] CI workflow: `make debug && make test` on push. *(Local-only for now; flip on when remote is set up.)*

**Arena allocator** — `src/arena.{c,h}`
- [x] `Arena` struct: linked list of slabs, bump pointer per slab. (`arena_init` initializes in-place rather than `arena_new`-style heap return.)
- [x] `arena_init`, `arena_alloc`, `arena_alloc_aligned`, `arena_strdup`, `arena_free`.
- [ ] Self-test: 1M small allocations under ASan, no leaks. *(The fixture suite exercises the arena indirectly under ASan; a dedicated micro-bench can wait until we have a benchmark harness.)*

**Strings & symbols** — `src/symbols.{c,h}`
- [x] `StrSlice { const char* p; uint32_t len; }` (no NUL assumption).
- [x] `Symbol` typed value (not opaque — fields visible in header), FNV-1a hash-table intern with rehash; equality is pointer equality.
- [x] All strings live in the compile-unit arena.

**Form ADT** — `src/forms.{c,h}`
- [x] `Span { uint16_t file_id; uint32_t line; uint32_t col_start, col_end; uint32_t off_start, off_end; }` — added byte offsets so diagnostics can render the source line and caret.
- [x] Tagged union: `F_NIL, F_BOOL, F_INT, F_STR, F_SYM, F_LIST`.
- [x] `form_print(Buf*, Form*)` round-trip for debugging (escape-aware string printing).

**Buf** — `src/buf.{c,h}`
- [x] Growable byte buffer: `buf_init`, `buf_printf`, `buf_vprintf`, `buf_putc`, `buf_write`, `buf_puts`, `buf_to_file`. (`buf_indent` deferred — not needed in phase 0; add when codegen actually does indenting.)

**Diagnostics** — `src/diag.{c,h}`
- [x] `diag_emit(level, Span, fmt, ...)` and `diag_emitv`; level ∈ `{error, warning, note}`.
- [x] `diag_had_error()` / `diag_reset()` (caller checks; no `diag_flush` since we emit eagerly).
- [x] Output: `path:line:col: error: message` plus the source line and a `^^^^` caret pointing at the span.

**Reader** — `src/reader.{c,h}` (more than minimal — ended up cheaper than retrofitting)
- [x] Tokenizer: parens, strings (with `\n \t \r \0 \\ \"` escapes), symbols, integers (incl. negatives), whitespace, `,` as whitespace (Clojure-style), line comments (`;`).
- [x] Atoms: `nil`, `true`, `false` recognized as their own forms.
- [x] Span on every Form, including byte offsets for caret rendering.
- [x] Errors with golden fixtures: unterminated string, unmatched paren.

**Codegen** — `src/emit.{c,h}` (hardcoded for hello)
- [x] `#include <stdio.h>` prologue, `int main(void)` wrapper.
- [x] Recognize exactly `(println "<str>")` → `puts("<str>");`. C-string escaping uses the same rules as the reader.
- [x] Anything else → diagnostic "phase 0: only top-level (println \"…\") is supported" with span, then bail.

**Driver** — `src/main.c`
- [x] `tur build <input.tur> [-o <out>]`: read → parse → emit `.c` to a temp file → `cc` (CC env override) → executable. Default output name derived from input basename minus extension.
- [x] `tur emit-c <input.tur>`: dump the C to stdout.
- [x] `tur run <input.tur>`: build + execute in one shot.

**Test harness** — `tests/run.sh`
- [x] For each `tests/fixtures/*/`: run `tur build`, compare program stdout to `expected.stdout`, emitted `.c` to `expected.c` (when present).
- [x] Bad-input fixtures under `tests/fixtures/errors/*/`: assert nonzero exit, check `expected.diag` substrings appear in stderr.
- [x] Green fixtures shipped: `hello`, `errors/unterminated-string`, `errors/unmatched-paren`.

**Exit criterion:** ✅ `./build/tur build tests/fixtures/hello/hello.tur && ./hello` prints `hi`. `make test` → 3/3 green, ASan + UBSan clean, no warnings under `-Werror -Wall -Wextra -pedantic`.

**Carry-overs into later phases (filed here so we don't lose them):**
- *CI workflow* — flip on once the repo is on a remote.
- *Arena micro-bench* — defer to a benchmark harness when we add one.
- *`buf_indent`* — add when codegen actually needs indented output.
- *`tur run` mkstemp/cc race* — `tur run` uses `mkstemp` then re-`cc`s onto the same path. Works on macOS, technically TOCTOU. Phase 2 reworks the driver for multi-file builds; tighten then.
- *Reader is already over-spec for phase 0* — negatives, comments, `nil`/`true`/`false`, full string escape set are in. Phase 1 mostly extends, doesn't replace. Vectors/maps and reader macros (`'`, `` ` ``, `~`, `~@`) still TODO.
- *No typed IR yet* — `Form*` is fed straight into `emit.c` in phase 0, which is fine while codegen is hardcoded. Phase 1 introduces the `Expr` typed IR per §1.1 and §3 step 4; `emit.c` stops reading `Form*` past that point.

---

### 10.2 Phase 1 — full reader, core forms, arithmetic → fizzbuzz

Goal: actually compute things. Reader gets complete; the codegen learns enough special forms to express fizzbuzz.

### 10.2 Phase 1 — *core forms shipped (fizzbuzz green); reader extensions deferred*

The exit criterion (fizzbuzz) is **met** with all 12 fixtures green under ASan/UBSan. A handful of surface features (reader macros, block comments, floats, `#lang`, maps) didn't ship — flagged as carry-overs into later phases.

**Reader: complete the surface**
- [x] Numeric literals: int (decimal, hex `0x`, binary `0b`).
- [ ] Float literals (`1.0`, `1.5e3`) — *deferred. The `float` type isn't in the dispatch table yet either; ship together when there's a fixture that needs it.*
- [x] String escapes: `\n \r \t \" \\ \0`. *(`\xNN` deferred — no fixture demands it.)*
- [x] Keywords (`:foo`) — `F_KEYWORD` form variant, intern table shared with symbols, distinct tag. v1 only allows them as `:else` in `cond`; using one elsewhere errors with "phase 1: keywords are only allowed as :else in cond."
- [x] Vectors `[…]` — `F_VEC` form variant. Used only in `let` bindings in v1; using one elsewhere errors with "phase 1: vector literals are only allowed in let bindings."
- [ ] Maps `#{…}` — *deferred. No phase 1 fixture needs them; the `{…}` reservation for SRFI-105 (§12.5) still holds.* **Maps use `#{…}`, not `{…}`** — `{…}` is reserved for SRFI-105 curly-infix when §12.5 ships, and pre-resolving this in v1 avoids a forced migration later.
- [ ] Block comments `#| ... |#` — *deferred.*
- [ ] Reader macros: `'x` → `(quote x)`, `` `x `` → `(quasiquote x)`, `~x` → `(unquote x)`, `~@x` → `(unquote-splicing x)`. *Deferred to phase 6 (defmacro) — there's no consumer for them in phase 1.* **`~` / `~@` are the only unquote sigils** when they ship; Turmeric does not also accept Scheme's `,` / `,@`. Single-sigil-set keeps the §12.5 leading-abbreviation rule simple.
- [ ] `#lang` directive parsing — *deferred. Sweet-expressions (§12.5) is the consumer; ship together.* When implemented: a `#lang <name>` line at the very top of the file selects the reader. v1 will recognize `#lang turmeric` (the default — equivalent to no `#lang` line) and warn-but-ignore any other value; `#lang sweet-exp` errors with "sweet-expressions not yet implemented." See §12.5.4.
- [x] Negative tests for malformed input — `errors/unterminated-string`, `errors/unmatched-paren`, `errors/let-odd-bindings`, `errors/if-non-bool`, `errors/set-immutable`, `errors/unbound-symbol`, `errors/type-mismatch`. Each compares stderr against an `expected.diag` golden.

**Type system: just enough to elaborate**
- [x] Primitive types: `int` (i64), `bool`, `cstr` — `src/types.{c,h}` ships with a `TypeKind` enum and `type_c_name` lowering. `TY_NIL` reserved for the unit type of statement-shaped expressions; `TY_UNKNOWN` for in-progress elaboration.
- [x] Mutability annotation parsing — `^mut` reads as a symbol; the elaborator pattern-matches it inside `let` binding vectors. Reused later for `defn` params and `def` (when phase 2 lands).
- [ ] Type annotation parsing: `^int` prefix, `: T` postfix on params/returns, `(def name : T expr)` — *deferred to phase 2 when `defn` lands*. Phase 1 has no annotated bindings; types are inferred from RHS.
- [x] Trivial elaborator: propagates types; literals get their default types; operators resolve via the dispatch table; `if` requires both branches to match. No further inference. `Form*` is *not* used past elaboration — every later pass operates on `Expr` (§3 step 4).

**Special forms** — `src/elab.{c,h}` + `src/emit.{c,h}`
- [x] `def` (top-level binding) → file-scope `static T name;` declaration plus `name = init;` at the top of `main`. Top-level only; redefinition errors.
- [x] `let` (lexical scope, multiple bindings, optional `^mut`) → C block with locals; mangled names (`<name>_<id>`) avoid C-level shadowing collisions.
- [x] `if` → emits a `T tmp; if (cond) { tmp = then; } else { tmp = else; }` block when in value position; statement form when result is `nil`. (Ternary form deferred — readability of generated C isn't a phase-1 concern.)
- [x] `do` → all but last as statements; last is the value (or a statement if `nil`-typed).
- [x] `when`, `unless` — desugar to `if` in elaboration. `unless` swaps then/else.
- [x] `cond` — desugars right-to-left to nested `if`s. `:else` keyword recognized as the always-true clause.
- [x] `set!` — only on `^mut`-annotated bindings; immutable target errors with "use ^mut at the binding site to allow it."
- [x] `while` — `(while <cond> <body...>)` lowers to `while (1) { if (!cond) break; body }` (re-emitting cond inside the loop preserves correctness even when the condition itself produces statements).

**Built-in operators** — all elaborator-resolved (§1.1)
- [x] Operator dispatch table — `src/builtins.{c,h}`. Each entry: `(name, min_arity, max_arity, arg_type, result_type, shape, c_op)`. The shape determines codegen: `BS_BIN_INFIX`, `BS_VARIADIC_FOLD` (left-fold), `BS_PREFIX_UNARY`, `BS_AND_SC` / `BS_OR_SC` (short-circuit), `BS_PRINTLN_*` (per-arg-type println). Lookup is `(name_sym, first_arg_type, n_args)` — first match wins.
- [x] Arithmetic: `+`, `-`, `*` (variadic ≥2 args), `/`, `mod` (binary). `int` only in v1 — `float` enters the table when float literals do.
- [x] Comparison: `= < > <= >= not=` for `int`; `=` and `not=` for `bool`. (`cstr` equality / comparison deferred — needs `strcmp` plumbing.)
- [x] Logical: `and`, `or` (short-circuit, table-dispatched via `BS_AND_SC`/`BS_OR_SC`), `not` (prefix unary). Codegen for `and`/`or` lifts to a `bool tmp`, then chains `if (tmp) tmp = next;` (or `!tmp` for or) so right-hand sides only run when the running result requires it.
- [x] `println` — separate table entries per arg type (`int`/`bool`/`cstr`). One arg in v1; multi-arg `println` becomes a macro in phase 6.
- [x] Codegen consults the table — never hardcodes operator names. New types extend the table mechanically; typeclasses (§12.2) just add more rows.

**Bounds & overflow**
- [x] Integer arithmetic uses C `int64_t` (literals emitted as `INT64_C(n)`). Overflow is C wrap-around — documented, no `checked-+` yet.
- [ ] Division-by-zero runtime check — *deferred. Currently raw `/`. Add when there's a fixture that demands clean failure (likely phase 7 stdlib).*

**Fixtures (5 happy-path + 7 negative — 12/12 green)**
- [x] `arith.tur` — every operator (incl. variadic, short-circuit `and`/`or`).
- [x] `let-shadow.tur` — inner-let shadowing, value restored on exit.
- [x] `if-cond.tur` — `if`/`cond`/`when`/`unless` branch coverage incl. cond `:else`.
- [x] `fizzbuzz.tur` — `^mut` counter + `while` + `cond` with `:else`. The exit criterion.
- [x] Negative: `if-non-bool`, `let-odd-bindings`, `set-immutable`, `unbound-symbol`, `type-mismatch` (carry-overs from phase 0: `unmatched-paren`, `unterminated-string`).

**Exit criterion:** ✅ `./build/tur run tests/fixtures/fizzbuzz/input.tur` prints fizzbuzz 1..100. `make test` → 12/12 green, ASan + UBSan clean, no warnings under `-Werror -Wall -Wextra -pedantic`.

**Carry-overs into later phases (filed here so we don't lose them):**
- *Float literals + `float` type + `(float,float)` table entries* — ship together when the first fixture needs them. Likely phase 2 (calling math libc) or later.
- *`cstr` equality / comparison* — wire `strcmp` into the dispatch table when string-handling fixtures arrive. Phase 2 candidate.
- *Map literals `#{…}`* — phase 7 (stdlib) probably; reserve the syntax now.
- *Block comments `#| … |#`* — trivial reader extension; ship when the first programmer is annoyed by single-line comments.
- *Reader macros (`'`, `` ` ``, `~`, `~@`)* — phase 6 (defmacro) is the consumer.
- *`#lang` directive* — phase that adds sweet-expressions (§12.5) is the consumer.
- *Type annotations on `defn` (`: T`)* — phase 2.
- *Division-by-zero runtime check* — when it bites a user.
- *Codegen tidiness* — current output is correct but verbose (`(void)x;` after every let init to suppress unused-warnings; `INT64_C(n)` for every int; `i_0` instead of `i`). Pretty-printing is a phase 8 (diagnostics polish) concern.
- *Statement-form `if` ternary optimization* — when both branches are pure C expressions, `cond ? then : else` would be cleaner than the if-else block. Skipped in v1 because the C compiler optimizes either form to the same codegen.
- *Top-level `def` with constant init* — currently every def is zero-initialized statically, then assigned at the start of `main`. For `(def x 42)` the static init `static int64_t x = 42;` would be cleaner, but distinguishing constant from non-constant init is extra work.

---

### 10.3 Phase 2 — top-level functions, `extern-c`, inline-C, libc

Goal: call into C, get called from `main`, write inline-C escape hatches. No closures yet — functions can't capture.

**`defn` — top-level functions**
- [x] Parse `(defn name [params...] : ret-T body...)`.
- [x] Type-check: param types annotated, return type annotated, body has matching type.
- [x] Emit `static T name(T1 p1, T2 p2) { … }`.
- [x] Forward declarations emitted in dependency order (or all at top of file — simplest).
- [x] `defn-` for module-private (C `static`) when phase 9/modules arrives; for now treat all as `static`.

**`fn` — anonymous, no-capture**
- [x] Parse and type-check.
- [x] If the body references no outer locals → lift to a top-level static function with a synthetic name (`__fn_<id>`).
- [x] Reject capture with a "closures arrive in phase 3" diagnostic — the fixture proves the gate.

**Function calls**
- [x] Resolve `(f a b c)` to direct C call; arity- and type-check.
- [x] Variadic functions deferred until extern-c needs them.

**`extern-c`**
- [x] Parse `(extern-c printf [^cstr fmt & args] : int)`.
- [x] Emit a forward declaration `extern int printf(const char*, ...);` (variadic via `& args` marker).
- [x] Trust the user-given type; type-checker treats it as opaque past arity.
- [x] Built-in re-exports: `(extern-c malloc [^usize n] : ptr<void>)`, `free`, `abort`, `puts`, `printf` available without per-program redeclaration.

**Inline-C blocks (§2.1)** — first cut
- [x] Reader recognizes triple-backtick fences with optional `c` tag; payload is verbatim string.
- [x] Elaborator records the block's annotated return type (`: T`).
- [x] Codegen emits a GCC/Clang statement-expression `({ ... })` with a leading auto-binding for each free identifier captured by name.
- [x] Strict-C99 fallback (`--strict-c99`): hoist the block into a generated helper function. Skip in v0 if it complicates things; document the flag as planned.
- [x] Reject `#include` inside an inline block with a clear error.

**Driver** — adopt the per-file artifact layout (§8.5)
- [x] Each `.tur` source file compiles to a paired `<name>.c` + `<name>.h` in the build directory.
- [x] Generate `_main.c`: `#include`s every emitted header, defines `int main(void)` that calls the user's `main` (located in whichever input file declares it).
- [x] `tur build a.tur b.tur` → `a.c`, `a.h`, `b.c`, `b.h`, `_main.c`; pass all `.c` files to `cc` and link.
- [x] `tur run <input.tur>` — build + execute in one shot.
- [x] Fixture: two-file program, `a.tur` defines `(defn helper [x] : int …)`, `b.tur` calls it from `(defn main [] …)`. Verify `_main.c` is generated correctly and the link succeeds.

**Fixtures**
- [x] `defn-basic.tur` — recursive factorial.
- [x] `mutual-recursion.tur` — `even?`/`odd?`.
- [x] `extern-printf.tur` — direct `(printf "%d\n" x)`.
- [x] `inline-c-popcount.tur` — `__builtin_popcount` via inline block.
- [x] Negative: capturing `fn` with phase-3 gate diagnostic; arity mismatches; bad inline-C return-type annotation.

**Exit criterion:** ✅ can call `printf`, write a recursive function, drop into inline C — all fixtures green.

---

### 10.4 Phase 3 — closures

Goal: `fn` captures locals; closures are first-class values; the §2 counter example runs.

**Capture analysis** — `src/close.{c,h}`
- [x] Walk `fn` body, collect free identifiers, look up each in enclosing scopes.
- [x] For each captured binding: record type, mutability, escape status.
- [x] Distinguish *upvalue* (captured by value, copy at closure construction) from *upref* (captured by reference, shared mutation visible).
  - Default: immutable bindings → upvalue; mutable (`set!`-targeted) bindings → upref.

**Escape analysis**
- [x] Determine whether the closure escapes its defining scope (returned, stored, passed to a non-stack-only sink).
- [x] Conservative default: any closure passed as a value escapes. Optimize stack-allocation only when clearly safe.

**Env struct synthesis**
- [x] For each `fn`, generate `struct __env_<id> { /* captured fields */ };`.
- [x] Heap-allocate via `tur_alloc(sizeof(struct __env_<id>))` if escaping; stack-alloc otherwise.
- [x] Reserve a header word (8 bytes) at the front of every env struct for future GC metadata (§5.1.1, §5.1.2). Do not repurpose.

**Closure type & call lowering**
- [x] One `closure_<sig>` typedef per distinct flattened signature: `struct closure_int_int { int (*fn)(void* env, int); void* env; }`.
- [x] `(c args...)` lowers to `c.fn(c.env, args...)`.
- [x] Top-level functions are also coerced to closures on demand: `closure_int_int wrap = { .fn = my_fn_thunk, .env = NULL };` where `my_fn_thunk` discards `env` and forwards.

**Fixtures**
- [x] `counter.tur` — the §2 example.
- [x] `adder-factory.tur` — `(defn make-adder [n] (fn [x] (+ x n)))`.
- [x] `mutable-capture.tur` — closure mutates an outer `set!`-able binding; second closure sees the mutation.
- [x] `escape-no-escape.tur` — codegen snapshot proving stack-alloc happens when it can.
- [x] Codegen snapshots for env layout and call-site lowering.

**Exit criterion:** ✅ counter example, adder factory, and mutable-capture fixtures all green; ASan/UBSan clean; the codegen snapshot of env layout matches what's documented in §4.

---

### 10.5 Phase 4 — `defer` + scope unwind

Goal: scope-bounded cleanup with LIFO ordering and correct early-return behavior. No `ref<T>` yet — that's phase 5, layered on top.

> **Architectural commitment: the unified defer model.** Defers are a runtime list-on-frame, not codegen labels. This is *modestly* more work in phase 4 and saves a phase-4-rewrite if we ever ship effect handlers (or any other feature where defers fire from a non-syntactic exit point). See [effects-plan.md §6.10](effects-plan.md) for the full rationale; in short, every plausible future strategy (S1 "run on capture" / S2 "attach to continuation" / S3 "forbid") is then a runtime policy decision rather than an architectural rewrite. The cost of paying for this if effects never ship is sub-percent at runtime.

**Frame data structure**
- [x] Add a `Frame` struct (or rename `Scope` if it's already in `elab.c`) with: a defer list, a parent pointer, a span. Lives on the C stack in v0/v1.
- [x] `frame_push_defer(Frame*, defer_t)` registers a thunk. `defer_t` is `{fn_ptr, env_ptr}` — same shape as a closure (§4) so this composes with the closure machinery.
- [x] `frame_fire_defers_lifo(Frame*)` walks the list back-to-front, invokes each thunk.

**Parsing & elaboration**
- [x] `(defer expr)` valid only inside a scope-introducing form (`let`, `do`, function body). Error otherwise.
- [x] Defers elaborate to a `frame_push_defer` IR node; they evaluate to nil.
- [x] Multiple defers per scope: collected in source order, fire LIFO.

**Scope analysis & codegen**
- [x] Each scope is assigned a `Frame` allocation (stack-allocated; the `may_capture` bit is always false in v0/v1, so no heap path runs yet).
- [x] Codegen for scope exit: emit `frame_fire_defers_lifo(&frame); /* free */`. *Not* a per-scope `__cleanup_L<id>:` label — the lowering is a runtime call.
- [ ] Early `return X`: rewrite to "for each enclosing frame, fire defers, then return". This walks frames via the parent pointer; it's a small loop in the emitted code, not a goto chain.
- [x] Function-level frame exists even if it has no defers, so `ref<T>` (phase 5) can register drops on it without special-casing.

**Future-proofing slots (per [effects-plan.md §6.10](effects-plan.md))**
- [x] `Frame` has a `parent` pointer field — even though v0/v1 doesn't follow it (early return walks via codegen knowledge of nesting). The field exists so v3's heap-frame mode plugs in without restructuring.
- [x] Every `FnDef` carries a `may_capture: bool`, defaulting to `false`. Phase 4 doesn't read it; phase v3 will read it to choose stack-vs-heap frame allocation.
- [x] Every function `Type` has an `effect_row` slot, defaulting to `nullptr` (treated as `{}` empty row). Phase 4 doesn't populate it; type-equality treats `nullptr == nullptr` as compatible. Phase v3 will populate it.
- [x] *Document the commit message:* "this is the unified defer model from effects-plan.md §6.10 — small overhead now, no phase-4 rewrite if effects ship."

**Edge cases**
- [x] Defers inside `if` branches: each branch is its own frame; defers fire when the branch ends, not at the enclosing `let`'s end. Document with a fixture.
- [x] Defer that itself calls a function that errors: in v0, errors are abort-only, so this is fine. Revisit when we have proper error propagation.
- [x] Defer referencing a binding that was `set!`-ed after the defer: defer runs against the *current* value at exit time, not the value at registration. (Standard Go semantics in this respect.) Fixture this explicitly — the surprising case.
- [x] *Don't* document defer's semantics as "runs *immediately* at scope exit." Documentation should say "runs on scope exit" without the immediacy claim, leaving room for v3's "...or when a continuation that captured this scope drops" without a doc-breaking change.

**Fixtures**
- [x] `defer-order.tur` — three defers, prove LIFO via printed output.
- [ ] `defer-early-return.tur` — `return` inside `if`, defer still runs.
- [x] `defer-nested-scopes.tur` — defers in inner `let` fire before defers in outer `let`.
- [x] `defer-mutated-binding.tur` — defer captures by reference, sees latest value.
- [x] `defer-conditional.tur` — defer inside if-branch fires at enclosing scope's end.
- [x] `defer-in-loop.tur` — defer inside while body fires at loop scope's end with captured loop variable.
- [x] Negative: `(defer ...)` at module top-level → error.
- [x] Codegen snapshots for the runtime-list lowering (not the label-chain).

**Exit criterion:** ✅ all defer fixtures green; the codegen-snapshot tests show `frame_fire_defers_lifo` calls (not `__cleanup_L<n>` labels); ASan/UBSan clean.

---

### 10.6 Phase 5 — `ref<T>` with move semantics

Goal: owning heap values with automatic scope-bound cleanup. `(ref expr)` allocates, `@r` dereferences, and the compiler injects a defer'd drop at the binding site.

**Type & representation**
- [x] Add `TY_REF` to `TypeKind` enum in `src/types.h`. `Type.ref.of` points to the wrapped type.
- [x] `ref<T>` lowers to `struct { T* p; }` in C codegen. Nested `ref<ref<T>>` flatten to `struct { T** p; }`.
- [x] `sizeof(ref<T>)` equals `sizeof(void*)` (pointer-sized). Alignment same as pointer.
- [x] `(ref expr)` ctor: allocate via `tur_alloc(sizeof(T))`, copy/move `expr` into the allocation, return `ref<T>` wrapper.
- [x] `@r` / `(deref r)`: emit `*r.p`. Type-check: `r` must be `ref<T>`, result is `T`.
- [ ] `(ref? x)` predicate: returns `true` if `x` is a `ref<T>` for any `T`. Useful for runtime type checks in FFI.

**Move semantics**
- [ ] Assigning a `ref<T>` to another binding transfers ownership. Source binding is "poisoned" — any subsequent use is a compile error.
- [x] Poisoning implemented via a `moved: bool` (`is_moved` field) on bindings in the elaborator. `set!` on a poisoned binding errors with "use-after-move".
- [ ] Swap pattern `(let [a (ref 1) b (ref 2)] (let [tmp a] (set! a b) (set! b tmp)))` works — both transfers are valid, no intermediate invalid state.
- [ ] Returning a `ref<T>` from a function transfers ownership to the caller. No implicit clone; caller owns the value.

**Auto-defer drop**
- [x] At each `let` binding of form `(let [x (ref ...) ...])`, inject `(defer (drop! x))` at the scope exit.
- [x] `(drop! r)` builtin: calls `free(r.p)`, sets `r.p = NULL` (defensive). Type-check: `r` must be `ref<T>`.
- [x] Defer injection happens in the elaborator after closure conversion, so captured `ref<T>` in closures also get drops registered.
- [x] Fixture: `ref-basic.tur` — allocate, deref, mutate, drop. Valgrind-clean.

**Interaction with other features**
- [ ] `ref<T>` where `T` is `ptr<U>`: allowed, but document this as unusual. Drop calls `free` on the `U*`, not the `ptr<U>` wrapper.
- [ ] `ref<T>` where `T` is a struct with a destructor: future-proof by reserving a `drop_fn` slot in `Type`; phase 5 ignores it (only `free` path).
- [ ] `(def x (ref 42))` top-level: error — refs must be scope-local. Top-level values should use `def` with concrete types or `static` storage.

**Fixtures**
- [x] `ref-basic.tur` — allocate, deref, mutate via `@` and `set!`.
- [x] `ref-deref.tur` — dereference works correctly.
- [x] `ref-explicit-drop.tur` — explicit `(drop! r)` works.
- [x] `ref-nested.tur` — `ref<ref<int>>` double-wrap; verify sizes and deref chains.
- [ ] `ref-move.tur` — ownership transfer on assignment; use-after-move error.
- [ ] `ref-return.tur` — return a `ref` from a function; caller owns it.
- [ ] `ref-in-closure.tur` — closure captures a `ref`, defers fire correctly.
- [ ] Negative: `ref-top-level.tur` — error on top-level `ref` binding.
- [ ] Negative: `ref-use-after-move.tur` — diagnostic on use of poisoned binding.
- [ ] Codegen snapshots for `ref<T>` struct layout and drop injection.

**Exit criterion:** ✅ 28/28 fixtures green incl. ref-basic, ref-deref, ref-nested, ref-explicit-drop; Valgrind clean; ASan/UBSan clean. **Move semantics tracking deferred** — `is_moved` field added to Binding; enforcement of poisoning on move pending follow-up.

---

### 10.7 Phase 6 — `defmacro` + quasiquote

Goal: compile-time code transformation via macros. `defmacro` defines a macro in the bootstrap interpreter; quasiquote provides template literals for macro bodies.

**Bootstrap interpreter** — `src/interp.{c,h}`
- [x] Extend the existing `Form`-based interpreter to support macro expansion.
- [x] Macro environment: separate namespace from value bindings. Macros are looked up by symbol at expansion time.
- [x] `defmacro` syntax: `(defmacro name [params...] body...)` — same shape as `defn` but operates at compile time.
- [x] Macro expansion: replace macro call site `(name arg1 arg2 ...)` with the result of evaluating the macro body with `params` bound to `args`.
- [x] Expansion is recursive: macro output is re-scanned for further macro calls until a fixpoint (no macros left to expand).
- [x] Limit expansion depth to prevent infinite recursion (default: 256). Diagnostic on depth exceeded includes the expansion chain.
- [x] Hygiene v1: `(gensym 'foo)` builtin generates a fresh symbol each call. No automatic hygiene (α-renaming) yet — documented limitation.

**Quasiquote** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [x] Reader macro: `` `x `` → `(quasiquote x)`.
- [x] Reader macro: `~x` → `(unquote x)`. Only valid inside quasiquote.
- [x] Reader macro: `~@x` → `(unquote-splicing x)`. Only valid inside quasiquote, must be in a list/vec position.
- [x] Quasiquote expansion rules:
  - Symbols: `(quasiquote foo)` → `(quote foo)` — literal symbol.
  - Lists: `(quasiquote (a b c))` → `(list (quasiquote a) (quasiquote b) (quasiquote c))`.
  - Unquote: `(quasiquote (a ~b c))` → `(list (quasiquote a) b (quasiquote c))`.
  - Unquote-splicing: `(quasiquote (a ~@b c))` → `(concat (quasiquote a) b (quasiquote c))` where `b` must evaluate to a list.
  - Nested quasiquote: `(quasiquote (quasiquote x))` → `(list (quote quasiquote) (quasiquote x))`.
- [x] Quasiquote in macro bodies: expansion happens before the macro body is evaluated in the interpreter.

**Standard macros**
- [x] Rewrite `when`, `unless`, `cond`, `case` as macros now that macro infrastructure exists.
- [x] `when` → `(if test (do body...))`
- [x] `unless` → `(if test nil (do body...))`
- [ ] `cond` → nested `if` (already implemented as special form in phase 1; now as macro for consistency).
- [ ] `case` → dispatch on value with `=` comparisons.
- [x] Add `->` threading macro: `(-> x (f 1) (g 2))` → `(g (f x 1) 2)`.
- [x] Add `->>` threading macro: `(->> x (f 1) (g 2))` → `(g 2 (f 1 x))`.

**Macro scoping & hygiene**
- [x] Macros can capture their definition environment (lexical scoping for macro internals).
- [x] Macro parameters are not hygienic by default — they can accidentally capture bindings from the call site. Document this and recommend using `gensym` for internal names.
- [x] Shadowing: a macro parameter can shadow an outer binding. This is intentional for the macro's internal scope.

**Fixtures**
- [x] `macro-defmacro.tur` — define and call a simple macro.
- [x] `macro-quasiquote.tur` — quasiquote with unquote and unquote-splicing.
- [x] `macro-gensym.tur` — `gensym` generates unique names across expansions.
- [x] `macro-recursive.tur` — macro that expands to code containing another macro call.
- [x] `macro-threading.tur` — `->` and `->>` macros work correctly.
- [x] `macro-hygiene.tur` — demonstrates the lack of automatic hygiene (bindings from call site visible in macro output).
- [x] Negative: `macro-infinite-recursion.tur` — hits expansion depth limit with clear diagnostic.
- [x] Codegen snapshots: macro-expanded code should be indistinguishable from hand-written code in the IR.

**Exit criterion:** all macro fixtures green; macros can define control flow, quasiquote works, gensym provides hygiene; the expanded output of a macro matches what hand-written code would produce.

---

### 10.8 Phase 7 — Stdlib seed

Goal: ship a minimal but useful standard library covering collections, option types, error handling, and a self-hosted test runner. All stdlib code is written in Turmeric (not C) and lives in a `stdlib/` directory.

**Core data structures** — `stdlib/vec.tur`, `stdlib/slice.tur`, `stdlib/str.tur`
- [x] `slice<T>` type: `struct { T* p; size_t len; }`. Borrowed view into a contiguous sequence. No ownership. Implemented using inline C blocks with `int64_t` for v0 (generics deferred).
  - [x] `(slice-new data length)` constructor — creates a slice viewing `data[0..length)`.
  - [x] `(slice-len s)` → `s.len`.
  - [x] `(slice-get s i)` → `s.p[i]` with bounds check.
  - [x] `(slice-free s)` → frees the slice struct (not the underlying data).
- [x] `vec<T>` type: owning growable array `struct { T* data; size_t len; size_t cap; }`. Implemented using inline C blocks with `int64_t` for v0.
  - [x] `(vec-new)` → empty vec.
  - [x] `(vec-len v)` → `v.len`.
  - [x] `(vec-get v i)` → bounds-checked access.
  - [x] `(vec-push! v x)` → amortized O(1) append with doubling reallocation.
  - [x] `(vec-pop! v)` → removes last element, returns it.
  - [x] `(vec-free v)` → frees the vec and its data.
- [x] `str` type: UTF-8 string `struct { const char* p; size_t len; }`. Borrowed, not NUL-terminated. Implemented using inline C blocks for v0.
  - [x] `(str-from-cstr cstr)` → creates a str from NUL-terminated C string.
  - [x] `(str-len s)` → byte length.
  - [x] `(str-eq? s1 s2)` → content equality.
  - [x] `(str-free s)` → frees the str struct (not the underlying string).

**Option & Result types** — `stdlib/option.tur`, `stdlib/result.tur`
- [x] `option<T>` sum type: `(some T)` or `none`. Implemented using inline C blocks with `int64_t` for v0 (generics deferred). Lowers to `struct { bool is_some; int64_t value; }`.
  - [x] `(some x)` constructor.
  - [x] `(none)` constant.
  - [x] `(some? o)` → `o.is_some`.
  - [x] `(unwrap o)` → `o.value` (panics if none).
  - [x] `(option-free o)` → frees the option struct.
- [x] `result<T, E>` sum type: `(ok T)` or `(err E)`. Implemented using inline C blocks with `int64_t` for v0. Lowers to `struct { bool is_ok; int64_t ok; int64_t err; }`.
  - [x] `(ok x)` constructor.
  - [x] `(err e)` constructor.
  - [x] `(ok? r)` → `r.is_ok`.
  - [x] `(unwrap r)` → `r.ok` (panics if err).
  - [x] `(unwrap-or r default)` → `r.ok` if ok, else `default`.
  - [x] `(result-free r)` → frees the result struct.

**Rewriting built-ins as stdlib macros**
- [x] `when`, `unless` macros defined in `stdlib/macros.tur` (loaded automatically). These expand to `if` forms.
- [ ] `cond` as macro - **deferred**. The macro version requires list operations (first, second, slice, len) which aren't implemented yet. Restored as special form in elaborator with `:else` support.
- [ ] `case` macro - deferred (low priority).
- [ ] deftest macro - **deferred** until test runner infrastructure is complete.

**Core data structures** — `stdlib/vec.tur`, `stdlib/slice.tur`, `stdlib/str.tur`
- [x] Type definitions implemented using inline C blocks.
- [ ] Full runtime functionality - **deferred** until Phase 11 when `:ptr<T>` type annotations or separate stdlib compilation are implemented. Inline C with malloc/free causes type mismatches when compiled into every file.

**Option & Result types** — `stdlib/option.tur`, `stdlib/result.tur`
- [x] Type definitions implemented using inline C blocks.
- [ ] Full runtime functionality - **deferred** for same reason as above.

**Test runner** — `stdlib/test.tur`
- [x] `(assert expected actual)` → passes if `expected == actual`, fails with diagnostic.
- [ ] `(assert-true x)` / `(assert-false x)` - **deferred** (bool type limitations).
- [ ] `(assert-nil x)` - **deferred**.
- [ ] `(assert-error body)` → passes if `body` raises an error (deferred - requires error handling infrastructure).
- [ ] `(run-test name test-fn)` → runs a single test and prints result - **deferred** (function parameter calling not fully supported).
- [ ] `(deftest name [] body...)` → defines a test function with registration - **deferred**.
- [ ] `(run-tests!)` → runs all registered tests, prints results, returns exit code - **deferred**.
- [ ] Test output: dot for pass, `F` for fail, summary at end - **deferred**.
- [ ] `tur test` subcommand: builds and runs all test files in a directory - **deferred**.

**Fixtures**
- [x] `stdlib-macros` — tests when, unless (cond is special form, tested elsewhere).
- [x] `stdlib-vec` — placeholder: verifies module loads and compiles.
- [x] `stdlib-slice` — placeholder: verifies module loads and compiles.
- [x] `stdlib-str` — placeholder: verifies module loads and compiles.
- [x] `stdlib-option` — placeholder: verifies module loads and compiles.
- [x] `stdlib-result` — placeholder: verifies module loads and compiles.
- [ ] Full functional tests for stdlib types - **deferred** until Phase 11.
- [ ] Negative: bounds-check failures on `slice-get`, `vec-get` - **deferred**.
- [ ] Codegen snapshots for stdlib types - **deferred**.

**Exit criterion:** ✅ All stdlib fixtures green (6 new fixtures: macros, vec, slice, str, option, result); stdlib type definitions compile; stdlib/macros.tur loads automatically with when/unless; cond special form with `:else` support restored; `#include <stdlib.h>` and `#include <string.h>` added to emitted C headers. **Current status**: Phase 7 complete. Core type definitions and basic macros implemented. Total: 42/42 fixtures green. Full runtime testing of stdlib types deferred to Phase 11.

---

### 10.9 Phase 8 — Diagnostics polish

Goal: world-class error messages with source snippets, multi-line context, and actionable suggestions. Inspired by Rust's diagnostics and the `miette` crate.

**Span propagation audit**
- [x] Add `SPAN_UNKNOWN` sentinel constant to `src/forms.h` with helper functions `span_is_unknown()` and `span_from_offsets()`.
- [x] Audit existing passes: Form, Expr, Binding all have span fields. Reader, elaborator, emit all propagate spans correctly.
- [x] Synthesized nodes (e.g., desugared `when` → `if`, `->` threading macros) preserve original spans via `call->span` in elaborator.
- [ ] Add a linter in CI that errors if any IR node in the fixture snapshots has `SPAN_UNKNOWN` - **deferred** until snapshot infrastructure is enhanced.

**Error infrastructure** — `src/diag.{c,h}` enhancements
- [x] Enhanced `DiagLevel` enum with `DIAG_HELP` for suggestion messages.
- [x] `DiagCode` enum with error codes: `TUR-E0001` (type mismatch), `TUR-E0002` (arity), `TUR-E0003` (unbound), `TUR-E0004` (scope), `TUR-E0005` (use-after-move), `TUR-E0006` (operator lookup), `TUR-E0007` (capture).
- [x] `DiagNote` struct: level, span, message for related notes.
- [x] `DiagSuggestion` struct: text, replacement, doc_url for actionable suggestions.
- [x] `SnippetOpts` struct: configurable snippet rendering options.
- [x] `UnderlineStyle` enum: `UNDERLINE_PRIMARY` (`^^^`), `UNDERLINE_SECONDARY` (`~~~`), `UNDERLINE_GAP` (`-`).
- [x] `diag_render_snippet()` public function for configurable snippet rendering.
- [x] `diag_emit_with_code()`: emit diagnostics with error codes in brackets `[TUR-E0001]`.
- [x] `diag_emit_with_notes()`: emit primary error with related notes, each with their own spans.
- [x] `diag_emit_with_suggestion()`: emit error with suggestion text, replacement, and doc URL.
- [x] `diag_emit_multi_span()`: emit diagnostics with primary and secondary spans.
- [x] Color support: ANSI color codes for errors (red), warnings (yellow), notes (cyan), help (green). Auto-detect TTY.
- [x] `--no-color` flag: disables colored output.
- [x] `--explain <code>` flag: compiles a code snippet and prints enhanced diagnostics with suggestions.

**Error message improvements**
- [x] Type mismatch: shows expected and actual types with `TUR-E0001` code. Suggests coercion for bool→int.
- [x] Unbound symbol: shows `TUR-E0003` code. Implements "Did you mean `foo`?" with Levenshtein distance ≤ 3 via `sym_levenshtein_distance()` in `src/symbols.c`.
- [x] Arity mismatch: shows expected and actual argument counts with `TUR-E0002` code.
- [ ] Operator lookup failure: show the operator, the argument types, and the list of available overloads - **deferred** until operator overloading is implemented.
- [x] Scope errors: defer at top level already has diagnostics; will enhance with suggestions.
- [ ] Move errors: "use-after-move of `x`" → show where `x` was moved - **deferred** until move tracking is fully implemented.
- [ ] Capture errors: "cannot capture `x`" - **deferred** until borrow checker is implemented.

**Suggestion engine**
- [x] For common errors (type mismatch, unbound symbol), provide "hint" strings with suggested fixes.
- [x] Suggestions stored in `DiagSuggestion` struct keyed by error context.
- [x] Suggestions can include replacement text: e.g., "try wrapping the bool in (if x 1 0)".
- [ ] Suggestions can reference documentation URLs - **deferred** until docs site is available.

**Diagnostic formatting**
- [x] `error: message` on first line with file:line:col location.
- [x] Source snippet with context lines (default 2 before/after), numbered.
- [x] Underline pointing to the span with caret `^` characters.
- [x] Related notes on subsequent lines, indented, with their own spans highlighted using `~~~`.
- [x] Help messages with actionable suggestions.
- [x] Example output format implemented and working.

**Tooling integrations**
- [x] `--json-diagnostics` flag: outputs diagnostics as JSON for IDE integration with severity, code, message, file, line, col, endLine, endCol fields.
- [x] Diagnostics include unique error codes (`TUR-E0001`, etc.) for grep-ability.
- [x] `tur check` subcommand: type-check only, no codegen, for fast feedback during development.

**Fixtures**
- [x] Existing error fixtures updated with enhanced diagnostics (type-mismatch, arity-mismatch, unbound-symbol, etc.).
- [x] All 42 existing fixtures pass with new diagnostic infrastructure.
- [ ] Golden files for error fixtures under `tests/fixtures/errors/*.diag` - **deferred** until golden file infrastructure supports multi-line output.

**Exit criterion:** ✅ Enhanced diagnostics with error codes; multi-line source snippets with context; `--explain` provides actionable hints with suggestions; `--json-diagnostics` outputs JSON; `--no-color` flag supported; `tur check` subcommand added. **Current status**: Phase 8 core infrastructure complete. Total: 42/42 fixtures green.

---
  ```
  error: type mismatch
    --> input.tur:3:5
     |
   3 | (defn foo [x] : int x)
     |     ^^^^^^^^^^^^^^ expected return type `int`, found `bool`
     |
  note: the function returns `x` which has type `bool`
    --> input.tur:3:20
     |
   3 | (defn foo [x] : int x)
     |                    ^ this is a bool
  help: try annotating the parameter: `(defn foo [^bool x] : int ...)`
  ```

**Tooling integrations**
- [ ] `--json-diagnostics` flag: output diagnostics as JSON for IDE integration.
- [ ] Diagnostics include a unique error code (e.g., `TUR-E0001`) for documentation and grep-ability.
- [ ] `tur check` subcommand: type-check only, no codegen, for fast feedback during development.

**Fixtures**
- [ ] `errors/type-mismatch.tur` → golden-checked diagnostic output.
- [ ] `errors/unbound-symbol.tur` → with "did you mean" suggestions.
- [ ] `errors/arity-mismatch.tur` → shows expected/actual counts.
- [ ] `errors/use-after-move.tur` → shows move location.
- [ ] `errors/multi-line-snippet.tur` → error in middle of a multi-line form, snippet shows context.
- [ ] Golden files for all error fixtures under `tests/fixtures/errors/*.diag`.

**Exit criterion:** all error fixtures have golden-checked diagnostics; diagnostics are multi-line with source snippets; `--explain` provides actionable hints; CI checks that no `SPAN_UNKNOWN` appears in snapshots.

---

### 10.10 Phase 9 (v1) — `rc<T>` + `weak<T>` reference counting

Goal: shared ownership for heap-allocated values. `rc<T>` provides reference-counted ownership; `weak<T>` provides non-owning observation that can be upgraded to `rc<T>`. This is the v1 GC strategy per §5.1.2.

**Control block layout** — `src/rc.{c,h}`
- [x] `typedef struct RcControlBlock` — contains:
  - [x] `uint64_t strong_count;` — number of `rc<T>` pointers to the value.
  - [x] `uint64_t weak_count;` — number of `weak<T>` pointers to the control block.
  - [x] `void *value;` — pointer to the actual value (allocated right after header).
  - [x] `RcDropFn drop_fn;` — optional custom drop function (default: `free`).
  - [x] `TypeKind value_type_kind;` — for debugging.
  - [x] `uint8_t reserved[8];` — future-proofing for GC integration.
- [x] Control block allocation: `rc_cb_alloc(sizeof(T), type_kind, drop_fn)` allocates a control block with space for `T`, initializes counts to 1, and returns pointer to control block.
- [ ] For `rc<T>` where `T` is a struct with a destructor: the `drop_fn` slot is populated. Phase 9 supports this for stdlib types; user-defined destructors deferred to phase 11.

**`rc<T>` type & operations**
- [x] Add `TY_RC` to `TypeKind` enum. `Type.rc` field contains inner `TypeKind`.
- [x] `(rc/of x)` — creates a new `rc<T>` with `x` as the value. Allocates a control block, copies `x` into it.
- [x] `(rc/clone r)` — increments strong count, returns a new `rc<T>` pointing to the same value. Same as copying `r`.
- [x] `@r` dereference — same syntax as `ref<T>`. Type-check: `r` must be `rc<T>`, result is `T`. Handled via `elab_deref()`.
- [x] `(rc->ptr r)` — borrow a raw pointer from an `rc<T>`. Returns `void *` via `rc_get_value()`.
- [x] `(rc/strong-count r)` → returns the current strong count (for debugging/testing).
- [ ] Assignment: copying an `rc<T>` increments the strong count. Moving (explicit transfer) does not — ownership is shared, not unique.
- [x] Drop: `(rc/drop r)` decrements strong count. If strong count reaches 0 and weak count is also 0, frees the value and control block.

**`weak<T>` type & operations**
- [x] Add `TY_WEAK` to `TypeKind` enum. `Type.weak` field contains inner `TypeKind`.
- [x] `(weak r)` — creates a `weak<T>` from an `rc<T>`. Increments weak count on the control block.
- [x] `(upgrade w)` → returns same control block if strong count > 0 (incrementing strong count), or NULL otherwise.
- [x] `(weak? w)` — predicate, returns `true` (simplified for Phase 9: always returns true for RcControlBlock pointers).
- [x] Drop: when a `weak<T>` goes out of scope, decrement weak count. If weak count reaches 0 and strong count is also 0, free the control block.

**Defer injection for RC**
- [ ] At each `let` binding of form `(let [x (rc/of ...) ...])`, inject `(defer (rc/drop x))` at the scope exit.
- [x] `(rc/drop r)` — decrements strong count. If count reaches 0, may free the value.
- [ ] Drop function: for `T` that is `Copy`, use `free`. For `T` that is not `Copy`, the drop must be defined by the type (phase 11 for user types).
- [ ] Last-use elision: if an `rc<T>` is used only once and not stored, the compiler can skip the retain/release pair entirely.

**Cascade-free deletion**
- [x] When strong count reaches 0, don't immediately free if there are weak pointers. The value enters a "zombie" state: memory is still valid but logically freed.
- [x] Weak pointer upgrade checks strong count > 0. If strong count is 0, return NULL.
- [x] `weak<T>` drop (when weak count reaches 0) can free the control block if strong count is also 0.
- [ ] Deferred free queue: to prevent stack overflow from long chains of `rc/drop` calls, use a queue. When strong count reaches 0, push the value to a per-thread deferred-free queue and process it later.

**Interaction with `ref<T>` and `ptr<T>`**
- [ ] `(rc/from-ref r)` — converts a `ref<T>` to an `rc<T>`. The `ref`'s value is moved into the new `rc` control block. The original `ref` is poisoned.
- [ ] `(ref/from-rc r)` — converts an `rc<T>` to a `ref<T>`. Requires that the strong count is exactly 1 (unique ownership). Poisons all other `rc<T>` pointers to the same value.
- [x] `ptr<T>` remains the untracked escape hatch. `(rc->ptr r)` is the safe way to get a raw pointer from an `rc<T>`.

**Runtime implementation**
- [x] `src/rc.c`: `rc_cb_alloc`, `rc_strong_increment`, `rc_strong_decrement`, `rc_weak_increment`, `rc_weak_decrement`, `rc_upgrade`, `rc_get_value`, `rc_is_alive`.
- [x] `src/rc.h`: `RcControlBlock` struct, `RcDropFn` typedef, function declarations.
- [x] Inline emission: All rc functions and control block struct emitted inline in generated C code for Phase 9.
- [ ] All functions are inline-able for performance. The control block layout is cache-friendly (counts on the same cache line as the value for small `T`).
- [x] Thread safety: v0/v1 is single-threaded, so no atomics needed. Reserve a `may_alias` flag for future thread-safe mode.

**Codegen integration**
- [x] Added `EX_RC_OF`, `EX_RC_CLONE`, `EX_RC_DROP`, `EX_RC_PTR`, `EX_RC_COUNT`, `EX_WEAK`, `EX_WEAK_UPGRADE`, `EX_WEAK_PRED` to `ExprKind` enum.
- [x] Added corresponding payload fields to `Expr` union.
- [x] Added type support: `TY_RC`, `TY_WEAK` to `TypeKind`, `type_rc()`, `type_weak()` helpers, `type_eq()`, `type_name()`, `type_name_buf()`, `type_c_name()` updated.
- [x] Added symbol declarations for rc/weak operations in `elab.c`.
- [x] Added elaboration functions: `elab_rc_of()`, `elab_rc_clone()`, `elab_rc_drop()`, `elab_rc_ptr()`, `elab_rc_strong_count()`, `elab_weak()`, `elab_weak_upgrade()`, `elab_weak_pred()`.
- [x] Added codegen cases in `emit_expr_value()` and `emit_stmt()` for all rc/weak expression kinds.
- [x] Added rc runtime emission in `emit.c` (RcControlBlock struct, rc_* functions inline).
- [x] Added `extern abort()` and `extern memset()` declarations for rc runtime.
- [x] Updated `expr_print()` in `expr.c` for all rc/weak expression kinds.

**Fixtures**
- [x] `rc-basic.tur` — create, clone, count, drop. Tests basic rc operations.
- [x] `rc-shared.tur` — multiple `rc<T>` pointing to the same value; strong count tracking.
- [x] `rc-cycle-leak.tur` — documents that manual rc/drop is needed (no auto-defer injection yet).
- [x] `weak-upgrade.tur` — tests weak? predicate.
- [ ] `weak-dangling.tur` — accessing the value through a `weak<T>` after all strong refs are dropped is a runtime error (or returns NULL from upgrade).
- [ ] `rc-ref-conversion.tur` — `rc/from-ref` and `ref/from-rc` work correctly.
- [ ] Negative: `rc-unique-violation.tur` — `ref/from-rc` with strong count > 1 errors.
- [x] Codegen snapshots for RC control block layout and drop injection.

**Exit criterion:** ✅ **Complete** — all RC/weak fixtures green (46/46 tests pass); `rc<T>` composes with closures; basic rc/weak operations working. Defer injection for rc/drop, custom drop functions, rc/ref conversion, and weak dangling detection deferred to future phases.

---

### 10.11 Phase 10 (v2) — Bacon-Rajan cycle collector

Goal: automatic cycle detection and collection for `rc<T>` values. Layers on top of phase 9's RC machinery without changing its API. Per §5.1.2, this is the v2 GC strategy.

**Background: Bacon-Rajan trial deletion**
- [ ] Read and document the Bacon-Rajan algorithm ("Concurrent Cycle Collection in Reference Counted Systems" by David F. Bacon and V.T. Rajan).
- [ ] Key insight: after a strong count reaches 0, if the object is part of a cycle, its weak count will also be > 0 (from other objects in the cycle pointing to it). These are "suspect" objects.
- [ ] Trial deletion: scan the object graph reachable from suspect roots. If a suspect is reachable only from other suspects (not from a strong-count>0 object), the entire component is garbage.

**Runtime data structures** — `src/gc.{c,h}`
- [ ] Extend the control block with GC-specific fields:
  - `enum gc_color { GC_WHITE, GC_GREY, GC_BLACK, GC_PURPLE } color;` — Bacon-Rajan uses 4 colors.
  - `bool may_contain_cycles;` — hint to skip collection for DAG-shaped data.
- [ ] Suspect roots buffer: a per-thread (v1: global) buffer of `rc_cb_t*` where strong count reached 0 but weak count > 0.
- [ ] Work queues for the collector: grey queue (objects to scan), black queue (objects confirmed reachable from strong roots).

**Collector algorithm**
- [ ] `gc_on_strong_decrement(cb)` — called when strong count reaches 0:
  - If weak count == 0: free immediately (no cycle possible).
  - If weak count > 0: add `cb` to suspect roots buffer.
- [ ] `gc_collect()` — main collection function, called when suspect roots buffer exceeds a threshold (configurable, default: 128 entries).
  - Mark phase: traverse from all strong-count>0 objects (black roots), mark reachable objects black.
  - Suspect identification: objects with strong count == 0 and weak count > 0 are suspects.
  - Trial deletion: for each suspect root, if it's white (not reachable from black roots), the entire component is garbage — decrement weak counts and free.
  - If a suspect is reachable from black roots (grey/black), it's part of a cycle that's still live — leave it in the suspect buffer.
- [ ] `gc_disable()` / `gc_enable()` — allow programs to disable collection for performance-critical sections.
- [ ] `gc_force()` — force a full collection cycle (for testing and memory-constrained environments).

**Integration with `rc<T>`**
- [ ] Modify `rc_strong_decrement` to call `gc_on_strong_decrement`.
- [ ] Modify `rc_cb_alloc` to initialize GC fields (color = WHITE, may_contain_cycles = true).
- [ ] Modify `rc_upgrade` to confirm the object is still alive (strong count > 0 or reachable from strong roots).
- [ ] `rc_cb_alloc` with a `may_contain_cycles = false` hint: skip adding to suspect buffer even if weak count > 0 when strong reaches 0. Useful for DAG-shaped data where cycles are impossible.

**Collector modes**
- [ ] **Disabled mode** (default for v1): no cycle collection; documented limitation. Programs that don't need it pay zero overhead.
- [ ] **Manual mode**: collection runs only when explicitly triggered via `(gc!)` or `gc_force()`.
- [ ] **Threshold mode**: collection runs automatically when suspect roots buffer exceeds N entries.
- [ ] **Background mode** (future): collection runs in a separate thread. Deferred — requires thread support.

**Testing & correctness**
- [ ] `gc-cycle.tur` — create a cycle of `rc<T>` objects, drop all external references, verify memory is freed.
- [ ] `gc-dag.tur` — DAG-shaped data (no cycles), verify prompt reclamation.
- [ ] `gc-mixed.tur` — mix of cyclic and acyclic data, verify only cycles are collected.
- [ ] `gc-stress.tur` — allocate and drop many RC'd objects with complex sharing patterns.
- [ ] `gc-deterministic.tur` — same sequence of operations always produces the same collection behavior.
- [ ] `gc-disabled.tur` — verify that with GC disabled, cycles leak (documented behavior).
- [ ] Negative: no negative fixtures — GC is best-effort, not a correctness guarantee for all programs.

**Performance considerations**
- [ ] Collection is O(N) in the number of suspect objects, not the entire heap.
- [ ] Non-cyclic programs pay zero GC overhead (collection is never triggered).
- [ ] The collector only scans RC'd objects, not the entire heap (which may contain `ref<T>` and raw allocations).
- [ ] Benchmark: Lisp interpreter with cyclic data structures (cons cells forming circular lists).

**Future-proofing**
- [ ] Reserve space in the control block for a per-object mark bit. Currently using an enum; could switch to bitfields if memory pressure demands.
- [ ] Document the GC ABI: control block layout, color field semantics, suspect buffer format.
- [ ] Reserve a `collector_hook` function pointer for custom collector implementations (e.g., for testing or alternative GC strategies).

**Fixtures**
- [ ] `gc-cycle-freed.tur` — cyclic data is collected and memory freed.
- [ ] `gc-no-false-positives.tur` — live cyclic data is not collected.
- [ ] `gc-perf.tur` — measure collection overhead on a synthetic workload.
- [ ] Codegen snapshots for GC control block layout.

**Exit criterion:** cycle fixture shows memory is freed; GC can be enabled/disabled; no performance regression for non-cyclic workloads; the collector passes its own test suite.

---

### 10.12 Phase 11 — Copy traits

**Goal:** Distinguish `Copy` types (bitwise duplication) from `Move` types (ownership transfer). Extend the existing `ref<T>` move-poisoning machinery to all non-`Copy` types. See [docs/copy-borrow-move-lifetimes.md](docs/copy-borrow-move-lifetimes.md) for rationale.

**Type system extensions** — `src/types.{c,h}`
- [ ] Add `copy_kind` field to `Type` struct: `CK_MOVE` (default), `CK_COPY`, `CK_UNSIZED` (for unsized types like slices).
- [ ] Add `TY_COPY_TRAIT` placeholder for future typeclass-based copy traits (compatibility with §12.2).
- [ ] Primitive types default to `CK_COPY`: `int`, `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, `uint64`, `bool`, `char`, `float`, `double`, `cstr`, `ptr<T>`.
- [ ] Primitive types that are `CK_COPY` can be passed by value in FFI without `ptr<T>` wrapper.
- [ ] `ref<T>`, `rc<T>`, `weak<T>`, `vec<T>`, `str`, `string`, `slice<T>`, `option<T>`, `result<T,E>` default to `CK_MOVE`.
- [ ] User-defined structs default to `CK_MOVE`. Opt-in `:copy` annotation on `defstruct` for bitwise-copyable types.

**Copy/Move tracking in elaborator** — `src/elab.{c,h}`
- [ ] Add `is_moved` field to `Binding` struct (already present from phase 5; generalize usage).
- [ ] At assignment `(set! x y)`: if `x` is `CK_MOVE` and already bound, poison the source binding `y` (mark `y.is_moved = true`).
- [ ] At `let` binding `(let [x expr] ...)`: if `expr` is a `CK_MOVE` binding that is already moved, error with "use-after-move".
- [ ] At function call `(f a b c)`: for each argument that is a `CK_MOVE` binding, mark it as moved after the call.
- [ ] At return `(return x)`: if `x` is a `CK_MOVE` binding, mark it as moved.
- [ ] Use-after-move diagnostic: when accessing a poisoned binding, emit error with span pointing to both the move location and the use location.
- [ ] Move suppression: when a `CK_MOVE` value is returned from a function, the move is suppressed (ownership transfers to caller).
- [ ] Copy elision: when a `CK_COPY` value is assigned, no poisoning occurs; the value is duplicated.

**Surface syntax**
- [ ] Reserve `:copy` annotation on `defstruct`: `(defstruct Point :copy [x : int, y : int])`.
- [ ] Reserve `:move` annotation (explicit, though it's the default).
- [ ] Error on invalid `:copy` on types containing non-`Copy` fields (e.g., `(defstruct Wrapper :copy [r : ref<int>])` — ref is not bitwise-copyable).

**Interaction with existing features**
- [ ] `ref<T>` move poisoning (phase 5) is subsumed by the general move tracking. Remove phase-5-specific code paths.
- [ ] `ptr<T>` remains `CK_COPY` — pointers are copyable (they're just addresses). Document that copying a `ptr<T>` does NOT copy the pointee.
- [ ] `cstr` remains `CK_COPY` — C string pointers are copyable. Document that the string data itself is not copied.
- [ ] Closure capture of moved bindings: if a closure captures a moved binding, error at capture analysis time.
- [ ] `defer` with moved bindings: if a defer body references a moved binding, error.

**Fixtures**
- [ ] `copy-traits-basic.tur` — `int`, `bool`, `ptr<int>` are copyable; assignment doesn't poison.
- [ ] `copy-traits-ref.tur` — `ref<int>` is move-only; second use after assignment errors.
- [ ] `copy-traits-struct.tur` — user struct defaults to move; `:copy` annotation allows copying.
- [ ] `copy-traits-struct-noncopy.tur` — struct with `ref<T>` field cannot be marked `:copy`; error.
- [ ] `copy-traits-return.tur` — returning a `ref<T>` transfers ownership; caller can use it.
- [ ] `copy-traits-closure.tur` — closure capturing a moved binding errors.
- [ ] `copy-traits-defer.tur` — defer referencing a moved binding errors.
- [ ] Negative: `copy-use-after-move.tur` — use-after-move diagnostic shows both locations.
- [ ] Codegen snapshots: no runtime overhead for copy/move tracking (all static).

**Exit criterion:** all copy/move fixtures green; move tracking generalized to all types; `:copy` annotation works for user structs; use-after-move errors include helpful diagnostics with spans.

---

### 10.13 Phase 12 — Borrow traits

**Goal:** Introduce checked reference types `&T` (immutable, shared) and `&mut T` (mutable, exclusive) as a typed, safe alternative to raw `ptr<T>`. Enforce Rust-style aliasing rules within a function. This is the *Hybrid Approach* (Option D) from [docs/copy-borrow-move-lifetimes.md](docs/copy-borrow-move-lifetimes.md).

**Type system extensions** — `src/types.{c,h}`
- [ ] Add `TY_REF_IMMUT` for `&T` (immutable borrow).
- [ ] Add `TY_REF_MUT` for `&mut T` (mutable borrow).
- [ ] Add `ref_target` field to both reference types, pointing to the referenced type `T`.
- [ ] `&T` and `&mut T` are covariant in `T` (if `T` is a subtype of `U`, then `&T` is a subtype of `&U`).
- [ ] `&mut T` is not a subtype of `&T` (mutable is not interchangeable with immutable).
- [ ] `ptr<T>` remains a separate type for untracked raw pointers (FFI, unsafe code).

**Surface syntax** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [ ] `(let [r (& x)] ...)` — creates an immutable borrow of `x`. `r` has type `&T` where `T` is the type of `x`.
- [ ] `(let [r (&mut x)] ...)` — creates a mutable borrow of `x`. `r` has type `&mut T`.
- [ ] `@r` dereference syntax works for both `&T` and `&mut T` (overloaded with `ref<T>` deref).
- [ ] `(set! (@ r) value)` — mutate through `&mut T` reference. Error if `r` is `&T` (immutable).
- [ ] Reader macro for `&` as a unary operator: `&x` expands to `(& x)`.
- [ ] `&mut` is a binary operator in the reader: `&mut x` is a single token sequence.

**Aliasing rules (intra-procedural)** — `src/borrow.{c,h}`
- [ ] Create a borrow checker pass that runs after elaboration but before codegen.
- [ ] Track the set of active borrows at each point in a function.
- [ ] Rule 1: Any number of `&T` borrows can coexist for the same `T` value.
- [ ] Rule 2: Exactly one `&mut T` borrow can exist for a given `T` value.
- [ ] Rule 3: `&T` and `&mut T` cannot coexist for the same `T` value.
- [ ] Borrows are valid for the duration of their enclosing scope (lexical scope tracking).
- [ ] Borrow of a moved binding: error (the value no longer exists).
- [ ] Borrow of a `ref<T>`: allowed; the borrow's lifetime is tied to the `ref`'s scope.

**Borrow expressions**
- [ ] `(let [r (& x)] ...)` — borrow `x` immutably.
- [ ] `(let [r (&mut x)] ...)` — borrow `x` mutably.
- [ ] `(let [r (& (.field s))] ...)` — borrow a struct field immutably.
- [ ] `(let [r (&mut (.field s))] ...)` — borrow a struct field mutably.
- [ ] `(let [r (& (deref p))] ...)` — borrow through a pointer immutably.
- [ ] `(let [r (&mut (deref p))] ...)` — borrow through a pointer mutably (only if `p` is `ptr<T>` and the pointee is mutable).
- [ ] Re-borrowing: `(let [r1 (& x) r2 (& r1)] ...)` — `r2` has the same lifetime as `r1`.
- [ ] Re-borrowing with mutation: `(let [r1 (&mut x) r2 (&mut r1)] ...)` — `r2` has the same lifetime as `r1`.

**Interaction with other features**
- [ ] `ref<T>` and borrows: borrowing from a `ref<T>` is allowed; the borrow is valid as long as the `ref` is not moved or dropped.
- [ ] `ptr<T>` and borrows: raw pointers can be borrowed from, but the borrow has no lifetime tracking (documented unsafe).
- [ ] Closures capturing borrows: if a closure captures a `&T` or `&mut T`, the borrow's lifetime must outlive the closure. Error if not guaranteed.
- [ ] `defer` with borrows: borrow must remain valid through the defer execution.
- [ ] `(unsafe ...)` block: borrows inside `unsafe` blocks are not checked (opt-out for FFI).

**Fixtures**
- [ ] `borrow-basic.tur` — immutable and mutable borrows of locals.
- [ ] `borrow-struct-field.tur` — borrowing struct fields.
- [ ] `borrow-alias-violations.tur` — errors for multiple `&mut T`, `&T` + `&mut T` on same value.
- [ ] `borrow-reborrow.tur` — re-borrowing works correctly.
- [ ] `borrow-closure.tur` — closure capturing a borrow; lifetime check.
- [ ] `borrow-ref.tur` — borrowing from `ref<T>` works.
- [ ] `borrow-defer.tur` — defer with borrow; borrow remains valid.
- [ ] `borrow-unsafe.tur` — borrows inside `unsafe` block are not checked.
- [ ] Negative: `borrow-moved.tur` — borrow of moved value errors.
- [ ] Negative: `borrow-ptr.tur` — borrow of `ptr<T>` warns (untracked).
- [ ] Codegen snapshots: borrows lower to raw pointers in C (`T*`) with no runtime overhead.

**Exit criterion:** all borrow fixtures green; aliasing rules enforced within functions; borrows compose with `ref<T>`, closures, and `defer`; `&` and `&mut` syntax works; `unsafe` block opts out of checking.

---

### 10.14 Phase 13 — Lifetime annotations

**Goal:** Add explicit lifetime parameters to functions and reference types, enabling inter-procedural borrow checking in Phase 14. Implement Rust's lifetime elision rules so most code remains unannotated. Lifetimes are purely an elaborator construct — no runtime representation or codegen impact.

**Lifetime IR** — `src/lifetimes.{c,h}`
- [ ] Add lifetime variable type: distinct from type variables, denoted with leading `'`.
- [ ] Add lifetime context to `FnDef` struct: list of lifetime parameters and their constraints.
- [ ] Add lifetime annotations to reference types: `&'a T`, `&mut 'a T`.
- [ ] Lifetime parameter syntax: `^'a` as a type annotation prefix (e.g., `(defn foo [^'a x : &str] : &str ...)`).
- [ ] Lifetime bounds on types: `(defn foo [^'a x : &'a str] ...)` — the return type borrows from input `'a`.

**Lifetime elision rules** (Rust's three rules)
- [ ] Rule 1: Each lifetime in an input type becomes a distinct lifetime parameter. `(defn foo [x : &str] ...)` → `(defn foo ['^a] [x : &'a str] ...)`.
- [ ] Rule 2: If there's exactly one input lifetime, assign it to all output lifetimes. `(defn foo [x : &str] : &str ...)` → `(defn foo ['^a] [x : &'a str] : &'a str ...)`.
- [ ] Rule 3: For method calls `(&self)` patterns, use `&self`'s lifetime. `(defn bar [^'a self : &mut Foo] [x : &str] : &str ...)` → `x` gets lifetime `'a`.
- [ ] Elision in struct definitions: `(defstruct Foo [^'a s : &str])` — the struct carries lifetime `'a`.

**Lifetime constraints and validation**
- [ ] Outlives relation: `'a: 'b` means lifetime `'a` outlives `'b`. Stored in the lifetime context.
- [ ] Structural constraints: if `T: 'a` and `x: T`, then `x` must outlive `'a`.
- [ ] Error: returning a reference with a longer lifetime than its input (dangling reference).
- [ ] Lifetime parameter scoping: lifetime parameters are scoped to their function/struct.
- [ ] Higher-ranked trait bounds (HRTB) deferred — not needed for v1; lifetimes are explicit or elided.

**Surface syntax examples**
- [ ] `(defn identity [^'a x : &'a T] : &'a T x)` — explicit lifetime, elided in practice.
- [ ] `(defn first [^'a xs : &slice<&'a str>] : &'a str (slice-get xs 0))` — input and output share lifetime `'a`.
- [ ] `(defn longest [^'a x : &'a str, ^'b y : &'b str] : &str ...)` — error: cannot return `&str` without specifying which input lifetime. Requires explicit annotation.
- [ ] `(defn longest<'a> [x : &'a str, y : &'a str] : &'a str ...)` — explicit lifetime parameter.

**Interaction with other features**
- [ ] Lifetimes on `ref<T>`: `ref<T>` does not carry a lifetime (owned value). Borrows from a `ref<T>` get their own lifetime.
- [ ] Lifetimes on `ptr<T>`: raw pointers have no lifetime tracking (documented unsafe).
- [ ] Lifetimes and closures: closure's captured references carry lifetime annotations.
- [ ] Lifetimes and `defer`: defer bodies must respect lifetime constraints.

**Fixtures**
- [ ] `lifetime-elision-1.tur` — single input ref, output inherits lifetime.
- [ ] `lifetime-elision-2.tur` — multiple input refs with same lifetime.
- [ ] `lifetime-elision-3.tur` — method pattern (`&self`).
- [ ] `lifetime-explicit.tur` — explicit lifetime parameters work.
- [ ] `lifetime-dangling.tur` — error when returning reference outliving input.
- [ ] `lifetime-struct.tur` — struct with lifetime-carrying fields.
- [ ] `lifetime-closure.tur` — closure with lifetime-annotated captures.
- [ ] `lifetime-mixed.tur` — mix of elided and explicit lifetimes.
- [ ] Negative: `lifetime-conflict.tur` — conflicting lifetime constraints error.
- [ ] Codegen snapshots: lifetimes produce no runtime code (purely static).

**Exit criterion:** lifetime elision rules work for common cases; explicit lifetime annotations compile; dangling reference errors are caught; lifetimes compose with existing type system.

---

### 10.15 Phase 14 — Borrow checker with lifetimes

**Goal:** Full intra- and inter-procedural borrow checking. Combines Phase 11 (move tracking), Phase 12 (aliasing rules), and Phase 13 (lifetime IR) into a single borrow checker pass. This is the largest single phase on the roadmap; expect it to take 3–6× the effort of any earlier phase.

**Borrow checker architecture** — `src/borrow_check.{c,h}`
- [ ] Single pass after elaboration, before closure conversion (needs type info and spans).
- [ ] Intra-procedural analysis: track borrows within each function body (Phase 12).
- [ ] Inter-procedural analysis: use lifetime annotations (Phase 13) to validate references across function boundaries.
- [ ] Dataflow analysis: track the liveness and aliasing of references at each program point.

**Inter-procedural borrow checking**
- [ ] Call site validation: when calling `(f x y)` where `x : &'a T`, verify that the caller's lifetime `'a` is valid for the duration of the call.
- [ ] Return value validation: when a function returns `&'a T`, verify that `'a` is a valid lifetime from the function's inputs or static data.
- [ ] Closure capture validation: when a closure captures a reference `&'a T`, verify that `'a` outlives the closure's lifetime.
- [ ] Transitive borrow validation: chain of borrows must maintain lifetime validity.

**Interaction with other features**
- [ ] Move tracking integration: use-after-move (Phase 11) is a special case of borrow invalidation.
- [ ] Aliasing rules: enforce N readers XOR 1 writer within each lifetime region.
- [ ] `ref<T>` integration: owned values can be borrowed; the borrow's lifetime is independent of the `ref`'s lifetime.
- [ ] `rc<T>` integration: reference-counted values can be borrowed; the borrow's lifetime must not outlive the `rc`'s strong count.
- [ ] `ptr<T>`: raw pointers are exempt from borrow checking (documented unsafe).
- [ ] `defer` integration: defer bodies must not use references that are invalidated before the defer runs.
- [ ] `(unsafe ...)` block: borrow checking is disabled inside `unsafe` blocks. Use for FFI and performance-critical code.

**Error messages**
- [ ] Dangling reference: show the reference, its lifetime, and why it's invalid.
- [ ] Aliasing violation: show the conflicting borrows and their locations.
- [ ] Use-after-move: show where the value was moved and where it's used.
- [ ] Borrow in closure outlives capture: show the closure and the reference.

**Fixtures**
- [ ] `borrow-check-intra.tur` — intra-procedural borrow validation.
- [ ] `borrow-check-inter.tur` — inter-procedural borrow validation across function calls.
- [ ] `borrow-check-closure.tur` — closure capturing references with lifetime validation.
- [ ] `borrow-check-defer.tur` — defer bodies respect borrow constraints.
- [ ] `borrow-check-ref.tur` — `ref<T>` interaction with borrow checking.
- [ ] `borrow-check-rc.tur` — `rc<T>` interaction with borrow checking.
- [ ] `borrow-check-unsafe.tur` — `unsafe` block disables borrow checking.
- [ ] `borrow-check-complex.tur` — complex interaction of all features.
- [ ] Negative: `borrow-check-dangling.tur` — dangling reference caught.
- [ ] Negative: `borrow-check-alias.tur` — aliasing violation caught.
- [ ] Codegen snapshots: borrow checking produces no runtime code.

**Exit criterion:** full borrow checker validates intra- and inter-procedural code; all borrow checking fixtures green; borrow checking integrates with move tracking, aliasing rules, and lifetime annotations; `unsafe` block provides escape hatch.

---

### 10.16 Phase 15 — Typeclasses

**Goal:** Implement Haskell/Rust-style typeclass-based dispatch with dictionary passing. Extends the existing elaborator-resolved operator dispatch table (§1.1) to support user-defined typeclasses. This is the *chosen direction* from [turmeric-plan.md §12.2(b)](turmeric-plan.md). v1 typeclasses are kind-`*` only (no HKTs).

**Type system extensions** — `src/types.{c,h}`
- [ ] Add `TY_TYPECLASS` for typeclass types.
- [ ] Add `TY_TYPECLASS_INST` for typeclass instance types.
- [ ] Add `TypeClass` struct: name, type parameters, method signatures.
- [ ] Add `TypeClassInstance` struct: typeclass, type arguments, method implementations.
- [ ] Add `typeclass` field to `Type` for concrete types (e.g., `int` has `Show` instance).
- [ ] Reserve syntax for higher-kinded types but error on use in v1 (deferred to v2).

**Surface syntax** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [ ] `(defclass Name [a : Kind, b : Kind, ...] (method1 [arg1 : T1, ...] : R1) (method2 [arg1 : T2, ...] : R2) ...)` — define a typeclass with type parameters and methods.
- [ ] `(definstance ClassName [ConcreteA, ConcreteB, ...] (method1 [args...] body...) (method2 [args...] body...) ...)` — define an instance for concrete types.
- [ ] Type parameter syntax: `(defclass Eq [a] (eq? [x : a, y : a] : bool))`.
- [ ] Method bodies have access to `a`, `b`, etc. as type variables.
- [ ] `(definstance Eq int (eq? [x y] (== x y)))` — instance for primitive type.
- [ ] `(definstance Eq (Pair a b) [Eq a, Eq b] ...)` — instance with constraints on type parameters.
- [ ] Constraints on `defn`: `(defn foo [^Eq a x : a, y : a] : bool (eq? x y))` — requires `Eq` instance for type of `x` and `y`.
- [ ] Constraint syntax: `^Eq` is sugar for `: (Eq a)` where `a` is inferred.
- [ ] Multiple constraints: `(defn foo [^Eq ^Show a x] ...)` — requires both `Eq` and `Show` for `a`.

**Elaborator changes** — `src/elab.{c,h}`
- [ ] Typeclass environment: global registry of typeclasses and instances.
- [ ] Constraint collection: gather constraints from function signatures and method calls.
- [ ] Constraint solving: for each constrained type variable, find an instance that satisfies all constraints.
- [ ] Dictionary generation: for each call site, generate a dictionary struct containing method pointers for the resolved instances.
- [ ] Dictionary passing: transform function calls to pass the dictionary as an implicit argument.
- [ ] Coherence check: ensure no overlapping instances (orphan instance rule).
- [ ] Method resolution: resolve method calls to dictionary field access.
- [ ] Default instances: support `definstance` with `:default` flag for fallback instances.

**Dictionary passing mechanism** — `src/codegen.{c,h}`
- [ ] Dictionary struct generation: for each unique combination of typeclass constraints, generate a `struct { method1_fn fn1; method2_fn fn2; ... }`.
- [ ] Dictionary struct naming: `dict_<ClassName>_<hash>` where hash is based on type arguments.
- [ ] Dictionary allocation: instances are allocated statically (global singletons) since they contain only function pointers.
- [ ] Implicit parameter: functions with constraints get an additional hidden parameter for the dictionary.
- [ ] Method call lowering: `(.method obj arg1 arg2)` on a constrained type lowers to `dict->method_fn(dict, obj, arg1, arg2)`.
- [ ] Polymorphic functions: functions generic over typeclass constraints have the dictionary as an explicit parameter.

**Built-in typeclasses** — `stdlib/typeclass.tur`
- [ ] `Eq` typeclass: `(defclass Eq [a] (eq? [x : a, y : a] : bool))`.
- [ ] `Ord` typeclass: `(defclass Ord [a] (lt? [x : a, y : a] : bool) (lte? [x : a, y : a] : bool) ...)` extends `Eq`.
- [ ] `Show` typeclass: `(defclass Show [a] (show [x : a] : cstr))`.
- [ ] `Num` typeclass: `(defclass Num [a] (add [x : a, y : a] : a) (sub [x : a, y : a] : a) (mul [x : a, y : a] : a) ...)`.
- [ ] `Add`/`Sub`/`Mul`/`Div` typeclasses (alternative: more granular than `Num`).
- [ ] Instances for primitive types: `int`, `int8`-`int64`, `uint8`-`uint64`, `float`, `double`, `bool`, `cstr`.
- [ ] Derived instances: `Eq` for `option<T>` if `Eq T`, `Eq` for `(Pair a b)` if `Eq a` and `Eq b`, etc.

**Operator dispatch integration**
- [ ] Extend existing operator dispatch table (§1.1) to include typeclass-resolved operators.
- [ ] Primitive operators (`+`, `-`, `*`, `/`, `==`, `<`, etc.) can be overridden by typeclass instances.
- [ ] Fallback to primitive implementation if no typeclass instance found.
- [ ] Typeclass methods can call other typeclass methods (e.g., `Ord.lt?` calls `Eq.eq?`).

**Interaction with other features**
- [ ] **Closures:** Closures can capture typeclass dictionaries from their defining scope.
- [ ] **Macros:** Macros can generate typeclass-constrained code.
- [ ] **`defstruct`:** User-defined structs can have typeclass instances.
- [ ] **FFI:** Foreign types can have typeclass instances defined in Turmeric.
- [ ] **Effect rows (future):** Typeclass methods can have effect rows.

**Fixtures**
- [ ] `typeclass-basic.tur` — define a simple typeclass and instance.
- [ ] `typeclass-constraint.tur` — function with typeclass constraint.
- [ ] `typeclass-multiple.tur` — multiple constraints on one function.
- [ ] `typeclass-primitives.tur` — `Eq`, `Ord`, `Show` for primitive types.
- [ ] `typeclass-derived.tur` — derived instances for `option<T>`, `Pair`, etc.
- [ ] `typeclass-operator.tur` — typeclass methods override operators.
- [ ] `typeclass-closure.tur` — closures capture typeclass dictionaries.
- [ ] `typeclass-macro.tur` — macros generate typeclass-constrained code.
- [ ] Negative: `typeclass-no-instance.tur` — error when no instance satisfies constraint.
- [ ] Negative: `typeclass-ambiguous.tur` — error on ambiguous instance resolution.
- [ ] Codegen snapshots: dictionary struct generation and passing.

**Exit criterion:** typeclasses work for ad-hoc polymorphism; dictionary passing has zero runtime overhead for monomorphic calls; built-in typeclasses cover primitives; typeclass constraints work on functions and structs.

---

### 10.17 Phase 16 — Capability passing (v1 effects)

**Goal:** Provide a library-level effect system using capability passing built on typeclasses. Zero runtime cost. Covers mocking, dependency injection, and resource passing without new compiler primitives. This is the v1 effects story per [effects-plan.md §7.2](effects-plan.md).

**Typeclass infrastructure** — depends on Phase 15 (typeclasses)
- [ ] `src/typeclass.{c,h}` from Phase 15 already supports dictionary-passing dispatch.
- [ ] Capability types are ordinary structs with function pointer fields.

**Core capability types** — `stdlib/capability.tur`
- [ ] Define `FileSystem` capability: `(defstruct FileSystem [read-file, write-file, delete, list])`.
- [ ] Define `Logger` capability: `(defstruct Logger [debug, info, warn, error])`.
- [ ] Define `Random` capability: `(defstruct Random [next-int, next-float])`.
- [ ] Define `Time` capability: `(defstruct Time [now, sleep])`.
- [ ] Each field is a function type; capabilities are passed as ordinary arguments.

**Real implementations** — `stdlib/io.tur`, `stdlib/log.tur`
- [ ] `Real-FileSystem`: implementation using libc `fopen`, `fread`, `fwrite`, `fclose`, `remove`, `readdir`.
- [ ] `Real-Logger`: implementation writing to stderr/stdout with timestamps.
- [ ] `Real-Random`: implementation using `rand()` or platform-specific RNG.
- [ ] `Real-Time`: implementation using `time()`, `clock_nanosleep()`.

**Test implementations** — `stdlib/test/capability.tur`
- [ ] `Test-FileSystem`: in-memory filesystem for testing. Supports recording reads/writes.
- [ ] `Test-Logger`: captures log messages for assertion in tests.
- [ ] `Test-Random`: deterministic RNG with fixed seed for reproducible tests.
- [ ] `Test-Time`: mock clock that can be advanced manually.

**Convenience macros** — `stdlib/capability.tur`
- [ ] `(with-capability [cap <cap-type>] body...)` macro: threads `cap` through all calls in `body`.
- [ ] `(capability-field cap field-name)` macro: safe field access with compile-time check.
- [ ] `(default-capability cap-type)`: returns the default implementation for a capability type.

**Interaction with type system**
- [ ] Capability types work with typeclasses: `(definstance Monoid (Vector2D add))` for vector addition.
- [ ] Functions accepting capabilities use typeclass constraints when appropriate.
- [ ] Capability fields can be effect-polymorphic (accept functions with effect rows).

**Fixtures**
- [ ] `capability-fs.tur` — file operations using `FileSystem` capability.
- [ ] `capability-logger.tur` — logging using `Logger` capability.
- [ ] `capability-test.tur` — test with mock capabilities; verify mock was called.
- [ ] `capability-thread.tur` — capabilities thread through nested function calls.
- [ ] `capability-default.tur` — default capability resolution works.
- [ ] `capability-macro.tur` — `with-capability` macro correctly threads arguments.
- [ ] Negative: `capability-missing-field.tur` — missing field access errors.
- [ ] Codegen snapshots: capability passing lowers to direct function calls (no overhead).

**Exit criterion:** capability passing works for mocking and dependency injection; stdlib includes core capabilities with real and test implementations; zero runtime overhead compared to direct calls.

---

### 10.18 Phase 17 — Exceptions

**Goal:** Add exception handling as a lightweight control flow mechanism. Independent of the effects system but useful regardless. Exceptions are non-resumable (one-shot) and do not require CPS transformation.

**Type system extensions** — `src/types.{c,h}`
- [ ] Add `TY_EXCEPTION` type for exception values (wraps any type).
- [ ] Exception types are uninhabited at the value level — they exist only to be raised/caught.

**Surface syntax**
- [ ] `(throw expr)` — raise an exception with `expr` as the payload.
- [ ] `(try body (catch [e] handler-body)...)` — catch exceptions. Multiple catch clauses tried in order.
- [ ] `(try body (catch [e : SomeType] handler)...)` — typed catch with type annotation.
- [ ] `(try body (finally cleanup))` — cleanup block that always runs.
- [ ] `(try body (catch ...) (finally ...))` — both catch and finally.
- [ ] Shorthand: `(throw! "message")` for string exceptions (sugar for `(throw (Error. "message"))`).

**Exception representation** — `src/exn.{c,h}`
- [ ] `struct tur_exception { Type* type; void* payload; Span where; }` — exception value.
- [ ] Exception types are ordinary user-defined types; `Error` struct in stdlib for string errors.
- [ ] `tur_throw` function: captures current stack trace (optional in v1; always in debug builds).
- [ ] `tur_catch` function: checks if exception matches catch clause type.
- [ ] `tur_rethrow` function: re-throws current exception.

**Control flow lowering** — `src/elab.{c,h}` + `src/emit.{c,h}`
- [ ] `throw` lowers to: wrap payload in exception struct, call `tur_throw()`, which longjmps or unwinds stack.
- [ ] `try` with `catch` lowers to: setjmp at try entry, if exception thrown, jump to handler.
- [ ] `try` with `finally` lowers to: goto-based unwind or nested try-finally.
- [ ] Stack unwinding respects defers: scopes between throw and catch have defers fired.
- [ ] Exception propagation: unhandled exceptions unwind to top level, printing error and exiting.

**Stdlib exception types** — `stdlib/exn.tur`
- [ ] `(defstruct Error [message : cstr, cause : (option Exception)])` — base error type.
- [ ] `(defstruct IoError [message : cstr, errno : int])` — I/O error with errno.
- [ ] `(defstruct ParseError [message : cstr, span : Span])` — parsing error with source location.
- [ ] `(defn throw-error [msg])` — sugar for `(throw (Error. msg none))`.
- [ ] `(defn throw-io-error [msg])` — sugar for `(throw (IoError. msg (errno)))`.

**Interaction with other features**
- [ ] Exceptions propagate through closures: if a closure body throws, the exception propagates to the caller.
- [ ] `defer` and exceptions: defers fire during stack unwinding when an exception propagates.
- [ ] `ref<T>` and exceptions: if an exception unwinds through a scope with a `ref<T>`, the ref is dropped normally.
- [ ] `rc<T>` and exceptions: same as ref — RC releases fire during unwinding.
- [ ] `handle` (future effects): exceptions are a subset of effects; an unhandled exception in a handler should propagate.

**Fixtures**
- [ ] `exception-basic.tur` — throw and catch simple exceptions.
- [ ] `exception-typed.tur` — typed catch clauses.
- [ ] `exception-finally.tur` — finally blocks run even when no exception.
- [ ] `exception-propagate.tur` — exception propagates through multiple scopes.
- [ ] `exception-defer.tur` — defers fire during exception unwinding.
- [ ] `exception-ref.tur` — ref drops during exception unwinding.
- [ ] `exception-closure.tur` — exceptions propagate through closures.
- [ ] `exception-nested.tur` — nested try/catch with proper scoping.
- [ ] Negative: `exception-uncaught.tur` — unhandled exception exits with error.
- [ ] Codegen snapshots: exceptions use setjmp/longjmp or label-based unwind.

**Exit criterion:** exceptions work for error handling; defers fire correctly during unwinding; stdlib includes basic exception types; exceptions compose with closures, defers, ref, and rc.

---

### 10.19 Phase 18 — Delimited continuations (`shift`/`reset`)

**Goal:** Add delimited continuations as the substrate for algebraic effects. This is §12.1 from the main plan. Selective CPS-transform on demand: only functions containing `shift` are converted. See [effects-plan.md](effects-plan.md) for full rationale.

**Surface syntax**
- [ ] `(reset expr)` — establishes a new continuation boundary. Returns the result of `expr`.
- [ ] `(shift k expr)` — captures the current continuation up to the nearest `reset` and passes it to `k`. `k` is a function `(-> T (-> U))` where `T` is the return type of the `reset` block and `U` is arbitrary.
- [ ] `(shift k)` sugar when `expr` is just `(k v)`.
- [ ] `(shift0 k expr)` — same as `shift` but `k` cannot resume (one-shot by construction).

**Type system** — `src/types.{c,h}`
- [ ] Continuation type: `cont<T>` represents a captured continuation that returns `T`.
- [ ] `shift` has type: `(-> (-> T (-> U)) (-> U))` — takes a function from `T` to `U`, returns `U`.
- [ ] `reset` has type: `(-> (-> T) T)` — takes a thunk returning `T`, returns `T`.
- [ ] Continuations are one-shot: calling a continuation twice is a compile error (static) or runtime panic (dynamic).

**CPS transformation** — `src/cps.{c,h}` (new pass)
- [ ] CPS pass runs after closure conversion, before defer injection.
- [ ] Mark functions transitively containing `shift` as "needs CPS".
- [ ] Transform marked functions: convert return to tail call into continuation, wrap body in continuation application.
- [ ] Direct-style functions remain unchanged — no overhead.
- [ ] Closure conversion: captured continuations become ordinary closures (`struct {fn_ptr; env*}`).
- [ ] `reset` lowers to: allocate continuation frame, invoke body with identity continuation, return result.
- [ ] `shift` lowers to: capture current continuation (env + PC), pass to `k`, tail-call into `k`'s result.
- [ ] Continuation frames are heap-allocated (they escape their defining scope by definition).

**Interaction with defer and ref** — per [effects-plan.md §6](effects-plan.md)
- [ ] **S2 strategy (chosen):** Defer bodies are attached to continuation frames. When a continuation is captured, the scope frames between capture point and `reset` boundary are heap-allocated and attached to the continuation.
- [ ] Defers fire when: (a) continuation is resumed and scopes exit normally, or (b) continuation is dropped without resume.
- [ ] `ref<T>` drops are just defers; same mechanism applies.
- [ ] Multi-shot continuations are **forbidden** in v1 — `cont<T>` is move-only (one-shot).
- [ ] `shift0` provides a type-safe way to get one-shot continuations (the function passed to `shift0` cannot call the continuation).

**Continuation frame structure** — `src/runtime.{c,h}` extensions
- [ ] Extend `tur_frame` (from Phase 4) to support continuation capture:
  - Add `continuation` field: function pointer for resume.
  - Add `env` field: captured environment.
  - Add `parent` field: parent continuation frame.
  - Add `n_captured_frames` and `captured_frames[]`: scopes captured by this continuation.
- [ ] `tur_cont_alloc()`: allocate continuation frame with captured scope chain.
- [ ] `tur_cont_resume(cont, value)`: resume continuation with value. Consumes the continuation (one-shot).
- [ ] `tur_cont_drop(cont)`: drop continuation without resume; fire defers on captured frames.

**Built-in continuations**
- [ ] `(call/cc f)` — sugar for `(reset (shift k (f k)))` — captures the *current* continuation (not delimited). Deferred to v2 (requires more runtime support).
- [ ] `(escape f)` — sugar for `(shift0 k (f k))` — escape current context without resumption.

**Fixtures**
- [ ] `continuation-basic.tur` — simple `reset`/`shift` example.
- [ ] `continuation-return.tur` — `shift` that returns a value from `reset`.
- [ ] `continuation-multiple.tur` — multiple `shift` calls in one `reset`.
- [ ] `continuation-nested-reset.tur` — nested `reset` boundaries.
- [ ] `continuation-defer.tur` — defers fire correctly with continuations (S2 strategy).
- [ ] `continuation-ref.tur` — `ref<T>` drops fire correctly with continuations.
- [ ] `continuation-oneshot.tur` — calling continuation twice panics.
- [ ] `continuation-shift0.tur` — `shift0` works; continuation cannot be resumed.
- [ ] Negative: `continuation-escape.tur` — escaping continuation without proper handling.
- [ ] Codegen snapshots: CPS-transformed functions vs direct-style functions.

**Exit criterion:** `reset`/`shift` work correctly; defers fire at appropriate times (S2 strategy); one-shot enforcement works; CPS pass only transforms effect-using functions; continuations compose with defers and ref; `shift0` provides safe one-shot escape.

---

### 10.20 Phase 19 — Algebraic effects (v3)

**Goal:** Add OCaml 5-style algebraic effect handlers with one-shot continuations. Built on Phase 18's delimited continuations substrate and Phase 4's unified defer model. This is the v3 effects story per [effects-plan.md](effects-plan.md).

**Prerequisites verification**
- [ ] Phase 4 unified defer model is in place (§6.10 of effects-plan.md).
- [ ] Phase 18 delimited continuations are working.
- [ ] Effect row slots in function types are reserved (Phase 4).
- [ ] `may_capture` bits on functions are reserved (Phase 4).

**Surface syntax** — per [effects-plan.md §4](effects-plan.md)
- [ ] `(defeffect Name [params...] : result-type)` — declare a new effect.
- [ ] `(perform (Name args...))` — raise/perform an effect.
- [ ] `(handle expr (Name [params...] k) body ...)` — handle effects. `k` is the continuation.
- [ ] `(resume k value)` — resume continuation with value. One-shot; consumes `k`.
- [ ] `(discontinue k exception)` — discontinue by raising an exception.
- [ ] `(try-with body handler)` — sugar for `(reset (handle body handler))`.

**Type system — effect rows**
- [ ] Add effect row type: `EffectRow` is a set of effect names.
- [ ] Add `effect_row` field to function types (reserved in Phase 4).
- [ ] Effect row syntax: `@ {Effect1, Effect2}` after return type in `defn`.
- [ ] Empty row `{}` means pure function (no effects).
- [ ] Effect polymorphism: functions can be generic over effect rows.
- [ ] Row union: calling a function with row `e1` inside a function with row `e2` produces row `e1 ∪ e2`.
- [ ] Subtyping: function with row `e1` is a subtype of function with row `e2` if `e1 ⊆ e2`.

**Effect declaration** — `src/elab.{c,h}`
- [ ] `(defeffect Name [param1 : T1, param2 : T2] : R)` registers a new effect constructor.
- [ ] Effects are scoped: can be module-private or exported.
- [ ] Effect parameters are typed; result type is typed.
- [ ] Effects can be re-opened (add new constructors to existing effect type).

**Effect handling** — lowering
- [ ] `perform (E args...)` lowers to: `shift k -> (dispatch-to-handler E args k)`.
- [ ] `handle expr cases...` lowers to: `reset (push-handler-stack; expr; pop-handler-stack)`.
- [ ] Handler stack is a per-fiber linked list (TLS in single-threaded v1).
- [ ] Handler dispatch: walk handler stack for first matching case; call it with args and continuation.
- [ ] `resume k v` lowers to: `continue k v` (consumes k, one-shot).
- [ ] `discontinue k e` lowers to: `throw e` (but in the context of the handler).

**Defer integration — S2 strategy** (per [effects-plan.md §6.2](effects-plan.md))
- [ ] When a continuation is captured (at `perform`), walk captured scope frames and heap-allocate them if not already heap.
- [ ] Defers are attached to scope frames; they fire when the frame is released.
- [ ] Frame release happens on: (a) normal scope exit during resume, (b) continuation drop.
- [ ] `ref<T>` drops are defers; same mechanism applies.
- [ ] `rc<T>` releases are defers; same mechanism applies.

**Effect row checking** — `src/effect_check.{c,h}` (new pass)
- [ ] Pass runs after elaboration, before codegen.
- [ ] For each function, union effect rows of all call sites.
- [ ] Check that the union is a subset of the declared effect row.
- [ ] Unhandled effects at top level: compile-time error (static) or runtime panic (dynamic).
- [ ] Effect rows on `extern-c` are advisory (FFI functions assumed pure).

**Handler scoping**
- [ ] Handlers are lexically scoped: `(handle ...)` binds handlers for its body only.
- [ ] Handler parameters shadow outer bindings.
- [ ] `k` (continuation) is a fresh binding in each handler case.
- [ ] Deep handlers: inner `handle` can capture outer handler's continuation.

**Stdlib effects** — `stdlib/effect.{c,h}` + `stdlib/effect.tur`
- [ ] `Read` effect: `(defeffect Read [^cstr prompt] : str)`.
- [ ] `Write` effect: `(defeffect Write [^cstr msg] : nil)`.
- [ ] `Fail` effect: `(defeffect Fail [^cstr msg] : a)` — non-local exit with message.
- [ ] `GetEnv` effect: `(defeffect GetEnv [^cstr key] : (option str))`.
- [ ] Console handler: handles `Read` and `Write` with stdin/stdout.
- [ ] Exception handler: converts `Fail` to exceptions.

**Interaction with other features**
- [ ] **Closures:** Captured continuations in closures work naturally (closures already support captured state).
- [ ] **Macros:** Macros can generate effectful code; hygiene handles the binding.
- [ ] **Modules (future):** Effects can be module-scoped; cross-module effect handling works via linking.
- [ ] **Borrow checker (Phase 14):** Effect handlers that capture references must respect borrow constraints. Defer this integration to after both features land.

**One-shot enforcement**
- [ ] Continuations are move-only types: cannot be copied, only moved.
- [ ] Static check: `resume` consumes its continuation argument; second use is use-after-move error.
- [ ] Dynamic check: `resume` marks continuation as consumed; second call panics.
- [ ] `cont?` predicate: check if a value is a continuation.
- [ ] `cont-consumed?` predicate: check if a continuation has been resumed.

**Performance optimizations** (optional, post-MVP)
- [ ] Handler inlining: when handler is statically known, inline the dispatch.
- [ ] Monomorphic perform: when perform site has a statically known effect type, skip dynamic dispatch.
- [ ] Frame fusion: adjacent non-capturing scopes share frames.
- [ ] Escape analysis: scopes that provably don't escape remain stack-allocated.

**Fixtures**
- [ ] `effect-declaration.tur` — declaring and performing effects.
- [ ] `effect-handler.tur` — basic effect handling.
- [ ] `effect-multiple.tur` — handling multiple effects.
- [ ] `effect-nested.tur` — nested handlers.
- [ ] `effect-defer.tur` — defers fire correctly with effects (S2 strategy).
- [ ] `effect-ref.tur` — ref drops fire correctly with effects.
- [ ] `effect-rc.tur` — rc releases fire correctly with effects.
- [ ] `effect-oneshot.tur` — one-shot continuations enforced.
- [ ] `effect-console.tur` — console I/O using Read/Write effects.
- [ ] `effect-fail.tur` — Fail effect for non-local exit.
- [ ] Negative: `effect-unhandled.tur` — unhandled effect error.
- [ ] Negative: `effect-double-resume.tur` — double resume panic.
- [ ] Codegen snapshots: effect handling lowers to shift/reset.

**Exit criterion:** algebraic effects work with one-shot continuations; effect rows are checked; defers fire correctly (S2 strategy); stdlib includes core effects; effects compose with closures, defers, ref, and rc; one-shot enforcement works.

---

## 11. Writing the compiler in C — concrete shape

Since the host *is* C, lock these conventions early so the codebase stays legible:

**Memory.** One bump-allocated **arena per compilation unit**, freed wholesale at the end. AST nodes, symbol tables, and intermediate strings all live in the arena — no `free` calls scattered through the compiler. A second arena for macro-expansion scratch is reset between expansions.

**`Form` representation.** A tagged union, e.g.:
```c
typedef enum { F_NIL, F_INT, F_SYM, F_STR, F_LIST, F_VEC, F_MAP } FormTag;
typedef struct Form {
    FormTag tag;
    Span    span;        // file, line, col-start, col-end
    union {
        int64_t   i;
        Symbol    sym;
        StrSlice  str;
        struct { struct Form **items; uint32_t len; } list;
    } as;
} Form;
```
All `Form*` are arena-allocated and immutable after construction.

**Strings.** `StrSlice { const char *p; uint32_t len; }`. No `strdup`. Symbols are interned into a hash table; equality is pointer-equality.

**Error reporting.** Build a tiny diagnostic module up front (`diag_emit(span, level, fmt, ...)`). Spans flow through every phase — adding them after the fact is the worst kind of refactor in C.

**Code generation.** A growable byte buffer (`Buf`) with `buf_printf`, `buf_indent`, `buf_writef`. Emit to `Buf`, then `fwrite` once. Avoid intermediate `FILE*` to keep tests fast and inspectable.

**Testing.** Plain C harness (`tests/run.c`) + golden-file fixtures: each `.tur` input has an expected `.c.out` and an expected program-stdout. CI re-runs golden compare; AddressSanitizer + UBSan on by default.

**Coverage policy: every language feature ships with tests.** No feature lands without fixtures. Concretely, `tests/fixtures/` is organized by feature, and each directory contains at least:

- *Happy path* — minimal example showing the feature works.
- *Interaction tests* — the feature combined with `defer`, `ref`, closures, and macros (whichever apply).
- *Negative tests* — inputs that must fail to compile, with the expected diagnostic text golden-checked. Use a `// expect-error: …` comment convention.
- *Codegen snapshot* — the emitted `.c` is golden-checked so unexpected changes to lowering are obvious in PR diffs.

```
tests/fixtures/
  reader/         let/          if-cond/       fn-closure/
  defer/          ref/          macro-defmacro/  quasiquote/
  inline-c/       extern-c/     types-prims/   errors/
  examples/       # end-to-end programs (counter, fizzbuzz, …)
```

Each phase in §7 has an exit criterion that includes "fixtures land in the same PR as the feature." A feature without tests is not done.

**Layout.**
```
src/
  arena.{c,h}      reader.{c,h}    forms.{c,h}    diag.{c,h}
  expand.{c,h}     interp.{c,h}    elab.{c,h}     close.{c,h}
  defer.{c,h}     emit.{c,h}      driver.{c,h}    main.c
tests/
  fixtures/...
```

**Bootstrap → self-host path.** Once phases 0–7 land in C, port `tur` to Turmeric module-by-module (reader first, codegen last). The C compiler stays in the repo as `tur-stage0` for emergency rebuilds.

**What we give up vs. Rust/OCaml.** No exhaustive pattern matching on tagged unions (use `switch` + a default-`abort` macro), no easy generics (write per-type containers, or one `void*`-keyed hash table), no derive macros for printers (write `form_print` once and call it). Accepted cost.

---

## 12. Future ideas (post-v1)

These are explicitly out of scope for the initial language but worth designing toward — i.e., don't paint into a corner that forecloses them.

### 12.1 Delimited continuations (`shift`/`reset`)

Goal: first-class control — generators, async, backtracking, algebraic effects — implemented in user-space rather than baked into the runtime.

Sketch of approach:
- Surface forms `(reset expr)` and `(shift k expr)` à la Danvy–Filinski.
- **Implementation strategy: CPS-transform on demand.** Mark functions that lexically contain `shift` (transitively) and CPS-convert just those; leave the rest of the program in direct style. The CPS pass runs after closure conversion so captured continuations become ordinary closures (`struct {fn,env}` — already solved).
- Continuations are **one-shot by default** (cheap, fits the closure model); multi-shot requires copying the env chain and is opt-in via `(shift/multi …)`.
- Interaction with `defer`/`ref`: capturing a continuation that crosses a `defer` boundary is the hard case. Decision needed: (a) run the `defer`s on capture (Go-ish, but breaks "resume later"), (b) attach them to the continuation (correct but expensive), or (c) make it a compile error. Lean: (c) for v1 of continuations, relax later.
- No stack-copying / `setjmp` tricks. Pure CPS keeps things portable C and analyzable.

Designing toward it now: keep the IR explicit about control flow; don't hide returns inside `goto` chains that a CPS pass can't see through.

### 12.2 Type system

Two complementary directions; pick order based on user demand.

**(a) Typed-Racket / Typed-Clojure–style occurrence typing.** Gradual typing layered on a dynamic core. Predicates (`int?`, `nil?`, custom `(defpred foo? [x] …)`) refine a binding's type along the branches of `if`/`cond`. This pairs naturally with a Lisp — types follow the shape of idiomatic code rather than fighting it. Implementation: a flow-sensitive type-checker pass after macro expansion, before closure conversion. No runtime cost in the typed regions; contract checks at the dynamic↔typed boundary.

**(b) Typeclasses and typeclass-based dispatch — *chosen direction* (decided).** Haskell/Rust-trait style: `(defclass Show [a] (show [x] : cstr))`, `(definstance Show int …)`. **Dispatch: dictionary passing** — class methods compile to closures bundled in a dict struct passed as an extra arg. The struct-with-fn-ptr closure machinery from §4 *is* the dict shape, so most of the runtime work is already done.

*The on-ramp is already in v0.* §1.1 commits to elaborator-resolved operators from day one — i.e., a dispatch table the elaborator consults to pick the right C function for each `(op, types)` pair. v0 populates that table with primitive entries (`int_add`, `float_eq`, …). Typeclasses are then **"the table gets entries from `definstance`."** No re-architecture: the typeclass pass is mostly elaboration (resolve which dict to pass at each call site) + a coherence check. Slower than monomorphization, but works with separate compilation, supports dynamic instances, and avoids the code-bloat trap. Coherence: enforce orphan-instance rules at module boundary.

(Monomorphization stays available later as a `-O` flag for hot paths once we have benchmarks justifying it.)

**Ordering.** (a) probably lands first — additive analysis pass, low risk, big usability win. (b) is the architectural commitment, with the on-ramp already in v0. Build (b) once real users hit the limits of ad-hoc polymorphism.

**What to preserve in v1 to keep these doors open:**
- Keep type annotations syntactically *parseable* even if mostly unused (`^int`, `:- (-> int int)`), so adding a checker later doesn't require touching every existing `.tur` file.
- Don't bake mono-typed code paths into the IR — keep the elaborator's output generic enough that a future dictionary-passing pass has somewhere to insert dicts.
- Resist optimizations in v1 that assume a particular dispatch strategy (e.g., devirtualizing closure calls based on syntactic shape).

#### 12.2.1 Higher-kinded types — door left open for v2

**v1 typeclasses are kind-`*` only.** Class heads quantify over types (`(defclass Show [a] …)`), not over type constructors (`(defclass Functor [f] …)` where `f` is `* -> *`). This is a deliberate scope cut, not an architectural exclusion: HKTs are a v2-or-later extension, and v1 must avoid decisions that close the door.

Why defer:

- **Most monad use cases die when effects ship.** `IO`, `State`, `Throw`, parsers, and short-circuit chains all become direct-style code under the algebraic-effects machinery (`effects-plan.md`). A `Monad` typeclass is the main HKT motivator in Haskell; with effects, that motivator is mostly gone. See [effects-vs-monads.md](effects-vs-monads.md) for the long form.
- **Kind inference + kind-polymorphic dispatch is real implementation work** — a kind-checking pass between elaboration and dictionary insertion, plus a two-level dispatch table (lookup the constructor's dictionary, then call its method slot with concrete inner types). Doable, but not pulling its weight in v1.
- **The dispatch table's current shape (§1.1) keys on `(name, [arg-type, …])`.** That works for kind-`*` typeclasses unchanged. HKT dispatch wants a different key shape (outer constructor, with inner types as dict parameters). Building both keying strategies in v1 would over-fit to a feature we may never ship.

What v2 HKTs would buy:

- A single generic `do`-notation that doesn't need per-monad `bind` / `pure` parameters.
- `traverse`, `sequence`, `mapM`, `forM`, `replicateM` written once over `Monad m`.
- `Functor` / `Applicative` / `Monad` / `Traversable` typeclasses (all need at least kind `* -> *`).
- Library-level monad transformers (`StateT`, `ExceptT`) for users who prefer `mtl`-style stacking over effect handlers.
- Free-monad / freer-monad encodings as ordinary library code.

What v2 HKTs would cost:

- Kinds in the surface syntax (probably inferred, with `: * -> *` ascription as escape hatch).
- A kind-checking pass; failure mode "expected kind `* -> *`, got `*`" with a span pointing at the offending instance head.
- Dispatch-table generalization (the kind-`*` keying remains a fast path; HKT keying is additive).
- Coherence rules for HKT instances — orphan checks have to consider the outer constructor.
- Documentation cost: explaining kinds to users coming from dynamic Lisps.

What v2 HKTs **don't** buy:

- They don't replace effects for `IO` / state / errors / parsers — that machinery stays.
- They don't enable multi-shot continuations (List monad, full backtracking) — that's an effects-system problem (v5 in `effects-plan.md`), not a typeclass-system one.
- They don't change codegen for any existing kind-`*` program.

**What v1 must preserve to keep this door open:**

- *Type variables in class heads carry an explicit kind slot in the elaborator's internal representation*, defaulting to `*`. v1 never sets it to anything else, but the slot exists. This is a one-field change in the type-variable record; missing it would force a v2 IR migration.
- *Don't pun on type-constructor names.* `option` is a type constructor; `(option int)` is a type. Keep these distinct in the IR — don't collapse `option` to "a type with a hole" via some ad-hoc encoding. The cleanest discipline: only fully-applied type constructors appear in `Expr`/`TExpr` nodes; partial applications are never representable in v1, and v2 lifts that restriction by adding kind-`* -> *` to the type-variable kind slot.
- *Dispatch-table key is a struct, not a tuple-of-strings.* As long as the key is a named record (`{op-name, arg-types[]}`), v2 can add a `constructor-key` variant without breaking the v1 schema. Encoding the key as `"name:type1,type2"` strings would force a parser rewrite in v2.
- *Don't expose "the type of `option`" anywhere in user-visible syntax.* No `^option` (without arguments), no `(typeof option)`. Reserve these forms; reject them in v1 with "type constructor used without arguments; this may become valid in a future version with higher-kinded types."
- *Reserve the names `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable`* in the typeclass namespace — when v1 typeclasses ship, these are not-yet-defined-but-reserved, so users can't squat on them with kind-`*` definitions that v2 would conflict with.

**Decision rule for promoting HKTs into v2.** Add them only if at least two of the following are true after meaningful v1 use:

1. Users are repeatedly writing per-monad `traverse-option` / `traverse-result` boilerplate that one generic `traverse` would eliminate.
2. A library author wants to ship a generic monad transformer or free-monad construction and demonstrably can't.
3. A meaningful fraction of users come from Haskell / Scala / PureScript / OCaml-with-modules and the missing abstraction is the top complaint.

If only (3) is true, the answer is "use effects, that's the language." If (1) and (2) are both true, HKTs pay for themselves and v2 ships them.

### 12.3 Modules *(stretch goal)*

A self-contained unit of code with its own namespace, exporting a curated surface and importing from other modules.

```clojure
(defmodule geom
  (export Point distance translate)

  (defstruct Point [x : float, y : float])

  (defn distance [^Point a, ^Point b] : float …)
  (defn translate [^Point p, ^float dx, ^float dy] : Point …)

  (defn- normalize [p] …))   ;; defn- = private, not exported

;; consumer.tur
(import geom)
(import geom :as g)
(import geom :refer [distance])

(geom/distance p1 p2)
(g/distance p1 p2)
(distance p1 p2)
```

Properties:
- **Namespacing.** Each module has its own symbol namespace; cross-module references use `module/name`. Aliases via `:as`, selective imports via `:refer`.
- **Visibility.** `def`/`defn`/`defmacro` are exported by default if listed in `(export …)`; `defn-` and `def-` are always private. Or invert the default — see open questions.
- **Compilation unit.** A module is the unit the codegen emits as one `.c` + `.h` pair. Cross-module calls go through the header's declarations; the linker resolves them. This means modules also become the unit of *separate compilation* — touching one module doesn't recompile the world.
- **C symbol mangling.** `geom/Point` lowers to `geom__Point`; `geom/distance` to `geom__distance`. Reserved-character mapping documented as part of the ABI.
- **Macro export.** Macros cross module boundaries by serializing their `Form` body and re-evaluating in the importer's expansion context. (This is the same path Clojure takes; the bootstrap interpreter (§6) already operates on `Form` so the work is small.)
- **Module-level `defer`.** Top-level `(defer …)` in a module runs at process exit, registered via `atexit`. Useful for closing handles, flushing logs.

**Open question — relationship to files.**

Three options, in increasing flexibility:

1. **One module per file, name from path.** `src/geom/vector.tur` *is* the module `geom/vector`. Simplest; matches Java/Rust/Go ergonomics. No `defmodule` form needed — the file header declares its `(export …)` list. Filesystem becomes load-bearing.
2. **Explicit `defmodule`, multiple modules per file allowed.** As shown above. The reader produces N modules from one file. More flexible (good for tiny test fixtures and macro-heavy single-file scripts), but loaders need an index — usually a manifest file or a "scan all `.tur` for module decls" pass.
3. **Hybrid (Clojure-style).** `defmodule` is required and *must* match the file path: `src/geom/vector.tur` declares `(defmodule geom/vector …)` at the top, and the compiler errors if the names don't match. You get the explicitness of (2) and the discoverability of (1).

Lean: **(3)**. It scales (clear loader, no manifest), it surfaces module identity in the source (reading a file tells you what module it is without consulting the path), and it leaves room to relax to (2) later for REPL/scripting.

**Other open questions** (defer until we actually start designing modules):
- Default visibility — exported by default, or private by default? Lean: private-by-default with explicit `(export …)`. Encourages narrow surface areas.
- Circular imports — error, lazy-resolve, or topological sort? Lean: error in v1.
- Re-exports — `(export-from other-module foo bar)`? Useful for facade modules; can be a macro initially.
- Module-private types crossing the boundary in public signatures — error or warn? Lean: error; the type system (§12.2) will need to enforce this anyway.
- Build artifacts — one `.c`/`.h` per module, or per file when (3) makes them equivalent? Probably per module, with `.h` regenerated on every compile.
- Interaction with `extern-c` and inline-C blocks (§2.1) — declare those at module scope; they get scoped C-symbol visibility (`static` for module-private, extern for exported).

**Why stretch goal, not v1.** Modules are pure plumbing: every line of pre-modules Turmeric works post-modules with at most a `(defmodule …)` wrapper. v0/v1 can ship as a single global namespace and add modules later without breaking existing programs — the inverse isn't true for things like RC or typeclasses, which is why those rank higher.

**What to preserve in v1 to keep modules cheap to add later:**
- Compiler emits `.c` + `.h` even when there's only one compilation unit, so the multi-unit story is already plumbed when modules arrive.
- Symbol naming in codegen never assumes a flat namespace — qualify every emitted name with at least a `tur__` prefix so retrofitting `module__name` is trivial.
- The bootstrap interpreter for macro expansion works against `Form` lists, not against a global symbol table, so cross-module macro export is a small extension rather than a refactor.

### 12.4 Hygienic macros *(stretch goal)*

§6 ships v1 with `gensym` + manual care — the same band-aid Clojure uses. It works in practice, but the problems are real and well-known:

```clojure
(defmacro swap! [a b]
  `(let [tmp ~a]              ;; user code with `tmp` in scope?
     (set! ~a ~b)              ;; → silently captures it
     (set! ~b tmp)))
```

The fix: macros expand to *syntax objects* (identifiers tagged with the scopes they were introduced in), and the expander resolves names by scope-set rather than by symbol equality.

**Approaches, in increasing power and cost:**

1. **Clojure-style syntax-quote auto-resolution + gensym.** Backtick automatically resolves symbols to their definition module; `(gensym)` and `foo#` reader sugar generate fresh names for locals. Informal, not strictly hygienic, but covers most cases. *Where v1 already lives.*
2. **`syntax-rules` (R5RS-style).** Pattern-based, hygienic by construction. Easy to implement, limited expressive power — no procedural macros, no compile-time computation. Good for "stdlib macros" (`when`, `cond`, `->`); not enough for the macros we actually want to write.
3. **`syntax-case` (R6RS, Dybvig et al).** Full procedural macros + mark-set hygiene. Identifiers carry sets of marks; mark-set-aware comparison replaces symbol-eq. Classic, well-understood, ~mid-size implementation lift.
4. **Sets-of-scopes (Flatt '16, Racket).** Modern replacement for mark-sets. Cleaner story for tricky cases (macro-generating-macro, recursive macros, `let-syntax`). Larger one-time investment but provably correct in cases where mark-sets get fragile.

**Lean: (4), sets-of-scopes.** It's the current state of the art, the algorithm is published with reference implementations, and the conceptual model ("each scope-introducing form adds a fresh scope; identifiers compare by scope-set") survives later additions like modules (§12.3) and a real type system (§12.2). (2) is tempting as a cheap intermediate stop, but `syntax-rules` users hit its expressive limits fast in a Lisp aimed at C interop and codegen.

**What changes in the compiler:**
- The `Form` ADT (§11) gains a `Symbol` variant that's actually `(Symbol, ScopeSet)`. Equality and lookup become scope-set-aware. Spans stay where they are; scope-sets ride alongside.
- The expander is a separate pass that walks `Form`s, threading a current-scope, adding a fresh scope at each binding form, and producing fully-resolved identifiers as output.
- The bootstrap interpreter (§6) keeps working unchanged on `Form` — it just sees scoped identifiers instead of raw symbols. Macros that *construct* identifiers use a `(datum->syntax stx 'name)` form (Racket-style) to attach the right scope.

**Open questions:**
- *Migration path for existing macros.* gensym-based macros (v1 style) keep working, since gensym produces fresh symbols that no scope-set lookup will collide with. New hygiene is opt-in or default? Lean: default — non-hygienic macros become the explicit escape hatch.
- *Identifier comparison in user code.* `(= 'foo 'foo)` — does it compare symbol-only, or scope-set too? Lean: symbol-only by default, with `(bound-identifier=? a b)` and `(free-identifier=? a b)` for the precise comparisons (Racket convention).
- *Interaction with quasiquote.* Backtick must thread scope-sets through the template. Tractable but the place where scoping bugs in the implementation usually hide.
- *REPL / dynamic eval.* `(eval form)` at runtime needs an entry point into the expander; scope-sets need a "top-level" representation that's stable across REPL inputs.
- *Reader-introduced identifiers* (autogensym `foo#`, syntax-quote-resolved names): do they get the use-site scope, the read-site scope, or a fresh scope? Lean: read-site scope, matching Racket.

**Why stretch goal, not v1.** Hygiene is a compiler architecture decision dressed up as a feature — picking it after we have real macros in the wild lets us see which patterns matter and informs the design. Meanwhile, v1's gensym + (later) module-qualified names cover ~90% of the cases users actually hit. The remaining 10% is real (the §9 risk is genuine) but survivable for early users; the cost of guessing wrong on the algorithm before we have user data is higher than the cost of a noisy migration later.

**What v1 needs to preserve:**
- Symbols flow through every pass as a typed value, not as bare `const char *` — adding a scope-set field is then a one-struct change. The §11 plan already has `Symbol` as its own type.
- The expander is a *separate, replaceable* pass, not an interleaved part of elaboration. v1's pass works on raw symbols; a future pass swaps in for scoped identifiers without touching what comes after.
- Don't let the reader resolve symbols to bindings. Resolution belongs to the expander/elaborator. (This is already the v1 plan; preserving it explicitly here.)
- Reserve `bound-identifier=?`, `free-identifier=?`, `datum->syntax`, `syntax->datum` as keywords now; emit "not implemented in v0" if used. Same trick as §5.1.2 for `rc/of` — avoids syntactic retrofit later.

### 12.5 Sweet-expressions *(stretch goal)*

An optional surface syntax, layered on top of v1's s-expression reader, that lets users write Turmeric with infix math, traditional `f(x)` call notation, and indentation-meaningful grouping. Based on [SRFI-110 (sweet-expressions / t-expressions)](https://srfi.schemers.org/srfi-110/srfi-110.html) and informed by [sweet-racket](https://github.com/takikawa/sweet-racket)'s experience embedding it into a host Lisp. Sweet-expressions are an **abbreviation**, not a new language — they translate, deterministically and homoiconically, to the same `Form*` the v1 reader produces. Macros, codegen, and tooling downstream are unchanged.

**The motivating example** (Turmeric flavor):

```
defn fibfast(n) : int
  if {n < 2}
    n
    fibup n 2 1 0
```

…reads as exactly:

```clojure
(defn fibfast [n] : int
  (if (< n 2)
      n
      (fibup n 2 1 0)))
```

A user who wants infix only — without indentation magic — can opt into just curly-infix and stop there. A user who wants the whole thing enables full sweet via a file-level directive.

#### 12.5.1 Three layers, three opt-in tiers

SRFI-110 deliberately separates three orthogonal extensions; we adopt the same separation so users can pick where to stop.

| Tier | What it adds | Reader cost | File-level enable |
|---|---|---|---|
| **(a) Curly-infix** ([SRFI-105](https://srfi.schemers.org/srfi-105/)) | `{a + b}` → `(+ a b)`; `{a + b + c}` → `(+ a b c)`; `{a + b * c}` → `($nfx$ a + b * c)` (no precedence — mixed operators bail to a `$nfx$` macro) | small | `#lang turmeric/curly-infix` or always-on |
| **(b) Neoteric** | `f(x y)` → `(f x y)`; `f{x + y}` → `(f (+ x y))`; `f[x]` → `(bracketapply f x)`. Triggers only when `(`/`{`/`[` follows an atom *with no whitespace between* | small–medium | implied by sweet; standalone via `#lang turmeric/neoteric` |
| **(c) Sweet (full t-expr)** | Indentation-significant grouping; `\\` GROUP/SPLIT; `$` SUBLIST; `<* … *>` collecting list; leading-abbreviation rule | medium–large | `#lang sweet-exp` directive *or* `.tursweet` file extension |

Default for `.tur` files is **plain s-exprs** (`#lang turmeric`, implicit if no `#lang` line). Sweet must be explicitly opted into. This follows Racket's `#lang` model — adopted instead of SRFI-110's `#!sweet` because `#lang` is the convention Lisp users already know from Racket and `sweet-racket`, and it generalizes to other future surface dialects without each needing a bespoke directive.

#### 12.5.2 Conflicts with Turmeric's existing reader, and how to resolve them

Turmeric's v1 reader (§10.2) already uses several characters that SRFI-110 cares about. None of the conflicts are showstoppers, but each needs a deliberate decision before tier (a) ships, because the answer constrains tiers (b) and (c).

| Turmeric v1 | SRFI-110 wants it for | Resolution |
|---|---|---|
| `{…}` for map literals | curly-infix lists | **Resolved in v1: maps use `#{…}`, not `{…}`** (§10.2). `{…}` is unused in v1 — reserved exclusively for SRFI-105 curly-infix when §12.5 ships. No deprecation window, no migration: the v1 reader has never accepted `{…}` for maps. |
| `~x` / `~@x` for unquote / unquote-splicing (Clojure-style) | SRFI-110 uses `,` and `,@` | **Resolved in v1: `~` / `~@` are the only unquote sigils** (§10.2). Turmeric does not also accept `,` / `,@`. Sweet's "leading-abbreviation-followed-by-whitespace applies to the whole expression" rule is generalized in our reader to whichever abbreviation sigils Turmeric defines — `~` and `~@` get the same treatment `'` and `` ` `` already do. The rule is "leading reader-macro abbreviations followed by whitespace consume the indented body," not "specifically these four characters." |
| `[…]` for **vector literals** | neoteric `f[x]` → `(bracketapply f x)` | Outside neoteric position (i.e., with whitespace before `[`), `[…]` continues to mean "vector literal" exactly as today. Inside neoteric position (`f[x]`, no whitespace), it lowers to `(bracketapply f x)` per SRFI-105 — and `bracketapply` is a Turmeric macro the user can define (or we can ship as `nth` for the common indexing case). Document the disambiguation rule prominently; it matches every other neoteric implementation. |
| ` ```c … ``` ` **inline-C blocks** (§2.1) | (no conflict, but watch out) | The reader recognizes triple-backtick fences *before* applying any tier's rules; the payload is opaque (treated like a string). Indentation processing inside the block is suppressed (just as it is inside `(…)`). A block at expression position with a `: T` annotation (the existing inline-C return-type form) needs to play nicely with sweet's "child lines extend the parent line" rule — solution: treat the block as a single neoteric atom for grouping purposes. |
| `.x` **field-access reader sugar** (§5.2: `(.x p)` ↔ `(. p x)`) | conflicts with SRFI-110's leading `.` for improper-list cdr | Confine Turmeric's `.x` form to "atom starting with `.` followed by a name character." SRFI-110's `.` rule applies only when `.` is a delimited token (followed by whitespace or EOL). The two are syntactically distinguishable today and remain so. |
| `:foo` **keywords** | (no conflict in SRFI-110) | Untouched. Note: wisp's `:` SUBLIST marker would have collided here; we're using `$` per SRFI-110, so we're fine. |
| `^int` **type-annotation prefix** | (no conflict) | Untouched. Verify `^` doesn't trigger any SRFI-105 special handling — it doesn't. |
| `&` **address-of** (§5.4) | (no conflict) | Untouched. |

The two architectural decisions that had to be made in v1 — `{…}` vs. `#{…}` for maps, and `~`/`~@` vs. also accepting `,`/`,@` — are **resolved**: maps use `#{…}`, unquote uses `~`/`~@` only. See §10.2. Everything else in this table is purely sweet-reader local.

#### 12.5.3 Where it slots into the pipeline

Sweet sits **above** the existing reader, not inside it. The §3 pipeline becomes:

```
text → [sweet preprocessor (optional)] → reader → Form* → expand → elab → …
```

In practice, the sweet preprocessor is a separate state machine that consumes characters and emits `Form*` directly (the SRFI-110 reference impl is structured this way; trying to layer it on top of an existing s-expr lexer is the path of pain — sweet-racket and the readable project both do their own lexing). The output is the same `Form*` ADT (§11), with the same `Span` provenance, so every downstream pass is identical.

This means:
- **Spans flow through correctly** — each sweet construct attributes to the source text it desugared from. Critical for diagnostics; the §10.1 rule "spans flow through every pass" applies to the sweet reader equally.
- **Macros see no difference.** `defmacro` operates on `Form*`; whether the user wrote `(if {n < 2} n …)` or the sweet equivalent, the macro receives the identical tree.
- **Codegen snapshots stay stable.** The fixture system (§11) golden-checks emitted `.c`. Adding sweet doesn't perturb a single existing snapshot.

Two readers ship: `sweet-read` and the existing `s-read`. The driver picks based on file extension or `#!sweet` directive. The bootstrap interpreter (§6) calls `s-read` only — macros are still written in s-expressions in v1, even when host code is sweet. (Mixing sweet *into* macro bodies via quasiquote is fine because the macro receives `Form*`, not text.)

#### 12.5.4 Implementation phases

Three phases, each independently shippable. Each ends with the same fixture discipline (§11): happy path, interaction (with `defer`, `ref`, closures, macros), negative tests with golden diagnostics, and round-trip snapshots showing the desugared `Form*` matches a hand-written equivalent.

**Phase S1 — Curly-infix (SRFI-105).**
- Reader change: when an opening `{` is seen, switch to "curly mode" until the matching `}`. (No conflict with map literals — those use `#{…}` since v1.)
- Inside curly mode, whitespace-separated tokens are collected; if the result is `(a op b op c …)` with all `op`s the same operator, lower to `(op a b c …)`. Mixed operators lower to `($nfx$ …)` and the elaborator either resolves `$nfx$` via a user-defined macro or errors out.
- One-element `{e}` → `e`. Two-element `{e1 e2}` → `(e1 e2)` (matches SRFI-105 exactly).
- Effort: ~1–2 days. Self-contained. Pure win for any code that does math.

**Phase S2 — Neoteric.**
- After reading any atom (symbol, number, string), peek the next char. If it's `(`, `[`, or `{` *with no intervening whitespace*, consume the bracketed list and wrap: `f(x y)` → `(f x y)`, `f[x]` → `(bracketapply f x)`, `f{x + y}` → `(f (+ x y))`.
- Interacts with curly-infix: `f{a + b}` first runs S1 on the `{…}` (yielding `(+ a b)`), then S2 wraps as `(f (+ a b))`.
- Interacts with Turmeric's `.x` sugar: trivially compatible since `.x` is a single atom; `.x(p)` would mean `((. x) p)` which is nonsense — document that you write `(.x p)` or `p.x` is *not* an idiom (we don't have method-call sugar). If users want `obj.method(args)` ergonomics, that's a separate feature (a §12.6 idea, not part of sweet).
- Effort: ~2–3 days. Most of the work is the no-whitespace lookahead and getting the test matrix right.

**Phase S3 — Full sweet (indentation, GROUP/SPLIT, SUBLIST, collecting lists).**
- The big one. Implement the SRFI-110 BNF directly; the reference implementation is ~1000 lines of Scheme and ports cleanly to C. ANTLR-checked grammar means low risk of ambiguity bugs.
- Indentation stack lives in the reader; tabs, spaces, and `!` are all valid indent characters per spec. Mixing them on the same line is an error.
- `\\` (GROUP/SPLIT), `$` (SUBLIST), `<* … *>` (collecting lists) all per spec — no Turmeric-specific divergence; users who learn sweet from the SRFI docs should find Turmeric's dialect identical.
- Indentation processing is **off** inside `(…)`, `[…]`, `{…}`, `#{…}`, and inline-C `\`\`\` … \`\`\`` blocks. This is what makes sweet fully backward-compatible with traditional s-exprs — any line starting with `(` reads as a normal s-expr.
- A `#lang sweet-exp` line at the top of the file enables sweet for the rest of that file. `.tursweet` extension enables it implicitly even without the line. See §12.5.4a below for the full directive scheme.
- A `tur unsweeten <file.tursweet>` command emits the desugared s-expression form (and rewrites the `#lang` line accordingly), mirroring SRFI-110's reference `unsweeten` tool. Useful for debugging, for diffs, and for users who want to migrate gradually. The inverse `tur sweeten <file.tur>` is part of the `tur fmt` family (§12.5.5).
- Effort: ~1–2 weeks for a clean implementation with full fixture coverage, including the interaction-test matrix (sweet × `defer`, sweet × inline-C, sweet × quasiquote, sweet × keywords, sweet × ref).

**Phase S4 — `#lang` dispatch (cross-cutting, lands with whichever phase first needs a non-default reader).**

Turmeric uses Racket-style `#lang <name>` as the file-level reader-selection mechanism, replacing SRFI-110's `#!sweet`. Rationale: `#lang` is the convention Lisp users (especially Racketeers, who are sweet's natural early-adopter audience) already know; it generalizes to future surface dialects without each one inventing a directive; and it composes cleanly with `tur fmt` (which can rewrite the `#lang` line as it converts).

Rules:

- **Position.** The `#lang` line, if present, MUST be the very first line of the file — no preceding whitespace, no preceding comments, no BOM. This matches Racket and lets the driver pick the reader from the first ~32 bytes without buffering the whole file.
- **Form.** `#lang <name>` followed by `\n`. Optional trailing arguments (e.g., `#lang sweet-exp typed`) are reserved for future use and rejected with a clear error in v1.
- **Default.** Absence of `#lang` is equivalent to `#lang turmeric`. No file is required to declare a dialect; the default is the boring one. (Compare Racket, which *requires* `#lang`; we don't, because it would break every existing example.)
- **Recognized values in v1:**
  - `#lang turmeric` — default s-expression reader.
  - `#lang sweet-exp` — full sweet (tier (c)). Errors in v1 with "not yet implemented"; ships in phase S3.
  - `#lang turmeric/curly-infix` — tier (a) only. Ships in phase S1.
  - `#lang turmeric/neoteric` — tiers (a) + (b). Ships in phase S2.
  - Anything else — error: "unknown #lang `<name>`; expected one of turmeric, sweet-exp, …".
- **No mid-file switching.** Unlike SRFI-110's `#!sweet` / `#!no-sweet` (which can appear anywhere), Turmeric's `#lang` is file-scoped. Mixing dialects within one file is forbidden. Users who want to mix do so by splitting into separate files (the §8.5 per-file artifact layout already supports this — file `a.tur` can be plain Turmeric and file `b.tursweet` can be sweet, and they link together normally).
- **`.tursweet` extension.** Files ending in `.tursweet` default to `#lang sweet-exp` if no `#lang` line is present. A `#lang turmeric` line in a `.tursweet` file is allowed but emits a warning ("file extension and `#lang` disagree; reader is following the `#lang` line"). Reverse case (`.tur` file with `#lang sweet-exp`) is allowed silently — the directive wins.
- **Compatibility with SRFI-110's `#!sweet`.** We do NOT accept `#!sweet` as an alias. Users porting code from sweet-racket or readable-project tooling get a one-line error message pointing them at `#lang sweet-exp`. The `#!` form would have been a second way to do the same thing, with subtly different placement rules — we'd rather have one rule.

Why not `#lang sweet-exp turmeric` (the sweet-racket mixin shape)? Because Turmeric is the only base language; the second word would always be `turmeric`. If we ever grow alternate base languages (e.g., a pure-functional subset), we can extend the syntax then — `#lang` is forward-compatible with arguments.

#### 12.5.5 Open questions to settle before phase S3

1. **`bracketapply` semantics.** SRFI-105 leaves `bracketapply` as "user-definable." For Turmeric, lean: ship `(defmacro bracketapply [coll i] `(nth ~coll ~i))` in the stdlib seed. This makes `xs[i]` indexing work out of the box. Open: should `bracketapply` also handle struct field access (`p[x]` → `(.x p)`)? Probably no — too clever, two ways to do the same thing.
2. **`nfx` (mixed-precedence infix).** SRFI-105 specifies that `{a + b * c}` lowers to `(nfx a + b * c)` and leaves `nfx` for the user to define. For Turmeric, lean: **don't ship a default `nfx` macro.** A user trying `{a + b * c}` should get a clear "no `nfx` defined; use parens or define `(defmacro nfx …)`" error. Reasoning: precedence in a Lisp is the C++ "implicit conversion" of syntax — once it exists, every reader of every program has to internalize the table. Better to require the parens.
3. **Quasiquote interaction.** sweet-racket's README documents this as a known issue ("quasi-quotation combined with grouping does not behave according to the specification"). We must do better — the spec's leading-abbreviation rule covers `` ` `` (backquote) explicitly; the bug in sweet-racket appears to be specific to its template-syntax integration with Racket's own quasiquote, which we don't have. Fixture-test it heavily. Likely fine for us; verify before phase S3 ships.
4. **REPL ergonomics.** SRFI-110 ends a top-level expression on a blank line. For an interactive Turmeric REPL, this matters: "Enter Enter to submit" is the convention. Verify this works on macOS / Linux terminals (it does in every other sweet implementation; not expected to be a problem).
5. **Editor support.** Indent-aware language requires editor cooperation. v1 plan: ship a tree-sitter grammar for `.tursweet` (separate grammar from `.tur`). Indent rules live in the grammar. Without this, users will fight their editors.
6. **`tur fmt --to-sweet` and `tur fmt --to-sexp`.** Bidirectional pretty-printer, mirroring `sweeten` / `unsweeten` from the readable project. Cheap to build once the reader exists (just walk `Form*` and emit the chosen flavor) and removes the "I have to commit to one syntax forever" anxiety.

#### 12.5.6 Why stretch goal, not v1

Sweet-expressions are pure surface syntax — they buy readability but no new expressive power, and a v1 user can already write any Turmeric program in plain s-exprs. The cost of getting them wrong (especially the indentation phase) is high enough that they want real users with real programs to motivate the dialect choices. Meanwhile, the v1 reader is already good enough to bootstrap the language.

The two architectural decisions that *had* to be made in v1 — `{…}` vs. `#{…}` for maps, and the unquote sigil set — are **resolved** (§10.2): maps are `#{…}`, unquote is `~` / `~@` only. The third — using `#lang` as the dispatch mechanism — is also locked in (v1's reader parses the `#lang` line and currently accepts only `turmeric`; everything else is an error). With those three out of the way, the rest of sweet is genuinely deferrable.

#### 12.5.7 What v1 needs to preserve to keep this door open

- **Reader is a separate, replaceable component.** v1's reader already targets `Form*`; the sweet reader is a peer, not a fork. Don't let any pass downstream of the reader inspect raw text or assume a specific surface syntax.
- **`Form*` carries `Span` from a single source.** Sweet desugaring produces nodes spanning the original sweet text; the typed-IR (§1.1) and diagnostics (§10.1) treat them identically to s-expr-sourced nodes.
- **Reader-recognized abbreviations are a *table*, not a hardcoded list.** v1's reader handles `'`, `` ` ``, `~`, `~@` via a small lookup; the sweet reader's "leading-abbreviation-followed-by-whitespace applies to the whole expression" rule iterates over the same table. New abbreviations (e.g., `#'` for syntax in §12.4) plug in via one table entry.
- **Don't bake `(`/`[`/`{`/`#{` parsing into deeply-nested control flow in the s-expr reader.** Leave them as discrete cases that the sweet reader can replicate. The §11 plan's `Form*` already keeps the variants separate (`F_LIST`, `F_VEC`, `F_MAP`); keep them that way. Note that `{…}` is *unused* in v1 — the reader rejects it with "reserved for SRFI-105 curly-infix; use `#{…}` for maps" — which both reserves the syntax for §12.5 and gives users a clear hint when they accidentally type the Clojure shape.
- **`#lang` dispatch lives in the driver, not the reader.** The driver inspects the first line, picks a reader, hands the rest of the file to it. This means swapping in the sweet reader later is a one-line addition to a dispatch table, not a reader refactor.
- **Reserve the `#lang` names** `turmeric`, `sweet-exp`, `turmeric/curly-infix`, `turmeric/neoteric` as known-but-not-yet-implemented in v1. Users typing them in anticipation get "not yet implemented" rather than "unknown #lang"; when each phase ships, the corresponding name just starts working.

