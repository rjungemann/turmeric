# Turmeric — Effect System Plan

A design and feasibility analysis for adding an OCaml-5-style algebraic effect system to Turmeric. References: [OCaml 5.4 effects manual](https://ocaml.org/manual/5.4/effects.html), [Jane Street, "Effective Programming"](https://www.janestreet.com/tech-talks/effective-programming/).

This is a **v3 stretch-goal plan**, not a near-term phase. It's written now to:
1. Decide whether to keep the door open in earlier-phase architectural choices.
2. Give us a concrete reference point so "we'll add effects later" doesn't mean "we'll do something nobody actually thought through."

Cross-references: `§12.1 Delimited continuations`, `§12.2 Type system`, `§5 defer/ref`, `§5.1.2 RC + weak refs` in [turmeric-plan.md](turmeric-plan.md).

---

## 1. What OCaml 5 effect handlers actually are

The minimum viable model. OCaml 5 ships *deep* algebraic effect handlers with **one-shot continuations**:

```ocaml
type _ Effect.t += Read_line : string Effect.t

let () =
  try_with
    (fun () ->
       let name = perform Read_line in
       Printf.printf "hello %s\n" name)
    ()
    { effc = (fun (type a) (e : a Effect.t) ->
        match e with
        | Read_line -> Some (fun (k : (a, _) continuation) ->
            continue k "Roger")
        | _ -> None) }
```

Three primitives:
- **`type _ Effect.t += E : T Effect.t`** — declares a new effect `E` whose `perform` returns `T`.
- **`perform E`** — raises the effect. Search the dynamic handler stack for a matching handler; jumps to it with the current continuation reified as `k`.
- **`try_with body () { effc }`** — installs a handler around `body`; `effc` is called when an unhandled effect surfaces. `continue k v` resumes the suspended computation with `v`; `discontinue k exn` resumes by raising.

Properties of the OCaml-5 baseline:
- **Continuations are one-shot.** Calling `continue k v` twice raises `Continuation_already_resumed`. This is a deliberate restriction — multi-shot would break the GC/finalizer model and the C stack convention.
- **Effects are *not* in the type system.** `perform` looks like a normal call returning `T`; whether it's handled is checked dynamically. Unhandled effects raise `Unhandled` at runtime.
- **Implementation: stack-segment copying.** Multicore OCaml uses fibers — each handler creates a stack segment; capturing the continuation copies the segment. Cost is roughly that of a setjmp + segment-copy on `perform`.

This is enough for: direct-style async, generators, exceptions-with-state, transactional retry, dependency injection, mocking I/O, scheduled fairness in concurrency.

## 2. What Jane Street's "typed effects" adds

The talk pitches putting effects **in the type system** via *effect rows*:

```ocaml
val read_file : path -> string @ (read_file)
val map      : ('a -> 'b @ 'e) -> 'a list -> 'b list @ 'e
val pure_function : int -> int @ ()
```

Properties:
- Functions carry an effect set (a row) in their type. `f : int -> int @ {Read, Write}` means calling `f` may perform `Read` or `Write`.
- `pure` functions have empty effect rows; the compiler can refactor freely.
- Effect-polymorphism lets `map` propagate effects from the function it's given.
- Total inference is hard; expect to write effect annotations on top-level signatures.

This is a v2 (or v3) on top of v1 untyped effect handlers. Jane Street's variant isn't in OCaml mainline as of OCaml 5.4; they ship it as an extension internally.

## 3. Plausibility for Turmeric

**Short answer: feasible, but only if §12.1 (delimited continuations via CPS) ships first.** Without delim-conts, this is a from-scratch runtime overhaul; with them, it's a typed API on top of an existing substrate plus a type-system extension.

**What aligns:**

| Turmeric feature | Why it helps effects |
|---|---|
| §12.1 CPS-based delim-conts | A handler is `reset`; `perform` is `shift`. The substrate is the same. |
| §4 closures as `struct {fn,env}` | Continuations under CPS are *the same shape* — we already typed them. |
| §1.1 elaborator-resolved operators + typed IR | Effect-polymorphic operators (typeclass-style + effect rows) plug in cleanly. |
| §12.2 typeclasses + dictionary passing | Effect handlers can be modeled as dynamically-bound dictionaries; the dispatch story is already designed. |
| §5.1.2 `rc<T>` + `weak<T>` | Continuations capture environments; RC handles their lifetime without a tracing GC. |

**What fights:**

| Turmeric feature | Why it complicates effects |
|---|---|
| §5 lexical-scope `defer` / auto-drop `ref<T>` | Capturing a continuation across a defer boundary is the §12.1 hard case. Effects make capture *implicit at every `perform`*, so the problem multiplies. |
| `ref<T>` move semantics | Multi-shot continuations re-execute `ref` constructions — but `ref<T>` was already moved in the first run. Multi-shot is incompatible with our ownership model. |
| C99 target, no setjmp tricks | OCaml uses fibers and stack-segment copying. We can't do that portably; CPS is the only option, which means we have to commit to it for the whole program. |

**Verdict:** plausible **if and only if** §12.1's CPS strategy is adopted. If §12.1 stays research-only, effects become a separate research project with a totally different runtime model. If §12.1 ships, effects are a natural follow-on.

## 4. Design sketch — what this looks like in Turmeric

Follow OCaml 5's surface as closely as makes sense for a Lisp.

### 4.1 Declaring an effect

```clojure
(defeffect Read-Line []           : str)
(defeffect Get [^cstr key]        : (option str))
(defeffect Yield [^int x]         : nil)        ;; for generators
(defeffect Random-Int [^int max]  : int)
```

`defeffect` is roughly a typeclass declaration: a name, a parameter list, a result type. Behind the scenes it registers an entry in the operator dispatch table (§1.1) — *the same table that holds typeclass methods* — flagged as effectful.

### 4.2 Performing an effect

```clojure
(defn greet []
  (let [name (perform (Read-Line))]
    (println (concat "hello " name))))
```

`perform` is the only new primitive. It looks like a function call but goes through the handler-search machinery.

### 4.3 Handling an effect

```clojure
(handle (greet)
  (Read-Line []     k) (resume k "Roger")
  (Get [key]        k) (resume k (lookup-from-env key)))
```

`handle` looks like `cond` over effect cases. Each case binds the effect's args and a continuation `k`. The default behavior for unhandled effects is to propagate up.

`resume` is one-shot: calling it twice errors at compile time (when statically obvious) or runtime (otherwise). A `resume-with-error e` form discontinues by raising.

### 4.4 Effect rows in types

```clojure
(defn read-config [^cstr path] : str  @ {Read-File})
(defn map         [f xs]       : (vec a) @ e
                  : (-> a (-> b @ e)) (vec a))
(defn pure-square [^int x]     : int  @ {})
```

The `@ <row>` annotation appears after the result type. Empty `{}` means pure. An effect-polymorphic variable (`@ e` without braces) means "whatever the input has."

Inference: row-polymorphism is hard. Lean: require explicit effect annotations on `defn` (matching the v1 type-system stance of "annotations on signatures, inference within bodies"). Inside a function body, the elaborator infers each call's row contribution and unions them; if the result doesn't fit the declared row, it errors.

### 4.5 Mock-handler pattern

The killer app for tests:

```clojure
;; Production
(defn lookup [^cstr key] : (option str) @ {Db}
  (perform (Db-Query key)))

;; Test — no real DB
(handle (lookup "user-42")
  (Db-Query [key] k) (resume k (some "fixture-value")))
```

The function under test is unchanged; the test substitutes a handler. This is the part that gets people excited.

## 5. Implementation strategy

Assuming §12.1 ships first.

### 5.1 Lowering

1. **Functions that perform effects (transitively) get CPS-converted.** §12.1's selective CPS pass: walk the call graph, mark functions whose declared effect row is non-empty; convert those, leave pure functions in direct style. *This is the same pass §12.1 already runs for `shift`/`reset` — effects just give the marker more reasons to fire.*

2. **`perform E args`** lowers to `shift k -> handler-dispatch E args k`. The dispatch function walks the dynamic handler stack (a per-fiber linked list rooted in TLS) looking for the first frame that handles `E`.

3. **`handle expr cases`** lowers to `reset (push-handler cases; expr; pop-handler)`. The handler frame is a `struct {fn_ptr; env*}` — same shape as our closures (§4) — which means *no new runtime data structure*.

4. **`resume k v`** lowers to a CPS-call into `k` with `v`. One-shot enforcement: `k` is consumed by `resume`; double-resume is a compile-time error when the second `resume` is statically reachable, runtime-checked otherwise.

### 5.2 Type-checking effects

Phase 1: untyped (rows ignored at type-check time). Unhandled effects raise at runtime. This matches OCaml 5.0–5.4 mainline.

Phase 2: typed. Add an `effect_row` field to function types. The elaborator unions effect rows of every call inside a body and checks against the declared row. Effect rows are *sets of effect names*, with row variables for polymorphism.

Inference rule: at every `perform E …` site, the enclosing function's row must contain `E`. At every `handle expr (E …) …` site, the row of `expr` may contain `E` but the enclosing function's row drops it (handler discharges the effect).

### 5.3 Calling C / FFI

Foreign functions are pure-by-assumption. The user can declare effect rows on `extern-c` if they want the type system to track effects-on-the-other-side, but it's advisory.

```clojure
(extern-c read [^int fd ^ptr<u8> buf ^usize n] : isize @ {Io})
```

## 6. Interaction with `defer`, `ref`, and RC — the actually-hard part

This is where most algebraic-effect systems run into trouble, and Turmeric isn't an exception.

### 6.1 Defer + continuation capture

Scenario:
```clojure
(defn open-and-read [path] : str
  (let [f (open-file path)]
    (defer (close-file f))
    (perform (Get :line))))
```

When `(perform (Get :line))` captures a continuation, the question is: what happens to `(defer (close-file f))`?

Three choices, none free:
1. **Run defers on capture (Go-ish).** Closes the file when the continuation is captured. Breaks "resume later" — the resumed code references a closed file.
2. **Attach defers to the continuation.** Correct: the continuation holds the deferred actions and runs them on its scope exit. But: continuations now own resources, and a captured-but-never-resumed continuation leaks the defer actions until the continuation itself is dropped.
3. **Forbid `defer` across handler boundaries.** Compile error. Easy to implement, very restrictive — you can't `defer` inside a function that performs effects.

**Lean: (2), with the caveat that one-shot enforcement makes the leak case bounded.** If a continuation is one-shot and goes out of scope unresumed, run its accumulated defers at drop time (this is automatic if the continuation is held by a `ref<continuation>` with the standard drop mechanism).

This is the most important architectural decision in this whole plan. Get it wrong and effects are unusable with Turmeric idioms.

### 6.2 Ref move semantics + multi-shot

Multi-shot continuations re-run code that already moved a `ref<T>`. The second run finds a poisoned (already-moved) ref. Not recoverable in any sensible way.

**Decision: forbid multi-shot continuations entirely in v1 of effects.** OCaml 5 already does. We're stricter.

If multi-shot is ever needed (e.g., for a backtracking parser), gate it behind a `cloneable<continuation>` type that requires every captured value to be `Clone`-trait — a stronger requirement than just "RC'd." Defer this to v2 of effects.

### 6.3 RC + continuation lifetime

A captured continuation holds an environment. That env may contain `rc<T>` values. Three cases:

1. **Continuation is resumed once, then dropped.** Simple: the env's RC values are released when the continuation drops. Same as any closure.
2. **Continuation is captured but never resumed.** The env's RC values stay alive until the continuation is GC'd. Visible in memory profiles as "dropped continuations holding large state."
3. **Continuation crosses a thread boundary.** Phase-2-of-effects problem. Atomics required. Defer.

**v1: case 1 only is well-tuned.** Case 2 works correctly but may surprise users. Document loudly.

### 6.4 Cycle interactions

Continuations can form cycles: a handler captures a continuation, the continuation calls into a function that performs an effect, the new handler is the same one… this is a generator pattern, and the cycle is resolved each time we resume. RC alone handles it because each capture is one-shot.

If we ever support multi-shot, cycles become a real problem and we'd need the §5.1's Bacon-Rajan cycle collector to actually collect them.

## 7. Other options

### 7.1 Just exceptions

Cheapest. Turmeric already plans to need *some* error-propagation mechanism. Exceptions cover errors-with-stack-unwind but not async, generators, or transactional retry.

**Verdict:** ship exceptions in v1 regardless. They're not a substitute for effect handlers; they're a separate, simpler tool. Effects subsume exceptions (an exception is a non-resumable effect), but exceptions are independently useful.

### 7.2 Capability passing

The simplest "no new compiler magic" alternative. Functions that need an I/O capability take it as an explicit argument:

```clojure
(defn read-config [^cstr path ^FileSystem fs] : str
  (.read-file fs path))
```

`FileSystem` is just a struct of function pointers (a typeclass-dictionary, basically). Tests pass a different `FileSystem`. No runtime substrate needed.

**Pros:** ships in v1 alongside typeclasses. Zero runtime cost. Test-substitutability is genuinely good.
**Cons:** every function in a callgraph must thread the capabilities; very viral. No direct-style async (you still need monads or callbacks). Composition is verbose.

**Verdict: this is what Roc and Lean do, and it works well for them. For Turmeric, ship capability-passing as a v1 idiom built on typeclasses. It's a genuine substitute for effects in many use cases.** Only escalate to full effect handlers if direct-style async or generators are a real demand.

### 7.3 Monad transformers

Haskell's classic. Need higher-kinded types, which we don't have and aren't planning to add. Composition is famously fiddly. Doesn't fit a Lisp aesthetic.

**Verdict: not a fit. Don't pursue.**

### 7.4 Free monads / extensible effects

Library-level approach in Haskell. Useful as a staging point in academic papers. In a non-HKT language, this collapses to capability-passing minus the type elegance.

**Verdict: subsumed by 7.2. Don't pursue separately.**

### 7.5 No effects, side effects everywhere

What C, Go, Rust (mostly), Python, etc. do. Simplest. Loses the testability and async-without-async/await wins.

**Verdict: this is where Turmeric *starts*. The question is whether to escalate to 7.2 or all the way to algebraic effects.**

## 8. Risks

1. **Defer/ref interaction (§6.1)** is the deepest issue. Lean is "attach defers to the continuation," but this changes the meaning of `defer` subtly — `defer` no longer means "runs at this scope exit" but "runs at this scope exit *or* at the drop of any continuation that captured this scope."
2. **Multi-shot is incompatible with `ref<T>`.** Forbidding it is the v1 answer; this means some classic effect use cases (backtracking parsers) need a different mechanism.
3. **Performance.** Every `perform` walks the dynamic handler stack. Optimizations exist (handler inlining, monomorphic perform sites) but the worst case is real.
4. **Stdlib churn.** Every I/O function needs an effect annotation. Backwards-incompatible by definition. Plan for a stdlib version bump.
5. **Type-system load.** Effect rows are research-grade. Implementing them well requires unification with row variables — bigger than typeclasses (§12.2). Adding rows on top of typeclasses doubles the type-checker complexity.
6. **Debugging.** Stack traces under CPS look bizarre. Need explicit shadow-stack instrumentation just like §5.1.1 (Cheney-MTA) flagged.
7. **Compiler complexity.** Selective CPS pass is delicate — get the marking wrong and either pure code pays the CPS tax (slow) or effectful code skips the conversion (incorrect).
8. **Interaction with §12.4 hygienic macros.** Effect handlers introduce new binding forms (`(handle … (E [args] k) …)` binds `args` and `k`). The hygiene story has to extend.
9. **Ecosystem split risk.** OCaml libraries written before OCaml 5 are mostly *not* effect-aware; libraries written after take advantage of effects. Same risk for Turmeric — early adopters may write code that doesn't compose with effect-using code.
10. **Premature commitment.** Designing effects before we have any users at all is a classic mistake. Worth keeping the door open; not worth designing for hypothetical demand.

## 9. Tradeoffs (summary table)

| Dimension | Algebraic effects (v3) | Capability passing (v1, §7.2) | No effects (v0) |
|---|---|---|---|
| Direct-style async | ✅ | ❌ (callbacks/futures) | ❌ |
| Mock for tests | ✅ (no signature change) | ✅ (capability arg) | ❌ |
| Generators | ✅ | ❌ | ❌ |
| Backtracking | only with multi-shot | ❌ | ❌ |
| Type-system cost | high (effect rows) | medium (typeclasses) | none |
| Runtime cost | medium (CPS + handler search) | none | none |
| Compiler complexity | high | low | none |
| Defer/ref interaction | hard (see §6) | none | none |
| Bus factor / familiarity | OCaml 5, Eff, Koka users | Roc, Lean users | everyone |
| Time to ship | months | weeks | already shipped |

## 10. Recommendation

**Three tiers:**

1. **v1 (now): capability passing** as the idiomatic mock-and-substitute story. Built on typeclasses (§12.2). Zero runtime cost. Covers ~70% of what people want algebraic effects for.

2. **v2: exceptions.** Independent of effects; useful regardless. Probably ships before §12.1 continuations.

3. **v3 stretch: algebraic effects, *only if* §12.1 delim-conts ships first.** Start with untyped one-shot effect handlers (OCaml 5.0 baseline); add effect rows in a v4 pass if user demand justifies the type-system extension. Multi-shot continuations are a v5 concern; don't promise them.

**What earlier phases need to preserve to keep the v3 door open:**

- *§12.1 CPS-friendly IR.* Already in the plan. Keep it.
- *Effect-row-shaped slot in function types.* Reserve a field in the function-type struct now, even if it's unused in v1. Adding a field later means rewriting every signature in the codebase.
- *Per-frame handler-stack pointer.* When CPS lowering happens, allocate space in the frame for a handler-stack-top pointer. Adding it post-hoc means revisiting every frame layout.
- *Don't bake `defer` into a model that can't be extended to per-continuation defer lists.* The §5 label-based unwind chain works for lexical scope; if we ever need defers attached to continuations, we'll need to lift them to a per-frame structure. **Decision now:** make defers a list-on-the-frame, not labels-in-codegen. This costs a tiny bit at compile time but keeps the door open. *(This is a small but real change to the §5 plan; flag it for review when phase 4 starts.)*

**When to reopen this analysis:**
- §12.1 actually ships → revisit the cost of (3).
- A real user hits the async wall and capability passing isn't enough → start v3 phase 1.
- The Jane Street effect-row paper lands in a stable form → revisit the type-system extension cost.

**When *not* to reopen:**
- Just because effects are interesting. They are. That's not enough.
- For a single use case capability passing covers (database mocking, config injection, etc.). Use the cheaper tool.

---

## Appendix A — quick comparison points (for further reading)

| System | Continuations | Types | Notes |
|---|---|---|---|
| OCaml 5 stock | one-shot | untyped | The baseline this plan targets. |
| Jane Street internal | one-shot | typed (effect rows) | Where this plan would head in v4. |
| Eff (research) | multi-shot | typed | Multi-shot incompatible with our ref model. |
| Koka | multi-shot | typed (effect rows) | Closest to "ideal" but research-grade. |
| Frank | one-shot | typed | Different surface; same underlying ideas. |
| Roc | none (capabilities) | typed | What §7.2 looks like in practice. |
| Lean | tactic monads | typed | Similar capability-passing flavor. |
| Haskell `eff` library | one-shot | typed | Library-level, GADT-heavy. |
| Java loom | continuations, no handlers | untyped | Direct-style async without algebraic effects. |
