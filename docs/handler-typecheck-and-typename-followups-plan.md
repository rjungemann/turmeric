---
title: Handler Arg-Checking + type_name Ownership -- Follow-up Plan
category: Type System
description: Two follow-ups surfaced by the first-class-handlers work -- making call-site handler-argument checking row-precise (not kind-only), and fixing the mixed-ownership leak in type_name's diagnostic call sites
---

# Handler Arg-Checking + `type_name` Ownership -- Follow-up Plan

> **Status:** Complete (PH0-PH3). Captures the two caveats called out at the end of
> [first-class-handlers-plan.md](first-class-handlers-plan.md) FH4.1. Both are
> pre-existing and **independent of** the first-class-handler *feature* (which
> is complete and correct); they surfaced because handler types now appear in
> call-site argument positions and diagnostics.
>
> **Snapshot:** `0.14.6`.
>
> **Last updated:** 2026-05-30

---

## Problem statement

Two distinct, pre-existing weaknesses became visible once handler *values* could
be passed as function arguments:

1. **Handler arguments are checked by `TypeKind` only.** The call-site argument
   check in `elab_call.c` starts from
   `bool arg_ok = (args[i]->type.kind == expected_arg_kind);`
   (`src/compiler/elab_call.c:1601`) and then adds *type-precise* refinements
   only for `TY_FN` (LT2 linearity, `fn_type_subtype`), `TY_UNION`, and
   `TY_INTERSECTION` -- each of which reaches into
   `fn_type.as.fn.arg_full_types[idx]` for the full declared type. **There is no
   `TY_HANDLER` refinement**, so any handler argument satisfies any handler
   parameter regardless of the handled effect set or value/result kinds. The
   row-precise `type_eq` / `type_is_subtype` added in FH4.1 (`src/compiler/types.c`)
   is therefore correct but *never invoked* at this site. Symptom: passing a
   single-effect `(handler (Ask ...) ...)` where `(handler #{Ask Tell} int int)`
   is required type-checks silently and then aborts at runtime on the unhandled
   effect.

   A second, related gap: the diagnostic at
   `src/compiler/elab_call.c:1830-1841` only consults `arg_full_types` for
   `TY_UNION` / `TY_INTERSECTION` / `TY_APP`, so a handler mismatch prints the
   generic `handler<?, ?, ?>` (built from `type_from_kind(TY_HANDLER)`) instead
   of the declared `handler<Ask | Tell, int, int>`. This implies the full
   handler parameter type may also not be reliably present in `arg_full_types`
   for handler params -- that needs verifying (step PH1.1).

2. **`type_name` has mixed ownership and leaks at diagnostic sites.**
   `type_name` (`src/compiler/types.c:966`) returns a **static string literal**
   for primitive/atomic kinds (`"int"`, `"cstr"`, `"handler"`-less atoms, ...)
   but a freshly **`tur_strdup`-ed heap string** for composite kinds (`TY_FN`,
   `TY_HANDLER`, `TY_UNION`, `TY_STRUCT` applications, ...). Its ~156 call sites
   treat the result as a borrowed `const char *` and never free it, because
   freeing would crash on the static-string cases. Result: every diagnostic that
   names a **composite** type leaks (e.g. the arg-mismatch formatter at
   `elab_call.c:1837/1840/1846` leaked 81 bytes in 2 allocations when the
   expected/actual type was a handler). No committed fixture triggers it today
   (error paths over composite types are rare in fixtures), but it is a real,
   process-wide leak that LeakSanitizer flags whenever such an error fires, and
   it will multiply as handler/union/intersection types appear in more
   diagnostics.

Neither is a soundness bug in the first-class-handler runtime; (1) is a missing
static check and (2) is a memory-hygiene defect.

---

## Phase ordering at a glance

| Phase | Deliverable | Notes |
|---|---|---|
| PH0 | Decision + scope | Pick the `type_name` ownership strategy; confirm desired handler subtyping variance |
| PH1 | Row-precise handler argument checking | `TY_HANDLER` refinement at the call site + correct diagnostic type name |
| PH2 | `type_name` ownership fix | Make ownership uniform; eliminate the diagnostic leaks without crashing on static strings |
| PH3 | Fixtures + leak gate | Expect-error fixtures for handler mismatches; opt a handler-error fixture into ASan/LSan so the leak can't regress |

---

## Phase PH0 -- Decisions

- **PH0.1** `type_name` ownership strategy. Choose one:
  - **(a) Always-owned (recommended).** `type_name` always returns a
    heap-allocated string (strdup the atomic cases too); every call site frees
    it. Clean and uniform, but touches ~156 call sites.
  - **(b) Caller-provided buffer.** Add `type_name_buf(Buf*, Type)` (already
    exists -- `src/compiler/types.c`) as the preferred API and migrate
    diagnostic sites to it; keep `type_name` for legacy/static use. Less
    churn at error sites, no allocation to free.
  - **(c) Arena-owned.** `type_name` allocates from a compiler arena that is
    freed in bulk; no per-site frees. Lowest churn, but grows the arena for the
    lifetime of compilation.

  *Recommendation:* **(b)** for the diagnostic sites that currently leak (small,
  surgical, no global churn), optionally followed by **(c)** if a broader
  cleanup is wanted. *Done when:* the strategy is recorded here.

  **Decision (PH0.1): strategy (b) -- caller-provided buffer.** The existing
  `type_name_buf(Buf*, Type)` is promoted to a public API (declared in
  `types.h`, no longer `static`) and used at the leaking diagnostic sites in
  `elab_call.c`. Each site builds the type name into a local `Buf`, passes
  `buf.data` to `diag_emit*`, and frees the `Buf` afterward -- no `tur_strdup`
  result is leaked. `type_name` keeps its mixed static/heap contract for the
  ~150 borrow-and-never-free call sites (a full strategy-(a) conversion is
  explicitly scoped out as optional follow-up to avoid a large mechanical
  diff). The known-leaking handler arg-mismatch path is the required scope.
- **PH0.2** Handler subtyping variance. FH4.1 implemented handler comparison as
  effect-set **equality** with `TY_UNKNOWN` value/result kinds as wildcards.
  Decide whether argument checking should use strict `type_eq` (equality) or
  `type_is_subtype` (to later allow a handler over a *superset* of effects where
  a subset is required, with the documented value/result variance). *Done when:*
  the intended relation for PH1 is chosen (default: `type_is_subtype`, matching
  how `TY_FN` args already use `fn_type_subtype`).

  **Decision (PH0.2): use `type_is_subtype`.** PH1's handler argument check
  calls `type_is_subtype(args[i]->type, *expected_full)`, matching how `TY_FN`
  args use `fn_type_subtype`. The relation stays exactly the FH4.1 one
  (effect-set equality + `TY_UNKNOWN` value/result wildcards) as implemented in
  `type_is_subtype`'s `TY_HANDLER` branch; effect-superset subtyping is *not*
  introduced here (see Risks).

---

## Phase PH1 -- Row-precise handler argument checking

- **PH1.1** Verify the full handler parameter type reaches the call site.
  Confirm whether `fn_type.as.fn.arg_full_types[idx]` is populated for
  `TY_HANDLER` params (the FH4.1 probe printed `handler<?, ?, ?>`, suggesting it
  may be `NULL`). If it is missing, thread the declared handler type into
  `arg_full_types` where function signature types are built
  (`src/compiler/elab_fns.c`). *Done when:* a handler param's full type
  (`handled_row`, value/result kinds) is retrievable at `elab_call.c` arg-check
  time.
- **PH1.2** Add a `TY_HANDLER` refinement to the argument check. Next to the
  existing `TY_FN` (LT2) block (`elab_call.c:~1799`), when
  `expected_arg_kind == TY_HANDLER && args[i]->type.kind == TY_HANDLER` and the
  full expected type is available, set
  `arg_ok = type_is_subtype(args[i]->type, *expected_full)` (per PH0.2). *Done
  when:* passing a handler whose handled set differs from the parameter's is a
  `TUR-E0001` error, and a matching/compatible handler still passes.
- **PH1.3** Fix the mismatch diagnostic to print the declared handler type.
  Extend the `arg_full_types` lookup at `elab_call.c:1831` to include
  `TY_HANDLER`, so the message reads `expected handler<Ask | Tell, int, int>,
  got handler<Ask, int, int>` instead of `handler<?, ?, ?>`. *Done when:* the
  error names both real handler types. (Mind the leak in PH2 -- this adds
  another composite `type_name` call.)

---

## Phase PH2 -- `type_name` ownership fix

- **PH2.1** Implement the PH0.1 strategy at the leaking diagnostic sites.
  Minimum scope: the argument-mismatch formatter
  (`elab_call.c:1837`, `:1840`, `:1846`) and any sibling formatters that pass a
  `type_name(...)` result for a potentially-composite type. Under strategy (b),
  rewrite these to build the names with `type_name_buf` into a local `Buf` (or a
  small `format_type_into(Buf*, Type)` helper) and pass `buf.data` to
  `diag_emit*`, freeing the `Buf` after. *Done when:* the handler arg-mismatch
  error path is LeakSanitizer-clean
  (`ASAN_OPTIONS=detect_leaks=1 ./build/tur -Xeffect-types check <case>`).
- **PH2.2** Audit the other `type_name` call sites that emit diagnostics over
  possibly-composite types (`grep -n type_name( src/compiler/*.c`; ~156 sites,
  but only those passing the result into a long-lived/leaking context matter).
  Convert or free as appropriate per PH0.1. *Done when:* no diagnostic path
  leaks a `type_name` result; the broad refactor (if strategy (a)/(c)) is either
  completed or explicitly scoped out with a tracked remainder.

  **Status: done for `elab_call.c` arg-check diagnostics.** Converted the three
  composite-leaking sites in the call-site argument loop to `type_print`-into-`Buf`:
  the arg-mismatch formatter (PH2.1), the IT3 intersection-member mismatch, and
  the LT2 linear-function mismatch. Also fixed `type_name_buf`'s `TY_HANDLER`
  case to format the full handled-effect *row* (it previously printed only the
  single `effect_name`, so `type_print` would have regressed the PH1.3 message
  to `handler<?, ...>`); it now matches `type_name`. **Tracked remainder
  (scoped out):** the broad strategy-(a) conversion of the remaining ~150
  borrow-only `type_name` sites is deferred; most name primitive/atomic types
  on hot or non-leaking paths. The ownership rule is documented at the
  `type_name` declaration so new diagnostic sites reach for `type_print`.
- **PH2.3** (If strategy (a)) make `type_name` return owned strings uniformly
  and update all sites; add a brief contract comment on `type_name` stating the
  ownership rule so new call sites do the right thing. *Done when:* the ownership
  rule is documented at the declaration and there is no mixed static/heap return.

  **Status: contract comment added; uniform-ownership conversion N/A (strategy
  (b)).** Since PH0.1 chose strategy (b), `type_name` keeps its mixed
  static/heap return by design. An OWNERSHIP CONTRACT comment now sits above the
  `type_name` definition in `src/compiler/types.c`, directing new
  composite-type diagnostic sites to `type_print(Buf*, Type)` instead.

---

## Phase PH3 -- Fixtures and leak gate

- **PH3.1** Expect-error fixtures under `tests/fixtures/errors/`:
  - handler argument with the wrong handled effect set
    (`(handler (Ask ...))` passed where `(handler #{Ask Tell} ...)` is required)
    -> `TUR-E0001`, message names both handler types (PH1.2 + PH1.3);
  - handler argument with mismatched value/result kind (where not wildcarded)
    -> `TUR-E0001`.
  *Done when:* both fail at compile time with the expected message substrings.

  **Status: done.** Added `tests/fixtures/errors/handler-arg-wrong-effect-set/`
  (single-effect handler passed where `handler<Ask | Tell, int, int>` is
  required) and `tests/fixtures/errors/handler-arg-wrong-value-kind/` (matching
  `#{Ask}` row but result kind `int` vs the required, non-wildcarded `cstr`).
  Each carries a `flags` file (`-Xeffect-types`) and an `expected.diag` asserting
  `TUR-E0001` plus both `expected handler<...>` / `got handler<...>` substrings.
  Both run green under `tests/run.sh` (suite: 1097 passed, 0 failed).
- **PH3.2** Leak regression gate. Because `tests/run.sh` compiles the *generated
  program* without ASan and only the `tur` binary is sanitized, add a check that
  actually exercises the sanitized compiler on an error path -- e.g. a ctest
  target (or `run.sh` hook) that runs `tur check` on a handler-mismatch fixture
  under `ASAN_OPTIONS=detect_leaks=1` and asserts a clean exit. *Done when:* a
  reintroduced `type_name` leak on a composite-type diagnostic fails CI.

  **Status: done.** Added `tests/run-leak-gate.sh` and registered it as the
  `tur_leak_gate` ctest target. It runs the sanitized `tur -Xeffect-types check`
  on both PH3.1 handler-mismatch fixtures with
  `ASAN_OPTIONS=detect_leaks=1`, asserts the `TUR-E0001` diagnostic fires (so we
  are genuinely on the error path) and that LeakSanitizer reports no leak.
  Verified the gate *catches* regressions: temporarily restoring the leaking
  `type_name(args[i]->type)` call made it fail with
  `SUMMARY: AddressSanitizer: 87 byte(s) leaked in 2 allocation(s)`; reverting
  the leak makes it pass again.

---

## Risks and notes

- **Call-site churn (PH2).** Strategy (a) touches ~156 sites; prefer the
  surgical (b) for the known-leaking diagnostic paths and treat a full audit as
  optional follow-up to avoid a large, mechanical, error-prone diff.
- **Subtyping scope creep (PH1).** Keep PH1 to the FH4.1 relation
  (set-equality + `TY_UNKNOWN` wildcards) unless PH0.2 explicitly opts into
  effect-superset subtyping; the latter interacts with the deferred answer-type
  variance and should not be smuggled in here.
- **No runtime behavior change.** Both phases are front-end/compiler-hygiene
  only; no codegen or fixture-snapshot (`expected.c`) changes are expected, so
  the suite should stay green without regeneration.

## See also

- [first-class-handlers-plan.md](first-class-handlers-plan.md) (FH4.1 -- where
  these caveats were recorded)
- [first-class-handlers-semantics.md](first-class-handlers-semantics.md)
