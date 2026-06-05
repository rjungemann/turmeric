---
title: Closure-Returning Instance-Method Codegen Fix
category: Planning
description: Fix the `definstance` codegen bug where a method whose return type is a closure has its dict field type resolved to `void *` instead of `int64_t` (the fat-closure handle type), causing C compile failures. This is the load-bearing language gap that prevents reintroducing the Arrow typeclass hierarchy and blocks several other stdlib instances (`mw-cors` currying, `Functor [(Either E)]`).
---

# Closure-Returning Instance-Method Codegen Fix -- Plan

## Why

When a `definstance` declares a method whose return type is a closure -- e.g.
`arr :: (b -> c) -> (->) b c` where the return value is itself a function --
the typeclass-dict lowering pass resolves that dict field's C type to
`void *` instead of `int64_t` (the fat-closure handle type the rest of the
language uses). The generated C then fails to compile because the call site
expects to invoke a fat closure via `TUR_APPLY1`/`TUR_APPLY2`, but the dict
field is a raw pointer with no callable shape.

This is the single most expensive language gap in stdlib right now:

- `stdlib/arrow.tur:91-100` -- the whole Arrow typeclass hierarchy was
  scaffolded but never instantiated specifically because of this bug.
  [[stdlib-arrow-scaleback-plan]] deleted the disabled declarations rather
  than carry them, and [[stdlib-arrow-typeclass-reintroduction-plan]] is
  blocked on this fix.
- `stdlib/httpd.tur:1688` -- `(mw-cors opts)` currying: the `__pap` wrapper
  stores `CorsOpts` (a struct) into an `int64_t` env field, also failing.
  This is a sibling bug; the fix here may or may not cover the struct case
  (see Task A5 / cross-link below).
- `Functor [(Either E)]` (planned in [[sum-types-either-plan]]) -- `fmap`
  returns a closure; the same bug will bite if not pre-fixed.

This plan is the standalone version of **Section A2** of
[[stdlib-type-erasure-cleanup-plan]]. That section stays as a coordinating
index; this plan supersedes it for execution.

## Scope

In scope:

- Reproduce the bug in a minimal fixture.
- Locate the dict-field type-resolution site in the typeclass lowering pass.
- Fix the propagation so closure-returning dict fields carry `int64_t`.
- Audit sibling dict-field types (struct returns, opaque returns) for the
  same bug class; either fix them in this plan or file a focused follow-up.
- Cover with fixtures and snapshot-stable `expected.c`.

Out of scope:

- The struct-in-`__pap`-env case (`mw-cors`) -- tracked in section A5 of
  [[stdlib-type-erasure-cleanup-plan]]. **Decision point in Task 4**: if
  the root cause is the same, fold A5 in; if not, leave it separate.
- Operator-name C identifier mangling (`>>>` / `<<<` collision) -- a
  different bug, tracked in section A3 of the type-erasure plan.
- Anything Arrow-specific. This is a pure compiler fix.

## Tasks

### T1. Minimal repro

1. Write a tiny `defclass` with a single method returning a closure:
   `(defclass HasArr [a] (arr-of [f] : (a int int)))` where `a` is the
   arrow constructor and the method returns a value of `a`-shape.
2. Write a `definstance HasArr [(->)]` whose method body returns a
   closure capturing a free variable.
3. Build it with `./build/tur emit-c`. Confirm the emitted C either fails
   to compile or that the dict-field type is `void *`.
4. Land the repro as `tests/fixtures/instance-closure-return-bug/` with a
   `requires.compiler-bug` marker (a new skip marker that PASS-skips the
   fixture until the bug is fixed). Add the marker to `tests/run.sh` if
   not present.

Deliverable: a failing repro that becomes a passing fixture once T4 lands.

### T2. Locate the resolution site

1. Grep the codegen pass for dict-field type resolution -- start with the
   typeclass elaborator and walk to C emission.
2. Identify the exact function/lines where a method's return type is
   inspected to decide the dict-field's C type.
3. Trace why a closure return type erases to `void *`. Likely candidates:
   a missing `case` arm for the closure type kind; a wrong default when the
   return is a type-variable; an early `void *` fallback in a type-map
   helper.
4. Record findings in a short note appended to this plan.

### T3. Audit sibling cases

Before fixing, enumerate every dict-field type that the resolver could see:

- closure (the bug),
- primitive `:int`/`:bool`/`:cstr`,
- struct value,
- opaque newtype,
- another typeclass dict (higher-rank),
- type variable bound by an outer instance head.

For each, write a one-liner: does the resolver handle it correctly today?
This is the audit T3 of A2 -- do it before the fix so the fix can address
the class, not just the symptom.

### T4. Fix the type propagation

1. Patch the resolution site so closure-returning dict fields are emitted
   as `int64_t` (the fat-closure handle type).
2. Confirm the audit cases from T3 either still work or are explicitly
   marked as separate bugs.
3. **Decision point**: if the struct-return case (`mw-cors` / A5) shares
   the same root, fold the fix here. Otherwise, write a one-paragraph
   note explaining why it is genuinely different and leave A5 standing.
4. Regenerate the affected snapshots in `tests/fixtures/`. Review the
   diff for unintended changes (snapshots in unrelated areas should be
   untouched).

### T5. Remove the skip marker from T1's fixture

The bug fixture now passes; delete `requires.compiler-bug` from its dir.
Confirm `bash tests/run.sh` includes it in the live suite.

### T6. Add coverage fixtures

Each is a `tests/fixtures/` directory with `input.tur` + `expected.c`:

1. `instance-closure-return-simple/` -- method returns a closure with no
   captures.
2. `instance-closure-return-capture-int/` -- method returns a closure
   capturing an `int`.
3. `instance-closure-return-capture-struct/` -- method returns a closure
   capturing a struct field.
4. `instance-closure-return-nested/` -- method returns a closure that
   itself returns a closure (curried).
5. `instance-closure-return-via-other-method/` -- method A returns a
   closure built by calling method B on the same dict.

All fixtures ASCII-only.

### T7. Re-enable downstream consumers (smoke test, not part of this plan's deliverable)

Verify (do **not** land here, but confirm the door is open):

1. A throwaway `definstance Arrow [(->)]` for `arr`/`>>>` compiles and runs.
   Do not commit -- [[stdlib-arrow-typeclass-reintroduction-plan]] owns
   landing the real instances.
2. A throwaway `Functor [(Either E)]` instance whose `fmap` returns a
   closure compiles. Same -- do not commit; [[sum-types-either-plan]]
   owns landing the real instance.

If either smoke test fails, the fix is incomplete -- iterate on T4.

### T8. Docs

1. Short note in `docs/guides/typeclass-internals-guide.md` (create if
   absent) describing the dict-field type-resolution rule and the
   closure-handle convention.
2. Annotate [[stdlib-arrow-scaleback-plan]] with a "the codegen
   prerequisite has landed" status line.
3. Annotate Section A2 of [[stdlib-type-erasure-cleanup-plan]] as
   "superseded by [[closure-returning-instance-method-codegen-plan]]".
4. Move `docs/archive/arrow-thin-call-segfaults-capturing-closures.md`
   stays where it is, but add a forward-pointer to this plan.

## Validation

- `bash tests/run.sh` -- zero `FAIL` lines.
- The T1 repro (now in T5 form) passes without `requires.compiler-bug`.
- All T6 fixtures pass with snapshot-stable `expected.c`.
- T7 smoke tests succeed in a throwaway worktree (not committed).
- Manual: search the emitted C for `void *` in dict struct definitions
  produced by a closure-returning method -- expect zero hits.

## Acceptance checklist

- [x] T1 repro fixture exists and (initially) demonstrates the bug. *(Fixed
      in-session, so the repros land directly as passing fixtures in T6 rather
      than as skip-marked failing fixtures; see Implementation Notes.)*
- [x] T2 root-cause note appended to this plan.
- [x] T3 sibling-case audit completed and recorded.
- [x] T4 fix landed; dict-field type for closure returns is `int64_t`.
- [x] T4 decision recorded on whether A5 (struct-in-`__pap`-env) shares
      the fix. *(It does not -- separate root cause; A5 left standing.)*
- [x] T5: no skip marker needed (fixed in-session); fixtures in live suite.
- [x] All T6 fixtures pass with snapshot-stable `expected.c`.
- [x] T7 smoke test passes (throwaway `Arrow`-style `arr` instance compiles
      and runs -> 42; not committed).
- [x] Docs updated; A2 marked superseded.

## Implementation Notes (landed 2026-06-03)

### T2 -- Root cause

The bug is **not** a `void *` fallback in a single type-map helper but a
**dropped return Type** in the instance-method elaborator, which then erases to
an unknown-void carrier at every codegen site.

`elab_definstance` (`src/compiler/elab_typeclasses.c`) builds each method's
function type with `type_fn(param_kinds, n, return_type.kind)`. `type_fn` stores
only the return **kind** (`TY_FN`) and drops the full return Type. The regular
`defn` path attaches the full `TY_FN` as `result_full_type` ("Issue 1b" in
`elab_fns.c`); the instance path never did. With no `result_full_type`, codegen
falls back to `emit_type_from_kind(TY_FN)` -- a zeroed `TY_FN` shell whose result
kind is `TY_UNKNOWN` -- and `type_c_name` lowers it to `/*unknown*/ void`
(`src/compiler/types.c` `TY_FN` arm: `boxed` -> `void *`, else recurse into the
result kind, which here is `TY_UNKNOWN`).

Net effect: the dict field, the `__inst_*` impl signature, **and** the call-site
let-binding all declared a `void`-returning function and silently dropped the
returned closure handle -- a miscompile, surfacing as `-Wint-conversion` /
"return with a value in function returning void" and, for curried returns, a
hard C error.

The fix touches three sites:

1. `elab_typeclasses.c` (`elab_definstance`): when `return_type.kind == TY_FN`,
   attach `fn_type.as.fn.result_full_type = &return_type`, mirroring the regular
   `defn` path. Fixes the single-level dict field + impl signature.
2. `types.c` (`type_c_name`, `TY_FN` arm): a bare `TY_FN` whose result kind is
   itself `TY_FN` (curried) or `TY_UNKNOWN` is a single closure handle -> carry
   it as `int64_t` instead of recursing to an unknown-void carrier.
3. `emit_expr.c` (`EX_LET`): a let-bound *curried* closure (the result of
   `(.method ...)` whose return is a function-returning-function) is declared as
   the `int64_t` handle, not a malformed thin function pointer.

### T3 -- Sibling dict-field-type audit

For each kind a method return / dict field can take, does the resolver handle it
correctly **after** the fix?

| Return shape | Carrier | Status |
| --- | --- | --- |
| primitive `:int`/`:bool`/`:cstr` | `int64_t`/`bool`/`const char *` | OK (always was) |
| dispatch tyvar `: a` -> instance type | concrete (struct/int64_t) | OK (`emit_carrier_return_override` / RT substitution) |
| by-value struct | struct name | OK |
| opaque newtype / ADT / `TY_APP` | `int64_t` | OK |
| **closure `(fn [..] :T)`** (single level) | `int64_t` handle | **FIXED** (site 1) |
| **curried `(fn [..] (fn [..] :T))`** | `int64_t` handle | **FIXED** (sites 2 + 3) |
| closure return whose result is a struct/ADT | result-type name | unchanged; not exercised by a known instance -- left as-is |

### T4 -- Decision on A5 (`mw-cors` struct-in-`__pap`-env)

**Separate root cause; A5 left standing.** This fix lives entirely in the
`definstance` dict-field / method-return type resolution and in `type_c_name`'s
`TY_FN` carrier rule. The `mw-cors` failure is in the **partial-application**
(`__pap`) wrapper, which packs a `CorsOpts` *struct* into an `int64_t` env slot
-- a different code path with no `TY_FN` return involved. Nothing in this fix
touches `__pap`. A5 remains tracked in
[stdlib-type-erasure-cleanup-plan](stdlib-type-erasure-cleanup-plan.md).

### Coverage (T6) and two orthogonal bugs found en route

Five fixtures land under `tests/fixtures/`:
`instance-closure-return-simple`, `-capture-int`, `-capture-struct`, `-nested`
(curried), and `-compose-methods` (two closure-returning methods composed at the
call site). Each is end-to-end (`expected.stdout`) **and** a codegen snapshot
(`expected.c`); every closure-returning dict field is `int64_t`.

The 5th fixture composes at the call site rather than via `(.other self ...)`
because **intra-instance method dispatch is unsupported** -- filed as
`docs/reported/intra-instance-method-dispatch-unsupported.md`. The "nested"
fixture's inner closure references only its immediate parent's parameter because
**nested closures do not transitively capture grandparent variables** -- filed
as `docs/reported/nested-closure-transitive-capture.md`. Both reproduce in plain
`defn` (no typeclasses) and are independent of the carrier-type fix.

### Pre-existing stale snapshot fixed

`tests/fixtures/float-fat-closure/expected.c` was already drifting on this
branch (a captured `^fat` closure env field showed `double` instead of the
correct `int64_t` handle) before any change here. Its `expected.c` was
regenerated and the runtime output verified (`3 / 4.5 / 6`).

## Non-goals

- Reintroducing the Arrow typeclass hierarchy. That is
  [[stdlib-arrow-typeclass-reintroduction-plan]].
- Implementing `Either`. That is [[sum-types-either-plan]].
- Fixing the struct-in-`__pap`-env case in general. Only folded in if the
  root cause is shared (T4 decision point).
- Operator-name mangling. Section A3 of
  [[stdlib-type-erasure-cleanup-plan]].

## Cross-references

- **Lambda retype now has reachable input**:
  [[bare-fat-lambda-param-plan]] (Implemented 2026-06-04) makes a bare
  `(fn [^fat g ...] (g ...))` binder elaborate, so a user-written lambda can
  now exercise the lambda-side retype step this plan introduced.
- **Supersedes** section A2 of [[stdlib-type-erasure-cleanup-plan]].
- **Unblocks** [[stdlib-arrow-typeclass-reintroduction-plan]] (hard
  prerequisite).
- **Unblocks** the `Functor [(Either E)]` instance step of
  [[sum-types-either-plan]] (T7 there).
- **Coordinates with** section A5 of [[stdlib-type-erasure-cleanup-plan]]
  (struct-in-`__pap`-env): possibly the same fix; decision in T4.
- **Coordinates with** [[language-readiness-for-typed-signal-plan]] (this
  bug is one of the readiness items there).
- **Historical context**:
  `docs/archive/arrow-thin-call-segfaults-capturing-closures.md` -- the
  original encounter with the bug.
- **Triggered by** the analysis in
  `docs/reported/signal-spice-broken-build.md` and
  [[stdlib-arrow-scaleback-plan]].
