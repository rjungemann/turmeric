# CPS backend N6 fallback follow-ups -- both tasks blocked as scoped

**Severity: medium (planning finding -- neither task in the archived N6
follow-ups plan is executable as written; the general fallback is load-bearing
far beyond the plan's premise).**

Investigating the two open items in the N6 follow-ups plan (now archived at
`docs/archive/cps-backend-n6-fallback-removal-followups-plan.md`) -- Task 1:
resuming SHIFT bodies; Task 2 / N6.5: delete the general whole-function fallback
-- turned up two facts that block both tasks in their current form. Each item
was split into its own follow-on plan:
`docs/upcoming/v1/cps-backend-n6-resuming-shift-plan.md` and
`docs/upcoming/v1/cps-backend-n6-fallback-deletion-plan.md`. Landed alongside
this report: form-named `CT_UNSUPPORTED` diagnostics and a `TUR_TRACE_EVICT`
readiness measurement (the plan's own "seed"), which produced the numbers below.

## Task 1 -- resuming SHIFT bodies -- not expressible in the language

A "resuming shift" (receiver invokes the captured continuation) **cannot be
written or type-checked** in Turmeric today, so there is no source program to
lower and no `direct == cps` fixture is constructible.

Turmeric's `shift` is **abortive on every path**:

- Interpreter: `eval_abortive_shift` (`src/turi/eval.c:1561`) evaluates the
  body to `v`, applies the receiver to `v` (`turi_call(env, fn, &v, 1)`), and
  aborts to the nearest reset with the result. The receiver receives the **body
  value**, never the continuation.
- Direct emitter: `emit_effects_shift` (`src/compiler/emit_effects.c:1429`)
  emits exactly `k_fn(body_val)` -- its own comment says "Without CPS, we can't
  capture the continuation ... Full implementation requires CPS transformation."
- CT-IR backend: `cps_shift_body_kf` (`src/passes/cps_ir.c`) synthesizes the
  same `receiver(body)` application; `dk_shift` passes `subk` to the shift-body
  helper but the helper ignores it (`(void)subk;`,
  `emit_cps_ir.c` `emit_lifted` LH_SHIFT_BODY).

The type rule enforces the abortive shape: the receiver's parameter type must
equal the **body's** type, not a continuation type. Probe:

```turmeric
(defn gen [] : int
  (reset (shift (fn [k : (-> int int)] (k 10)) 5)))
```

```
error [TUR-E0001]: shift: body type mismatch -- the continuation receiver
expects (fn [int] : int), but the body has type int
```

To make the receiver reference the continuation you would have to change the
type rule, `eval_abortive_shift`, and `emit_effects_shift` together -- i.e.
implement non-abortive first-class shift across the interpreter and the direct
emitter. That is explicitly a **separate plan**, not a CT-IR extension:
`docs/archive/compiled-first-class-continuations-plan.md` (lines 83-104) --
"Turmeric's `shift` lowering is **abortive** ... There is no resumable-capture
codegen path yet ... it is a plan of its own -- not a mechanical extension of
the existing abortive DK path." The resumable path that *does* exist is
`perform`/`resume`/`handle` (CT_PERFORM / CT_RESUME = `dk_invoke`), already
landed in N6.2/N6.3.

**Directions:** either (a) rescope Task 1 out of this plan and point it at the
first-class-continuations plan, or (b) if a non-abortive `shift` is genuinely
wanted, spec it as its own change (type rule + interp + direct emitter + CT-IR)
so a `direct == cps` fixture can exist.

## Task 2 -- N6.5 delete the general fallback -- fallback is load-bearing

The plan's premise is that N6.1-N6.4 achieved near-total coverage and only
resuming-shift + fallback-deletion remain. Measurement contradicts this. Running
the new `TUR_TRACE_EVICT` trace across the whole fixture corpus
(`tests/fixtures/*`), the **distinct** colored functions that still fall back to
the direct emitter:

| Category | Distinct fns | Meaning |
| --- | --- | --- |
| `SIG-REJECT` | 401 | poly-fat / aggregate / borrow signature -- direct emitter owns the ABI |
| `SIG-EXPORT` | 26 | exported C symbol -- fixed linkage |
| `SIG-MAIN` | 1 | program entry ABI |
| `BODY-STRUCT-OR-TAINT` | 86 | body outside the subset (slot/atom/heap-join) or effect-tainted |
| `BODY-UNSUPPORTED` | 15 | a source **form** not in the CPS2 subset |

`SIG-*` is not "the fallback" -- it is permanent routing for signatures the CPS
backend cannot spell (`fn_sig_ok`, `emit_cps_ir.c:1455`). But the **BODY-***
rows (~100 distinct functions, including all of `hamt/*`, higher-order
generics, parser combinators) *are* the general whole-function fallback, and
**none** of them are resuming shifts or the delimited-control carve-out
(cloneable/serial/async). The residual `BODY-UNSUPPORTED` forms are ordinary:

- `EX_WHILE` + `EX_SET` mutation loops (e.g. `sum-loop`, `closure-env-no-leak`)
- capturing-closure creation `EX_CLOSURE` / `EX_FN` in tail/bind position
  (e.g. `closure-capture`, `make-adder`)
- a scattering of `EX_MATCH`, generator, and effect-combinator forms.

Deleting the general fallback and hard-erroring these (as N6.5 prescribes) would
turn ~100 currently-working colored functions into build errors -- the corpus
would go heavily red. N6.5 is therefore **premature**, not one-shift away.

**Directions:** N6.5 is gated on closing the `BODY-*` coverage gap first --
each residual form must either be lowered into the CT-IR subset (a capturing
closure in bind/tail position, a `while`/`set!` region, `match`, ...) or be
added to the named carve-out with justification. The `TUR_TRACE_EVICT` trace is
the readiness gate: N6.5 can proceed only once `BODY-UNSUPPORTED` and
`BODY-STRUCT-OR-TAINT` are empty modulo the delimited-control carve-out. This is
a substantial body of expressiveness work, not a deletion.

## What landed with this report

- `src/passes/cps_ir.c`: `cps_form_name` / `unsupported_form` -- residual
  `CT_UNSUPPORTED` nodes now name the offending source form (the plan's
  "annotate CT_UNSUPPORTED with the Expr kind" seed) instead of the generic
  "form not in CPS2 subset".
- `src/compiler/emit_cps_ir.c`: `first_unsupported` + the `TUR_TRACE_EVICT`
  categorized eviction trace (off by default) -- the N6.5 readiness gate.

Both are internal (no emitted-C change, no fixture snapshot churn).
