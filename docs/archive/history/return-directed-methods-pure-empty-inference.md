# Paper trail: return-directed `pure`/`empty` context inference (if-sibling)

Resolved 2026-07-04. Companion to the resolved report at
`docs/archive/return-directed-methods-pure-empty-inference.md`.

## What was already working (fix direction #1)

Before this change the elaborator already threaded `e->expected_type` from:

- a `let` binding's type annotation (`elab_forms.c`, the
  `generic-return-type-not-inferred-from-context` path around the
  `let_init_expected` push), and
- an enclosing `defn` return type (`elab_fns.c`, `return_full_type` push).

A return-directed method (`pure`/`empty`, whose class tyvar `f` appears only in
the result) grounds off that channel in `elab_try_return_dispatch`
(`elab_typeclasses.c`). So `(let [x : (Option int) (pure 42)] ...)` and
`(defn f [] : (Option int) (pure 42))`, and `if`/`match` nested under either,
already worked. The report's premise that these "aren't consulted" was stale --
they are; only the pure sibling-arm-without-outer-context case was open.

## What this change added (fix direction #2, `if` only)

`elab_if` (`src/compiler/elab_forms.c`): when `e->expected_type` is NULL and
exactly one branch is a return-directed method call, probe the concrete sibling
branch to discover the join type and thread it as the expected type for both
arms.

- Probe runs under `diag_push_capture()` / `diag_pop_capture()` with
  `move_state_snapshot_bindings` / `linear_state_snapshot_bindings` +
  `..._restore`, so a failed or side-effecting probe leaves no diagnostics and
  no move/linear-state change. The real arm elaboration then re-runs normally.
- The probe type is used only when the sibling elaborated cleanly
  (`perr == 0`) to a concrete result kind (not UNKNOWN / NEVER / NIL).
- Works in both branch orders (`(if c (pure 1) known)` and
  `(if c known (pure 1))`).

New predicate `elab_symbol_is_return_dispatch_method(Elab*, const Symbol*)`
(`src/compiler/elab_typeclasses.c`, declared in `elab_internal.h`) mirrors the
method-finding phase of `elab_try_return_dispatch` and is gated on no shadowing
binding (a user/local defn of the same name wins). `if_form_is_return_dispatch`
(static, `elab_forms.c`) wraps it for the `if` arm forms.

Regression fixture: `tests/fixtures/hkt-return-dispatch-if-sibling/` (stdout).
Covers `pure` in either arm and `empty` grounded from a concrete sibling.

## Deliberately not done

- **`match` sibling-without-outer-context.** `match`'s per-arm pattern scope
  makes the `if`-style sibling probe unsafe to apply verbatim (a probed arm body
  may reference pattern variables not in scope outside its arm). A `match` used
  as a value almost always sits under a typed binding / return type (fix #1),
  which already grounds it. Deferred to the end-to-end monomorphization work
  (fix direction #3).
- **stdlib `::` removals.** No return-directed-method workaround ascriptions
  exist in `stdlib/` to remove. A probe removal of the `Schema` by-value
  representative pins (`(:: (make-struct Schema ..) (Schema int))`) and the
  `Set` `eq?` param ascriptions was reverted -- the `Set` one changed codegen
  (set-typed-consumer snapshot) and the `Schema` ones touch the documented
  by-value carrier-representative pattern.

## Side finding filed separately

`docs/reported/byvalue-option-if-join-function-call-arm-aggregate-cast.md` --
a pre-existing codegen defect (unrelated to this fix): a by-value `Option`
flowing through an `if` whose arm is a *function call* returning that Option is
cast aggregate->pointer in emitted C. Reproduces with a plain `some` arm and no
return-directed method, so it is out of scope here.

## Suite

`bash tests/run.sh` -> `1932 passed, 0 failed`.
