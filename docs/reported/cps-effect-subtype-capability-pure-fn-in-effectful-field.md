# effect-subtype-capability: a PURE fn in an effectful-typed capability field evicts

**Severity:** medium (blocks `effect-subtype-capability` from the CPS/DK backend under
`--enable=cps-tramp-resume`; correctness is fine -- it runs on the fiber). Represents a
class: effect *subtyping* (a pure value `#fx{}` satisfies an effectful field `#fx{Write}`).

## The fixture

```turmeric
(defeffect Write [s :cstr] :nil)
(defstruct Action :copy [run : fn #fx{Write}])   ; capability field DECLARED effectful

(defn pure-greet [msg : cstr]                     ; but the value stored is PURE
  (println msg))                                  ; (inferred row = #fx{})

(defn main [] : int
  (let [act (make-struct Action pure-greet)]
    (handle
      (do (.run act "hello from pure") 0)         ; call the fn-value through the field
      (Write [s] k) (do (println s) (resume k nil)))))
```

`pure-greet`'s row #fx{} is a subset of the field's declared #fx{Write}, so the store
type-checks. At runtime the Write handler is NEVER triggered -- the compiler even proves
this and emits **TUR-W0033: handler clause for 'Write' is unreachable: the body does not
perform 'Write'**. So the effect INFERENCE already knows the truth: the handled body is
pure.

## Why it evicts (the CPS classifier over-approximates)

`main` evicts BODY-STRUCT-OR-TAINT. Its handled body lowers to a delegated
(`CT_LETRAW`) call `(.run act "hello")` followed by a `KK_PROMPT` deliver:

```
handle __t0 = {
  let __t1 = <owning-op ?>  ; direct-emitted     ; (.run act "hello")
  (<prompt> 0)
}
with Write(s) k -> ...
```

The handled-body `CT_LETRAW` is checked by `handle_delim_ok`'s CT_LETRAW case (added by the
"pure delegated call" fix) via `letraw_effect_free` (emit_cps_ir.c). `letraw_effect_free`
walks the delegated expr with `expr_collect_effects_acc`, and the **fn-value-call effect
crediting** block (emit_cps_ir.c ~2734) credits the call's DECLARED row:

```c
/* a call THROUGH a fn-value performs the fn-value's DECLARED effect row */
if (g_opt_cps_tramp_resume) {
    const Type *ft = ... fn_expr->type ... ;      /* `.run act` : fn #fx{Write} */
    const EffectRow *row = ft ? ft->as.fn.effect_row : NULL;   /* #fx{Write} */
    if (row && row->kind == ERK_CONCRETE)
        for (...) mark_effect(effects[i]->name, acc->plo, acc->phi);   /* marks Write */
}
```

So `letraw_effect_free` sees Write as "performed" (`lo != 0`) and returns false -> the
`CT_LETRAW` falls to `term_core_ok`, which rejects the following `KK_PROMPT` deliver ->
`main` evicts. (An indirect field-call may ALSO set `callee_overflow`/`ov` -- confirm with
`TUR_DBG`-style tracing; both must be handled.)

The mismatch: the classifier uses the STATIC field type `fn #fx{Write}`; the effect
inference (W0033) computed that the actual call performs #fx{} (pure). This is the SAME
declared-vs-inferred gap the row-variable fix closed (`callee_effect_free` inferred-row
fallback) -- but here there is NO `FnDef` to consult, because the callee is a struct-field
value, not a named binding. That is what makes it harder.

## Fix approaches (for the agent)

**A. Precise per-call effect (preferred if the info exists).** Find where the effect
inference records that `(.run act ...)` performs #fx{} (the same determination that raises
W0033). If the CALL node (or the handle body) carries an inferred effect row, have the
crediting block (emit_cps_ir.c ~2734) prefer it over the fn-value's declared type. Start by
locating the W0033 raise site (grep `TUR_W0033_UNREACHABLE_HANDLER`; likely in the
effect-row inference pass) and see what per-node effect it consults -- that is the precise
row you want. This is the most surgical fix and mirrors the row-variable inferred-row win.

**B. Dead-handle elimination (most general).** When effect inference proves a `handle`'s
body performs NONE of the handled effects (ALL clauses W0033-unreachable), the `handle` is
semantically identity -- replace `(handle pure-body cases...)` with `pure-body` in a
source-level pass BEFORE CPS lowering. Then `main` has no handle and CPS-emits trivially.
Sound because the removed prompt catches nothing the body performs (a different effect
handled by an OUTER handler is unaffected). This fixes the whole class and is independent of
the CPS backend, but it is a new optimization pass -- scope it tightly (only when the body's
inferred effect is disjoint from the handled set) and watch drop/linearity of the handler
closure.

**C. Value-flow (do NOT start here).** Track that `act.run == pure-greet` and use its
inferred row. General value-flow is out of scope; A or B subsumes the corpus case.

Recommendation: try **A** first (surgical, matches an existing pattern). Fall back to **B**
if the per-call inferred effect is not reachable at emit_cps_ir.c.

## Guardrails / how to verify

- The EFFECTFUL case must still evict: a field `fn #fx{Write}` holding a genuinely
  Write-performing value must stay on the fiber (its inferred row is non-empty). Add a
  companion fixture with an effectful field value and confirm it still evicts + runs correct.
- `handle-effectful-fn-param-same-fn` and `cps-backend-fn-param-effectful` must be unchanged.
- Repro to iterate on: the fixture above. Expected DK state: `main` emits `main__cps`, zero
  `eff=1` evictions (`TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume ...`), output
  `hello from pure`.
- Gate: default suite (`bash tests/run.sh`, 12-min timeout) must stay green; if the change
  is NOT flag-gated (a shipping-backend classifier change, like the row-var / letraw fixes),
  it must be flag-off byte-identical -- verify no snapshot churn. Then run a full flag-on
  build sweep (all BUILDFAILs must be the known flag-independent `-lturi`/turi-runner
  false-positives -- confirm any new one fails flag-off too).
- Add a flag-on regression fixture (`cps-tramp-resume-...`) that locks the DK path.

## Context

This is one of the compound BODY-STRUCT-OR-TAINT roots from
docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md. The related, already-landed pattern:
`callee_effect_free`'s inferred-row fallback (commit "recognize a runtime-pure row-variable
fn as effect-free"), which is Approach A applied to a NAMED callee. This ticket is the
struct-field-value analogue.
