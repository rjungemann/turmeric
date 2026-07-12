---
title: CPS backend N6 fallback removal -- re-measurement findings (plan is stale)
category: Planning
status: finding -- both tasks in the followups plan rest on premises that no
  longer hold; the plan needs rescoping before any code change lands
description: An attempt to execute cps-backend-n6-fallback-removal-followups-plan.md
  surfaced that (1) Task 1 "resuming SHIFT bodies" describes a construct Turmeric
  does not have -- the base shift is abortive-only at every layer -- and (2) Task 2
  "delete the general fallback" would hard-error ~130+ fixtures of ordinary
  algebraic-effect / tier-C / sized / session programs, not just the delimited-
  control carve-out the plan assumes is the sole residual.
---

# CPS backend N6 fallback removal -- re-measurement findings

This note records what an attempt to execute
[cps-backend-n6-fallback-removal-followups-plan.md](cps-backend-n6-fallback-removal-followups-plan.md)
found. **No code change was made** -- the investigation concluded that both
tasks, as written, rest on premises that do not hold against the current
(graduated, always-on) cps-backend, and executing either literally would either
be a no-op (Task 1) or break the build (Task 2). The plan should be rescoped
before work resumes.

## Method

- Debug build of `tur` at `8185ef9`.
- Instrumented the fallback edge in `emit_cps_ir_try_fn`
  (`src/compiler/emit_cps_ir.c`, the `return false` where a colored function is
  not in `S`) to print, per fallen-back colored function: whether its body
  syntactically reaches a control op (`ctl`), whether `fn_sig_ok` passes
  (`sig`), and the first `CT_UNSUPPORTED` why in its CTerm (or `core-reject`
  when `term_core_ok` fails without an unsupported node).
- Ran `tur emit-c` over every `tests/fixtures/*/input.tur` and aggregated.
- Instrumentation was reverted; the working tree is clean apart from this note.

## Finding 1 -- Task 1 ("resuming SHIFT bodies") has no implementable input

Turmeric's base `shift` / `shift0` is **abortive-only at every layer**. In
`(shift (fn [k] ...) body)` the receiver's parameter `k` is bound to the *value
of `body`*, never to a continuation:

- Interpreter: `eval_abortive_shift` (`src/turi/eval.c`) computes
  `r = f(eval(body))` then aborts to the nearest plain reset. The work-stack
  path (`EX_SHIFT`/`EX_SHIFT0` -> `abortive_shift_resume`) does the same.
- Direct emitter: `emit_effects_shift` (`src/compiler/emit_effects.c`) emits
  `k_fn(body_val)`.
- CT-IR backend: `cps_shift_body` (`src/passes/cps_ir.c`) synthesizes
  `(recv body)` delivered to the prompt.

All three agree, and the front end enforces it: a receiver that *calls* its
parameter is a compile-time type error --

```
(reset (+ 1 (shift (fn [k] (k 10)) 0)))
; error: 'k' is not a function or continuation
```

Resumable continuations exist only through `cloneable-shift` / `serial-shift`
(receiver param typed `:cont` / `:serial-cont`) and algebraic effects
(`perform` / `handle` / `resume`) -- exactly the forms the plan lists as the
carve-out. So `cps_shift_body` never receives a "receiver that invokes the
captured continuation"; the shape is unreachable. The N6.3 tail item
"resuming SHIFT bodies" is therefore **vacuous** -- there is nothing for the
proposed `subk`-threading lowering to lower. (Internally, `effect_lower.c` has a
`perform -> (shift k (handler ... k))` comment, but that lowering does not feed
the CT-IR path: the CT-IR translates `EX_PERFORM`/`EX_HANDLE`/`EX_RESUME`
directly to `CT_PERFORM`/`CT_HANDLE`/`CT_RESUME`, never through `cps_shift_body`.)

## Finding 2 -- Task 2's residual surface is not the delimited-control carve-out

The plan asserts Task 2 (delete the general whole-function fallback) is "blocked
only by Task 1 for full coverage" -- i.e. that once resuming-shift lands, the
only colored functions still on the direct path are the delimited-control
carve-out (`cloneable` / `serial` / `async` and the raw reset/shift they build
on). The measured residual is much larger and mostly unrelated to that carve-out.

Control-*reaching* colored functions that fall back (body syntactically contains
`perform`/`handle`/`resume`/`shift`/`reset`/`callcc`), deduped by (fixture, fn):

| reason | count |
| --- | --- |
| `sig=ok : core-reject` | 135 |
| `sig=no : core-reject` | 27 |
| `sig=ok : form not in CPS2 subset` | 26 |
| `sig=no : form not in CPS2 subset` | 2 |

**131 of ~1740 fixtures** carry at least one such control-reaching fallback, and
none of them are the delimited-control carve-out. Representative examples:

- Ordinary algebraic effects: `use-double` (`effect-resume-value`), `use-ask`
  (`effect-perform-handle`), `do-log`, `ask-and-log`, `three-steps`,
  `run-log`/`run-write` (`stdlib-effects-annotated`), dozens more.
- Tier-C aggregate crossings: `inner`/`outer` (`cps-backend-tierc-shift`,
  `cps-backend-tierc-return`), `run`/`use-get` (`cps-backend-tierc-effect`).
- Sized / existential / unsafe forms: `sized-buf-*`, `dense-*`,
  `make-box`/`box-set!` (`defopaque-struct-payload-through-unsafe-lift`).
- Session-typed effects: `exchange` (`session-effects`), `role-a`
  (`session-mp-effects`).

### Why the effect functions fall back: taint, not a missing form

The machinery works -- `f`/`g` in `cps-oracle-shift-under-handle` CPS-emit as
`f__cps`/`g__cps`. The dominant reason ordinary effect programs fall back is
**effect taint**: top-level program code and `main`-embedded handles
(`(println (handle (use-double 21) ...))`) are emitted by the direct/fiber
machine, which taints that effect so every performer of it is co-classified out
of `S`. This is structural: as long as top-level / `main` handler code uses the
fiber path, the effects it handles taint their performers into the fallback,
regardless of Task 1.

### And the control-*free* colored fallbacks depend on the dual path too

Beyond the control-reaching set, **thousands** of control-free colored functions
fall back on signature grounds. They are colored only conservatively -- a
function that makes an indirect call (through a fn-value param) is colored by
`cps_color_program` (`has_indirect`) even though it never does delimited
control. Examples: `map-eq-driver`, `set-eq-driver`, `httpd-call`,
`__cons-fmap`, most typeclass instance methods, and every colored generic
*template* (tyvar signatures reject in `fn_sig_ok`; only monomorphs emit). These
are 100% sound on the direct emitter (no control op -> no direct-vs-CPS
divergence possible) and rely on the dual path.

## Consequence

"Remove ... the direct-vs-CPS dual path ... any residual form becomes a hard
error" would turn all of the above into hard errors -- ~130+ fixtures from the
control-reaching set alone, plus a large fraction of real programs from the
control-free colored set. The dual path is currently load-bearing far beyond the
delimited-control carve-out.

## Suggested rescope (for whoever picks this up)

Reaching gate item 7 (CPS as the sole lowering for colored functions) is a large
multi-part effort, not the two-item tail the plan describes. Concretely, before
the general fallback can be deleted, the following must first stop falling back
(or be explicitly carved out with justification):

1. **Effect taint from top-level / `main` handler code.** Either CPS-emit the
   synthetic top-level/`main` handler bodies onto the DK machine, or accept a
   documented carve-out that effects handled outside a CPS-emitted function stay
   on the fiber path (which then also carves out their performers).
2. **Tier-C aggregate effect/shift crossings** that still `core-reject`
   (`cps-backend-tierc-*`).
3. **Sized / existential / unsafe-lift forms** carrying a control op
   (`sized-buf-*`, `dense-*`, `defopaque-struct-payload-through-unsafe-lift`).
4. **Session-typed effects** (`session-effects`, `session-mp-effects`).
5. **Control-free conservatively-colored functions** (indirect-call-colored,
   generic templates, typeclass instances): either narrow the coloring so a
   control-free function is not colored, or keep a sound direct path for them --
   deleting the dual path outright is not safe for this class.

Only after 1-5 is "delete the general fallback, hard-error the rest" a change
that keeps the suite green. Task 1 (resuming-shift) should be struck from the
plan as vacuous.
