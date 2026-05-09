# Turmeric Language — Design & Implementation Plan

A Lisp (Clojure/Fennel-flavored) that compiles to C, with homoiconic macros, struct-backed closures, scope-based `defer`, and `ref` values that auto-deallocate at scope end.

---

## Progress Summary

| Phase | Status | Exit Criterion | Notes |
|---|---|---|---|
| 0 | ✅ **Complete** | hello.tur round-trip | All infrastructure in place: arena, reader, forms, diag, buf, emit, main |
| 1 | ✅ **Complete** | Fizzbuzz | All core forms, arithmetic, comparison, logical ops; 12/12 fixtures green under ASan/UBSan |
| 2 | 🚧 **In Progress** | Top-level functions + extern-c | Type system extended with TY_FN, TY_PTR_VOID; Expr kinds extended with EX_FN, EX_CALL, EX_FN_DEF, EX_EXTERN_C, EX_INLINE_C. Core implementation pending. |
| 3 | ⏳ Pending | Closures | Capture analysis, env struct synthesis, escape analysis |
| 4 | ⏳ Pending | defer + scope unwind | Label-based goto chains, LIFO ordering |
| 5 | ⏳ Pending | ref<T> | Move semantics, auto-defer drop |
| 6 | ⏳ Pending | defmacro + quasiquote | Bootstrap interpreter, gensym-based hygiene |
| 7 | ⏳ Pending | Stdlib seed | vec, slice, str, option, result; test runner |
| 8 | ⏳ Pending | Diagnostics polish | Span propagation audit, miette-style errors |
| 9 | ⏳ Pending | rc<T> + weak<T> | Reference counting v1 GC |
| 10 | ⏳ Pending | Bacon-Rajan cycle collector | v2 GC layered over RC |

**Last updated:** 2024-05-09 (Phase 2 infrastructure in place, core implementation in progress)

---

**Phase 2 Implementation Checklist:**
- [x] Extend TypeKind with TY_FN and TY_PTR_VOID
- [x] Extend ExprKind with EX_FN, EX_CALL, EX_FN_DEF, EX_EXTERN_C, EX_INLINE_C
- [x] Update type_eq, type_name, type_c_name for function types
- [x] Update emit.c switch statements for new expr kinds
- [x] Update expr.c print function for new expr kinds
- [ ] Add sym_defn, sym_fn, sym_extern_c, sym_inline_c to elaborator
- [ ] Parse type annotations (^int, : T) in reader/elaborator
- [ ] Implement elab_defn for (defn name [params...] : ret-T body...)
- [ ] Implement elab_fn for (fn [params...] body...) - no capture, lift to static
- [ ] Implement elab_extern_c for (extern-c name [params...] : ret-T)
- [ ] Implement elab_inline_c for ```c ... ``` blocks
- [ ] Implement elab_call for (f a b c) function calls
- [ ] Emit function declarations and definitions in emit.c
- [ ] Emit function calls in emit.c
- [ ] Emit extern-c declarations in emit.c
- [ ] Emit inline-C blocks in emit.c
- [ ] Update driver for multi-file support (_main.c generation)
- [ ] Add fixtures: defn-basic, mutual-recursion, extern-printf, inline-c-popcount
- [ ] Add negative tests: capturing fn gate, arity mismatches, bad inline-C annotations

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
| 7 | Stdlib seed: `vec`, `str`, `option`, `result`; `cond`, `case` as macros | Self-checked test runner written in Turmeric |
| 8 | Error reporting: source spans through every phase, miette-style diagnostics | Bad-input fixtures show pointer-and-caret errors |
| 9 *(v1)* | `rc<T>` + `weak<T>`: control-block layout, retain/release, `(upgrade w)`, defer-injected `rc-release`, last-use elision | RC fixtures pass; cycle-leak fixture documents the known cycle limitation |
| 10 *(v2)* | Bacon-Rajan cycle collector layered over RC: color field in control block, suspect-roots buffer, trial-deletion pass | Cycle fixture from phase 9 reclaims memory; collector can be disabled with no effect on non-cyclic programs |

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
- [ ] Parse `(defn name [params...] : ret-T body...)`.
- [ ] Type-check: param types annotated, return type annotated, body has matching type.
- [ ] Emit `static T name(T1 p1, T2 p2) { … }`.
- [ ] Forward declarations emitted in dependency order (or all at top of file — simplest).
- [ ] `defn-` for module-private (C `static`) when phase 9/modules arrives; for now treat all as `static`.

**`fn` — anonymous, no-capture**
- [ ] Parse and type-check.
- [ ] If the body references no outer locals → lift to a top-level static function with a synthetic name (`__fn_<id>`).
- [ ] Reject capture with a "closures arrive in phase 3" diagnostic — the fixture proves the gate.

**Function calls**
- [ ] Resolve `(f a b c)` to direct C call; arity- and type-check.
- [ ] Variadic functions deferred until extern-c needs them.

**`extern-c`**
- [ ] Parse `(extern-c printf [^cstr fmt & args] : int)`.
- [ ] Emit a forward declaration `extern int printf(const char*, ...);` (variadic via `& args` marker).
- [ ] Trust the user-given type; type-checker treats it as opaque past arity.
- [ ] Built-in re-exports: `(extern-c malloc [^usize n] : ptr<void>)`, `free`, `abort`, `puts`, `printf` available without per-program redeclaration.

**Inline-C blocks (§2.1)** — first cut
- [ ] Reader recognizes triple-backtick fences with optional `c` tag; payload is verbatim string.
- [ ] Elaborator records the block's annotated return type (`: T`).
- [ ] Codegen emits a GCC/Clang statement-expression `({ ... })` with a leading auto-binding for each free identifier captured by name.
- [ ] Strict-C99 fallback (`--strict-c99`): hoist the block into a generated helper function. Skip in v0 if it complicates things; document the flag as planned.
- [ ] Reject `#include` inside an inline block with a clear error.

**Driver** — adopt the per-file artifact layout (§8.5)
- [ ] Each `.tur` source file compiles to a paired `<name>.c` + `<name>.h` in the build directory.
- [ ] Generate `_main.c`: `#include`s every emitted header, defines `int main(void)` that calls the user's `main` (located in whichever input file declares it).
- [ ] `tur build a.tur b.tur` → `a.c`, `a.h`, `b.c`, `b.h`, `_main.c`; pass all `.c` files to `cc` and link.
- [ ] `tur run <input.tur>` — build + execute in one shot.
- [ ] Fixture: two-file program, `a.tur` defines `(defn helper [x] : int …)`, `b.tur` calls it from `(defn main [] …)`. Verify `_main.c` is generated correctly and the link succeeds.

**Fixtures**
- [ ] `defn-basic.tur` — recursive factorial.
- [ ] `mutual-recursion.tur` — `even?`/`odd?`.
- [ ] `extern-printf.tur` — direct `(printf "%d\n" x)`.
- [ ] `inline-c-popcount.tur` — `__builtin_popcount` via inline block.
- [ ] Negative: capturing `fn` with phase-3 gate diagnostic; arity mismatches; bad inline-C return-type annotation.

**Exit criterion:** can call `printf`, write a recursive function, drop into inline C — all fixtures green.

---

### 10.4 Phase 3 — closures

Goal: `fn` captures locals; closures are first-class values; the §2 counter example runs.

**Capture analysis** — `src/close.{c,h}`
- [ ] Walk `fn` body, collect free identifiers, look up each in enclosing scopes.
- [ ] For each captured binding: record type, mutability, escape status.
- [ ] Distinguish *upvalue* (captured by value, copy at closure construction) from *upref* (captured by reference, shared mutation visible).
  - Default: immutable bindings → upvalue; mutable (`set!`-targeted) bindings → upref.

**Escape analysis**
- [ ] Determine whether the closure escapes its defining scope (returned, stored, passed to a non-stack-only sink).
- [ ] Conservative default: any closure passed as a value escapes. Optimize stack-allocation only when clearly safe.

**Env struct synthesis**
- [ ] For each `fn`, generate `struct __env_<id> { /* captured fields */ };`.
- [ ] Heap-allocate via `tur_alloc(sizeof(struct __env_<id>))` if escaping; stack-alloc otherwise.
- [ ] Reserve a header word (8 bytes) at the front of every env struct for future GC metadata (§5.1.1, §5.1.2). Do not repurpose.

**Closure type & call lowering**
- [ ] One `closure_<sig>` typedef per distinct flattened signature: `struct closure_int_int { int (*fn)(void* env, int); void* env; }`.
- [ ] `(c args...)` lowers to `c.fn(c.env, args...)`.
- [ ] Top-level functions are also coerced to closures on demand: `closure_int_int wrap = { .fn = my_fn_thunk, .env = NULL };` where `my_fn_thunk` discards `env` and forwards.

**Fixtures**
- [ ] `counter.tur` — the §2 example.
- [ ] `adder-factory.tur` — `(defn make-adder [n] (fn [x] (+ x n)))`.
- [ ] `mutable-capture.tur` — closure mutates an outer `set!`-able binding; second closure sees the mutation.
- [ ] `escape-no-escape.tur` — codegen snapshot proving stack-alloc happens when it can.
- [ ] Codegen snapshots for env layout and call-site lowering.

**Exit criterion:** counter example, adder factory, and mutable-capture fixtures all green; ASan/UBSan clean; the codegen snapshot of env layout matches what's documented in §4.

---

### 10.5 Phase 4 — `defer` + scope unwind

Goal: scope-bounded cleanup with LIFO ordering and correct early-return behavior. No `ref<T>` yet — that's phase 5, layered on top.

> **Architectural commitment: the unified defer model.** Defers are a runtime list-on-frame, not codegen labels. This is *modestly* more work in phase 4 and saves a phase-4-rewrite if we ever ship effect handlers (or any other feature where defers fire from a non-syntactic exit point). See [effects-plan.md §6.10](effects-plan.md) for the full rationale; in short, every plausible future strategy (S1 "run on capture" / S2 "attach to continuation" / S3 "forbid") is then a runtime policy decision rather than an architectural rewrite. The cost of paying for this if effects never ship is sub-percent at runtime.

**Frame data structure**
- [ ] Add a `Frame` struct (or rename `Scope` if it's already in `elab.c`) with: a defer list, a parent pointer, a span. Lives on the C stack in v0/v1.
- [ ] `frame_push_defer(Frame*, defer_t)` registers a thunk. `defer_t` is `{fn_ptr, env_ptr}` — same shape as a closure (§4) so this composes with the closure machinery.
- [ ] `frame_fire_defers_lifo(Frame*)` walks the list back-to-front, invokes each thunk.

**Parsing & elaboration**
- [ ] `(defer expr)` valid only inside a scope-introducing form (`let`, `do`, function body). Error otherwise.
- [ ] Defers elaborate to a `frame_push_defer` IR node; they evaluate to nil.
- [ ] Multiple defers per scope: collected in source order, fire LIFO.

**Scope analysis & codegen**
- [ ] Each scope is assigned a `Frame` allocation (stack-allocated; the `may_capture` bit is always false in v0/v1, so no heap path runs yet).
- [ ] Codegen for scope exit: emit `frame_fire_defers_lifo(&frame); /* free */`. *Not* a per-scope `__cleanup_L<id>:` label — the lowering is a runtime call.
- [ ] Early `return X`: rewrite to "for each enclosing frame, fire defers, then return". This walks frames via the parent pointer; it's a small loop in the emitted code, not a goto chain.
- [ ] Function-level frame exists even if it has no defers, so `ref<T>` (phase 5) can register drops on it without special-casing.

**Future-proofing slots (per [effects-plan.md §6.10](effects-plan.md))**
- [ ] `Frame` has a `parent` pointer field — even though v0/v1 doesn't follow it (early return walks via codegen knowledge of nesting). The field exists so v3's heap-frame mode plugs in without restructuring.
- [ ] Every `FnDef` carries a `may_capture: bool`, defaulting to `false`. Phase 4 doesn't read it; phase v3 will read it to choose stack-vs-heap frame allocation.
- [ ] Every function `Type` has an `effect_row` slot, defaulting to `nullptr` (treated as `{}` empty row). Phase 4 doesn't populate it; type-equality treats `nullptr == nullptr` as compatible. Phase v3 will populate it.
- [ ] *Document the commit message:* "this is the unified defer model from effects-plan.md §6.10 — small overhead now, no phase-4 rewrite if effects ship."

**Edge cases**
- [ ] Defers inside `if` branches: each branch is its own frame; defers fire when the branch ends, not at the enclosing `let`'s end. Document with a fixture.
- [ ] Defer that itself calls a function that errors: in v0, errors are abort-only, so this is fine. Revisit when we have proper error propagation.
- [ ] Defer referencing a binding that was `set!`-ed after the defer: defer runs against the *current* value at exit time, not the value at registration. (Standard Go semantics in this respect.) Fixture this explicitly — the surprising case.
- [ ] *Don't* document defer's semantics as "runs *immediately* at scope exit." Documentation should say "runs on scope exit" without the immediacy claim, leaving room for v3's "...or when a continuation that captured this scope drops" without a doc-breaking change.

**Fixtures**
- [ ] `defer-order.tur` — three defers, prove LIFO via printed output.
- [ ] `defer-early-return.tur` — `return` inside `if`, defer still runs.
- [ ] `defer-nested-scopes.tur` — defers in inner `let` fire before defers in outer `let`.
- [ ] `defer-mutated-binding.tur` — defer captures by reference, sees latest value.
- [ ] Negative: `(defer ...)` at module top-level → error.
- [ ] Codegen snapshots for the runtime-list lowering (not the label-chain).

**Exit criterion:** all defer fixtures green; the codegen-snapshot tests show `frame_fire_defers_lifo` calls (not `__cleanup_L<n>` labels); ASan/UBSan clean.

---

### 10.6 Beyond phase 4 — sketch only

Phases 5–10 retain the §7 summary level for now; they get the same task-list treatment when phase 4 lands and we know the surrounding code's actual shape. In rough order:

- **Phase 5 — `ref<T>`** with move semantics, `(ref expr)` ctor, `@r` deref, auto-defer drop. Hooks the function-level cleanup label from phase 4. Adds move-tracking to the elaborator.
- **Phase 6 — `defmacro` + quasiquote** in the bootstrap interpreter. Reuses `Form` as the data type macros operate on.
- **Phase 7 — stdlib seed**: `vec`, `slice`, `str`, `option`, `result`. `cond`, `case`, `when`, `unless` rewritten as macros now that phase 6 exists.
- **Phase 8 — diagnostics polish**: span propagation audit, miette-style multi-line snippets, `--explain <code>` for long-form errors.
- **Phase 9 (v1) — `rc<T>` + `weak<T>`**: control-block layout, retain/release, `(upgrade w)`, defer-injected `rc-release`, last-use elision (§5.1.2).
- **Phase 10 (v2) — Bacon-Rajan cycle collector**: color field, suspect-roots buffer, trial-deletion (§5.1).

Don't pre-plan in detail past phase 4 — too much will rotate based on what we learn.

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

