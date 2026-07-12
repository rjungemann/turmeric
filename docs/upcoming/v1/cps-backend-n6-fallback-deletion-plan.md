---
title: CPS backend N6.5 -- delete the general whole-function fallback (gate item 7)
category: Planning
status: open (gated on closing the BODY-* coverage gap -- measured, not one-shift away)
description: Split out of cps-backend-n6-fallback-removal-followups-plan.md (Task 2). Remove the CT_UNSUPPORTED whole-function bail-out / direct-vs-CPS dual path for colored functions, hard-erroring residuals with form-named diagnostics, keeping the delimited-control carve-out. Blocked until the BODY-* eviction surface is empty modulo the carve-out; the TUR_TRACE_EVICT trace is the readiness gate.
---

# CPS backend N6.5 -- delete the general whole-function fallback

## Where this came from

This was Task 2 of
[cps-backend-n6-fallback-removal-followups-plan.md](../../archive/cps-backend-n6-fallback-removal-followups-plan.md)
(now archived). Executing that plan established that deleting the fallback now
would hard-error ~100 currently-working colored functions -- the fallback is
load-bearing far beyond resuming shifts. Full analysis + numbers:
[docs/reported/cps-backend-n6-fallback-followups-blocked.md](../../reported/cps-backend-n6-fallback-followups-blocked.md).

## The goal (unchanged from N6.5)

Make the CPS backend the **sole** lowering for colored (may-capture) functions:
remove the `CT_UNSUPPORTED` whole-function bail-out and the direct-vs-CPS dual
path (`emit_cps_ir.c:emit_cps_ir_try_fn` returns false -> direct emission in
`emit_fns.c:emit_fn_def`). Any residual **form** becomes a hard error with a
form-named diagnostic. The one deliberate carve-out is the delimited-control
tail (cloneable / serial / async and the raw reset/shift they build on), owned
by the program-level whole-program transform -- routed there intentionally, not
ported into the CT-IR backend.

## Why it is blocked -- measured eviction surface

The `TUR_TRACE_EVICT` trace (landed with the archived plan;
`src/compiler/emit_cps_ir.c`) categorizes every colored function that falls back.
Distinct functions across `tests/fixtures/*`:

| Category | Distinct | Is it "the fallback"? |
| --- | --- | --- |
| `SIG-REJECT` | 401 | No -- poly-fat/aggregate/borrow ABI the direct emitter owns (`fn_sig_ok`) |
| `SIG-EXPORT` | 26 | No -- exported C symbol / fixed linkage |
| `SIG-MAIN` | 1 | No -- program entry ABI |
| `BODY-STRUCT-OR-TAINT` | 86 | **Yes** -- body outside the subset (slot/atom/heap-join) or effect taint |
| `BODY-UNSUPPORTED` | 15 | **Yes** -- a source form not in the CPS2 subset |

The `SIG-*` rows are **permanent routing**, not the fallback -- N6.5 keeps them
on the direct emitter (their signatures cannot be spelled by the CPS backend).
The `BODY-*` rows are the general fallback, and none are resuming shifts or the
carve-out. `BODY-UNSUPPORTED` residual forms (from the now form-named
diagnostics) are ordinary:

- `EX_WHILE` + `EX_SET` mutation loops (`sum-loop`, ...)
- capturing-closure creation `EX_CLOSURE` / `EX_FN` in bind/tail position
  (`closure-capture`, `make-adder`, ...)
- a scattering of `EX_MATCH` and effect-combinator forms.

## Plan -- close the gap, then delete

1. **Establish the readiness gate.** `BODY-UNSUPPORTED` and
   `BODY-STRUCT-OR-TAINT` (minus the delimited-control carve-out) must reach
   **zero** across the corpus:
   ```sh
   for d in tests/fixtures/*/; do
     i="$d/input.tur"; [ -f "$i" ] || i="$d/$(basename "$d").tur"; [ -f "$i" ] || continue
     TUR_TRACE_EVICT=1 ./build/tur emit-c "$i" 2>&1 >/dev/null | grep -E 'BODY-'
   done | sort -u
   ```
2. **Close `BODY-UNSUPPORTED` form by form** -- each residual either lowers into
   the CT-IR subset or joins the named carve-out with justification:
   - capturing closure in bind/tail position (`EX_CLOSURE`/`EX_FN`): lift into a
     CT_LETRAW delegation whose captured env rides the continuation (extend the
     N6.1 `safe_to_delegate` / capture-scan path to a capturing closure value).
   - `while`/`set!` region (`EX_WHILE`/`EX_SET`): lower the mutation loop, or
     delegate the whole control-op-free region via CT_LETRAW.
   - `EX_MATCH` and the remaining forms: extend `cps_tail`/`cps_bind`.
3. **Close `BODY-STRUCT-OR-TAINT`** -- these already translate to supported CT
   forms but fail `term_core_ok` (a non-slot binder / heap-join) or effect
   taint. Widen the slot/join admission where sound; the taint cases resolve as
   the performers/handlers they share effects with become CPS-emittable.
4. **Delete the general fallback.** In `emit_fn_def`, after
   `emit_cps_ir_try_fn` returns false: if the function is `cps_colored` and is
   **not** on a `SIG-*` routing and **not** a carve-out form, emit a hard error
   naming the form (reuse `cps_form_name`) instead of falling through to direct
   emission. Uncolored functions and `SIG-*` colored functions keep the direct
   path.
5. **Keep the carve-out explicit.** cloneable / serial / async (and the raw
   multi-shot reset/shift they build on) stay routed to the whole-program
   transform, gated on `emit_cps_program_uses_cloneable_dk` / `_uses_delimited`
   / `_contains_serial`. Document each carve-out route at the deletion site.
6. **Verify.** Full suite green; the stackless sign-off probe green with the
   general fallback gone; `TUR_TRACE_EVICT` shows only `SIG-*` + carve-out.

## Ordering vs. resuming shift

The archived plan ordered Task 1 (resuming shift) before Task 2 "so
resuming-shift functions CPS-emit rather than hard-error." That dependency is
**vacuous**: resuming shifts are not expressible (see
[cps-backend-n6-resuming-shift-plan.md](cps-backend-n6-resuming-shift-plan.md)),
so none exist to hard-error. N6.5 does not depend on the resuming-shift plan;
it depends on closing the `BODY-*` gap above.

## Depends on / reuses

- `TUR_TRACE_EVICT` + `first_unsupported` + form-named `CT_UNSUPPORTED`
  (`cps_form_name`/`unsupported_form`) -- the readiness gate + seed.
- `CT_LETRAW` delegation + `safe_to_delegate` (`src/passes/cps_ir.c`).
- The whole-program transform (the delimited-control carve-out owner).

## Out of scope

- Resuming (non-abortive) shift -- its own plan.
- `SIG-*` colored functions -- permanent direct-emitter routing, never deleted.
- Owning-field aggregate / carrier crossings -- gate item 4.
