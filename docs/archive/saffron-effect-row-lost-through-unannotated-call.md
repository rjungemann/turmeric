# A Saffron `handle` does not see an effect performed one call deeper

**RESOLVED 2026-09-14.** Two gaps, not one, and the filing's hypothesis was
wrong about both. It guessed the row was lost through the unannotated RETURN
(`any` carrying no effect row); the control `(defn straight [] (g))` -- the same
call chain with no dynamic node in between -- has always worked with every
signature just as unannotated, so the `any` return was never involved.

**The row.** Every structural walk in `src/passes/effect_check.c`, and the two
in `src/passes/cps.c` that build the coloring, lacked arms for the Saffron
dynamic nodes and for the `any` WIDEN. Same species as
`saffron-dynamic-surface-pass` H1 (`collect_free_vars`). The widen took two
rounds to find and is the more interesting half: an unannotated `defn` whose
body is not already an `any` gets a RETURN-position widen wrapped around the
whole body, so `(defn tc [] (do (g) 1))` was invisible to the walk while
`(defn tc [] (> (g) 0))` -- already `any`, no widen -- inferred correctly. That
divergence is what identified it; a single repro would not have.

**The call.** With the row right, the compiled back end still aborted. The CPS
IR's delegation probe (`is_delegatable_value` -> `operand_uses_control`) asks
whether a lexical control OPERATOR is in a subtree, and a call to an effectful
FUNCTION is not one -- so the whole dynamic node (or the whole widened body)
went to the direct emitter, which called the callee's DIRECT entry, opening a
fresh DK root inside the handler. Fixed by hoisting a call-bearing operand into
a `let` at elaboration (`elab_hoist_control_operands` and the return-position
widen in `elab_coerce_to_any`), which puts the call at a bind position where it
lowers to the DK-threaded cps->cps edge.

Three things that fix needed, each found by a failing measurement rather than by
reading:

- **Gated on `unit_has_user_effect`**, a new flag set by `defeffect`. Without a
  gate the hoist touches every Saffron program; with it, the fixture corpus
  showed ZERO snapshot churn. It is deliberately not the effect env's count,
  which is never zero (the built-in `Unsafe` is always registered).
- **Never for `and` / `or`.** They are lazy, and hoisting evaluates an operand
  before the node -- `(and (f false) (f (boom)))` reached `boom`.
  `tests/fixtures/saffron-dyn-truthy` caught it, which is exactly what its
  short-circuit row is there for.
- **Never for a NIL-typed operand.** `void __ctlhoist_N = f(...)` does not
  compile; `(when (> n 0) (do (println n) (countdown ...)))` in the tour guide's
  own fixture is the shape. Nothing is lost -- a unit-valued operand delivers no
  word, so there is no seam for it to cross.
- **Return position only, through its own entry point** (`elab_coerce_to_any_return`),
  and **the gate had to actually gate**. Both were found by CI's leak gate after
  the first version had already landed, and they are the two most useful things
  in this report:
  - The first version hoisted inside `elab_coerce_to_any` itself, which also
    serves the CALL-ARGUMENT widen. Three stamps at the argument site
    (`frame_box`, the fat shim's `stack_ok`, `any_drop_after`) key on that
    helper returning an `EX_UNION_INJECT` **directly**; a let around it skipped
    all three and reinstated exactly the per-widen malloc
    [any-struct-box-leak-per-widen](any-struct-box-leak-per-widen.md) had
    removed -- 5999 orphaned allocations in `any-widen-frame-box`.
  - The gate `unit_has_user_effect` was set by **`stdlib/trail.tur`**, which
    declares a `defeffect` and is autoloaded into every program, so it was
    unconditionally true and gated nothing. It now ignores a `defeffect` read
    during `in_stdlib_load`. The "zero fixture churn" measured before this was
    therefore luck rather than the gate working, which is worth knowing: a
    no-churn result does not by itself prove a transform is confined.

  `tests/run.sh` cannot see either -- it compiles fixture programs
  unsanitized, exactly as CLAUDE.md's leak-detection section warns -- so
  **`tests/run-leak-check.sh` is part of validating a codegen or elaboration
  change, not an optional extra**. `saffron-effect-through-dyn-operand` now
  carries `requires.leak-check` so a repeat fails on the fixture that owns the
  behaviour.

The earlier note in this report about tightening `is_delegatable_value` directly
STANDS as a do-not-repeat: it makes the node non-delegatable, the CPS IR has no
native lowering for one, and the function evicts wholesale. The hoist is what
avoids that, by making the operand something the existing lowering already
admits.

Pinned by `tests/fixtures/saffron-effect-through-dyn-operand`, now a COMPILED
fixture (it was `requires.interp-only` while only the row half was fixed),
carrying six arms that are not interchangeable -- the control, a dynamic
arithmetic op, a comparison, a widened `do`, a widened `if`, and the truthiness
path. Both back ends agree, and the suite is clean with no snapshot churn.

**One residue, filed separately because it is NOT Saffron-specific:** an
effectful call as a CONSTRUCTOR argument (`(Box (g))`) still loses its row, in
plain typed Turmeric too. See
[effect-row-lost-through-a-constructor-argument](../reported/effect-row-lost-through-a-constructor-argument.md).

---

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
