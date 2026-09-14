# A Saffron `handle` does not see an effect performed one call deeper

**Severity: high.** In typed Turmeric a `handle` catches an effect performed by
a callee of the handled expression. In a `#lang saffron` file it does not: the
clause is reported *unreachable* (TUR-W0033) and the program aborts with
`tur: unhandled effect (tag N)`.

**Status 2026-09-14: HALF FIXED.** The effect-row half is fixed on both back
ends, and the INTERPRETER now runs the repro correctly. The compiled back end
still aborts, for a second and separate reason described below. Pinned as far as
it goes by `tests/fixtures/saffron-effect-through-dyn-operand`
(`requires.interp-only`, because the compiled arm is still open).

Found writing `docs/guides/introducing-saffron.md`.

## Repro -- the same program, both dialects

```turmeric
#lang saffron
(defeffect Ask [] : int)
(defn g [] (perform (Ask)))
(defn f [] (+ (g) 1))
(defn main []
  (println (handle (f) (Ask [] k) (resume k 41)))
  0)
```

Drop the `#lang` line and annotate the three signatures `: int` and the
identical program prints `42`.

## The filed hypothesis was wrong, and the control says so

The original filing guessed that the row was lost through the unannotated
RETURN -- `any` carries no effect row, so `g`'s row never reached `f`'s type.
Measured, that is not it. The control:

```turmeric
(defn straight [] (g))     ;; no dynamic operator between handle and perform
```

has always worked, with every signature just as unannotated. The `any` return
was never involved.

The actual cause is that `+` on an `any` is a **dynamic operator node**, and
the walks that needed to see through it had no arm for one.

## What was fixed

Two passes, same species of defect -- the one
`saffron-dynamic-surface-pass` H1 already found in `collect_free_vars`:

- **`src/passes/effect_check.c`.** All four structural walks
  (`collect_effects_in_expr`, `check_closures_in_expr`,
  `check_call_site_rows_in_expr`, `check_unreachable_handlers_in_expr`) lacked
  arms for `EX_DYN_OP` / `EX_DYN_CALL` / `EX_DYN_FIELD` / `EX_DYN_METHOD`, so
  each fell to its `default` and stopped. A shared child accessor
  (`saffron_dyn_child_count` / `saffron_dyn_child`) now serves all four, rather
  than four transcriptions of the same shape knowledge.
- **`src/passes/cps.c`.** `cps_collect_calls` (the call-graph edge builder the
  coloring fixpoint runs over) and `cps_body_calls_colored` had the same gap,
  so an edge whose call sat inside a dynamic operand was never recorded and the
  caller was never colored. Both now carry the Saffron nodes plus the `any`
  widen and readers -- the set `cps_directly_uses_control` beside them already
  had.

After these: no spurious TUR-W0033, `f` is correctly colored (`f__cps` is
emitted, and `main__cps` threads the handler into it), and `--interpret` gives
the right answer.

## What is still broken, and why it is a different bug

The compiled program still aborts, and the emitted C says exactly where:

```c
static int64_t f__cps(DK *__kont) {
    tur_tagged_t __ps_303 = (g());          /* <- g's DIRECT entry */
    __t0 = __tur_dyn_arith(1, __ps_303, ...);
    return dk_run(__kont, ...);
}
```

`f` is colored and threaded; the call inside it is not. `g()` is the direct
entry, which opens a FRESH DK root, so the `perform` searches a chain that has
no handler while `main`'s handler sits one frame out.

The decision is in `is_delegatable_value` (`src/passes/cps_ir.c`), whose arm for
the dynamic nodes reads `return !operand_uses_control(e);`.
`operand_uses_control` asks whether a lexical control OPERATOR hides in the
subtree -- a call to an effectful FUNCTION is not one. So the whole `(+ (g) 1)`
subtree is judged delegatable and handed to the direct emitter as one
`CT_LETRAW`.

### The obvious fix does not work -- do not repeat it

Tightening that arm to `!operand_uses_control(e) && !subtree_calls_colored(e)`
(with a `subtree_calls_colored` written against `cps_colored`) makes the node
non-delegatable, and the CPS IR then has **no native lowering for a dynamic
node at all** -- delegation is the only path it has. The function becomes
`CT_UNSUPPORTED` and evicts wholesale to the direct emitter, which is strictly
worse: the same abort, plus every other Saffron function that merely mentions
an effectful callee loses the CPS backend. Measured on this repro.

Descending the operands in `safe_to_delegate` instead does not help either:
`cps_tail` and `cps_bind` gate on `is_delegatable_value` DIRECTLY (cps_ir.c
~3426 and ~3951), so `safe_to_delegate` is not on the path that decides.

### What it would actually take

The call has to be hoisted OUT of the dynamic node before the CPS IR sees it,
so the operand is an ordinary variable and the call lands at a bind position
where it lowers to a DK-threaded `cps->cps` letcall. That is exactly the
mechanism the elaborator already has for control-bearing operands --
`elab_hoist_control_operands` / `elab_dyn_hoist_control` -- and the
`cps-coloring-walk-has-no-arm-for-union-inject` note in `cps.c` explains why it
must happen there rather than in the IR: a CPS-bound `CVar` has no `Expr`
spelling, so nothing downstream can synthesise the rewritten operand.

So: extend the elaboration hoist to also hoist an operand that CALLS a function,
not only one holding a control operator. The conservative version (hoist every
non-atomic operand of a dynamic node) needs no effect information at
elaboration time, which is the sticking point -- a callee's `inferred_effect_row`
is not available until `effect_check` runs, and forward references would make
any ordering-based answer fragile. It costs a `let` binding per operand and
will move a lot of fixture snapshots, so it wants its own change.

## Two residues of the same species, found on the way

Neither is pinned, and the fixture says why:

- `(if (> (g) 0) 1 0)` still loses the row, though the bare `(> (g) 0)` no
  longer does -- so something in the `if` lowering is a further walk with the
  same gap.
- `(.v (Box (g)))` -- an effectful call as a CONSTRUCTOR argument -- loses it
  in **plain typed Turmeric** as well, so that one is dialect-independent and
  older than any of this.
