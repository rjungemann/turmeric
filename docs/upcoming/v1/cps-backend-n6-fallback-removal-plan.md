---
title: "CPS backend N6 (gate item 7): remove the whole-function fallback -- readiness, measurement, and phased plan"
status: planning
description: Graduation gate item 7 makes the CPS backend the SOLE lowering for colored (may-capture) functions -- no CT_UNSUPPORTED whole-function bail-out, no direct-vs-CPS dual path. This document measures the current fallback surface (which colored functions still bail out and on which forms), shows that removing the fallback today would turn 400+ sites into hard errors, and lays out the phased path: (1) delegate every control-op-free, colored-call-free subexpression to the direct emitter (the big coverage lever, reusing CT_LETRAW), then (2) handle the remaining control-carrying forms (multi-case handle, shift0, cloneable/serial reset, async, capturing/multi-shot continuations), then (3) delete the fallback and turn any residual into a hard error with a form-named diagnostic.
---

## Why this exists

The `cps-backend` graduation gate
([cps-backend-non-scalar-values-plan.md](cps-backend-non-scalar-values-plan.md#graduation-gate----what-must-hold-before-cps-backend-goes-always-on),
item 7 / N6) requires deleting the whole-function fallback: a colored function is
emitted through the CPS backend, full stop, with no second lowering to fall back
to. Any form that still cannot be emitted then becomes a hard compiler error, not
a silent reroute to the fiber/direct path.

Every other gate item is closed (Tier A/B/C, narrow-int, struct/ADT locals,
owning pointers re-scoped to item 4). N6 is the terminal item -- and by far the
largest, because it is gated on **100% form coverage for colored functions**, not
on any single value representation.

## Measurement -- the current fallback surface

Instrumenting the `CT_UNSUPPORTED` catch-all with the offending `Expr` kind and
running `--dump-cps emit-c` over every fixture that uses a control op
(`perform`/`handle`/`shift`/`reset`/`defeffect`) tallies the reasons a colored
function's body leaves the CPS subset:

| Reason | Count | Meaning |
| --- | ---: | --- |
| `form not in CPS2 subset` | 402 | an `Expr` kind the translator's switch does not handle |
| `indirect call` | 6 | `EX_CALL` with no resolvable `fn_binding` (a call through a value) |
| `handle: only single-case handlers` | 3 | a multi-case `handle` |

Breaking down the 402 "form not in subset" by `Expr` kind:

| Kind | Count | Note |
| --- | ---: | --- |
| `EX_FN_TO_FAT` | 384 | a bare fn wrapped as a fat closure (higher-order stdlib: `hamt/map`, `hamt/filter`, ... -- colored because they take an fn arg that *might* perform) |
| `EX_PANIC` | 4 | `(panic ...)` |
| `EX_CLOSURE` | 4 | a closure literal |
| `EX_SHIFT0` | 2 | `shift0` (the other delimiter) |
| `EX_REF` | 2 | `(ref x)` owning-reference constructor |
| `EX_INLINE_C` | 2 | an inline-C block |
| `EX_SET` | 2 | `(set! ...)` |
| `EX_ASYNC` | 1 | `(async ...)` |
| `EX_DEFER` | 1 | `(defer ...)` |

The translator (`src/passes/cps_ir.c`, `cps_bind` / `cps_tail`) currently handles
only 11 `Expr` kinds: `EX_BUILTIN`, `EX_CALL`, `EX_DO`, `EX_HANDLE`, `EX_IF`,
`EX_LET`, `EX_PERFORM`, `EX_RESET`, `EX_RESUME`, `EX_RETURN`, `EX_SHIFT` -- plus
atoms and the `CT_LETRAW`-delegated leaf ops (`make-struct` / `.field` /
`default-of` / `rc/*`, and cps->direct calls with atomic args). Everything else
falls back.

**Conclusion: N6 is not a safe deletion today.** Removing the fallback now would
turn all 411 sites into hard errors -- the suite would fail to compile broadly
(the `EX_FN_TO_FAT` count alone spans the higher-order stdlib that any effect
program pulls in). The fallback must stay until coverage is complete.

## The key architectural lever -- delegate control-op-free subexpressions

Most of the surface is not control flow at all: `EX_FN_TO_FAT`, `EX_CLOSURE`,
`EX_PANIC`, `EX_REF`, `EX_INLINE_C`, `EX_SET`, `EX_DEFER` -- none of these thread
a continuation. They fall back only because the CPS translator's switch does not
enumerate them.

The N3 work already established the pattern for this: `CT_LETRAW` delegates a
subexpression's emission to the direct emitter (`emit_value`), binding its result
as a local. It is used today for `make-struct` / `.field` / `rc` ops. **Generalize
it**: any subexpression that

1. contains no syntactic control op (`perform`/`handle`/`resume`/`shift`/`shift0`
   /`reset`/`cloneable-*`/`serial-*`/`discontinue`), **and**
2. contains no call to a *colored* function (a `cps->direct` call to an
   *uncolored* callee is fine -- it already is delegated; a colored callee must
   thread the continuation, so it cannot be delegated wholesale), **and**
3. is not itself a nested fn/closure body (a call-graph boundary -- colored on its
   own merits),

can be emitted by the direct emitter via `CT_LETRAW`, exactly like `make-struct`.
This collapses `EX_FN_TO_FAT`, `EX_CLOSURE`, `EX_PANIC`, `EX_REF`, `EX_SET`,
`EX_DEFER`, `EX_INLINE_C`, `EX_MATCH`, `EX_FOR`, `EX_WHILE`, literals, casts, etc.
into delegated locals in one move -- the single biggest coverage step.

### Safety conditions (why the predicate is conservative)

- **Condition 2 (no colored call) is load-bearing.** If a delegated subexpression
  called a colored function that performs effect `E` handled by an *enclosing*
  handler in the current function, the direct emitter would call it through its
  entry wrapper (a fresh DK root), so the `perform` would not reach the enclosing
  DK handler -- the machine-split crash. The existing `cps->direct` delegation
  already respects this (`callee_colored` gates `CT_TAILCALL` vs delegation); the
  general predicate must too.
- **The scan must be sound, so its default is "not delegatable."** There is no
  generic `Expr` child-walker in the tree, so the predicate enumerates the safe
  composite forms and recurses into their children; any un-enumerated kind is
  conservatively treated as not-delegatable (it falls back, as today). This can
  only *under*-delegate (safe), never over-delegate (unsound).
- **Captures inside lifted bodies still gate.** A delegated subexpression that
  references enclosing locals is fine in the main function body (they are C locals
  in scope) but is rejected by the existing `has_capture` zero-capture cut inside
  a lifted reset/shift/handler helper -- so it falls back there, as today.

This step is **fallback-safe and blast-radius-contained**: the CPS backend only
activates under `--enable=cps-backend`, so growing coverage only changes the
`cps-backend-*` fixtures, never the default suite. It is worth landing on its own
(more colored functions CPS-emit) well before N6.

## Remaining control-carrying gaps (after delegation)

These genuinely thread a continuation and need real CPS handling before the
fallback can be deleted:

- **Multi-case `handle`** (`handle: only single-case handlers`) -- the translator
  admits one case; a `dk_handler` keyed by effect tag needs to dispatch N cases.
- **`shift0`** and the **cloneable / serial** reset/shift variants -- other
  delimiters with distinct prompt semantics.
- **`async`** -- scheduler-backed continuation capture.
- **Capturing / multi-shot continuations** -- the C3/C4 subset is zero-capture
  abortive + single resume; a handler case or shift body that captures other
  locals, or a genuinely multi-shot resume, must lift with a real env (not the
  zero-capture `k`-as-env shortcut) and copy correctly.
- **Indirect calls** (`indirect call`) -- a call through an fn value whose
  coloring is unknown; needs a conservative "treat as colored" thread or a
  runtime bridge.
- **Non-tail cps->cps with a capturing join** -- the heap-join landed for the
  zero-capture direct-body form; a capturing join needs an env-carrying frame
  (also the `docs/reported/cps-backend-fallback-intermediary-splits-effect-chain.md`
  residual).

## Phased plan

- **N6.1 -- general delegation** (recommended first; fallback-safe, contained).
  Add `is_delegatable_general(b, e)` (conditions above) and route it through
  `CT_LETRAW` in `cps_bind`/`cps_tail` ahead of the `CT_UNSUPPORTED` default.
  Round-trip fixtures: a colored function whose body wraps an effect in a
  `match` / `while` / closure, `direct == cps`. Re-measure the surface.

  **Started -- capture-free fn-values landed.** `is_delegatable_value` (bare fn /
  `EX_FN_TO_FAT` / `EX_POLY_WRAP`, and an `EX_CLOSURE` with `n_captures == 0`) now
  delegates through `CT_LETRAW` in both `cps_bind` and `cps_tail`. A capture-free
  fn value references no enclosing local, so it is sound to delegate anywhere
  (including a lifted zero-capture body); a capturing closure still falls back
  (its free vars need the `has_capture` cut -- that is N6.3). Fixture
  `cps-backend-closure-local`: a colored `f` builds a `Box` holding
  `(fn [n] (+ n 1))` in its perform continuation; the closure delegates and `f`
  CPS-emits (`direct == cps == 10`) where it previously fell back. Full suite:
  2009 passed, 0 failed. Remaining N6.1: the general control-op-free /
  colored-call-free predicate (`match` / `while` / `panic` / `ref` / `set!` /
  `defer` / inline-C) with the lifted-body subset-predicate widening.

  **Entanglement to plan for (found while scoping):** a delegatable form in a
  colored function most often sits in a *continuation* -- the body of a
  `perform` / `shift` / `reset` / handler case -- not the straight-line main
  body. Those positions are gated by `perform_body_ok` / `shift_body_ok` /
  `reset_body_ok`, which admit only straight-line `letval`/`letprim`/`letcall`/
  `letraw`/`if` today. So N6.1 is not just "route delegatable forms through
  `CT_LETRAW`": the lifted-body subset predicates must also admit the delegated
  `CT_LETRAW` in those positions (they already admit `CT_LETRAW` for the leaf
  ops, so this is widening the operand set, not a new node), **and** the
  `has_capture` zero-capture cut must be satisfied or lifted -- a delegated
  `while`/`match` that references other locals inside a lifted body captures
  them, which the current cut rejects (that is the N6.3 capturing-continuation
  work). Net: N6.1 delivers real coverage in the *main body* immediately, and
  full continuation-position coverage lands together with N6.3.
- **N6.2 -- multi-case handle + shift0** -- the highest-frequency real control
  gaps after delegation.
- **N6.3 -- capturing / multi-shot continuations** -- lift with a real env;
  removes the zero-capture cut's fallbacks.
- **N6.4 -- the long tail** -- cloneable/serial reset, async, indirect calls, per
  the re-measured surface.
- **N6.5 -- delete the fallback.** Remove the `CT_UNSUPPORTED` whole-function
  bail-out and the direct-vs-CPS dual path from `emit_cps_ir.c` / the classifier.
  Any residual form becomes a hard error; give it a **form-named diagnostic**
  (the measurement patch that annotates `CT_UNSUPPORTED` with the `Expr` kind is
  the seed for this). Re-run the full suite and the sign-off probe with the
  fallback gone.

Only after N6.5 does `cps-backend` satisfy gate item 7. Until then the fallback
stays and coverage grows monotonically under the flag.

## Depends on / reuses

- `CT_LETRAW` delegation + `emit_value` (N3) -- the mechanism N6.1 generalizes.
- `callee_colored` (`cps_ir.c`) -- the colored-call gate for the delegation
  predicate.
- `cps_directly_uses_control` (`cps.c`) -- the control-op seed enumeration to
  mirror (but with a sound, not-delegatable default).
- Parent: [cps-backend-non-scalar-values-plan.md](cps-backend-non-scalar-values-plan.md)
  (the graduation gate), [cps-ir-to-c-backend-plan.md](cps-ir-to-c-backend-plan.md)
  (the C1-C6 backend).

## Out of scope

- Uncolored functions -- they are never CPS-emitted; N6 is only about colored
  functions.
- Owning-field aggregate / carrier crossings -- gate item 4, not N6.
