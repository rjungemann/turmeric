---
title: The CPS tree-rebuild pass drops call abi_bindings, omitting by-value specs for calls in CPS-transformed functions
severity: FIXED 2026-06-14 -- cps_mark_expr's EX_CALL rebuild now struct-assigns the whole call_ member. Was: silent-miscompile-shaped (a needed by-value specialization omitted, carrier base called with a by-value struct -> cc type error).
date: 2026-06-14
---

## FIXED 2026-06-14 (session 4 cont.)

Root cause located by backtracing `EX_CALL` allocation: the duplicate node
is born in **`cps_mark_expr` (`src/passes/cps.c:810`)**, the CPS pass's
tree-rebuild (run by `run_core_passes`, `src/main.c:417`, after
elaboration).  Its `EX_CALL` case field-copied only
`fn_binding`/`fn_expr`/`args`/`n_args` into the fresh node, leaving
`abi_bindings`/`n_abi_bindings` (and `dict_arg`/`is_poly_call`/
`poly_arg_mask`) zeroed by `expr_new`'s memset.  So any call inside a
function the CPS pass rebuilds lost its named-tyvar substitution, and
`emit_abi_register_call` early-returned at the `n_bindings == 0` gate --
no by-value spec interned.

The `Eq Vec` by-value rewrite exposed it because the new
constrained-poly-helper-calling instance pulls the enclosing functions
through the CPS rebuild; the same drop is why gap 4's hamt-delete
regressor and earlier session reverts behaved as they did.

**Fix**: struct-assign the whole `as.call_` member before overriding the
recursively-rebuilt `fn_expr`/`args`/`n_args`, so every field survives the
copy.  Suite: `1622 passed, 4 failed` -- the same 4 pre-existing failures
as the gap-4 baseline, **zero regressions**.  `thead` now interns its
`Cons__int` spec under the Eq Vec rewrite (`BT_EMIT thead n_bindings=1`).

Note: this clears the spec-drop (Finding 6 of
`docs/upcoming/m5-residual-straddle-retirement.md`).  The Eq Vec by-value
rewrite still needs the representation-precise field-access change
(Finding 5) before it can land -- that is the remaining, independent half.

---

(Original investigation notes below.)

## Summary

When `stdlib/vec.tur`'s `(definstance Eq [Vec])` is rewritten so its body
calls a sibling *constrained-polymorphic* helper (`vec-eq-loop-byval [A]
[(Eq A)] ...`) instead of the existing `^fat`-lambda + carrier
`vec-eq-loop`, the ABI-specialization worklist stops interning the
by-value spec for an **unrelated** stdlib accessor -- e.g. `thead`
(`(defn thead [A] [l : (Cons A)] : A (.head l))`).  The downstream
program then emits `thead(Cons__int)` against the carrier base
`thead(int64_t)` -> `error: incompatible type for argument 1 of 'thead'`.

This is the same *class* of defect as gap-4's hamt-delete regressor
(`m5-instance-spec-doesnt-propagate-constraint-var-bindings.md`): adding
or changing one instance-method's spec composition perturbs the global
spec-interning worklist and a sibling spec that used to be minted no
longer is.

## Repro

In `stdlib/vec.tur`, replace the `Eq [Vec]` instance with the by-value
form:

```turmeric
(defn vec-len-byval [A] [v : (Vec A)] : int
  (.len v))

(defn vec-get-byval [A] [v : (Vec A) i : int] : A
  (unsafe (:: (array-get-unchecked (.data v) i) A)))

(defn vec-eq-loop-byval [A]
  [(Eq A)]
  [x : (Vec A) y : (Vec A) i : int len : int]
  : bool
  (if (= i len)
    true
    (if (eq? (vec-get-byval x i) (vec-get-byval y i))
      (vec-eq-loop-byval x y (+ i 1) len)
      false)))

(definstance Eq [Vec]
  [(Eq A)]
  (eq? [x y]
    (let [lx (vec-len-byval (:: x (Vec A)))
          ly (vec-len-byval (:: y (Vec A)))]
      (if (= lx ly)
        (vec-eq-loop-byval (:: x (Vec A)) (:: y (Vec A)) 0 lx)
        false))))
```

Then `tur build tests/fixtures/list-basic/input.tur` fails:

```
error: incompatible type for argument 1 of 'thead'
note: expected 'int64_t' but argument is of type 'Cons__int'
```

The emitted C has **no** `thead__spec__...` (`grep -c thead__spec` == 0),
even though the call site constructs `Cons__int l = (Cons__int){...}`
by value and passes it to `thead`.

### Bisection (each step rebuilds + checks `list-basic`)

- clean tree: `list-basic` passes, `thead__spec` interned.
- add the three `*-byval` helpers only (Eq Vec UNCHANGED): `list-basic`
  passes.  The helpers' mere existence is harmless.
- rewrite the `Eq [Vec]` instance body to call `vec-eq-loop-byval`:
  `list-basic` fails, `thead__spec` missing.

So the trigger is specifically the **instance-method body calling a
constrained-poly helper**, not the helper definitions and not the
field-access codegen (the failure reproduces with the companion
`emit_expr.c` field-access change reverted).

## Observed vs expected

- Observed: `thead`'s by-value spec is not interned once `Eq[Vec]`'s
  body composes through a constrained-poly helper; `list-basic` (and
  ~10 other list/option/tuple fixtures) fail to build.
- Expected: rewriting one instance body must not change whether an
  unrelated accessor (`thead`/`ttail`/`unwrap`) gets its by-value spec.

## Root cause -- PINNED 2026-06-14 (session 4 cont.): emit scans a DUPLICATE call node with `abi_bindings` dropped

It is *not* a worklist ordering/collision/capacity issue (the original
three hypotheses are disproven below).  Tracing `thead` end-to-end with
the rewrite applied:

1. **Elaboration is correct and identical.**  In both clean and rewritten
   trees, `elab_call` collects `{A -> int}` for `(thead l)` (inputs at the
   binding-collection site are byte-identical: expected `(type-app Cons
   tyvar)`, arg `(type-app Cons int)`), and the call node is saved with
   `n_abi_bindings = 1`:
   ```
   DBG_TH SAVE thead: node=0x..072c8 n_type_bindings=1   # rewrite
   DBG_TH SAVE thead: node=0x..fdf90 n_type_bindings=1   # clean
   ```

2. **Emit scans a DIFFERENT node in the rewritten tree.**
   `emit_abi_register_call` sees, for the *same single* source-level
   `(thead l)` call (list-basic has exactly one, at `input.tur:12`):
   ```
   # clean:   ENTRY node=0x..fdf90 n_bindings=1   <- SAME node as SAVE -> interns Cons__int spec
   # rewrite: ENTRY node=0x..cf558 n_bindings=0   <- DIFFERENT node, abi_bindings empty -> early return at
   #                                                 emit_module.c:959 (`!bindings || n_bindings==0`), no spec
   ```
   The early return at `n_bindings==0` is why `thead__spec` is never
   interned and the carrier base is called with `Cons__int`.

So the rewrite causes a **post-elaboration AST node duplication**: the
`(thead l)` node that emit scans (`cf558`) is a copy of the elaborated
node (`072c8`) with `call_.abi_bindings` / `n_abi_bindings` NOT carried
over.  In the clean tree no such copy happens (emit scans the original
elaborated node).

### What the duplicate is NOT

- Not the variadic-call path (`elab_call.c:2549`, sets `abi_bindings=0`)
  -- `thead` is not variadic.
- Not the poly-call path (`elab_call.c:3878`, `is_poly_call=true`,
  `abi_bindings` unset) -- instrumented, never fires for `thead`.
- Not a second elaboration of the call -- there is exactly one `SAVE`
  for `thead` in both trees.

### Where to look next

The duplicate node is created by a **conditional whole-program AST pass
inside `elaborate_program`** that rewrites/replaces call nodes and
field-copies an `EX_CALL` without `abi_bindings`/`n_abi_bindings`.  Ruled
out so far:

- not `emit_module.c` / `emit_fns.c` (no `as.call_.fn_binding/args` field
  reconstruction there; the emit ABI scan reads whatever node the AST
  already holds);
- not a second elaboration of the call (one `SAVE` only);
- not the variadic (`elab_call.c:2549`) or poly (`elab_call.c:3878`)
  call-construction paths.

So the duplicate is produced somewhere **between the `elab_call` save and
the emit ABI scan** by a pass that rebuilds the `(thead l)` node without
carrying `abi_bindings`.  It is NOT a generic `expr_clone` (none exists in
`expr.c`) and NOT field-reconstructed in `emit_module.c`/`emit_fns.c`.
Remaining candidates, to check in order:

- a post-elaboration whole-program rewrite inside `elaborate_program`
  (e.g. the Phase M7 promotion noted at `src/main.c:4901`, contract
  insertion, or a monomorphization/normalization pass);
- an emit-phase body clone in a TU not yet grepped (`emit_core.c`,
  `emit_cps.c`, `emit_stmt.c`) that reconstructs call nodes.

The fix is to **propagate `abi_bindings` + `n_abi_bindings` across that
rebuild** (or struct-assign the whole `as.call_` member instead of
copying individual fields).  Next tracing step: instrument `EX_CALL` node
construction / `as.call_.fn_binding` assignment across those passes to
catch where node `cf558` is born for `thead` (it carries
`fn_binding->name == "thead"`, `is_global == 1`, `fn_expr == NULL`).  A
regression guard: `grep -c thead__spec` on the emitted C must be > 0 with
the Eq Vec by-value rewrite applied.

This also retro-explains gap 4's hamt-delete regressor and the recurring
session reverts: they are the same node-duplication-drops-`abi_bindings`
fragility surfacing under different instances, not three separate worklist
bugs.

## Why it matters

This is the standing blocker for the M5 single-body-two-ABIs goal.  The
field-access half of that work (making `(.len v)` dual-ABI) is solvable
(see `docs/upcoming/m5-residual-straddle-retirement.md`), but it is moot
until the Eq Vec instance body can call a by-value helper *without*
perturbing sibling specs.  The same worklist fragility is the recurring
theme behind gap 4's hamt-delete regressor and the earlier session
reverts -- it deserves a focused, instrumented fix to the worklist's
registration/ordering invariants rather than another local patch.

## Validation under a fix

With the Eq Vec by-value rewrite in `stdlib/vec.tur`:
`bash tests/run.sh` must intern `thead__spec`/`ttail__spec`/etc. exactly
as on the clean tree (the ~11 list/option/tuple/hrt fixtures that
regressed must pass), and `vec-of-tvec-eq-manual` must still link via the
Eq Vec carrier base.

## Related

- `docs/upcoming/m5-residual-straddle-retirement.md` -- the design map;
  findings 1-4 of the session-4 continuation.
- `m5-instance-spec-doesnt-propagate-constraint-var-bindings.md`
  -- gap 4 (FIXED); its hamt-delete regressor was the same worklist
  fragility, sidestepped by scoping the augmentation, not by fixing the
  worklist invariant.
