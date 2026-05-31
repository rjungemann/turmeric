# Finish `call/cc` -- Completion Plan (CC0--CC6)

> **Status:** Not started. Closes audit item 1 of the control-flow audit
> (`call/cc`/`escape` are degenerate sugar, currently gated behind `-Xcallcc`
> with diagnostics `TUR-E0700`/`TUR-E0701` -- see CF4 of
> [`control-flow-completeness-plan.md`](archive/history/control-flow-completeness-plan.md)).
>
> **Key insight:** the audit's CF4 deferral assumed real capture requires a
> whole-program CPS pass. The runtime now ships working **delimited**
> continuations (`tur_cont`, one-shot) and **cloneable** continuations
> (`tur_cloneable_cont`, multi-shot) via fiber-context save/restore
> (`src/async/fiber_ctx_{x64,arm64}.S`). `call/cc*` already exposes the
> cloneable path. We can finish `call/cc` and `escape` today by lowering them
> through the same machinery -- with an honest **delimited** semantics --
> instead of waiting for the full CPS pass.
>
> **Last updated:** 2026-05-31
>
> **Open questions resolved (2026-05-31):**
> - **OQ1** -- Require an explicit enclosing `(reset ...)`; `call/cc`/`escape`
>   outside one is a compile-time error. Honest about delimited semantics;
>   matches Racket composable / Haskell ContT / OCaml. CC2 below is replaced
>   with a diagnostic phase, not an implicit-boundary phase.
> - **OQ2** -- `escape` lowers to `shift0` (not as an alias of `call/cc`).
>   Matches C `longjmp` / CL `return-from` / Java `throw` / OCaml `discontinue`:
>   `(k v)` unwinds without re-installing a prompt.
> - **OQ3** -- `k`'s default usage is `^unique` (one-shot, drop OK), matching
>   handler-clause defaults. Users may opt in to `^linear` (exactly-once);
>   the existing `EX_SHIFT` plumbing handles it. CC1 ships a fixture that
>   exercises `^linear k` through the new `call/cc` desugar to lock this in.
>
> **Related:**
> - [`control-flow-completeness-plan.md`](archive/history/control-flow-completeness-plan.md) -- CF4 (the gate this plan retires)
> - [`control-flow-completeness-audit.md`](archive/history/control-flow-completeness-audit.md) -- audit item 1
> - [`multishot-continuations-plan.md`](archive/multishot-continuations-plan.md) -- the *full*-CPS direction (still deferred)
> - [`linear-continuations-plan.md`](archive/linear-continuations-plan.md) -- `^linear` / one-shot accounting
> - [`stubs-and-workarounds.md`](archive/stubs-and-workarounds.md) §1.4 -- the original stub entry

---

## Motivation

`(call/cc f)` currently elaborates to `(let [__cc_f f] (__cc_f 0))` -- `f`
receives the literal integer `0` as its "continuation." Calling that "k" does
nothing useful. `(escape f)` is identical. Both are gated behind `-Xcallcc`,
which keeps the codebase honest but leaves a high-profile name pointing at
nothing.

Meanwhile, `call/cc*` (cloneable, multi-shot) has *real* semantics: it
desugars to `(cloneable-reset (cloneable-shift f 0))` and lowers to
`tur_cloneable_cont_alloc` / `tur_cloneable_cont_resume` via the fiber-context
runtime (`src/compiler/elab_effects.c:302`, `src/compiler/emit_effects.c:1156`).
The same runtime backs `shift`/`reset` (one-shot via `tur_cont`).

This plan finishes `call/cc` and `escape` by re-using that runtime. The cost
of doing so honestly is one constraint: capture is **delimited** -- it extends
to the nearest enclosing `reset` (or an implicit function-level reset), not
to the top of the program. That is exactly the semantics Scheme's
`call-with-current-continuation` has inside a `reset` boundary, and it covers
every motivating use case for `call/cc`/`escape` in this codebase (early
return, ambiguous choice, generators) without the soundness landmines of the
v1 stub.

### Goals

- Replace the `0`-as-continuation stub with a real, callable continuation.
- Make `call/cc` / `escape` work without `-Xcallcc`; retire `TUR-E0700` /
  `TUR-E0701`.
- Keep `call/cc*` (cloneable, multi-shot) unchanged -- it is already correct.
- State the delimited boundary precisely so users do not confuse it with
  undelimited Scheme `call/cc`.

### Non-goals (still deferred)

- A whole-program CPS transform -- still post-1.0, tracked separately.
- Undelimited `call/cc` whose capture extends past the enclosing function.
- Multi-shot semantics for the bare `call/cc` (use `call/cc*` for that).

---

## Disposition

| Form | Today | After this plan | Notes |
|---|---|---|---|
| `(call/cc f)` | `TUR-E0700` ungated; `(f 0)` under `-Xcallcc` | Delimited capture via `tur_cont` (lowered to `(reset (shift k (f k)))`); `f` receives a real `cont<T>` | One-shot `^unique` by default; `^linear` opt-in. Use `call/cc*` for multi-shot. **Requires an enclosing `reset`** -- ungated use without one is `TUR-E0705` (see CC2). |
| `(escape f)` | `TUR-E0701` ungated; `(f 0)` under `-Xcallcc` | Delimited early-exit via `tur_cont` (lowered to `(reset (shift0 k (f k)))`); `(k v)` unwinds without re-installing the prompt | One-shot abort. Same boundary requirement as `call/cc`. |
| `(call/cc* f)` | Real cloneable capture | Unchanged | Already shipped (CPS-CL8). |
| `-Xcallcc` flag | Unlocks the stub | Removed (warn-and-noop for one release) | See CC5. |
| `TUR-E0700` / `TUR-E0701` | Active | Retired (kept reserved in the registry) | See CC5. |

---

## Semantics (the contract)

`(call/cc f)` evaluates as follows:

1. Determine the enclosing **continuation boundary** -- the nearest dynamically
   enclosing `(reset ...)` form. If none is present, elaboration **fails** with
   `TUR-E0705` (see CC2). There is no implicit boundary.
2. Allocate a one-shot `tur_cont*` representing "the rest of the computation
   from this `call/cc` site up to the boundary."
3. Invoke `f` with that continuation as its argument. `f : cont<T> -> T` where
   `T` is the type the boundary returns.
4. If `f` returns normally, that return value becomes the value of the
   `call/cc` expression (the captured `k` is dropped).
5. If `f` (or anything it calls) invokes `(k v)`, control returns to the
   `call/cc` site with `v` as its value; whatever `f` was doing is abandoned.

`(escape f)` is the one-shot abort flavor: invoking `(k v)` *unwinds to the
boundary* and produces `v` from the *boundary*, not the `escape` site. Useful
for early-exit out of deep recursion without manual `Option` plumbing.

The boundary distinction maps directly onto the existing
`EX_SHIFT` (captures up to `EX_RESET`, value returns at the shift site) vs.
`EX_SHIFT0` semantics that already type-check (CF2).

### What this is not

- **Not undelimited.** A captured `k` cannot escape past its boundary; calling
  `k` after the boundary has returned is a defined-behavior runtime error
  (`tur_cont_consumed` returns true, runtime aborts with `TUR_RT_E_CONT_USED`).
  This matches the existing `tur_cont` one-shot discipline.
- **Not multi-shot.** Calling `k` twice is `TUR-E0100` / `TUR-E0101` per
  linear-continuations-plan. Use `call/cc*` for multi-shot.
- **Not a CPS transform.** No whole-program rewrite is introduced; this is a
  desugar to existing primitives.

---

## Phase ordering at a glance

| Phase | Scope | Why this order |
|---|---|---|
| CC0 | Spec ratify + audit cross-link | Lock the delimited semantics before code moves |
| CC1 | Lower `(call/cc f)` to `(reset (shift k (f k)))` | The minimum viable real implementation, reuses CF2 typing |
| CC2 | "Missing-boundary" diagnostic (`TUR-E0705`) | Per OQ1: require explicit `reset`, no implicit promotion |
| CC3 | Lower `(escape f)` to `(reset (shift0 k (f k)))` | Per OQ2: abort semantics, no prompt re-install |
| CC4 | Typing: `cont<T>` parameter for `f`, drop placeholder result type; `^linear k` fixture | Replaces the v1 `:int` punt; locks in OQ3 |
| CC5 | Retire `-Xcallcc`, `TUR-E0700`/`TUR-E0701`; convert fixtures | User-visible cleanup |
| CC6 | Docs: guide + audit closure | Update compiler-flags-guide, control-flow audit, archive `TUR-E07xx` |

---

## Phase CC0 -- Ratify delimited semantics

Lock the [Semantics](#semantics-the-contract) section above as the 1.0
contract; the rest of the phases assume it.

- **CC0.1** Confirm the table under [Disposition](#disposition) with
  maintainers: delimited `call/cc`/`escape` ships, undelimited stays
  post-1.0 (tracked by multishot-continuations-plan). *Done when:* this
  section is annotated "ratified" with a date.
- **CC0.2** Cross-link from
  [`control-flow-completeness-audit.md`](archive/history/control-flow-completeness-audit.md)
  pre-v1.0 gap item 1 to this plan, noting that CF4's "gate-only" disposition
  has been **superseded by CC1-CC5** for the delimited case. *Done when:* the
  audit's item-1 status line points here.
- **CC0.3** Inventory call sites that depend on the *stub* semantics (i.e.
  programs that pass `0` to `f` and expect it to type-check as `:int`). Today
  this is just the three `-Xcallcc` fixtures
  (`continuation-callcc`, `continuation-escape`, `continuation-escape-fn`);
  they will be rewritten in CC5.3 to use real continuations.

---

## Phase CC1 -- Lower `(call/cc f)` to a real shift/reset

Replace the placeholder desugar in `elab_call_cc`
(`src/compiler/elab_effects.c:1570`) with a real lowering through the existing
delimited-continuation IR.

- **CC1.1** Change `elab_call_cc` to build, instead of `(let [__cc_f f]
  (__cc_f 0))`, the form `(reset (shift __k (f __k)))`. Re-use the existing
  elaborator paths for `EX_RESET` / `EX_SHIFT` so the new lowering inherits
  the CF2 typing rule (the shift site's type is `f`'s codomain, and `f`'s
  domain is `cont<T>`). *Done when:* a `call/cc` whose `f` ignores `k`
  returns the body value; a `call/cc` whose `f` invokes `(k v)` returns `v`
  at the `call/cc` site.
- **CC1.2** Gate the v1 stub behind a feature check that's now always-false;
  delete the dead branch and the `__cc_f` / `zero` form-building helpers
  once CC2 ships the implicit boundary. *Done when:* `elab_call_cc` no longer
  references `intern_cstr(e->st, "__cc_f")`.
- **CC1.3** Plumb the elaborator's known `g_callcc_enabled` short-circuit
  through CC5; for CC1 alone, keep the diagnostic in place so the build stays
  green between phases (ungated `call/cc` keeps erroring as `TUR-E0700`
  until CC5).
- **CC1.4** Fixture: `tests/fixtures/callcc-real-capture/input.tur` --
  ```
  (defn main [] :int
    (+ 1 (reset (call/cc (fn [k] (+ 100 (k 41)))))))
  ;; expected: 42  (k aborts the (+ 100 ...), returns 41 at the call/cc site,
  ;;               outer +1 makes 42)
  ```
  Snapshot under `tests/run.sh`; the test runs **without** `-Xcallcc`
  (after CC5) or with it (during CC1-CC4 transition).

---

## Phase CC2 -- "Missing-boundary" diagnostic (`TUR-E0705`)

Per OQ1, Turmeric does **not** synthesize an implicit boundary. `(call/cc f)`
and `(escape f)` outside any enclosing `(reset ...)` are a hard compile-time
error pointing the user at `reset` (or `call/cc*` for the multi-shot case).
This is the Racket-composable / Haskell-ContT / OCaml-effect-handler model.

- **CC2.1** Track an `enclosing_reset_depth` counter on the elaborator
  (`Elab`), incremented on entry to an `EX_RESET` body and decremented on
  exit. This mirrors the existing `serial_reset_depth`
  (`src/compiler/elab_effects.c`, `elab_serial_reset` /
  `elab_serial_shift`). *Done when:* the counter is non-zero exactly inside a
  `reset` body.
- **CC2.2** In `elab_call_cc` / `elab_escape`, before invoking the CC1/CC3
  desugar, check `enclosing_reset_depth > 0`. If zero, emit `TUR-E0705`
  (new code in the reserved `E07xx` band):
  > `'call/cc' requires an enclosing 'reset' boundary; wrap the call site in
  > '(reset ...)' or use 'call/cc*' for a multi-shot continuation that
  > establishes its own boundary.`
  Symmetric wording for `escape`. *Done when:* `(call/cc (fn [k] (k 1)))`
  at top level fails with `TUR-E0705`; the same form inside
  `(reset ...)` compiles.
- **CC2.3** Note that `call/cc*` is *unaffected* -- its desugar already wraps
  itself in `cloneable-reset`, so it brings its own boundary and never needs
  one from the user. Update the diagnostic's help text to suggest `call/cc*`
  as one of the two fixes.
- **CC2.4** Fixture: `tests/fixtures/errors/callcc-no-reset/input.tur` and
  `tests/fixtures/errors/escape-no-reset/input.tur`, each asserting the new
  `TUR-E0705` diagnostic. The happy-path fixtures from CC1.4 / CC3.3 already
  exercise the in-`reset` case.
- **CC2.5** Document the boundary rule precisely in CC6.1: "the nearest
  dynamically enclosing `reset` -- *not* the function body; there is no
  implicit promotion."

---

## Phase CC3 -- Lower `(escape f)` to `(reset (shift0 k (f k)))`

Per OQ2, `escape` lowers to `EX_SHIFT0` (not `EX_SHIFT`). The semantic match
is with C `longjmp`, CL `return-from`, Java `throw`, OCaml `discontinue`:
calling `(k v)` unwinds to the boundary without re-installing a prompt around
the captured continuation. For a one-shot abort with no nested boundaries the
behavior is indistinguishable from `shift`; in nested cases `shift0` avoids
surprising prompt re-installation.

- **CC3.1** Replace the placeholder desugar in `elab_escape`
  (`src/compiler/elab_effects.c:1632`) with the `EX_SHIFT0`-based lowering:
  `(escape f)` becomes `(reset (shift0 __k (f __k)))`. Re-use the existing
  `EX_SHIFT0` elaborator and codegen paths so `escape` inherits the CF2
  typing rule for free. *Done when:* `(reset (escape (fn [k] (k 7))))`
  returns `7`, and the generated C calls the `shift0` emitter, not the
  `shift` emitter.
- **CC3.2** Add the missing-boundary check from CC2.2 to `elab_escape` (same
  diagnostic `TUR-E0705`, wording adjusted for `escape`). *Done when:*
  `(escape (fn [k] (k 7)))` at top level fails with `TUR-E0705`.
- **CC3.3** Fixture: `tests/fixtures/escape-real/input.tur` --
  ```
  (defn find-first-positive [xs] :int
    (reset
      (escape (fn [k]
        (for-each (fn [x] (when (> x 0) (k x))) xs)
        -1))))
  ```
  Expected: returns the first positive element, or -1 if none. The explicit
  outer `reset` is required (CC2).
- **CC3.4** Fixture: `tests/fixtures/escape-nested-reset/input.tur` -- an
  `escape` nested inside two `reset`s, asserting the abort lands at the
  *inner* boundary and the outer `reset` is unaffected (this is the
  observable difference between `shift0` and `shift`). Lock in the OQ2
  decision with a snapshot test rather than a comment.

---

## Phase CC4 -- Typing: `cont<T>` for `f`'s parameter

The v1 stub gave `f` the parameter type `:int` (because `0` was passed). The
real lowering passes `cont<T>` where `T` is the boundary's answer type. The
elaborator already has `TY_CONT` (`src/compiler/types.h:92`) and constructs
it in `elab_shift` / `elab_shift0`; CC4 wires it through `call/cc` / `escape`.

- **CC4.1** Make `elab_call_cc` *require* that `f` has function type. If `f`'s
  declared signature is `(fn [k] T)` with `k`'s type defaulted to `:int`,
  unify `k`'s type with `cont<T'>` where `T'` is the inferred answer type at
  the `call/cc` site. Emit `TUR-E0001` on mismatch with a span pointing at
  `k`'s parameter. *Done when:* `(call/cc (fn [k :cont<int>] (k 42)))` types,
  and `(call/cc (fn [k :str] ...))` fails with a clear message.
- **CC4.2** Default rule: if `f`'s `k` parameter has no annotation, infer
  `cont<T>` rather than `:int`. (`:int`-by-default for unannotated lambda
  params is a general v1 wart; CC4 is a *local* exception inside `call/cc` /
  `escape`'s receiver position.) *Done when:* `(call/cc (fn [k] (k 5)))`
  type-checks without any annotation.
- **CC4.3** Replace the result-type placeholder in `elab_call_cc` /
  `elab_escape` with the boundary's answer type (analogous to the CF2 fix
  for `shift`/`shift0`'s codomain). *Done when:* a deliberately mistyped
  `call/cc` body is rejected by elaboration rather than miscompiling.
- **CC4.4** Per OQ3, `k` defaults to `^unique` (one-shot, may be dropped --
  matches handler-clause defaults and the early-return idiom). `^linear k`
  is accepted as an opt-in for the exactly-once discipline; it composes
  through the new desugar because `EX_SHIFT` / `EX_SHIFT0` already plumb
  `^linear` through to `tur_cont` consumption checks. *Done when:*
  - `tests/fixtures/callcc-linear-k/input.tur` -- `(call/cc (fn [^linear k] (k 42)))`
    inside a `reset` returns `42` and snapshots cleanly;
  - `tests/fixtures/errors/callcc-linear-k-dropped/input.tur` --
    `(call/cc (fn [^linear k] 99))` inside a `reset` is rejected by
    `TUR-E0100` (linear value dropped), proving `^linear` reaches the new
    desugar's binder.

---

## Phase CC5 -- Retire `-Xcallcc`, retire `TUR-E0700` / `TUR-E0701`

The gate exists only because the stub was unsound. Real semantics retire it.

- **CC5.1** In `src/main.c` and `wk_apply_flags`, change `-Xcallcc` to a
  warn-and-noop (`warning: -Xcallcc is no longer required; call/cc is now
  always available`) so existing build commands keep working for one release.
  Schedule removal of the flag for the release *after* the one that ships
  this plan. *Done when:* `tur -Xcallcc run prog.tur` warns but still
  succeeds; `tur run prog.tur` with `(call/cc ...)` also succeeds.
- **CC5.2** Remove the `TUR_E0700_CALLCC_GATED` / `TUR_E0701_ESCAPE_GATED`
  emit sites in `src/compiler/elab_effects.c`. Keep the codes in
  `src/compiler/diag.h` and the registry under `tests/E07xx` for one
  release with a "retired" annotation so external docs that link to them
  don't 404. *Done when:* `grep -n "TUR_E0700\|TUR_E0701" src/compiler/elab_effects.c`
  is empty.
- **CC5.3** Rewrite the three existing `-Xcallcc` fixtures to exercise the
  *real* semantics:
  - `tests/fixtures/continuation-callcc` -- replace the "test ignores k"
    program with one that invokes `(k v)` and observes the abort.
    Drop the `flags` file (the gate is gone).
  - `tests/fixtures/continuation-escape` -- same treatment.
  - `tests/fixtures/continuation-escape-fn` -- same treatment.
  Regenerate `expected.c` per CLAUDE.md's snapshot rule.
- **CC5.4** Delete `tests/fixtures/errors/callcc-gated` and
  `tests/fixtures/errors/escape-gated`. *Done when:* `bash tests/run.sh`
  shows zero `FAIL` lines.

---

## Phase CC6 -- Docs

Make the contract findable, accurate, and not contradicted by older guides.

- **CC6.1** Add a `Continuations (call/cc, escape)` section to a single
  authoritative guide -- recommend extending
  `docs/guides/effects-system-guide.md` (which already covers shift/reset)
  rather than creating a new guide. Cover:
  - the delimited semantics (boundary = nearest enclosing `reset`, else
    function body);
  - the one-shot rule (use `call/cc*` for multi-shot);
  - typing (`f : cont<T> -> T`);
  - what is and isn't possible (no escape past boundary; resumption inside the
    same function only).
- **CC6.2** Update `docs/guides/compiler-flags-guide.md` -- remove the
  `-Xcallcc` "experimental (unsound)" entry from the quick-reference table
  and replace the detail section with a one-line "deprecated; no-op" notice
  pointing at the new guide section.
- **CC6.3** Update `docs/archive/history/control-flow-completeness-audit.md`
  pre-v1.0 gap item 1: mark it **resolved by CC1-CC5** with a one-line
  pointer to this plan and to the post-1.0 CPS note (the latter is now only
  about undelimited capture, not `call/cc` itself).
- **CC6.4** Move `docs/archive/stubs-and-workarounds.md` §1.4 to the
  "resolved" section, citing this plan.
- **CC6.5** Once everything above is green, archive this plan to
  `docs/archive/history/call-cc-completion-plan.md` per the project's
  shipped-plan convention.

---

## Exit criteria for finishing `call/cc`

- `bash tests/run.sh` shows zero `FAIL` lines with the new
  `callcc-real-capture` / `escape-real` fixtures *and* the rewritten
  `continuation-callcc` / `continuation-escape` / `continuation-escape-fn`
  fixtures running **without** `-Xcallcc`.
- `grep -n "TUR_E0700\|TUR_E0701" src/compiler/elab_effects.c` returns no
  emit sites; the diagnostic codes are annotated "retired" in the registry.
- `tur run prog.tur` with `(call/cc (fn [k] (k 42)))` returns `42` without
  any flag.
- `docs/guides/effects-system-guide.md` documents the delimited semantics
  and the typing rule.
- `docs/archive/history/control-flow-completeness-audit.md` item 1 is marked
  resolved.

---

## Resolved questions (2026-05-31)

- **OQ1 resolved -- require explicit `reset`.** Implicit boundary rejected
  as a silent semantic drift from Scheme; CC2 ships `TUR-E0705` instead.
- **OQ2 resolved -- `escape` lowers to `shift0`.** Matches abort semantics
  in C/CL/Java/OCaml; nested-boundary behavior tested by
  `escape-nested-reset` (CC3.4).
- **OQ3 resolved -- `^unique` default, `^linear` opt-in.** Matches handler
  clauses; locked in by `callcc-linear-k` and
  `errors/callcc-linear-k-dropped` (CC4.4).

---

## Appendix A -- `call/cc` vs. `call/cc*`

Same idea (capture the current continuation), different ownership discipline.
The split is exactly the `tur_cont` (one-shot) vs. `tur_cloneable_cont`
(cloneable) split already in the runtime.

| Aspect | `call/cc` (this plan) | `call/cc*` (already shipped) |
|---|---|---|
| Shot count | **One-shot** -- calling `k` twice is `TUR-E0101` | **Multi-shot** -- clone before each resume |
| Continuation type | `cont<T>` (`TY_CONT`) | `cloneable_cont<T>` (`TY_CLONEABLE_CONT`) |
| Runtime backing | `tur_cont` -- single saved fiber frame | `tur_cloneable_cont` -- frames are heap-copied; each clone is independent |
| Boundary | **User-written `(reset ...)` required** (CC2 / `TUR-E0705`) | **Self-establishing** -- lowers to `(cloneable-reset (cloneable-shift f 0))`, no user `reset` needed |
| Capture restriction | Whatever `shift` already allows (no Clone requirement) | All captured values must implement `Clone` (`TUR-E0014`) so frames can be duplicated |
| Linearity annotation | `^unique` default; `^linear` opt-in (CC4.4) | `^multishot` (implicit, by construction) |
| Default cost | Cheap -- one alloc | More expensive -- alloc + per-clone deep copy of captured frames |
| Canonical use cases | Early exit, coroutine handoff, single-resume callbacks | Backtracking, non-determinism, generators that fork, time-travel / replay |
| Lowering after CC1 | `(reset (shift __k (f __k)))` | `(cloneable-reset (cloneable-shift f 0))` (unchanged) |

### Side-by-side example

```lisp
;; call/cc -- one-shot early exit. k is called at most once.
(reset
  (+ 1 (call/cc (fn [k]
                  (when bad? (k 0))   ; abort path
                  41))))              ; happy path
;; bad? = true  => 1   (k abort returns 0 at the call/cc site, +1 = 1)
;; bad? = false => 42  (no abort; body returns 41, +1 = 42)

;; call/cc* -- multi-shot. k is cloned each time, used many times.
(let [k (call/cc* (fn [k] k))]      ; capture and exfiltrate k
  (tur_cloneable_cont_resume (tur_cloneable_cont_clone k) 100)
  (tur_cloneable_cont_resume k 200))
;; Prints 100, then 200 -- two independent resumes from the same point.
```

### Mental model

**`call/cc` is for control flow; `call/cc*` is for search.** If you would
reach for a `try`/`throw` in Java or a `goto exit` in C, you want `call/cc`.
If you would reach for Prolog backtracking or a Lisp `amb` operator, you
want `call/cc*`.

---

## Appendix B -- OQ1 rationale: why "require explicit `reset`"

### The underlying problem

Scheme's `(call/cc f)` is *undelimited*: `k` captures "the rest of the
program from this point." Implementing that in Turmeric requires a
whole-program CPS transform (still post-1.0; tracked separately by
[`multishot-continuations-plan.md`](archive/multishot-continuations-plan.md)).
The runtime we have today (`tur_cont` via fiber-context save/restore) only
supports *delimited* capture: `k` captures up to some **boundary** the
runtime knows about. That boundary is `(reset ...)`.

The design question is not *what* the semantics are -- they are delimited --
it is **how the user finds out where the boundary is**.

### The three options considered

**(A) Implicit function-body boundary.** Every `defn` containing `call/cc`
is silently wrapped in `(reset ...)`. *Pro:* `(call/cc f)` "just works."
*Con:* silently lies about being Scheme. A user who writes the textbook
Scheme `call/cc` pattern gets *different* runtime behavior, and the
difference is invisible in the source -- the bug surfaces only when the
captured `k` is used somewhere the user expected the function to have
already returned.

**(B) Require explicit `reset`, error if missing.** Chosen. The user
writes `(reset ... (call/cc f) ...)`; without the `reset`, elaboration
fails with `TUR-E0705` pointing at fixes (write `reset`, or use `call/cc*`).

**(C) Rename to `call/dc` ("delimited").** Don't take the `call/cc` name
at all, leaving it free for a future undelimited form. *Pro:* maximally
honest. *Con:* breaks every user expectation; nobody knows what `call/dc`
is.

### Why (B) wins on the merits

1. **Symmetry with everything else.** Turmeric already requires explicit
   boundaries for every other capture form:
   - `(shift k ...)` requires `(reset ...)`
   - `(shift0 k ...)` requires `(reset ...)`
   - `(serial-shift k ...)` requires `(serial-reset ...)`
     (`TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET`)
   - `(cloneable-shift k ...)` requires `(cloneable-reset ...)`

   Making `call/cc` the *one* shifty form that magically synthesizes its
   own boundary would be a special case to remember. `TUR-E0705` slots
   into the same `E07xx` band as the other shift-outside-reset errors.

2. **Boundaries are observable.** Two textually identical `(call/cc f)`
   calls inside two different functions would, under (A), behave
   differently if one is wrapped in a `reset` and the other isn't. That is
   invisible at the call site. Under (B), the boundary is right there in
   the source.

3. **Refactor-safe.** Moving `(call/cc ...)` from one function into
   another (inlining, extraction) does not silently change what `k`
   captures, because there is no implicit thing to drag along. The `reset`
   either comes with it or doesn't, and either way it's visible.

4. **Forward-compatible with undelimited `call/cc`.** If/when the CPS pass
   lands, *real* undelimited `call/cc` can ship without changing the
   meaning of any program written today. With (A), the implicit-boundary
   semantics would already have squatted on the name -- and any existing
   code relying on the implicit-boundary timing would silently break when
   the boundary disappears.

5. **The error is good teaching.** `TUR-E0705` says "wrap the call site in
   `(reset ...)` or use `call/cc*`." A user who hits this learns the
   boundary model the first time. With (A), they may never learn it --
   until something breaks at runtime in a way they cannot trace.

### Subtle rule: only `EX_RESET` counts

The `enclosing_reset_depth` counter from CC2.1 tracks **only `EX_RESET`**,
not `EX_CLONEABLE_RESET` or `EX_SERIAL_RESET`. These three boundary forms
use different runtime continuation types (`tur_cont` vs.
`tur_cloneable_cont` vs. serializable frames) and are not interchangeable.
A user inside `(cloneable-reset ...)` who writes `(call/cc f)` still gets
`TUR-E0705`; the diagnostic suggests `call/cc*` (which matches the
cloneable family they are already in). This rule is added to CC2's
acceptance criteria and to CC6.1's documentation.

### Shape of the user-visible diagnostic (`TUR-E0705`)

```
error[TUR-E0705]: 'call/cc' requires an enclosing 'reset' boundary
 --> prog.tur:7:3
  |
7 |   (call/cc (fn [k] (k 42)))
  |   ^^^^^^^^
  |
  = help: wrap the call site in '(reset ...)' to establish the boundary
          where the captured continuation returns
  = help: or use 'call/cc*' for a multi-shot continuation that
          establishes its own boundary
```

Symmetric wording for `escape`. When the user is already inside a
`cloneable-reset` or `serial-reset`, the second help line is reworded to
recommend the matching cloneable / serial primitive instead.

### Prior-art table (for reference)

| Language | Boundary model | Notes |
|---|---|---|
| Scheme (R5RS) | Undelimited | Whole-program capture; no boundary at all |
| SML/NJ (`SMLofNJ.Cont`) | Undelimited | Same as Scheme |
| Racket | Both, explicit | `call/cc` undelimited; `call-with-composable-continuation` delimited, requires `prompt` |
| Haskell (`Control.Monad.Cont`) | Explicit prompt | `callCC` delimited by enclosing `runContT`; types force the boundary |
| OCaml 5 (effect handlers) | Explicit prompt | `try ... with effect` is the boundary; no implicit one |
| Common Lisp | Implicit, lexical | `block` / `return-from` -- `shift0`-style abort, statically scoped |
| Ruby (`Kernel#callcc`) | Undelimited (deprecated) | Replaced in practice by `Fiber` |

The Turmeric choice (B) aligns with Racket's *composable* form, Haskell's
`ContT`, and OCaml's effect handlers -- the three production languages
whose delimited-continuation experience is most relevant here.
