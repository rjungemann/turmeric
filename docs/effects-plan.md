# Turmeric — Effect System Plan

A design and feasibility analysis for adding an OCaml-5-style algebraic effect system to Turmeric. References: [OCaml 5.4 effects manual](https://ocaml.org/manual/5.4/effects.html), [Jane Street, "Effective Programming"](https://www.janestreet.com/tech-talks/effective-programming/).

This is a **v3 stretch-goal plan**, not a near-term phase. It's written now to:
1. Decide whether to keep the door open in earlier-phase architectural choices.
2. Give us a concrete reference point so "we'll add effects later" doesn't mean "we'll do something nobody actually thought through."

Cross-references: `§12.1 Delimited continuations`, `§12.2 Type system`, `§5 defer/ref`, `§5.1.2 RC + weak refs` in [turmeric-plan.md](turmeric-plan.md).

---

### Status & sequencing commitments

Two load-bearing decisions have been propagated since this doc's first draft:

1. **The unified defer model (§6.10) is now committed in phase 4.** The list-on-frame mechanism, parent pointers, `may_capture` bits, and effect-row slots all ship with phase 4 of the main plan ([turmeric-plan.md §10.5](turmeric-plan.md)). Cost in the no-effects world is sub-percent; the win in the effects-ship world is *no phase-4 rewrite*. This means the S1/S2/S3 strategy choice in §6.2 is now a runtime policy decision, not an architectural one — making this whole plan substantially cheaper to act on.

2. **§12.1 (delimited continuations) is a sequencing precondition for v3 effects.** By design, §12.1 will already have shipped by the time we revisit this plan. The CPS substrate that effects depend on — selective CPS conversion, closure-shaped continuations, scope frames as heap objects — will exist. Several "if §12.1 ships" hedges remain in this doc from the older framing; treat them as historical. The operative framing is: *given §12.1, what does effects implementation cost?*

These two commitments together change the shape of the recommendation in §10. They don't change *what* effects look like (the design space in §4 is unchanged); they reduce the cost of *getting there* from "rewrite phase 4 + commit to CPS + design effects" to just "design effects."

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

**Short answer: feasible. §12.1 ships before we revisit this plan (see Status & sequencing above), so the CPS substrate is a given — effects become a typed API on top of an existing substrate plus a type-system extension, not a from-scratch runtime overhaul.**

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

**Verdict:** plausible. With §12.1's CPS strategy as a sequencing precondition (see Status & sequencing) and phase 4's unified defer model already in place (§6.10), effects are a natural follow-on rather than a separate research project. The remaining work is the type-system extension (effect rows, §4.4) and the policy choice for `perform`'s defer behavior (§6.2 strategies, now a runtime decision).

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

Given §12.1 has shipped (precondition; see Status & sequencing) and phase 4 ran with the unified defer model (§6.10), implementation is mostly *plumbing on top of existing parts* rather than new substrate.

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

This is where most algebraic-effect systems run into trouble. The Turmeric-specific complication is that we have **scope-bounded resource ownership** (defer + ref), **shared ownership** (rc), and **destructive moves** — all of which assume a clean, single-threaded, lexical control-flow model. Effect handlers break that model: a `perform` mid-scope captures the rest of the scope into a continuation that may be resumed later, dropped, or (in some systems) resumed multiple times.

This section is long because the design is load-bearing. Get it wrong and either (a) effects are unusable with Turmeric's resource-management idioms, or (b) the runtime has subtle use-after-free bugs that escape ordinary testing.

### 6.1 The problem, in concrete scenarios

Five scenarios that make the design pressure visible. Each is correct under direct-style execution; the question is what happens when a continuation is captured mid-scope.

**Scenario A — defer + perform.**
```clojure
(defn read-line []
  (let [f (open "/tmp/in")]
    (defer (close f))               ;; A1: registered here
    (perform (Yield-Lines f))       ;; A2: continuation captured here
    (println "after yield")))
```
At A2, the continuation captures everything after it, including the `(println "after yield")` and the *implicit pending* `(close f)`. If the handler resumes the continuation, both run as expected. If not, *who closes `f`*?

**Scenario B — ref auto-drop.**
```clojure
(defn use-buf []
  (let [r (ref [1 2 3])]            ;; B1: defer (drop! r) injected
    (perform (Yield @r))            ;; B2: continuation captured
    (println @r)))                  ;; B3: still uses r
```
`r` is a `ref<vec<int>>`; the elaborator injects `(defer (drop! r))` at B1. At B2, the continuation captures B3 *and* the implicit defer-drop. Resuming runs B3 then drops `r`. Not resuming should still drop `r` exactly once.

**Scenario C — ref moved before perform.**
```clojure
(defn move-then-yield []
  (let [r (ref 42)]
    (let [r2 (move r)]              ;; C1: r poisoned, r2 owns
      (perform (Yield @r2))         ;; C2: capture; only r2's drop is pending
      @r2)))
```
After the move, only `r2`'s drop is pending. The continuation captures `r2`'s scope; whichever exit path runs (resume or drop), `r2` drops once. `r`'s scope still exists but has no live drop registered.

**Scenario D — multi-shot meets ref.**
```clojure
(defn maybe-twice []
  (let [r (ref 42)]
    (perform (Branch))              ;; D1: handler resumes TWICE
    (println @r)))                  ;; D2: ?!
```
If the handler at D1 resumes twice, the first resume runs D2 then drops `r`. The second resume tries to run D2 with a freed `r`. **This is unrecoverable** — there's no "un-drop" operation, and we can't snapshot heap state cheaply.

**Scenario E — escaping continuation (the generator pattern).**
```clojure
(defn make-gen []
  (let [r (ref 0)]
    (defer (println "gen done"))
    (handle (loop-yielding r)
      (Yield [v] k) k)))             ;; E1: continuation escapes!
```
The handler returns the continuation `k` as a value. The function `make-gen` returns. By the time the caller invokes `k`, `make-gen`'s stack frame is long gone. Yet the resume still needs to access `r` and eventually fire `(println "gen done")`. Where does that state live?

These five scenarios exercise the full design space. Any working strategy has to give a coherent answer to all of them.

### 6.2 The full strategy space

Nine strategies, ranked roughly from simplest to most ambitious. None is free.

**S1 — Run defers on capture (Go-ish).** At every `perform`, walk up to the handler frame and fire all defers in scopes between. The continuation, when resumed, runs in a "scopes still exist but their resources were already released" state.
- *Scenario A:* `f` closes at A2. Resume runs B3 referring to a closed file. Use-after-free.
- *B:* Same — `r` drops at B2; resume reads freed memory.
- *C:* `r2`'s drop fires at C2; resume reads freed memory.
- *E:* Generator pattern is dead — `r` is freed before any resume.
- *Verdict:* fails the basic use cases. Reject.

**S2 — Attach defers to the continuation (chosen).** Heap-allocate scope frames in functions that perform effects. Defers live on those frames. The continuation owns its captured scope chain. On resume, scopes exit normally and defers fire as the resumption walks out. On drop without resume, the continuation's drop walks scopes innermost-to-outermost firing defers.
- *A:* Continuation owns `(close f)`. Resume → close fires at scope exit. No resume → `(close f)` fires when continuation drops. ✅
- *B:* Continuation owns `(drop! r)`. Same story. ✅
- *C:* `r`'s scope has no live drop; `r2`'s drop is owned by continuation. Same story. ✅
- *D:* Forbidden by the multi-shot prohibition (§6.3).
- *E:* The continuation env contains a heap-allocated reference to `r`'s scope frame; `make-gen`'s frame is gone but the heap state survives. ✅
- *Verdict:* this is what we want.

**S3 — Forbid `defer` in effect-using functions.** Compile error if a function declares an effect row and contains a `defer`.
- *A through E:* Don't compile.
- *Verdict:* defeats the purpose. Most realistic functions need both. Reject.

**S4 — Snapshot heap state on capture.** Copy the entire heap on `perform`; restore on resume. Enables true multi-shot.
- *D:* Each resume restores `r`. Works.
- *Verdict:* O(heap size) per perform. Catastrophic for any non-toy program. Used by Eff and Frank for research; not viable for a production language. Reject.

**S5 — Region-based rollback.** Each scope is an arena region; capture remembers the bump pointer; resume rewinds. Multi-shot replays from rewound state.
- Doesn't handle external mutation (foreign files, network, globals), `ref<T>` moves, or `rc<T>` shared state.
- Niche functional languages do this; not a fit for a C-targeted Lisp with destructive update.
- *Verdict:* incompatible with our memory model. Reject.

**S6 — Persistent data only.** No moves, no drops, GC'd everything. Multi-shot becomes free.
- Rebuilds the language from the type system up. We're not Clojure-on-the-JVM; this would erase §5 entirely.
- *Verdict:* a different language. Reject.

**S7 — Eager finalization with marked unsafe-after.** S1 plus a static "this resource is gone after capture" annotation on the captured types. Programmer takes responsibility.
- *Verdict:* worse footgun than S1. Reject.

**S8 — Defer "freezing": defers don't fire on resume's normal scope exit; they only fire when the continuation is finally dropped (after resume completes or before any resume).** A subtle variant of S2 where defers always fire at continuation-drop time, never at scope exit during resumption.
- *A:* `(close f)` fires when continuation drops, not when scope exits during resume. The `println` after the perform runs *before* the close, since the continuation hasn't dropped yet. Subtle: file is closed *much* later than expected.
- Holds resources longer; surprising semantics.
- *Verdict:* technically correct, intuition-violating. Worse UX than S2. Reject.

**S9 — Per-scope handler binding declarations.** Functions explicitly declare which scopes can be captured by which handlers. Type system enforces.
- Massive cognitive load. Effect rows are already a type-system extension; this is another orthogonal one.
- *Verdict:* too restrictive without major language work. Park.

**Outcome: S2 wins.** It's the only strategy that handles all five scenarios with intuitive semantics, doesn't break the memory model, and has an O(captured-scope-depth) cost rather than O(heap). The rest of §6 details how S2 actually works.

### 6.3 Why multi-shot is forbidden in v1

S2 handles single-shot cleanly. Multi-shot is a separate question, and the answer is: incompatible with our memory model, period.

**Multi-shot + ref<T> moves (Scenario D).**

`ref<T>` enforces single-owner semantics via destructive moves. The compiler statically tracks which name owns the ref; `(move r)` poisons the source. There's no runtime "is r poisoned" check — it's a compile-time flow analysis.

Multi-shot resumption *replays* the suspended code. If that code contains a move, the second replay has the source already poisoned — but the static check at compile time says it should be valid (this is the first resume's view). The runtime would either need to:
- Snapshot the move state per-capture (S4 territory) — too expensive.
- Add runtime "is moved" checks everywhere — costs perf in the common case for a feature most code doesn't use.
- Forbid moves in effect-using code — kills the language's resource-management idioms.

**Multi-shot + rc<T> retain-once-release-once.**

`rc<T>` retains on assignment, releases on scope exit. Multi-shot replays the scope exit, so release fires multiple times — *without* the retain firing again on each replay (since the binding init is captured *outside* the replay-loop). Net: under-release, premature free.

Could we replay the binding init too? Yes, but then each replay creates a *fresh* allocation, not the same one — observers comparing identities would see different objects across resumes. And the first replay's allocation leaks (never released).

**Multi-shot + defer in general.**

If defers fire on each scope exit during replay, side-effecting defers (closing files, freeing resources, sending messages) execute multiple times. Defer is supposed to be a *finalization* mechanism; running it twice violates its contract.

**The bottom line.** Multi-shot makes sense in a world of immutable data, garbage-collected memory, and idempotent finalizers. We have none of those. **One-shot is the only consistent choice.**

The compile-time enforcement: `resume` is a one-shot consume — the `k` value is moved into `resume`, becoming inaccessible after. Trying to use `k` twice in the handler is a move-after-move error, caught by the same flow analysis that handles `ref<T>`. Runtime check for the cases the analysis can't see (e.g., k stored in a mutable cell): fail with a clear panic.

**If multi-shot is ever needed** (backtracking parsers, probabilistic programming), gate it behind a `cloneable<continuation>` type that requires every captured value's type to implement a `Clone` trait. Defer this to v2 or v3 of effects.

### 6.4 RC and continuation lifetime — detailed

When a continuation captures a scope, the scope's `rc<T>` bindings live in the continuation's environment. Three cases, each with a precise reference-count story.

**Case 1 — captured, resumed once, runs to completion.**
- At capture: continuation env holds a strong reference to each `rc<T>` in its scopes. RC count is *unchanged at capture* — capture is just a pointer copy; the value's ownership is still through the binding.
- During resume: scopes exit, defers fire, `rc-release` decrements counts.
- On final scope exit: handler returns. Continuation drops (already consumed). All counts are correctly maintained.

**Case 2 — captured, never resumed (E above is one variant).**
- Continuation lives in the handler's return value or some long-lived cell.
- The captured scopes' `rc<T>` bindings keep their counts elevated for as long as the continuation lives.
- This means: storing a continuation pins everything in its environment alive — *including transitively-reachable rc-shared data*.
- When the continuation drops (RC count of the continuation itself reaches 0): the runtime walks the captured scopes innermost-out, firing defers. Each `rc-release` fires.
- *Memory profile gotcha:* "I dropped my generator and memory didn't go down" can happen if the generator's continuation is held by something else. Same RC trap as elsewhere.

**Case 3 — continuation crosses a thread boundary.**
- Single-threaded v1: not possible. Defer this.
- Multi-threaded later: every `rc<T>` in the captured env needs atomic counts (or we restrict cross-thread continuation passing to `arc<T>`-only environments).

**Detailed retain/release walk for a non-trivial scenario:**

```clojure
(defn pipe [src sink]
  (let [buf (rc/of (vec/new))]    ;; rc count: 1
    (defer (rc-release buf))      ;; injected by ref-tracking pass
    (perform (Yield buf))         ;; capture — buf retained in env
    (drain src buf sink)))
```

At `perform`:
- `buf`'s RC count is 1.
- Continuation env stores `buf`; it's *the same pointer* — the binding's ownership transfers to the env conceptually, but mechanically nothing changes (the binding stops being live in the original frame, the env starts holding the same pointer).
- The injected `(rc-release buf)` defer is part of the captured scope frame.

If the handler resumes:
- `(drain src buf sink)` runs.
- Scope exits, `(rc-release buf)` fires. Count → 0. Free.

If the handler drops the continuation:
- Continuation's drop walks captured scopes. `(rc-release buf)` fires. Count → 0. Free.

Either way: count released exactly once. ✅

If something *else* held a strong ref to `buf` before capture:
- Initial count: 2 (binding + outside ref).
- At capture: still 2; binding moves into env.
- On resume completion or continuation drop: release fires. Count → 1.
- Outside ref's eventual release brings it to 0. Free.

The key invariant: **capture is conservative — it doesn't change RC counts at the moment of capture; it just transfers ownership of the count from the suspended scope frame to the continuation's owned scope chain.** Each "release" fires exactly once at exactly one of {scope-exit-during-resume, continuation-drop}.

### 6.5 Cycles and the cycle collector

Continuations don't naturally form cycles in single-shot land:

- A handler captures `k`, returns `k` to its caller. Caller may invoke `k`. `k` is consumed. No cycle.
- A generator: handler captures `k`, stores it in a state cell, returns. The cell holds `k` strongly. When the cell drops, `k` drops. No cycle unless the cell is reachable *from* `k`'s environment — which is possible but requires the user to deliberately build it.

If the user *does* build a cycle (e.g., a state machine where the env contains a reference to the generator that yields the env), we need the §5.1's Bacon-Rajan cycle collector to break it. This is the same situation as any other RC-cycle case; effects don't introduce a new requirement, just a new pattern that's somewhat more likely to appear in practice.

If multi-shot were ever supported, cycles would become *much* more common (each invocation creates a new cycle through the same env), and the cycle collector would need to handle them efficiently. Another reason to defer multi-shot.

### 6.6 Defers that themselves perform effects

The recursive case. What if a defer invokes `perform`?

```clojure
(defn outer []
  (let [resource (acquire)]
    (defer (perform (Log "released")))      ;; defer that performs!
    (release resource)))
```

When the defer fires, it performs `Log`. The runtime searches the dynamic handler stack at *the moment the defer fires* for a `Log` handler. Two cases:

- **Defer fires on normal scope exit.** Handler stack is whatever's outside the scope — typically the function's caller. If the caller has a `Log` handler installed, it runs. Otherwise, unhandled-effect runtime error.
- **Defer fires on continuation drop.** The continuation is being torn down. The handler stack at this moment is the dropper's stack. May or may not have a `Log` handler. Subtle.

Decision: **defers themselves contribute to a function's effect row.** A defer that performs `Log` makes the enclosing function's row include `Log`. The type-checker enforces this, just like any other call site.

This is unusual — most languages treat defers as "just code that runs at the end" without effect tracking. We need to track them because the deferred effects are part of the function's external behavior.

A subtler question: when a defer fires during continuation drop, the dropper's effect row should permit the deferred effects. Otherwise, dropping a continuation could surface unhandled effects from a context where they couldn't be handled.

**Conservative rule:** dropping a continuation may invoke any effect in the captured scopes' defer effects — so the type of the continuation includes those effects in its drop signature. The dropper's enclosing function's effect row must include them.

This is a real type-system feature beyond plain effect rows. Document the rule, accept that it makes "implicit drops" potentially surprising, and consider providing a `(drop! k)` form that's explicit so users can reason about it.

### 6.7 Implementation walkthrough — how S2 actually works

Concrete sketch. §12.1 CPS conversion has shipped (precondition); phase 4's unified defer model is in place (§6.10). Implementation builds directly on both.

**1. Scope frames are heap-allocated when capture is possible.**

The CPS pass marks every function transitively containing a `perform` as "may capture." For those functions, every lexical scope (let, do, function body) becomes a heap-allocated frame:

```c
struct scope_frame_<id> {
    struct frame_header header;     /* parent ptr, RC count */
    /* scope's local bindings here */
    int      n_defers;
    defer_t *defers[MAX_DEFERS_PER_SCOPE];   /* or a heap list */
};
```

The frame header has its own RC count — captured continuations retain it; normal scope exit releases it. At RC=0, walk defers innermost-out and free.

For functions that *don't* perform (or whose every `perform` is statically inlinable into a known handler), scopes stay on the stack — same as today.

**2. Defers register on the frame, not via codegen labels.**

This is the §5 plan change flagged in §10 of the effects plan. Today, §5 uses goto-chains for the unwind. With effects, we need defers as a runtime list because the unwind target may be "the continuation that captured this scope" rather than "the C label N lines down."

Concretely, `(defer (close f))` lowers to:
```c
scope_frame->defers[scope_frame->n_defers++] = make_defer_thunk(close, f);
```

Normal scope exit:
```c
for (int i = scope_frame->n_defers - 1; i >= 0; i--) {
    fire_defer(scope_frame->defers[i]);
}
```

**3. Capture transfers scope-frame ownership to the continuation.**

`perform` lowers to:
```c
continuation_t *k = make_continuation();
k->resume_pc = <after-perform-PC>;
k->captured_frames = list_of_frames_from_here_to_handler();
for (each frame in captured_frames) frame_retain(frame);  /* RC bump */
/* current C function returns to handler with k */
```

The original scope frames are *not* released when we return — the continuation holds them.

**4. Resume re-enters the captured chain.**

`resume k v` walks back into `k->captured_frames`, sets up the C stack to resume execution, and jumps to `k->resume_pc`. As scopes exit normally during the resumption, frames release one by one; defers fire on release.

**5. Drop walks the captured frames in unwind order.**

If `k` is dropped without resume:
```c
void continuation_drop(continuation_t *k) {
    /* Innermost frame first. */
    for (frame in k->captured_frames innermost-first) {
        fire_all_defers(frame);
        frame_release(frame);  /* RC -- ; if 0, free */
    }
    free(k);
}
```

This is symmetric with resume's path, just without the "execute remaining code" step.

**6. Multi-shot enforcement.**

`resume` consumes `k`; trying to resume twice is a use-after-move. The flow analysis we already use for `ref<T>` catches the static cases; the dynamic cases (k stored in a mutable cell) get a runtime "k already consumed" panic.

**7. `ref<T>` and `rc<T>` integrate without special cases.**

Both are just defers from the runtime's perspective:
- `(let [r (ref 42)] …)` — emits `frame->defers[n++] = drop_ref_thunk(r)`.
- `(let [c (rc/of 42)] …)` — emits `frame->defers[n++] = release_rc_thunk(c)`.

Capture + resume + drop all work through the same scope-frame-RC mechanism described above. No special path needed for moves (the static analysis still runs in the resumed code) or RC (release fires exactly once at exactly one path).

### 6.8 Performance implications

S2's costs are real. Worth quantifying.

**Heap allocation for scope frames.** Every `let` in a function that performs effects becomes a heap allocation. Mitigation: escape analysis. If the scope provably doesn't outlive its function (no perform inside, no perform in any callee), keep it on the stack. In practice, most scopes don't directly perform; only specific scopes need heap allocation.

**RC bumps on capture.** Each captured frame retains. For deep call stacks under a generator, this is O(depth) per perform. Usually small (single-digit).

**Defer-list overhead.** Each scope carries a small array (or linked list) of defer thunks. ~16 bytes per defer. Modest.

**Resume cost.** Resumption is "set up C frame, jump to PC" — comparable to a function call plus a small loop to push the frame chain. Sub-microsecond.

**Worst case.** A pathological program that opens a deep nest of resources and yields once per scope creates a long captured chain. Each capture is O(depth). Real-world generators don't do this.

**Optimizations available later:**
- Frame fusion: adjacent scopes that don't introduce captures can share a frame.
- Inline handlers: when the handler is statically known, skip the dynamic search.
- Stack-allocated continuations for handlers that are statically known not to escape.

Don't pre-optimize. Ship the simple version, profile, then optimize the cases that matter.

### 6.9 What v0/v1 needs to preserve

To keep S2 viable as a v3 stretch goal, earlier phases should:

1. **Make defers a list-on-the-frame, not labels-in-codegen.** This is the small-but-load-bearing change to the §5 plan. Filed in §10 of this doc; flag for review when phase 4 (defer) starts.

2. **Reserve a header word in every heap-boxed scope/closure env** for future RC + GC metadata. The §4/§5.1 plans already preserve this; don't repurpose it.

3. **Don't bake "scope exit ≡ specific C label" into anything past elaboration.** Codegen can use labels, but the IR should treat scope exit as "fire defers then control transfers" — abstract enough that a per-frame-list lowering is a swap-out, not a rewrite.

4. **Keep the closure-env-as-heap-box model uniform.** Adding "and now scope frames work the same way" is a small extension if the env model is uniform; a major rewrite if scope frames have a different shape.

5. **Track "may capture continuations" on every function in the IR.** Even in v0/v1 where no one captures, the bit exists and is always false. Adding it later means re-typing every signature in the codebase.

6. **Don't expose `defer` semantics that depend on synchronous scope exit.** Specifically, don't tell users "the defer runs *immediately* at scope exit" without the qualifier "...or when a continuation that captured this scope drops." Today, no one captures, so the qualifier is vacuous; setting the expectation now avoids a doc-breaking change later.

These six are cheap individually and together preserve the v3 option without locking us into it.

### 6.10 The unified defer model — making the strategy choice a policy switch

The framing question: can phase 4 (defer) commit to a *mechanism* that makes the eventual S1/S2/S3 choice a runtime policy decision rather than an architectural rewrite? **Yes — and the unification is cheap.**

The insight is that S1, S2, and S3 differ only in *when* and *where* defers fire, not in *what* defers are or *how* they're registered. If phase 4 picks the right shape for the underlying mechanism, every later strategy is a different traversal of the same data structure.

#### 6.10.1 The mechanism (six commitments for phase 4)

1. **Defers are entries in a list-on-frame, not codegen labels.** Each scope has a `Frame` struct with a defer list (initially empty). `(defer expr)` lowers to `frame_push_defer(current_frame, thunk)`. Scope exit walks the list LIFO, fires each, frees. *Replaces the §5 "label-based unwind chain" plan with a runtime-list lowering.*

2. **`ref<T>` / `rc<T>` drops are just defers.** No parallel cleanup path. `(let [r (ref 42)] …)` injects the same `frame_push_defer` call as a user-written defer. The drop function pointer differs; the registration mechanism doesn't.

3. **Frames have a parent pointer.** In v0/v1, this is stack-allocated; parent points to the outer stack frame's defer block. In future phases that need heap frames (continuations, escape), the same parent pointer applies — the allocator changes; the chain shape doesn't.

4. **Exit-from-frame is "fire defers, transfer control."** Both halves are first-class operations in the IR. *Where* control transfers is policy:
   - *Normal scope exit:* lexical parent.
   - *`return`:* walk every enclosing frame, fire defers per frame, then function-exit.
   - *`perform` (v3):* policy hook — see §6.10.2.
   - *Continuation drop (v3):* walk captured frames, fire defers per frame, then free.

5. **Every function carries a "may capture continuations" bit.** False in v0/v1. The phase 4 codegen reads it to decide stack-vs-heap frame allocation; in v0/v1 it's always false, so frames are always stack. Phase v3 starts setting it true for effect-using functions and the same codegen path automatically heap-allocates them.

6. **Every function type carries an effect row slot.** Empty in v0/v1. Phase v3 starts populating it. In v0/v1 the slot is `nullptr`; type-equality treats `nullptr` as `{}`. No effect-row code paths run.

These six choices are roughly as good as the alternatives even in a no-effects world. The cost in v0/v1: ~one pointer's worth of frame-overhead per scope that has any defer (most scopes have zero), plus a tiny amount of static-analysis bookkeeping for the always-false bits. Sub-percent in any realistic program; unmeasurable in most.

#### 6.10.2 The strategy as policy

With the mechanism above, S1/S2/S3 in v3 reduces to *one runtime decision at `perform`*:

**S1 (run on capture):**
```
on_perform(handler_frame):
    for f in frames_from_here_to(handler_frame):
        fire_all_defers(f)
        free_frame(f)
    transfer_to_handler_with_no_continuation()
```

**S2 (attach to continuation):**
```
on_perform(handler_frame):
    captured = frames_from_here_to(handler_frame)
    for f in captured: frame_retain(f)
    k = make_continuation(captured, resume_pc)
    transfer_to_handler_with(k)
```

**S3 (forbid):** elaborator rejects functions containing both `defer` and `perform`. Runtime never reaches the policy decision.

Three different traversals of the same data structure. **The mechanism doesn't change; the policy does.** Phase 4 ships any of them by writing 20-ish lines of `on_perform`; the rest of the runtime is identical.

This means: phase 4 ships *without committing to a strategy*. We can even ship a v3 prototype with S1, observe how badly it breaks generators, switch to S2 — and *phase 4 doesn't move at all*.

#### 6.10.3 What we lock in (and what we don't)

**Locked in by phase 4:**
- Defer-as-list-on-frame representation.
- `ref<T>` / `rc<T>` drops as entries in that list.
- Parent pointers (even when always stack-allocated).
- "May capture" bit (even when always false).
- Effect row slot (even when always empty).

**Not locked in:**
- Strategy (S1 vs S2 vs S3).
- Whether multi-shot is ever supported.
- Whether handlers use dynamic or lexical scope.
- Whether effect rows are inferred or annotated.
- The precise type-system rules for defer-effect-rows.
- Whether the `perform` lowering is CPS-based (§12.1) or one-shot setjmp.

The locked-in set is just enough plumbing to make the policy choice late; the not-locked-in set is everything that genuinely depends on usage patterns we don't have yet.

#### 6.10.4 The cost of *not* unifying

If phase 4 ships with goto-chain codegen as the current §5 plan describes, and v3 then wants S2:

- **Codegen rewrite.** Defers aren't data, they're emission patterns. Can't move them onto a heap frame; have to extract them into runtime thunks.
- **ref/rc migration.** Today's plan has `ref<T>`'s drop as a compiler-injected `defer`; if defer is a codegen pattern, this migration touches every existing program's compilation.
- **"May capture" bit retrofit.** Doesn't exist; adding it means re-typing every signature.
- **Parent-pointer retrofit.** Stack frames don't have one; adding it for some-but-not-all functions creates a heterogeneous frame model that every pass has to special-case.
- **Test churn.** Codegen snapshot tests (§11) are all stale; effectively a phase 4 redo.

Total: probably a phase 4 rewrite, plus interaction repair across phase 5 (`ref<T>` integration), phase 7 (stdlib), and phase 9 (RC). Several months of refactoring across multiple passes.

#### 6.10.5 Cost of getting it wrong (paying for unification we don't use)

If phase 4 commits to the unified model and we *never* need it (effects don't ship, §12.1 continuations don't ship):

- *Per-frame defer list:* one pointer per scope-that-defers. Most scopes have zero defers, so they pay nothing. Scopes with one defer pay one cache-line-friendly indirection on exit vs. an inline goto. Sub-nanosecond.
- *"May capture" bit:* one bool per function. Trivially eliminated by the C compiler when always false.
- *Effect row slot:* pointer-sized field on function types. Used only for type-display; eliminated at codegen.
- *Parent pointer on every frame:* zero cost when stack-allocated (the C frame already has one — saved RBP); a pointer assignment when explicitly tracked, dead-code-eliminated when unused.

Worst case: low single-digit percentage overhead in defer-heavy programs. Best case (more likely): unmeasurable.

#### 6.10.6 Generalization: this is a pattern, not a one-off

The unified-defer technique generalizes. Whenever a future phase might want to vary *when* something happens without changing *what* it is, the corresponding earlier phase should:

1. Represent the *what* as data (a list, a struct, a tagged union) rather than as inline codegen.
2. Make the *when* a function over that data.
3. Mark the data structure with bits for "future variations might apply" so later passes can opt in without retrofitting the schema.

We've already done this in two places without naming it:
- *Operator dispatch table* (§1.1, phase 1): operators are *data* (table entries); typeclass dispatch (§12.2) adds entries without changing the lookup function.
- *Allocator indirection* (§5.1, v0): all heap allocations go through `tur_alloc`, so swapping in RC (§5.1.2) or Bacon-Rajan (§5.1) is a one-file change.

The unified defer model is the same pattern applied to control flow. Phase 4 is the third place we apply it.

#### 6.10.7 Recommendation

**Commit to the unified defer model in phase 4.** The cost is modest in the no-effects world; the upside in the v3 world is "we don't need to rewrite phase 4."

Concrete checklist to file in [turmeric-plan.md §10.5 Phase 4](turmeric-plan.md) when phase 4 starts:

- [ ] Defer codegen is a runtime list, not goto-chains. `Frame` struct has a `defers: defer_t[]` field.
- [ ] `ref<T>` / `rc<T>` drop registration uses the same `frame_push_defer` mechanism as user defers.
- [ ] Frame struct has a `parent: Frame*` field (initially set to the outer frame's address).
- [ ] Function metadata has a `may_capture: bool` field (initially false).
- [ ] Function type has an `effect_row: EffectRow*` field (initially nullptr; treated as `{}`).
- [ ] Frame allocator chooses stack vs heap based on `may_capture` (always stack for now).
- [ ] *Document the rationale in the phase 4 commit message:* "this is the unified defer model from effects-plan.md §6.10 — small overhead now, big savings if effects ship."

That's the entire commitment. Phase 4 ships when these are in place; effects (or whatever the future v3 brings) plug in without touching phase 4 again.

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

2. **v2: exceptions.** Independent of effects; useful regardless. Almost certainly ships before §12.1 continuations.

3. **v3 (post-§12.1): algebraic effects.** §12.1 is a sequencing precondition (Status & sequencing); by the time this tier is on the table, the CPS substrate exists and phase 4 already shipped the unified defer model. Start with untyped one-shot effect handlers (OCaml 5.0 baseline); add effect rows in a v4 pass if user demand justifies the type-system extension. Multi-shot continuations are a v5 concern; don't promise them.

**What earlier phases need to preserve (mostly already locked in):**

- *§12.1 CPS-friendly IR* — committed; ships before this plan revives.
- *Effect-row slot in function types* — locked in by phase 4 per [turmeric-plan.md §10.5](turmeric-plan.md) and §6.10.
- *`may_capture` bit on every function* — locked in by phase 4 per §6.10.
- *Defers as list-on-frame, not codegen labels* — locked in by phase 4 per §6.10. The §6.2 strategy choice (S1 / S2 / S3) is now a runtime policy decision, not an architectural one.
- *Per-frame parent pointer* — locked in by phase 4 per §6.10.
- *Per-frame handler-stack pointer* — when CPS lowering happens (§12.1), allocate space in the frame for a handler-stack-top pointer. The §6.10 frame struct already has unused header bits reserved.

The list shrank from "things to remember" to "things already committed." That's the value of the Status & sequencing decisions: this plan is now a *design-and-prioritize* exercise, not a *preserve-the-option* exercise.

**When to reopen this analysis:**
- A real user hits the async wall and capability passing isn't enough → start v3 phase 1.
- The Jane Street effect-row paper lands in a stable form → revisit the type-system extension cost.
- §12.1 finishes shipping and we have spare capacity → optionally start v3 prototyping early to validate the §6.10 mechanism in practice.

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
