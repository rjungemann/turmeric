# emit_effects.c Extraction Plan (EE0--EE4)

> **Status:** Not started. Supersedes the `emit_effects.c` row of
> [source-layout-plan.md](source-layout-plan.md) §3a.
>
> **Last updated:** 2026-05-18

---

## Why this plan exists

The source layout plan ([source-layout-plan.md](source-layout-plan.md) §3a)
proposed splitting the codegen monolith `emit.c` into six translation units.
Five of those landed and exist today:

```
src/compiler/emit_core.c     emit_expr.c     emit_stmt.c
                emit_fns.c   emit_module.c
```

The sixth -- `emit_effects.c`, scoped at "~1,200 lines" and rated
"medium effort, low risk" -- was never created. That estimate assumed the
algebraic-effects and CPS (`shift`/`reset`) code lived in a clean, contiguous
block that could be cut and pasted. It does not.

The plan itself anticipated this. Its §3a note says the estimates were
"scaled from the original plan proportionally" and that "actual boundaries
should be verified against `emit.c` section markers before splitting." This
document is that verification, plus a revised approach.

---

## The real boundaries

The effects/CPS code is interleaved inside the two largest functions in the
codegen layer. Neither is a block of straight text that can be relocated.

### `emit_value` (`emit_expr.c:425`--`3472`, ~3,048 lines)

`emit_value` is a single function: one `switch` over `ExprKind` with ~80
`case` arms. The effects/CPS arms are 13 cases in three regions:

| Region | Lines | Cases | Size |
|---|---|---|---|
| A -- delimited / cloneable / serial continuations | 692--1143 | `EX_RESET`, `EX_CLONEABLE_RESET`, `EX_SHIFT`, `EX_SHIFT0`, `EX_CLONEABLE_SHIFT`, `EX_SERIAL_RESET`, `EX_SERIAL_SHIFT` | ~452 |
| B -- continuation predicate | 1804--1820 | `EX_CONT_PRED` | ~17 |
| C -- algebraic effects | 2350--3021 | `EX_DEFECT`, `EX_PERFORM`, `EX_HANDLE`, `EX_RESUME`, `EX_DISCONTINUE` | ~672 |

Total ~1,141 lines -- close to the plan's ~1,200 estimate. Regions A and C
are each contiguous; region B is a 17-line stray sandwiched between
`EX_REF_PRED` and `EX_ASYNC`. The non-effects cases between the regions
(`EX_CALL`, the RC operations, `EX_ASYNC`/`EX_AWAIT`, `EX_SELECT`, the STM
cluster, the borrow-trait cases) stay behind.

The blocker is structural, not arithmetic: **a `case` arm has no existence
outside its `switch`.** You cannot move `case EX_HANDLE:` to another file.
Extraction means converting each case body into a function and replacing the
arm with a one-line dispatch:

```c
/* in emit_expr.c, after extraction */
case EX_HANDLE:  return emit_effects_handle(ctx, body, e);
```

That is function-level surgery on `emit_value`, not a mechanical move.

### `emit_program` (`emit_module.c:6`--`2528`, ~2,522 lines)

`emit_program` is worse. It is one function that emits the program's C
**runtime prelude** as straight-line `buf_puts(out, ...)` calls. The effects
runtime is not even contiguous here -- fragments are woven through the body:

| Lines | Fragment | Coupling |
|---|---|---|
| ~539 | algebraic effect handler runtime type (`ET3`) | inline |
| ~952--1011 | cloneable-continuation runtime + CPS reset context | guarded by `if (cps_expr_contains_cloneable_shift(program) \|\| expr_has_multishot_handler(program))` |
| ~1014--1253 | effect handler chain, `TurContK`, 19D effect-capture context, continuation helpers | inline |
| ~1062, ~1072 | `effect_handler_chain` / `eff_ctx` fields **emitted into the middle of the fiber struct definition** | order-locked |
| ~1201--1213 | `global_effect_handler_chain` decl + fiber-block wiring | comment at 1201 states it must precede `tur_fiber_block_new` |
| ~2125+ | `tur_effect_perform` definition | inline |

Two of these fragments are emitted *inside* unrelated declarations (the fiber
struct, the fiber-block constructor) and one carries an explicit ordering
constraint. There is no single insertion point a helper could be called from.

---

## Why the original "low risk, mechanical" rating was wrong

1. **No movable block exists.** In `emit_value` the effects logic is `switch`
   arms; in `emit_program` it is interleaved straight-line emission. Both
   require restructuring the host function, which is exactly what a
   "mechanical move" is supposed to avoid.
2. **The host functions are the two biggest in the layer.** Any edit to
   `emit_value` (3,048 lines) or `emit_program` (2,522 lines) is high-blast-radius
   -- they are on the critical path for every fixture.
3. **`emit_program`'s fragments are order-coupled.** The fiber struct and
   `global_effect_handler_chain` ordering mean the runtime fragments cannot be
   hoisted into one helper without also restructuring fiber-struct emission.

What the original rating got *right*: the helper surface is already shared.
`emit_internal.h` already exports `emit_value`, `fresh_tmp`, `atom_nil`,
`indent_buf`, `name_for_binding`, `collect_handle_captures`,
`expr_has_multishot_handler`, and the `EmitCtx` struct. The effects cases lean
almost entirely on those, so the `emit_value` half needs little new cross-TU
plumbing. That makes the **`emit_value` extraction genuinely tractable** -- it
is the `emit_program` half that does not pay off.

---

## Revised approach

Split the work and treat the two halves differently. The recommendation is to
**do EE0--EE3 (the `emit_value` half) and stop there**; EE4 is documented for
completeness but is not recommended.

### EE0 -- Prep: scaffold `emit_effects.c` and `emit_internal.h`

- Create `src/compiler/emit_effects.c` with the standard header comment.
- Add it to `src/CMakeLists.txt` next to the other `emit_*.c` files.
- Add an `emit_effects.c` section to `emit_internal.h` declaring the
  per-case functions introduced in EE1--EE3.
- Confirm `emit_value` and every helper an effects case calls are already
  declared in `emit_internal.h` (they are, today -- but verify after any
  intervening change). Promote anything still `static` in `emit_expr.c` only
  if a moved case actually needs it.
- `just build` -- empty file links cleanly.

### EE1 -- Extract region C (algebraic effects)

Region C is the largest contiguous block and the cleanest to move first.

- For each of `EX_DEFECT`, `EX_PERFORM`, `EX_HANDLE`, `EX_RESUME`,
  `EX_DISCONTINUE`, lift the case body into a function in `emit_effects.c`:
  `emit_effects_perform`, `emit_effects_handle`, etc.
- Replace each arm in `emit_value` with a one-line `return emit_effects_*(...)`.
- `EX_DEFECT` returns `atom_nil()` with no runtime work -- it can stay inline
  or become a trivial function; keep it with the group for cohesion.
- `just test` -- effects fixtures (`tests/fixtures/**/effect*`,
  `**/handle*`, `**/perform*`) are the canary.

### EE2 -- Extract region A (delimited / cloneable / serial continuations)

- Same treatment for `EX_RESET`, `EX_CLONEABLE_RESET`, `EX_SHIFT`,
  `EX_SHIFT0`, `EX_CLONEABLE_SHIFT`, `EX_SERIAL_RESET`, `EX_SERIAL_SHIFT`.
- `EX_CLONEABLE_RESET` (~196 lines) and `EX_CLONEABLE_SHIFT` (~152 lines) are
  the substantive ones; the rest are short.
- `just test` -- continuation fixtures (`shift`/`reset`, cloneable, serial).

### EE3 -- Extract region B and finalize

- Move the lone `EX_CONT_PRED` case.
- Final tally: `emit_effects.c` ~1,150 lines; `emit_expr.c` drops to
  ~2,350 lines; `emit_value` drops by ~1,140 lines, with 13 one-line dispatch
  arms remaining.
- `just test` && `just docs` (no docstring impact, but keep the invariant).

### EE4 -- (Optional, NOT recommended) the `emit_program` runtime fragments

Documented so the decision is on the record. Extracting the effects fragments
from `emit_program` would require one of:

- **(a)** Restructure `emit_program` so the fiber struct and runtime prelude
  are themselves emitted by ordered sub-helpers, then route the effects
  fragments through `emit_effects_runtime(Buf *out, const Expr *program)`
  helpers called at the correct points. This is a refactor of `emit_program`'s
  architecture, not an extraction.
- **(b)** Extract only the cleanly-guarded contiguous chunk (the
  cloneable-continuation runtime + CPS reset context, ~952--1011, already
  wrapped in one `if`) and leave the order-locked fragments in place. This
  yields a ~60-line move for a partial, lopsided result.

Neither is worth the risk. The runtime-prelude fragments are arguably *better*
understood as part of `emit_program`'s assembly responsibility than as part of
a per-node `emit_effects.c`. **Recommendation: leave `emit_program` as-is** and
note in `emit_internal.h` that `emit_effects.c` covers expression-position
emission only.

---

## Revised effort and risk

| Phase | Work | Effort | Risk |
|---|---|---|---|
| EE0 | Scaffold file + CMake + header | ~0.5 h | None |
| EE1 | Extract region C (algebraic effects) | ~0.5 day | Low--medium |
| EE2 | Extract region A (continuations) | ~0.5 day | Low--medium |
| EE3 | Extract region B + finalize | ~1 h | Low |
| EE4 | `emit_program` fragments | 2--3 days | High -- **not recommended** |

Original §3a: "medium effort, low risk" for a 1,200-line file. Revised: the
`emit_value` half (EE0--EE3) is **~1.5 days, low--medium risk** -- each case
becomes a function with an unchanged body, verified by the effects test suite
between phases. The `emit_program` half is high risk and low payoff; skip it.

Net result if EE0--EE3 ship: `emit_effects.c` exists (~1,150 lines),
`emit_expr.c` shrinks to ~2,350 lines, and the source-layout-plan's six-file
target is met for the part of it that is genuinely a win.

---

## Verification checklist

- [ ] `just build` clean after EE0 (empty file links).
- [ ] `just test` green after **each** of EE1, EE2, EE3 -- never batch.
- [ ] No case body is edited during the move; only relocated and wrapped in a
      function signature. Diff each extracted case against its original.
- [ ] `emit_internal.h` declares every `emit_effects_*` function; no implicit
      declarations (build with `-Werror=implicit-function-declaration`).
- [ ] No new non-`static` symbol leaks beyond the `emit_effects_*` prefix.
- [ ] The 13 dispatch arms left in `emit_value` cover exactly the 13 cases
      listed above -- grep `case EX_` before and after to confirm the switch
      stays exhaustive.

## Out of scope

- `elab.c` effects elaboration (`elab_effects.c`) -- see
  [source-layout-plan.md](source-layout-plan.md) §3b.
- The effect-row and CPS *passes* (`src/passes/effect_check.c`,
  `effect_lower.c`, `cps.c`) -- already separate translation units.
- Any behavior change to codegen. This is a pure restructuring; generated C
  output must be byte-identical.
