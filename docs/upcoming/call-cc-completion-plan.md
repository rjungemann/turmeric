# Finish `call/cc` -- Completion Plan (CC0--CC6)

> **Status:** Blocked on [`cps-transform-plan.md`](cps-transform-plan.md)
> (CPS0--CPS6). Scheduled to be worked on *after* the CPS substrate lands.
> Closes audit item 1 of the control-flow audit (`call/cc`/`escape` are
> degenerate sugar, currently gated behind `-Xcallcc` with diagnostics
> `TUR-E0700`/`TUR-E0701` -- see CF4 of
> [`control-flow-completeness-plan.md`](archive/history/control-flow-completeness-plan.md)).
>
> **Reframed 2026-06-01.** An earlier draft of this plan shipped a *delimited*
> `call/cc`/`escape` today (lowering to `(reset (shift k (f k)))`) and required
> an explicit enclosing `(reset ...)` (old OQ1). That framing was dropped: a
> delimited `call/cc` adds almost nothing, because Turmeric **already** ships
> delimited continuations -- `shift`/`reset`/`shift0` (one-shot) and `call/cc*`
> (cloneable, multi-shot). The only thing a `call/cc` named primitive buys over
> those is the *undelimited* Scheme semantics: capture "the rest of the
> program" with an implicit program-wide prompt and no explicit `reset`. That
> requires unbounded continuation capture, which is precisely the deferred CPS
> pass. So this plan now waits for CPS and targets the undelimited semantics;
> **old OQ1 is reversed** (see below).
>
> **Last updated:** 2026-06-01
>
> **Open questions resolved (2026-06-01):**
> - **OQ1 (reversed)** -- *Do not* require an explicit enclosing `(reset ...)`.
>   With CPS (CPS5.3) an implicit **program-wide** prompt is installed around
>   `main`; bare `call/cc`/`escape` capture up to it, matching Scheme. The
>   earlier "require explicit `reset`" decision was an artifact of the bounded
>   16-frame delimited runtime, where an implicit program-wide prompt could not
>   be captured soundly. Once continuations are heap-reified that constraint is
>   gone. The old CC2 "missing-boundary diagnostic" (`TUR-E0705`) is therefore
>   **removed**, not implemented.
> - **OQ2** -- `escape` is the abort flavor (calling `(k v)` unwinds to the
>   prompt without re-installing it), matching C `longjmp` / CL `return-from` /
>   Java `throw` / OCaml `discontinue`. On the CPS substrate this is the
>   `shift0`-style sub-continuation (no prompt re-install) up to the captured
>   prompt.
> - **OQ3** -- `k`'s default usage is `^unique` (one-shot, drop OK), matching
>   handler-clause defaults. Users may opt in to `^linear` (exactly-once);
>   multi-shot stays `call/cc*`. CC4 ships a fixture exercising `^linear k`.
>
> **Related:**
> - [`cps-transform-plan.md`](cps-transform-plan.md) -- **prerequisite**; supplies
>   unbounded capture and the implicit root prompt (CPS5.3).
> - [`control-flow-completeness-plan.md`](archive/history/control-flow-completeness-plan.md) -- CF4 (the gate this plan retires)
> - [`control-flow-completeness-audit.md`](archive/history/control-flow-completeness-audit.md) -- audit item 1
> - [`multishot-continuations-plan.md`](archive/multishot-continuations-plan.md) -- `CK_MULTISHOT` ownership (for `call/cc*`); rides the same substrate
> - [`linear-continuations-plan.md`](archive/linear-continuations-plan.md) -- `^linear` / one-shot accounting
> - [`stubs-and-workarounds.md`](archive/stubs-and-workarounds.md) §1.4 -- the original stub entry

---

## Motivation

`(call/cc f)` currently elaborates to `(let [__cc_f f] (__cc_f 0))` -- `f`
receives the literal integer `0` as its "continuation." Calling that "k" does
nothing useful. `(escape f)` is identical. Both are gated behind `-Xcallcc`,
which keeps the codebase honest but leaves a high-profile name pointing at
nothing.

The naive fix -- desugar to `(reset (shift k (f k)))` -- is cheap but
misleading: it produces a *delimited* operator that needs an explicit `reset`
and captures only up to it. We already expose that capability under its honest
names (`shift`/`reset`/`shift0`, and `call/cc*` for the multi-shot, cloneable
case). Re-spelling it as `call/cc` would invite users to expect Scheme's
undelimited semantics and quietly hand them something else.

What `call/cc` should mean -- and the only thing it adds to the existing
toolkit -- is **undelimited** capture against an **implicit program-wide
prompt**: usable anywhere, no `reset` required, capturing the rest of the
computation up to program entry. That needs continuations that can be captured
to arbitrary depth, which the current delimited runtime cannot do
(`tur_cont_alloc` caps capture at `TUR_CONT_MAX_CAPTURED_FRAMES` = 16 and
returns `NULL` past it). The [CPS transform](cps-transform-plan.md) reifies
continuations on the heap, removes that ceiling, and installs the root prompt
(CPS5.3). This plan builds `call/cc`/`escape` on that substrate.

### Goals

- Replace the `0`-as-continuation stub with a real, undelimited continuation
  captured against the implicit program-wide prompt.
- Make `call/cc` / `escape` work without `-Xcallcc` and without an explicit
  `reset`; retire `TUR-E0700` / `TUR-E0701`.
- Keep `call/cc*` (cloneable, multi-shot) unchanged -- it is already correct,
  and after CPS5.2 it simply rides the same substrate.
- Type `f` as `cont<T> -> T` and the result as the prompt's answer type.

### Non-goals

- The CPS transform itself -- that is
  [`cps-transform-plan.md`](cps-transform-plan.md), a prerequisite, not part of
  this plan.
- Multi-shot semantics for the bare `call/cc` (use `call/cc*`).
- A delimited-only stopgap `call/cc` before CPS lands. Explicitly rejected in
  the 2026-06-01 reframe: it would duplicate `shift`/`reset` under a misleading
  name. If a delimited capture is wanted today, use `shift`/`reset` or
  `call/cc*` directly.

---

## Disposition

| Form | Today | After this plan (post-CPS) | Notes |
|---|---|---|---|
| `(call/cc f)` | `TUR-E0700` ungated; `(f 0)` under `-Xcallcc` | Undelimited capture against the implicit program-wide prompt (CPS5.3); `f` receives a real `cont<T>` | One-shot `^unique` by default; `^linear` opt-in. Use `call/cc*` for multi-shot. **No explicit `reset` required** -- old `TUR-E0705` is removed (OQ1 reversed). |
| `(escape f)` | `TUR-E0701` ungated; `(f 0)` under `-Xcallcc` | Undelimited early-exit; `(k v)` unwinds to the prompt without re-installing it (abort/`shift0`-style) | One-shot abort. |
| `(call/cc* f)` | Real cloneable capture (fiber path) | Unchanged semantics; re-expressed on the CPS substrate by CPS5.2 | Already shipped (CPS-CL8). |
| `-Xcallcc` flag | Unlocks the stub | Removed (warn-and-noop for one release) | See CC5. |
| `TUR-E0700` / `TUR-E0701` | Active | Retired (kept reserved in the registry) | See CC5. |
| `TUR-E0705` (missing-boundary) | n/a | **Not introduced** | OQ1 reversed; the implicit prompt makes it unnecessary. |

---

## Semantics (the contract)

`(call/cc f)` evaluates as follows:

1. The **prompt** is the implicit program-wide prompt installed around `main`
   by CPS5.3. There is no need for an enclosing `reset`; a nearer explicit
   `reset` does *not* shorten `call/cc`'s capture (use `shift`/`call/cc*` if you
   want delimited capture).
2. Capture the current continuation -- "the rest of the computation from this
   `call/cc` site up to program exit" -- as a one-shot `cont<T>` heap value
   (CPS4.1). Unbounded depth (CPS6).
3. Invoke `f` with that continuation. `f : cont<T> -> T` where `T` is the
   answer type at the prompt.
4. If `f` returns normally, that value becomes the value of the `call/cc`
   expression (the captured `k` is dropped).
5. If `f` (or anything it calls) invokes `(k v)`, control returns to the
   `call/cc` site with `v`; whatever `f` was doing is abandoned.

`(escape f)` is the one-shot abort flavor: invoking `(k v)` unwinds to the
prompt and produces `v` there, without re-installing the prompt around the
captured continuation (the `shift0`-style behavior from CF2).

### What this is and isn't

- **Undelimited.** Capture reaches the implicit root prompt, not the nearest
  `reset`. This is the difference from `shift`/`reset`, which is exactly why
  this primitive earns its own name.
- **One-shot.** Calling `k` twice is `TUR-E0100` / `TUR-E0101` per
  linear-continuations-plan. Use `call/cc*` for multi-shot. Calling `k` after
  the program-wide prompt has returned is a runtime error
  (`TUR_RT_E_CONT_USED`).
- **Not a fresh transform.** This plan introduces no rewrite of its own; it
  consumes the CPS substrate and lowers `call/cc`/`escape` to a capture against
  the root prompt.

---

## Phase ordering at a glance

| Phase | Scope | Why this order |
|---|---|---|
| CC0 | Spec ratify + audit cross-link; confirm CPS prerequisite | Lock the undelimited semantics and the dependency before code moves |
| CC1 | Lower `(call/cc f)` to a capture against the implicit root prompt | The minimum viable real implementation on the CPS substrate |
| CC2 | *(removed)* -- no missing-boundary diagnostic | OQ1 reversed; implicit program-wide prompt means there is no boundary to require |
| CC3 | Lower `(escape f)` to the abort (no prompt re-install) flavor | Per OQ2 |
| CC4 | Typing: `cont<T>` parameter for `f`, drop placeholder result type; `^linear k` fixture | Replaces the v1 `:int` punt; locks in OQ3 |
| CC5 | Retire `-Xcallcc`, `TUR-E0700`/`TUR-E0701`; convert fixtures | User-visible cleanup |
| CC6 | Docs: guide + audit closure | Update effects guide, compiler-flags guide, control-flow audit |

---

## Phase CC0 -- Ratify undelimited semantics + confirm prerequisite

Lock the [Semantics](#semantics-the-contract) section above as the contract;
the rest of the phases assume it and assume CPS5.3 has shipped.

- **CC0.1** Confirm the [Disposition](#disposition) table with maintainers:
  undelimited `call/cc`/`escape` against the implicit program-wide prompt;
  delimited capture stays with `shift`/`reset`/`call/cc*`. *Done when:* this
  section is annotated "ratified" with a date.
- **CC0.2** Confirm the dependency: this plan does not start until
  [`cps-transform-plan.md`](cps-transform-plan.md) CPS5.3 (implicit root
  prompt) and CPS6 (unbounded capture) are green. *Done when:* the status line
  references the specific CPS phases.
- **CC0.3** Cross-link from
  [`control-flow-completeness-audit.md`](archive/history/control-flow-completeness-audit.md)
  item 1 to this plan and to the CPS plan, noting CF4's "gate-only" disposition
  is superseded by CPS + CC1--CC5. *Done when:* the audit's item-1 status line
  points here.
- **CC0.4** Inventory call sites depending on the *stub* semantics (programs
  that pass `0` to `f`). Today this is the three `-Xcallcc` fixtures
  (`continuation-callcc`, `continuation-escape`, `continuation-escape-fn`);
  rewritten in CC5.3.

## Phase CC1 -- Lower `(call/cc f)` to a root-prompt capture

Replace the placeholder desugar in `elab_call_cc`
(`src/compiler/elab_effects.c:1570`) with a real capture against the implicit
program-wide prompt provided by CPS5.3.

- **CC1.1** Change `elab_call_cc` to capture the current continuation up to the
  root prompt and pass it to `f` as a `cont<T>` (reusing the CPS5 prompt
  primitives and CF2 typing: `f`'s domain is `cont<T>`, codomain `T`). *Done
  when:* a `call/cc` whose `f` ignores `k` returns the body value; a `call/cc`
  whose `f` invokes `(k v)` returns `v` at the `call/cc` site -- **with no
  enclosing `reset`**.
- **CC1.2** Delete the v1 stub branch and the `__cc_f` / `zero` form-building
  helpers. *Done when:* `elab_call_cc` no longer references
  `intern_cstr(e->st, "__cc_f")`.
- **CC1.3** Keep the ungated `TUR-E0700` diagnostic in place until CC5 so the
  build stays green between phases.
- **CC1.4** Fixture: `tests/fixtures/callcc-real-capture/input.tur` --
  ```
  (defn main [] :int
    (+ 1 (call/cc (fn [k] (+ 100 (k 41))))))
  ;; expected: 42  -- no reset; k aborts the (+ 100 ...), returns 41 at the
  ;;               call/cc site, outer +1 makes 42
  ```
  Note the absence of an explicit `reset`: that is the observable difference
  from the old reframed-away delimited design. Snapshot under `tests/run.sh`.

## Phase CC2 -- *(removed)*

Per the OQ1 reversal there is **no** missing-boundary diagnostic. `call/cc` and
`escape` are legal anywhere; capture extends to the implicit program-wide
prompt. (Historical note: the previous draft introduced `TUR-E0705` here to
*require* an explicit `reset`. That requirement existed only because the bounded
fiber runtime could not capture an implicit program-wide prompt. CPS removes the
limitation, so the diagnostic is dropped. The `E07xx` band stays reserved.)

## Phase CC3 -- Lower `(escape f)` to the abort flavor

Per OQ2, `escape`'s `(k v)` unwinds to the prompt without re-installing it
(the `shift0`-style sub-continuation from CF2).

- **CC3.1** Replace the placeholder desugar in `elab_escape`
  (`src/compiler/elab_effects.c:1632`) with the root-prompt abort capture.
  *Done when:* `(escape (fn [k] (k 7)))` returns `7` at top level with no
  `reset`, and the generated code uses the no-reinstall (`shift0`-style) path.
- **CC3.2** Fixture: `tests/fixtures/escape-real/input.tur` --
  ```
  (defn find-first-positive [xs] :int
    (escape (fn [k]
      (for-each (fn [x] (when (> x 0) (k x))) xs)
      -1)))
  ```
  Expected: first positive element, or -1 if none -- with no explicit `reset`.
- **CC3.3** Fixture: `tests/fixtures/escape-nested-reset/input.tur` -- an
  `escape` inside an explicit `reset` that is itself inside the implicit root
  prompt, asserting `escape` still aborts to the *root* prompt (undelimited),
  distinguishing it from a delimited `shift0`. Lock in OQ2 with a snapshot.

## Phase CC4 -- Typing: `cont<T>` for `f`'s parameter

The v1 stub gave `f` the parameter type `:int` (because `0` was passed). The
real lowering passes `cont<T>` where `T` is the root prompt's answer type. The
elaborator already has `TY_CONT` (`src/compiler/types.h:92`).

- **CC4.1** Make `elab_call_cc` require that `f` has function type and unify
  `k`'s type with `cont<T>` for the inferred answer type. Emit `TUR-E0001` on
  mismatch, span on `k`. *Done when:* `(call/cc (fn [k :cont<int>] (k 42)))`
  types and `(call/cc (fn [k :str] ...))` fails clearly.
- **CC4.2** Default rule: an unannotated `k` infers `cont<T>` rather than
  `:int` (a local exception to the v1 `:int`-by-default lambda-param wart,
  scoped to the `call/cc`/`escape` receiver). *Done when:*
  `(call/cc (fn [k] (k 5)))` type-checks unannotated.
- **CC4.3** Replace the result-type placeholder in `elab_call_cc`/`elab_escape`
  with the prompt's answer type. *Done when:* a deliberately mistyped body is
  rejected by elaboration.
- **CC4.4** Per OQ3, `k` defaults to `^unique`; `^linear k` is an opt-in that
  composes through the new lowering. *Done when:*
  - `tests/fixtures/callcc-linear-k/input.tur` -- `(call/cc (fn [^linear k] (k 42)))`
    returns `42` and snapshots cleanly;
  - `tests/fixtures/errors/callcc-linear-k-dropped/input.tur` --
    `(call/cc (fn [^linear k] 99))` is rejected by `TUR-E0100` (linear value
    dropped), proving `^linear` reaches the binder.

## Phase CC5 -- Retire `-Xcallcc`, retire `TUR-E0700` / `TUR-E0701`

The gate exists only because the stub was unsound. Real semantics retire it.

- **CC5.1** In `src/main.c` and `wk_apply_flags`, change `-Xcallcc` to a
  warn-and-noop for one release. *Done when:* `tur -Xcallcc run prog.tur` warns
  but succeeds; `tur run prog.tur` with `(call/cc ...)` also succeeds.
- **CC5.2** Remove the `TUR_E0700_CALLCC_GATED` / `TUR_E0701_ESCAPE_GATED` emit
  sites in `src/compiler/elab_effects.c`; keep the codes reserved + "retired"
  in the registry for one release. *Done when:*
  `grep -n "TUR_E0700\|TUR_E0701" src/compiler/elab_effects.c` is empty.
- **CC5.3** Rewrite the three `-Xcallcc` fixtures to exercise the real
  undelimited semantics (invoke `(k v)`, observe the abort, no `reset`); drop
  their `flags` files. Regenerate `expected.c` per CLAUDE.md's snapshot rule.
- **CC5.4** Delete `tests/fixtures/errors/callcc-gated` and
  `tests/fixtures/errors/escape-gated`. *Done when:* `bash tests/run.sh` shows
  zero `FAIL` lines.

## Phase CC6 -- Docs

- **CC6.1** Extend `docs/guides/effects-system-guide.md` with a
  `Continuations (call/cc, escape)` section: undelimited semantics vs the
  delimited `shift`/`reset`; the implicit program-wide prompt; the one-shot rule
  (use `call/cc*` for multi-shot); typing (`f : cont<T> -> T`). Cross-link the
  CPS plan for the substrate.
- **CC6.2** Update `docs/guides/compiler-flags-guide.md` -- remove the
  `-Xcallcc` "experimental (unsound)" entry; replace with a "deprecated; no-op"
  notice pointing at the new section.
- **CC6.3** Update `control-flow-completeness-audit.md` item 1: mark **resolved
  by CPS0--CPS6 + CC1--CC5**, undelimited `call/cc` now shipping.
- **CC6.4** Move `docs/archive/stubs-and-workarounds.md` §1.4 to "resolved",
  citing this plan and the CPS plan.
- **CC6.5** Once green, archive this plan to
  `docs/archive/history/call-cc-completion-plan.md`.

---

## Exit criteria for finishing `call/cc`

- `bash tests/run.sh` shows zero `FAIL` lines with the new
  `callcc-real-capture` / `escape-real` fixtures *and* the rewritten
  `continuation-callcc` / `continuation-escape` / `continuation-escape-fn`
  fixtures running **without** `-Xcallcc` and **without** an explicit `reset`.
- `grep -n "TUR_E0700\|TUR_E0701" src/compiler/elab_effects.c` returns no emit
  sites; codes annotated "retired".
- `tur run prog.tur` with `(call/cc (fn [k] (k 42)))` at top level (no `reset`)
  returns `42` without any flag.
- `docs/guides/effects-system-guide.md` documents the undelimited semantics and
  the typing rule.
- `control-flow-completeness-audit.md` item 1 is marked resolved.

---

## Resolved questions (2026-06-01)

- **OQ1 resolved (reversed) -- no explicit `reset`; implicit program-wide
  prompt.** The earlier "require explicit `reset`" decision was an artifact of
  the bounded 16-frame delimited runtime. CPS reifies continuations on the heap
  and installs a root prompt (CPS5.3), so bare `call/cc`/`escape` capture
  undelimited as in Scheme. The old `TUR-E0705` diagnostic is removed, not
  implemented (CC2).
- **OQ2 resolved -- `escape` is the abort flavor.** `(k v)` unwinds to the
  prompt without re-installing it; nested-`reset` behavior tested by
  `escape-nested-reset` (CC3.3).
- **OQ3 resolved -- `^unique` default, `^linear` opt-in.** Matches handler
  clauses; locked in by `callcc-linear-k` and
  `errors/callcc-linear-k-dropped` (CC4.4).

---

## Appendix A -- `call/cc` vs. `call/cc*`

Same idea (capture the current continuation), different ownership discipline:
`call/cc` is one-shot (`^unique` by default, `^linear` opt-in); `call/cc*` is
cloneable/multi-shot. After [CPS5.2](cps-transform-plan.md) both are captures
on the same multi-prompt substrate -- `call/cc` against the implicit root
prompt, `call/cc*` against the `cloneable-reset` boundary it establishes for
itself. The distinction is ownership, not the capture mechanism.
