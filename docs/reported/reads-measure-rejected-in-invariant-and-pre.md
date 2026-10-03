# A `#reads` loop invariant is runtime-checked but never proved, even inside `frozen`

**Severity: low-medium (completeness: a correct invariant keeps both runtime
checks).** **Narrowed 2026-10-03.** The rejection this report was filed for is
fixed: the CT1 purity gate (`TUR-E0375`) now accepts a `#reads` measure in every
contract position -- `:invariant`, `:pre`, `:post`, a return refinement, and the
parameter refinement that already accepted it -- and each keeps its runtime
check. What is left is the other half of the original acceptance line: inside a
`frozen` region a `#reads` invariant should be **proved** and its checks elided,
and it is not. Filed 2026-10-02, found looking for an illustrating use case for
`loop-invariants`.

## What was fixed (2026-10-03)

`rt_diag_impure_pred` (`src/compiler/elab_fns.c`) is the sole `TUR-E0375`
emitter, and it now asks `rt_pred_observably_impure` instead of the plain purity
walk. That classifier treats a call the predicate makes **directly** to a
`#reads` measure as UNKNOWN rather than walking the measure's inline-C body:

- CT1's concern is a check whose evaluation changes state, so compiling it in or
  out is observable. Reading borrowed state is neutral however often it runs.
- `#reads` is the trusted claim that the body only reads, the same trust a
  parameter refinement already extended to it (`TUR-W0383` reports the
  violations the compiler can see).
- Only depth 0 is exempt. The measure's ARGUMENTS are still walked, an impure
  term beside the measure is still `E0375`, and so is an unannotated wrapper
  around the measure. No callee verdict is computed or memoized under the
  exemption.

`elab_loop_invariant_pred`'s hard bail uses the same classifier, so the
invariant now survives as a runtime-checked contract instead of being dropped.
The guard in `rt_inject_param_checks` still skips a `#reads` parameter's entry
check, but its comment now says why: a choice of enforcement point (the
crossing), not a purity verdict.

Pinned by:

- `tests/fixtures/refine-reads-measure-contract-positions`: all five positions
  accepted, run and passing.
- `tests/fixtures/refine-reads-measure-invariant-runtime-check`: the kept entry
  check fires at runtime.
- `tests/fixtures/errors/refine-impure-accessor-contract-positions`: the
  negative control. Raw `vec-len` is `E0375` in all four positions, and so are
  a `(tick)` beside a `#reads` measure and an unannotated wrapper around one.

## What is still open -- the proof inside `frozen`

```turmeric
(defn vlen [^borrow v : (Vec int)] #reads v : int (vec-len v))

(defn sum-all [^borrow v : (Vec int)] : int
  (let [__f (& v)]                       ;; what `(frozen v ...)` lowers to
    (let [^mut acc 0
          ^mut i   0]
      (while (< i (vlen v)) :invariant (and (>= i 0) (<= i (vlen v)))
        (set! acc (+ acc (vec-get v i)))
        (set! i (+ i 1)))
      acc)))
```

```
warning [TUR-W0372]: the invariant of the while loop in 'sum-all' is not
analysed statically: 'v' is borrowed, or assigned inside a lambda or handler
clause, in 'sum-all', so it can change with no `set!` in the loop
```

Without the region, the conjunct `(<= i (vlen v))` is `unknown` on entry and on
preservation, because each `(vlen v)` is a fresh symbol. That is correct
outside `frozen`. Three things stand between this and a proof inside it, and
each is a separate change:

1. **The region itself declines the loop.** `li_lambda_targets`
   (`elab_fns.c`) marks every name borrowed anywhere in the function as
   *volatile*, including by `(& v)`, and `li_analyze_one` declines any loop
   whose invariant mentions a volatile name. The channel it guards is real for
   `&mut` (`errors/loop-invariant-unseen-writes`). A shared `(& x)` cannot be
   written through: `(set! @r ...)` on one is a type error, measured. So the
   remaining exposure is an inline-C callee handed `(& x)`. Narrowing the
   volatile set to `&mut` borrows (or to `^mut` names) is a soundness decision
   for the feature's conservative posture, so it is not made here.
2. **Loop obligations carry no frozen set.** Call-site crossings call
   `refine_obligation_set_frozen` so the encoder can grant a `#reads` measure
   congruence (`enc_reads_args_frozen`, `refine_collect.c`). `li_obligation`
   builds its obligations without one, so even with (1) fixed, each
   `(vlen v)` would stay a fresh symbol.
3. **Elision is vetoed for any non-pure head.** `li_analyze_one` returns before
   eliding when `rt_pred_is_impure(e, s->inv)`, and a `#reads` measure is not
   `info.pure`. A proof would stand for the post-loop fact but leave both checks
   in. For a crossing, the trusted-annotation argument is in
   [stateful-refinements-guide](../guides/stateful-refinements-guide.md#why-a-trusted-annotation-is-sound-here)
   (the callee's own check is the backstop). An invariant has no such backstop,
   so eliding one on the strength of `#reads` needs its own argument.

## Orthogonal, unchanged

`vec-len` is declared `#fx{}` -- pure on the effect row -- while the refinement
purity walk calls its inline-C body impure. A reader who sees the `#fx{}` row
reasonably concludes it is pure. The real fix is the one
[trusted-refinement-claims-plan](../archive/trusted-refinement-claims-plan.md)'s
R4 is blocked on: make the measure layer hold its state in Turmeric-visible
structs instead of a malloc'd block behind inline C, so the stdlib accessors
stop being inline C. A purity allowlist of stdlib accessors would be the
stopgap.

## Relationship to graduating `loop-invariants`

The bounded-index walk can now be **written** against a real container and is
runtime-checked. Before this fix, `loop-invariants-plan` and
`ecs-refinement-typed-apis-plan` could not express it at all. Proving it is the
remaining gap. The companion completeness report,
[loop-invariant-declines-more-than-soundness-requires](../archive/loop-invariant-declines-more-than-soundness-requires.md),
was resolved the same day.
