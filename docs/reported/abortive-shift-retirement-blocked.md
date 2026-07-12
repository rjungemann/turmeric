# Retiring abortive `shift` into `k-shift` regresses cross-function control

**Severity: medium (planning finding -- the consolidation step "delete the
abortive-specific interp/emit paths" in
`docs/upcoming/v1/cps-backend-n6-resuming-shift-plan.md` is not safe as written;
the abortive lowering is load-bearing, not redundant).**

## What was attempted

Per the plan's consolidation section, the next step was to retire abortive
`shift`/`shift0` by re-expressing them as `k-shift` (a `k-shift` whose receiver
ignores `k`) and deleting the abortive-specific paths. Empirically, that
desugar regresses a large fraction of the abortive corpus.

## Why it regresses -- two mechanisms, not one

Abortive `shift` and `k-shift` (aliased onto the cloneable pipeline) capture
the delimited continuation by **different mechanisms**, and the abortive one is
strictly more expressive for the discard-continuation case:

- **Abortive** (`dk_shift` + `__dk_abort_body`; interp `eval_abortive_shift`):
  the shift aborts to the nearest enclosing `reset` **dynamically**. The context
  between shift and reset is never reified (the receiver ignores `k`), so it
  works **cross-function** and with **arbitrary context shapes**.
- **k-shift / cloneable**: the delimited context is reified **syntactically**
  within the reset body. `elab_cloneable_shift` requires the shift to sit
  lexically inside a `cloneable-reset` (`cloneable_reset_depth > 0`), and only
  the native `build_cloneable` context subset is supported.

### Probe 1 -- cross-function (TUR-E0016)

```turmeric
(defn inner [x : int] : int (shift (fn [v] v) (+ x 5)))   ; abortive: 105
(defn outer [x : int] : int (reset (inner x)))
```

Works abortively (`direct == turi == 105`). The `k-shift`/`k-reset` equivalent
fails to elaborate:

```
error [TUR-E0016]: cloneable-shift used outside of any cloneable-reset boundary
```

because the shift is in a callee and the reset is in the caller -- there is no
lexical reset to reify against.

### Probe 2 -- context shape (TUR-E0710)

Abortive shift also works under contexts the cloneable reifier rejects
(both-branch `if` shifts, nested resets, non-subset spines), because it discards
the context. The cloneable path reifies eagerly and rejects those with
`TUR-E0710`.

## Regression surface

Of the ~36 fixtures using plain `shift`/`shift0`, a scan for a defn that
contains a `shift` but no `reset` (a cross-function shift) counts **13**. On top
of those, several "local" fixtures use both-branch shifts / nested resets /
non-subset contexts that hit `TUR-E0710`. So **well over a third** of the
abortive corpus would break under a naive desugar -- e.g.
`cps-oracle-shift-capture-body`, `cps-backend-tierc-shift`,
`cps-backend-capture-shift-body`, `cps-oracle-reset-nested`,
`cps-oracle-reset-both-branch-shift`.

## The correction -- surface unification, NOT deletion

The abortive lowering is not redundant with k-shift; it is the **dynamically-
scoped, no-reification** mode that alone supports cross-function abort and
arbitrary contexts. So the plan's step "(c) delete the abortive-specific interp
(`eval_abortive_shift`) and emit (`emit_effects_shift`) paths" is misconceived --
those paths are load-bearing.

The correct end state is **surface unification with dual lowering**:

- One `shift`/`reset` keyword pair (retire the *name* `k-shift`, and abortive's
  restriction to ignore-k receivers), routed by whether the receiver **uses its
  continuation**:
  - receiver ignores `k` -> the abortive DK fast-path (`__dk_abort_body`):
    dynamic, cross-function, arbitrary context, no reification.
  - receiver resumes `k` -> the reified-context path (today's cloneable), with
    its lexical-scope + subset constraints, *or* the CT-IR DK-subk threading
    (the N6 backend work) for cross-function resume.
- Both lowerings stay; only the surface collapses to one keyword. Deletion is
  possible only for whatever becomes genuinely unreachable after routing (e.g.
  a redundant *elaboration* entry point), never the abortive runtime path.

Cross-function **resume** (not just abort) remains genuinely unsupported by the
syntactic reifier and is the province of the CT-IR DK-subk work
(`cps_shift_body` threading `subk`), which is separate.

## Recommendation

Do not land the desugar-and-delete retirement. Either:

1. Rescope the consolidation to **surface unification with dual lowering**
   (keep both runtimes, collapse the keyword, route by receiver-uses-k), which
   is a real design effort; or
2. Leave abortive `shift`/`reset` as-is (a distinct, dynamically-scoped
   primitive) and treat `k-shift` as the additive resumable surface it already
   is -- documenting the two as complementary rather than one retiring the
   other.
