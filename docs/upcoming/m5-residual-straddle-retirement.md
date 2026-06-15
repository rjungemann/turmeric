---
title: M5 Residual Straddle Retirement
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: Retire the two CK_CONCRETE -> CK_CARRIER bridges introduced by M4c-pre-ext so the bridge call count can drop and M3 (delete `emit_carrier_bridge`'s accessor-side path) becomes mechanical. Per audit §10.8 and the end-to-end monomorphization plan's M5 phase.
---

# M5 Residual Straddle Retirement -- Plan

## STATUS 2026-06-15 (session 6): MutableMap straddle RETIRED

The last real M4c-Path-A producer, `Eq [MutableMap]`, is now retired the
same by-value way as Vec/Cons. Two coordinated changes landed:

1. **Elaborator** (`elab_typeclasses.c`): a multi-param struct instance now
   brings its *full* type-ctor param list (`K` and `V` of
   `MutableMap [K V]`) into sig-tyvar scope, not just the constraint-named
   ones, so an unconstrained `K` in `(:: x (MutableMap K V))` resolves to a
   named tyvar (was an opaque `TY_STRUCT`, which blocked spec interning).
2. **Emit composition** (`emit_module.c`): the instance-method augmentation
   resolves **all** type-ctor params by name from the receiver struct's own
   `type_params`, not only the constrained ones.

`stdlib/mutmap.tur` `Eq [MutableMap]` rewritten through `mutmap-eq?-byval`
+ a shared `mutmap-eq-storage?` inline-C core (over raw `ptr<void>`
storage). The by-value spec now interns and the instance spec body passes
by-value args with zero `(intptr_t)(&...)` spills. Suite green at 1635/0;
75 codegen snapshots regenerated (gensym churn from the new stdlib
helpers). Resolved report:
`docs/archive/history/m5-multiparam-instance-unconstrained-tyvar-blocks-byval-spec.md`.

**Bridge firing now** (env-gated probe + emit-c sweep):

| Bridge site | Fires for |
|---|---|
| M4c Path A (~`emit_expr.c:2748`) | `m5-lambda-aft-tyvar-prior-accepts-concrete` only |
| EX_ASCRIBE (~`emit_expr.c:4500`) | `m5-instance-spec-constraint-var`, `m5-spec-body-ascription-bridge` (dedicated bridge-pin fixtures) |

So `mutmap-eq` has dropped out. The remaining M4c-Path-A producer is the
single `m5-lambda-aft-tyvar-prior-accepts-concrete` fixture; the EX_ASCRIBE
sites are only the two fixtures that exist to *pin* bridge behaviour.
Before D.4 ("delete the branches outright") can proceed, those producers
need their own resolution (the lambda fixture's straddle + retiring or
re-pinning the two pin fixtures).

## STATUS 2026-06-15 (session 5): Vec/Cons rewrite LANDED & green; MutableMap is the last real producer

The earlier "session 4 cont. 3" note below said the field-access predicate
and `Eq Vec` stdlib rewrite were *reverted from the tree*. **That is now
stale.** The `Eq [Vec]` / `Eq [Cons]` by-value rewrite **and** the Finding-7
per-call `(call, active-spec)` clone keying both **landed** in commit
`deee4c6` ("fix(m5): gap 4 FIXED ..."). The suite is green at
**1635 passed, 0 failed**. `stdlib/vec.tur` carries `vec-len-byval` /
`vec-eq-loop-byval`; `vec-eq-ascribed-multi` (the multi-element case
Finding 7 unblocked) passes.

**Measured current bridge state** (env-gated probe at the two
`emit_carrier_bridge(CK_CONCRETE, CK_CARRIER, ...)` sites -- now
`emit_expr.c` ~2748 M4c Path A and ~4500 EX_ASCRIBE -- plus an `emit-c`
sweep over every fixture):

| Bridge site | Fixtures it still fires for |
|---|---|
| M4c Path A (~2748) | `mutmap-eq`, `m5-lambda-aft-tyvar-prior-accepts-concrete` |
| EX_ASCRIBE (~4500) | `m5-instance-spec-constraint-var`, `m5-spec-body-ascription-bridge` (both exist to *pin* bridge behaviour) |

So the Vec/Cons producers are gone; **`mutmap-eq` (the `Eq [MutableMap]`
instance) is the one remaining "real" producer** of the M4c Path A bridge.
Retiring it the Vec way was attempted this session and hit a new gap:
`Eq [MutableMap]` is over the **multi-param** `MutableMap [K V]`, and the
*unconstrained* type-ctor param `K` is recorded in the instance-method
call's `abi_bindings` as a concrete `TY_STRUCT`, not an abstract `TY_TYVAR`
(the constrained `V` resolves fine). So no by-value spec interns and the
spec body passes a by-value struct into the int64 carrier base (`cc`
error). Full trace + root cause + proposed elaborator fix filed under
`docs/reported/m5-multiparam-instance-unconstrained-tyvar-blocks-byval-spec.md`.
The experiment (stdlib + an emit-side composition extension) was reverted;
the tree stays at the green baseline.

**Net:** D.4 ("delete the M4c Path A / EX_ASCRIBE branches outright") still
cannot proceed -- the branches remain load-bearing for `mutmap-eq`,
`m5-lambda-aft-tyvar-prior-accepts-concrete`, and the two pin fixtures.
The next workable piece toward the audit's "bridge count -> 0" is the
elaborator fix in the filed report (carry unconstrained instance type-ctor
params as named tyvars), after which the `Eq [MutableMap]` by-value rewrite
becomes mechanical.

## Why

M4c Path A specializes typeclass-instance methods on parameterized
concrete-layout types (Vec, Cons, Tuple2) to take their receivers
*by value* (`Vec__int x, Vec__int y`).  M4c-pre-ext rewrote `Eq Vec`
and `Eq Cons` instance bodies as pure-Turmeric loops over Path A
specs.  The loop bodies still need to consult primitive Vec helpers
(`vec-len`, `vec-get`, and the symmetric `vec-eq?` outside Path A)
whose inline-C bodies take an int64 carrier and cast through
`(void*)(intptr_t)v`.  The bridge mediates: a by-value `Vec__int`
spilled to a temp whose address is cast to int64.

Concretely, two new `emit_carrier_bridge` call sites emit a
`CK_CONCRETE -> CK_CARRIER` widening in M4c-pre-ext spec bodies:

1. **`src/compiler/emit_expr.c:2662`** -- M4c Path A direct call to
   an int64-carrier-sink helper.  Fires for `(vec-len xi)` /
   `(vec-get xi i)` inside an `Eq Vec` Path A spec.  Gated on
   `!matched_spec && !dict_arg`, by-value-carrier producer,
   `fn_binding`'s i-th arg_kind is `TY_INT`.

2. **`src/compiler/emit_expr.c:4393`** -- symmetric EX_ASCRIBE
   widening, `(let [xi (:: x :int)] ...)` inside `Eq Vec`'s body.
   Resolves the spill-local cname through
   `current_abi_specialization->arg_types[]`.

Both site are documented at audit §10.8.  Together they keep the
total `emit_carrier_bridge` call count at 7 (vs. the audit's
target 0).  M3's deletion of `emit_carrier_bridge`'s
`CK_CARRIER -> CK_CONCRETE` accessor-side path is blocked until
these two `CK_CONCRETE -> CK_CARRIER` sites stop firing, because
the producer-side and consumer-side paths share the bridge module
and removing either half independently would leave dangling
callers.

## Prereqs landed

- M4c Path A per-instantiation specs (Vec/Cons/Tuple2): `78589845`,
  refresh `0506bab2`.
- M4-rest direct dict dispatch: commit `a45ff6c1`.
- M5 elab dispatch fix (parametric receiver picks right instance):
  commit `a301229e`.
- M5 emit arg-bridge fix (`find_matched_abi_spec` consults
  `specialized_call_exprs[]`): this session.

## The actual disagreement

`(definstance Eq [Vec] [(Eq A)] (eq? [x y] ...))`'s body is:

```turmeric
(let [xi (:: x :int)
      yi (:: y :int)
      lx (vec-len xi)
      ly (vec-len yi)]
  (if (= lx ly)
    (vec-eq-loop xi yi 0 lx (fn [a b] (eq? a b)))
    false))
```

At the carrier-ABI dispatch (`A = TYVAR_unresolved`), `x` and `y`
arrive as int64 carriers; `(:: x :int)` is a pure relabel; the
helpers see what they expect.

Under Path A specialization (`A = int`), the spec body declares
`Vec__int x, Vec__int y` -- the value is the by-value struct.
`(:: x :int)` emits the L4393 bridge: spill to a local, take its
address, store as int64.  Downstream `(vec-len xi)` calls the
carrier helper at L2662.

The straddle works, but every Path A spec for `Eq Vec` re-emits
the bridge plumbing.

## Retirement options

### Option A: by-value variants of the carrier helpers

Write `vec-len-byval [A] : (Vec A) -> int`, etc., bodies that
access the struct directly.  Path A specs call the by-value
variant; carrier consumers keep the existing helper.

- Pro: minimal change to spec emit; helpers become pure-Turmeric
  field accesses.
- Con: stdlib API duplication.  Every helper that touches a
  parameterized struct needs a twin.  As M2/M3 land more of the
  stdlib through the monomorphization path, the duplication grows.

### Option B: pattern-match transform in the spec emitter

When a Path A spec body emits `(:: x :int)` followed by `(vec-len
xi)`, recognize the pattern and rewrite to direct field access
(`x.len`).  Likewise for `vec-get` (field index plus bounds check
inlined).

- Pro: no stdlib API change.
- Con: brittle; pattern coverage grows with each new helper.

### Option C: auto-monomorphize the carrier helper at the spec
boundary

Extend the ABI-spec interning machinery (`emit_abi_intern_spec`)
to fire on `vec-len` / `vec-get` / etc. when called from a
by-value spec context.  The interned spec rewrites the inline-C
body for the by-value receiver: instead of `(void*)(intptr_t)v`,
the body takes `Vec__int v` directly and accesses `v.len`.

The inline-C body needs to be inspected for `int64_t v;
struct { ... } *vec = (void*)(intptr_t)v;` shape and rewritten
to `struct { ... } vec = v;`.  This is exactly the kind of
inline-C-spec rewriting M2/M3 already do for `ok`/`err` etc.,
but extended to a wider set of stdlib carrier helpers.

- Pro: the cleanest fit with the plan's M2-M5 trajectory --
  generalizes the monomorphization mechanism, doesn't add new
  surfaces.
- Con: requires the helper's inline-C body to carry enough
  structure for the by-value rewrite.  Today's bodies hand-roll
  the cast, so the recogniser needs a parse-and-rewrite pass over
  the inline-C string.  Risky if the helpers diverge from the
  expected shape.

### Option D: rewrite the `Eq Vec` / `Eq Cons` bodies to use
field access directly

The simplest pragmatic fix.  The stdlib `Eq Vec` body becomes:

```turmeric
(definstance Eq [Vec]
  [(Eq A)]
  (eq? [x y]
    (let [lx (.len x)
          ly (.len y)]
      (if (= lx ly)
        (vec-eq-loop-byval x y 0 lx)
        false))))
```

`vec-eq-loop-byval` is a new helper that walks two by-value
`Vec__int` values via `.data[i]` field access.  The carrier
helpers stay intact for non-spec consumers; the Path A spec body
never crosses the carrier boundary.

- Pro: smallest blast radius; the rewrite is local to two stdlib
  files (`stdlib/vec.tur` + maybe `stdlib/list.tur`); the L2662
  and L4393 bridges stop firing for `Eq Vec` / `Eq Cons` immediately.
- Con: hand-written `*-byval` helpers; doesn't generalize to
  other helpers automatically.  Fits the SAME pragmatic shape as
  M4c-pre-ext itself (which is also a stdlib-side rewrite).
- Audit alignment: §10.5 already calls M4c-pre-ext "stdlib
  helpers rewritten as pure-Turmeric loops".  Option D extends
  that same rewrite to drop the int64 ascription step.

## Update 2026-06-14 (session 4): gap 4 FIXED -- constrained-poly helper from instance body now monomorphizes

The 4th gap (a sibling constrained-poly helper called from an instance-
method Path A spec body did not get a by-value spec interned) is now
**fixed**.  See
`docs/archive/history/m5-instance-spec-doesnt-propagate-constraint-var-bindings.md`
for the full write-up.  Summary: a three-part coordinated change
(definstance records the constraint var symbol; instance bodies
elaborate constraint vars as named tyvars; emit composition augments the
active instance-method spec's bindings with the constraint var's
concrete element type, *scoped to that spec only* so it doesn't collide
with sibling specs the way the session-3 dispatch-call attempt did).
The hamt-delete regressor stays green.  Pinned by
`tests/fixtures/m5-instance-spec-constraint-var/`.

This clears the elaboration-infra blocker the session-3 retirement note
called out: an `Eq Vec` rewrite using `vec-eq-loop-byval` (a constrained-
poly helper) can now be called from the `Eq Vec` instance body without
hitting the "no spec interned for callee" wall.  The remaining
single-body-two-ABIs question for the `Eq Vec` definstance (its body
serves both the int64 carrier base and the by-value Path A spec) is
still open -- it is a separate design choice, not this gap.

## Update 2026-06-14 (session 4 cont. 3): Finding 5 confirmed; two compiler bugs fixed; one blocker remains

Picked Finding 5 (representation-precise field-access) back up now that the
CPS `abi_bindings` drop (Finding 6) is fixed.  Progress and the new wall:

1. **Field-access predicate works (Finding 5 confirmed).**  Gating the
   carrier deref on a `TY_APP` receiver that is a *carrier-represented*
   `EX_VAR` param (`emit_byvalue_carrier_abi == false`) -- by-value
   `TY_APP` receivers (Option/Result/Tuple) keep direct `.field` -- runs
   the full suite at `1622 passed, 4 failed` **in isolation** (same 4
   baseline failures, zero regressions).  The blanket-`TY_APP` blast
   radius (54 fixtures) does not recur.

2. **Bug fixed: CPS dropped `abi_bindings`** (Finding 6).  Committed
   separately (`fix(cps): preserve call abi_bindings ...`).  See
   `docs/archive/history/m5-eq-vec-byval-rewrite-drops-sibling-specs.md`.

3. **Bug fixed: heap-use-after-free in `emit_abi_intern_spec`.**  The
   `Eq Vec` by-value rewrite over `vec-eq-ascribed-multi` (Eq[Vec[A]] for
   bool/cstr/int32/uint64/...) interns enough specs to grow
   `ctx->abi_specializations` past its capacity; the `realloc` moved the
   array while `ctx->current_abi_specialization` (a raw pointer into it)
   stayed dangling, and `emit_abi_scan_expr`'s EX_CALL case then read it
   (ASan: heap-use-after-free at emit_module.c:1565, freed at :678).
   Fixed by capturing the active spec's index before the realloc and
   re-pointing it after.  Pure memory-safety fix -- emitted C is
   byte-identical, it just stops reading freed memory.

4. **Remaining blocker (Finding 7): per-call-node clone recording can't
   distinguish multiple specs of one source body.**  With (1)-(3) in
   place the `Eq Vec` rewrite compiles+runs for `int` (probe), nested
   `Vec[Vec[int]]` (`vec-of-tvec-eq-manual`, carrier-base path), and
   `m5-byval-marker`.  But `vec-eq-ascribed-multi` (many element types)
   miscompiles: the `Eq Vec` spec for element `bool`
   (`__inst_Eq_eq_qu_Vec__spec__bool_Vec__bool_Vec__bool`) calls
   `vec_len_byval__spec__int64_t_Vec__int` (the *int* spec) with a
   `Vec__bool` arg -> cc type error.  The per-element specs ARE all
   interned (Vec__bool/cstr/int32/uint64 exist), but the single shared
   `Eq Vec` source body's inner `vec-len-byval` call node records ONE
   clone name (last writer / the int one) via the specialized-call
   table -- so every element-type Eq Vec spec emits the same callee
   clone.  The recording must be keyed on `(call node, active spec)`,
   not the call node alone.  That is the next focused piece; filed
   thinking lives in this section.

Net for this continuation: bugs (2) and (3) land as standalone compiler
fixes; the field-access predicate (1) is validated and ready; the
`Eq Vec`/`Eq Cons` rewrite waits on Finding 7.  The field-access change
and the stdlib rewrite are reverted from the tree (re-apply is mechanical
once Finding 7 lands); bugs (2)/(3) stay.

## Update 2026-06-14 (session 4 cont.): single-body-two-ABIs -- design space fully mapped

Continued straight into the single-body-two-ABIs question.  The
investigation rewrote `Eq Vec` end-to-end through by-value helpers
(experiment reverted) and pinned down exactly where the wall is.

### Finding 1 -- gap 4's fix already solves it for CONCRETE element types

With gap 4 fixed, a single `Eq Vec` source body

```turmeric
(definstance Eq [Vec]
  [(Eq A)]
  (eq? [x y]
    (let [lx (vec-len-byval (:: x (Vec A)))
          ly (vec-len-byval (:: y (Vec A)))]
      (if (= lx ly)
        (vec-eq-loop-byval (:: x (Vec A)) (:: y (Vec A)) 0 lx)
        false))))
```

emits BOTH a working carrier base and a working by-value spec:

- **spec** `__inst_Eq_eq_qu_Vec__spec__bool_Vec__int_Vec__int(Vec__int x,
  Vec__int y)` passes `x`/`y` straight into the by-value helper specs --
  **no `(int64_t)(intptr_t)(&...)` spill**.  Whole-file bridge count for
  the probe dropped to 0.  The L2662/L4393 bridges are retired for this
  path.
- **carrier base** `__inst_Eq_eq_qu_Vec(int64_t x, int64_t y)` lowers the
  `(:: x (Vec A))` ascription to `(*(Vec__int *)(intptr_t)(x))` (the
  accessor-side CK_CARRIER -> CK_CONCRETE deref) and calls the by-value
  helper specs.  The carrier base is kept alive because the typeclass
  dictionary singleton holds `.eq_qu = __inst_Eq_eq_qu_Vec` (uniform
  int64 dispatch slot).

So `(:: x (Vec A))` is the load-bearing construct: a no-op in the spec,
a deref in the carrier base.  One body, two ABIs -- **for concrete A**.

### Finding 2 -- the residual hard case is ABSTRACT element types

`tests/fixtures/vec-of-tvec-eq-manual` dispatches `(.eq? @Vec a b)` on
vecs built with bare `(vec-new)` -- **untyped** (`:int` carrier, element
type abstract).  This reaches `Eq Vec`'s *carrier base* with no concrete
`Vec__int` to deref to, so the `vec-*-byval` calls inside it fall back to
the helpers' own **carrier bases** (no by-value spec exists for an
abstract element).  Those carrier bases must therefore exist and compile.

Two helper shapes were tried for the carrier base; both fail:

- **inline-C `#{ByVal}` helper** (body assumes the by-value C struct):
  its carrier base is uncompilable (`v.len` on an `int64_t`).  Suppressing
  it via `prefer_byvalue_spec` is **unsafe** -- `vec-of-tvec-eq-manual`
  emits a *real* carrier call to `vec_hylen_hybyval`, so suppression turns
  the C compile error into an `ld: undefined reference`.
- **pure-Turmeric field-access helper** `(.len v)` (no inline-C, no
  marker): the spec is perfect, but the carrier base still emits `v.len`
  on the `int64_t` param -- **field access is not ABI-aware**.  The deref
  only happens at a CK_CARRIER -> CK_CONCRETE *ascription* boundary; a
  bare `(.field v)` on a carrier-represented struct value does not deref.

### Finding 3 -- the real fix is ABI-aware field-access lowering

The root cause is narrow and now precise: **`(.field v)` on a value whose
static type is a parameterized struct but whose C representation is the
int64 carrier must lower to `((Layout *)(intptr_t)v)->field`, not
`v.field`.**  For element-agnostic fields (`len`, `cap`, `data`) the
layout offset does not depend on the element type, so even an abstract-A
carrier base can deref against a representative `{ void* data; int64_t
len; int64_t cap; }` shape.  With that one change:

- `vec-len-byval` / `vec-get-byval` become genuinely dual-ABI in pure
  Turmeric (deref in the carrier base, direct in the spec), so their
  carrier bases compile and `vec-of-tvec-eq-manual` links.
- `Eq Vec` (and `Eq Cons`, same shape) can drop `(:: x :int)` + the
  carrier `vec-len`/`vec-eq-loop` helpers entirely; the spec path is
  bridge-free and the carrier path still works.
- No `#{ByVal}` marker, no carrier-base suppression, no stdlib API twin.

### Finding 4 -- the naive ABI-aware field-access change has WIDE blast radius (empirical)

The field-access lowering already casts through the carrier typedef for a
parameterized receiver, but only gates on `struct_expr->type.kind ==
TY_STRUCT` (`emit_expr.c` EX_GET_FIELD, the `through_carrier` predicate).
A `(Vec A)` receiver is `TY_APP`, so the cast never fired for it -- hence
`(.len v)` emitting `v.len` on an int64 in the helper carrier base.

Extending `through_carrier` to also fire for `TY_APP` receivers
(`def->n_type_params > 0`) **made the `Eq Vec` rewrite fully work**:

- spec body bridge-free (0 `(int64_t)(intptr_t)(&...)` spills whole file),
- `vec-len-byval` carrier base correctly `((Vec *)(intptr_t)(v))->len`,
- `vec-of-tvec-eq-manual` (untyped `@Vec` carrier-base path) links AND
  runs (`true/false/true/false`).

BUT the full suite regressed by **54 build failures** (e.g. `list-basic`,
`option-basic`, `result-typed-basic`, `tuple-*`, `fn-type-*`, `hrt-*`,
`hkt-stdlib-*`).  Root cause: **a `TY_APP` parameterized-struct receiver
is NOT uniformly carrier-represented.**  `Option__int` / `Result__T__B` /
`Tuple2__..` frequently arrive *by value* (their by-value specs, value-
struct payloads, tuple accessors), and the blanket `TY_APP` cast wrongly
forced `((Option *)(intptr_t)v)->is_some` on an already-by-value struct.
The `TY_STRUCT` path avoided this because the Path A.2 override
(`current_abi_specialization->arg_types[]`) reliably proved by-value-ness;
for `TY_APP` the same override does not catch the many non-spec / value-
struct sites, so the default-to-carrier-cast broke them.

Experiment reverted.

### Recommendation

ABI-aware field-access lowering is still the right *direction*, but it
**cannot** key off the static `TY_STRUCT`/`TY_APP` kind alone -- that is
the empirically-confirmed trap (Finding 4).  It needs a
**representation-precise predicate**: cast through the carrier typedef
only when the receiver's actual C representation at this site is the int64
carrier, not a by-value struct.  Sketch of what that predicate must
consult, in priority order:

1. the active spec's `arg_types[]` (already done for `TY_STRUCT` via the
   Path A.2 override) -- generalize it to resolve `TY_APP` receivers too;
2. for non-`EX_VAR` / non-spec receivers, the expression's *emit-time*
   representation (whether `emit_value` produced a carrier int64 or a
   by-value struct) rather than its static `Type.kind`.

(2) is the hard part: today field access reads `struct_expr->type.kind`,
which conflates the two representations a `(Vec A)` value can have.  A
clean fix likely threads a "this value is carrier-represented" bit out of
`emit_value` (or a helper like `expr_emits_byvalue_carrier_abi`, already
used by the bridge machinery) and gates the cast on it.  That is a
real -- but bounded -- emit refactor, and the correct scope for it is its
own focused change, NOT a one-line `TY_APP` add.

The two fallback options if the representation-precise predicate proves
too invasive:

- **B -- two-body instance emission**: emit the carrier base from a
  carrier-helper body and the spec from a by-value-helper body.  Removes
  the need for ABI-aware field access but doubles the instance source or
  needs an ABI-conditional body selector in `emit_typeclasses.c`.
- **C -- drop the uniform dict carrier base** (full monomorphization,
  the M-plan endgame): the abstract-element dispatch path stops existing,
  so by-value helpers never need a carrier base.  Biggest blast radius;
  belongs to the later M-phases, not this retirement.

### Finding 5 -- a representation-precise field-access predicate IS clean, but does not unblock on its own

The follow-up gated the carrier cast precisely: cast a `TY_APP` receiver
only when it is a carrier-represented `EX_VAR` param (its
`emit_byvalue_carrier_abi` flag is false -- its C type is the int64
carrier).  By-value `TY_APP` receivers (flag true) keep direct
`.field`.  This is the representation-precise form Finding 4 called for,
and it is correct: the `Eq Vec` probe compiles bridge-free, the carrier
base derefs, and `vec-of-tvec-eq-manual` links.

**But the suite still regressed** -- and crucially, the regression is NOT
the field-access change.  Reverting `emit_expr.c` and keeping ONLY the
stdlib `Eq Vec` rewrite still fails `list-basic` et al.  So the field-
access predicate is sound; the blocker is elsewhere (Finding 6).

### Finding 6 -- the real blocker: the Eq Vec rewrite drops UNRELATED sibling specs

Bisecting the stdlib change (each step rebuilds + checks `list-basic`):

- add the three `*-byval` helpers only, `Eq Vec` UNCHANGED -> `list-basic`
  passes.  Helper *existence* is harmless.
- rewrite the `Eq [Vec]` instance body to call `vec-eq-loop-byval`
  (a constrained-poly helper) -> `list-basic` FAILS, and the emitted C
  has **no `thead__spec`** at all (`thead` is an unrelated Cons accessor,
  `(.head l)`).  The call site builds `Cons__int` by value and passes it
  to the carrier base `thead(int64_t)` -> cc type error.

So composing one instance-method body through a constrained-poly helper
causes a sibling spec that used to be minted (`thead`/`ttail`/`unwrap`/
tuple accessors) to be dropped.  Filed as
`docs/archive/history/m5-eq-vec-byval-rewrite-drops-sibling-specs.md`.

**Root cause now PINNED** (see the report): it is NOT a worklist
ordering/collision/capacity issue.  Elaboration is correct and identical
(`(thead l)` is saved with `n_abi_bindings = 1` in both trees).  The
rewrite triggers a **post-elaboration AST node duplication**: emit scans
a *copy* of the `(thead l)` node whose `call_.abi_bindings` were not
carried over (`n_bindings == 0`), so `emit_abi_register_call` early-
returns and never interns `thead__spec`.  Node-pointer trace:
`SAVE node=072c8 n=1` (elab) vs `ENTRY node=cf558 n=0` (emit) for the one
source-level call; in the clean tree both are the same node (`n=1`).  The
fix is to propagate `abi_bindings`/`n_abi_bindings` across whichever
emit-phase body/items copy produces that duplicate.  This retro-explains
gap 4's hamt-delete regressor and the earlier reverts as the same
node-duplication fragility, not three separate worklist bugs.

### Revised recommendation

The field-access work (Findings 1-5) is solvable and the
representation-precise predicate is the right shape -- but it is **moot
until the spec-worklist fragility (Finding 6) is fixed**.  The Eq Vec
rewrite cannot land while changing one instance body silently drops
unrelated specs.  Correct order of operations for a future session:

1. **First**, fix the spec-interning worklist invariant (Finding 6 /
   the filed report): an instance-method body composing through a
   constrained-poly helper must not change whether sibling defns get
   their by-value specs.  Instrument `emit_abi_register_call` for
   `thead`'s binding with vs without the Eq Vec rewrite -- determine
   whether it reaches the intern path or `emit_abi_note_carrier_call`,
   and why the rewrite flips it.
2. **Then**, land the representation-precise field-access predicate
   (Finding 5) so the by-value helpers are dual-ABI.
3. **Then**, rewrite `Eq Vec` / `Eq Cons` + regen snapshots in one PR.

Until step 1 lands, the residual L2662/L4393 straddle stays; gap 4's fix
(committed) remains the standalone enabler.

Nothing was landed in this continuation (all experiments reverted); the
deliverable is this fully-mapped design space plus the filed
spec-worklist report.

## Update 2026-06-14 (session 2, attempt 3): D-lite Eq Vec rewrite hits a 4th gap

With the bridge-side strip fixed, attempted a less-ambitious Eq Vec
rewrite using verified patterns:

```turmeric
(defn vec-eq-loop-byval [A]
  [(Eq A)]
  [x : (Vec A) y : (Vec A) i : int len : int]
  #{ByVal}
  : bool
  (if (= i len)
    true
    (if (eq? (:: (vec-get (:: x :int) i) A) (:: (vec-get (:: y :int) i) A))
      (vec-eq-loop-byval x y (+ i 1) len)
      false)))

(definstance Eq [Vec]
  [(Eq A)]
  (eq? [x y]
    (let [lx (vec-len (:: x :int))
          ly (vec-len (:: y :int))]
      (if (= lx ly)
        (vec-eq-loop-byval (:: x (Vec A)) (:: y (Vec A)) 0 lx)
        false))))
```

`vec-eq-loop-byval` works correctly when called from non-instance
contexts (verified in m5-spec-body-ascription-bridge fixture).
Called from Eq Vec's spec body, NO byvalue spec is interned for
vec-eq-loop-byval -- only its int64 carrier base is emitted, and the
Eq Vec spec body passes Vec__int (by value) into the int64 carrier
formals -> cc error.

Trace via instrumented `emit_abi_register_call`:

- Reaches register_call for `(vec-eq-loop-byval xv yv 0 lx)` under
  Eq Vec's spec context (DBG "vebs register_call ENTRY:
  spec=__inst_Eq_eq_qu_Vec__spec... n_bindings=1").
- Pre-gate state: `abi_changes=0`, `arg_types[0].kind=21 (TY_APP)`,
  `c_name=int64_t`.

The composition through Eq Vec's outer spec ({Eq Vec's A -> int})
isn't producing concrete arg_types for vec-eq-loop-byval -- (Vec A)
stays abstract, c_name falls back to int64.  Almost certainly a
Symbol-identity issue between Eq Vec's class-var A and
vec-eq-loop-byval's own callee A (the call's `abi_bindings` maps
`{A_callee -> A_outer}` but the composition pass doesn't recognize
A_callee as needing concrete substitution because the Symbol names
don't line up in the expected way).

**This is the 4th independent gap** uncovered along the Eq Vec rewrite
path:

1. (FIXED) Bridge-side EX_ASCRIBE strip at emit_expr.c:2419
2. (FIXED) Wrong-instance dispatch on EX_ASCRIBE-to-tyvar receiver
3. (FIXED) SEGV at elab_typeclasses.c:3388 (NULL def)
4. (NEW) Composition pass doesn't substitute callee tyvars to concrete
   under instance-method outer spec when callee is a sibling
   constrained-poly defn (Symbol-identity issue)

The composition pass IS used by today's `vec-eq-loop-spec-probe`
fixture (which works) -- so the failure is specifically when the
outer spec is an INSTANCE METHOD spec (`__inst_Eq_eq_qu_Vec__spec__...`),
not a regular per-defn spec.  Plausibly the instance-method's
abi_bindings get attached with a different shape.

Reverted the Eq Vec rewrite.  The bridge-side fix from earlier this
loop stays in tree -- it's a real bug fix with independent value
(pinned by m5-spec-body-ascription-bridge fixture).

## Update 2026-06-14 (session 2, follow-up): Bridge-side strip FIXED

The bridge-side gap traced in this session turned out to be an
**emit-side EX_ASCRIBE strip**, not an elab-time AST transform.  At
`emit_expr.c:2419`, the call-arg emit path stripped all EX_ASCRIBE
wrappers BEFORE invoking emit_value -- so the L4393 CK_CONCRETE ->
CK_CARRIER bridge inside the EX_ASCRIBE handler never got a chance
to fire for call arguments.

Fix: a narrow gate that preserves EX_ASCRIBE wrappers when they
match the L4393 bridge's firing conditions (target=TY_INT, inner is
a by-value carrier-ABI EX_VAR, inner's elab-time type uses carrier
ABI).  Now `(:: x :int)` on a by-value `Vec__int` spec param spills
to a temp and yields `(int64_t)(intptr_t)(&__t)` as expected.

Pinned by `tests/fixtures/m5-spec-body-ascription-bridge/`.  gap2b's
constrained-poly defn with parametric receivers + direct `(eq? ...)`
dispatch now compiles AND runs correctly end-to-end (exit=0).

This UNBLOCKS the broader Option D direction for ANY constrained-poly
helper that uses `(:: param :int)` to bridge by-value spec params
into carrier-ABI helpers.  The `Eq Vec` rewrite specifically still
hits the "single-body-two-ABIs" wall (Update below) because its
body shape requires `(.len x)` to work for both the carrier base
(x: int64) AND the spec (x: Vec__int).  That's an architectural
design choice for the Eq Vec definstance, not a bridge issue.

## Update 2026-06-14 (session 2): Bridge-side trace, partial progress (SUPERSEDED by follow-up above)

After landing the M5 elab fix for wrong-instance dispatch on
EX_ASCRIBE-to-tyvar receiver
(`docs/archive/history/m5-constrained-poly-wrong-instance-on-tyvar-receiver.md`),
attempted to push further into the bridge-side gap that prevents
gap2b's `vec-eq-loop-byval` spec body from compiling.

Concretely, the spec body emits

```c
return __inst_Eq_eq_qu_int(vec_hyget(x, i), vec_hyget(y, i));
```

with `x: Vec__int` by value passed to `vec_hyget(int64_t v, ...)` —
cc type error.  The source has `(:: x :int)` ascriptions around `x`
and `y` that should fire the CK_CONCRETE → CK_CARRIER bridge at
`emit_expr.c:4393`.

Trace finding: the EX_ASCRIBE node for x/y **never reaches the emit
handler** for vec-eq-loop-byval's spec body.  Instrumented at the
EX_ASCRIBE case entry; only Eq Vec's carrier-base body fires it
(twice, for its own `(:: x :int)` / `(:: y :int)`).  The
vec-eq-loop-byval spec body's ascriptions have been transformed or
elided at elab/AST time — they don't survive to emit.

Tried extending the `emit_byvalue_carrier_abi` flag setter at
`emit_fns.c:617-624` to recognize the ORIGINAL elab-time TY_APP
param type (so the bridge gate's `expr_emits_byvalue_carrier_abi`
check passes for spec params).  Compiled and tested; no effect on
the bridge because the bridge ITSELF doesn't run for this body.

Reverted the unproductive change.  The real fix needs to address
the earlier AST transformation — probably in `elab_call.c`'s arg-
coercion path, where `(:: x :int)` passed as `vec-get`'s `v:int`
formal gets rewritten or elided.  Hours of additional elab-side
trace needed; not in scope for this session.

## Update 2026-06-14: Option D execution found a deeper wall

Execution of Option D in this session ran into a structural constraint
not visible in the original plan:

The `(definstance Eq [Vec])` body serves BOTH the carrier base
(`__inst_Eq_eq_qu_Vec(int64_t, int64_t)`) AND the Path A spec
(`__inst_Eq_eq_qu_Vec__spec__bool_Vec__int_Vec__int(Vec__int, Vec__int)`).
Both share one Turmeric source body.  For the spec body, `(.len x)` on
by-value `Vec__int` is correct.  For the carrier base, `(.len x)` on
int64 carrier is a hard cc error.  The same helpers (`vec-get-byval`,
`vec-eq-loop-byval`) cannot be called from both ABIs of the same body
because `#{ByVal}`'s `prefer_byvalue_spec` flag suppresses the carrier
base of the helpers (via `emit_abi_fn_skip_generic`) -- correct for
the spec-only use, but it leaves the carrier base of the instance
method with unresolved-symbol calls.

Additionally, three latent elab gaps surfaced and were filed under
`docs/reported/`:

- `m5-eq-vec-rewrite-fn-arg-loses-annotation.md` (gap 1): untyped
  lambda inside a plain polymorphic defn loses its `(fn [A A] bool)`
  expected-type vs. inside a definstance body where the lambda
  inherits TY_TYVAR(A) from the surrounding context.  Mechanism
  unidentified.  The diagnostic-message half is a one-line fix.
- `m5-constrained-poly-wrong-instance-on-tyvar-receiver.md` (gap 2-
  followup): a `(eq? (:: <int> A) (:: <int> A))` inside a constrained-
  poly defn dispatches to `__inst_Eq_eq_qu_MutableMap` (silent
  miscompile + SIGSEGV).  Real constraint-dispatch infra missing.
- The original `elab_typeclasses.c:3388` SEGV — **FIXED** this session.

Conclusion: Option D as a tactical clearance is NOT reachable without
also addressing the constraint-dispatch gap and the
single-body-two-ABIs design choice for instance methods.  Both are
multi-session pieces of elaboration infra work.

The composition fix + `#{ByVal}` marker landed this session ARE useful
on their own (they're prerequisites for any eventual byval-helper
migration) and are pinned by the
`tests/fixtures/m5-byval-marker-spec-emit/` fixture.  But the
audit's M3 deletion remains blocked by the same bridge sites; the
straddle persists.

The plan below (Option D detail) is preserved for reference but
should be considered superseded -- the right next step is either:

1. Diagnostic fix for gap 1 (one-liner, ships independently).
2. Designed approach for the single-body-two-ABIs question -- either
   per-instance-method ABI-conditional body emission, or moving Eq
   Vec to an entirely-by-value design with no carrier base at all
   (which requires every dispatch site to be ABI-aware).

## Recommendation (original; see Update above)

**Option D as the immediate step**; **Option C as the eventual
M5 generalization**.

Rationale:

- Option D unblocks M3 deletion mechanically within one session
  (estimated 2-3 hours, including fixture pinning).  The audit's
  framing -- "the bridge count drops to 0 only after M5 retires the
  residual straddle" -- becomes literally true once Option D lands,
  because L2662 and L4393 are the only `CK_CONCRETE -> CK_CARRIER`
  sites that exist purely to serve the M4c-pre-ext Path A spec
  bodies.  The remaining bridges (L1792, L2554, L2618 and the
  EX_ASCRIBE pair) are HKT / typeclass / pre-existing and
  documented in §7.

- Option C is the right shape for the broader "constrained-
  polymorphic defns flow through monomorphized helpers"
  trajectory the plan calls M5.  But it needs the inline-C body
  rewriter, which is a separate piece of infrastructure and the
  audit defers it to M5's worklist generalization phase.  Doing it
  here under the "residual straddle" banner conflates two pieces
  of work.

## Plan (Option D)

### D.1 -- Add `vec-len-byval` and `vec-eq-loop-byval` to stdlib/vec.tur

```turmeric
(defn vec-len-byval [A] [v : (Vec A)] : int
  (.len v))

(defn vec-eq-loop-byval [A]
  [x : (Vec A) y : (Vec A) i : int len : int]
  : bool
  (if (= i len)
    true
    (if (eq? (.get x i) (.get y i))    ; field index via .data
      (vec-eq-loop-byval x y (+ i 1) len)
      false)))
```

Question for D.1: does the elaborator already accept `(.field x)`
for a parameterized struct receiver, including when `x` is a
Path A spec param (the binding's type is the elab-time
unparameterized `Vec`, but the spec resolves to `Vec__int`)?
Spot-check needed; if not, the field-access path needs a small
elab fix first.

### D.2 -- Rewrite `(definstance Eq [Vec])` body

Drop the `(:: x :int)` / `(:: y :int)` ascriptions; call
`(.len x)` / `(.len y)` directly; call
`(vec-eq-loop-byval x y 0 lx)` instead of the int64
`vec-eq-loop`.  Same for `Eq Cons`.

### D.3 -- Verify the L2662 and L4393 bridges stop firing

Add a probe fixture that emits the M4c-pre-ext shape; confirm the
emitted C for the Path A spec body does no `(int64_t)(intptr_t)(&...)`
spills.  Compare against today's emitted C as a snapshot.

### D.4 -- Tighten bridge predicates

With the producer sites gone, the L2662 and L4393 branches in
emit_expr.c can be deleted outright (or shrunken to a TODO comment
if a future helper still needs them).  Suite must stay green and
snapshots regenerate cleanly.

### D.5 -- M3 unblock

Once D.1-D.4 land and no carrier-bridge call uses
`CK_CONCRETE -> CK_CARRIER`, the symmetric `CK_CARRIER ->
CK_CONCRETE` accessor-side path (which M3 targets) can be reviewed
for deletion.  M3 itself is out of scope for this doc; it gets
its own execution plan once Option D ships.

## Validation harness

Per the project's STRICT RULE: `bash tests/run.sh 2>&1` must
report the same FAIL set as the pre-change baseline.  Any
snapshot drift gets regenerated in the same PR.

End-to-end probe:

```turmeric
(defn check-eq-vec [A] [(Eq A)] [x : (Vec A) y : (Vec A)] : bool
  (eq? x y))
(defn main [] : int
  (let [a (:: (vec-of) (Vec int))
        b (:: (vec-of) (Vec int))]
    (vec-push! a 1) (vec-push! b 1)
    (if (check-eq-vec a b) 0 1)))
```

After Option D, the emitted C for the spec body
`__inst_Eq_eq_qu_Vec__spec__bool_Vec__int_Vec__int(Vec__int x,
Vec__int y)` should contain no `(int64_t)(intptr_t)(&...)` spill.

## Risks

- `(.field x)` on Path A spec params: needs spot-check.  If the
  elaborator doesn't already resolve the field through the spec's
  arg type, that's a small fix (analogous to the
  `find_matched_abi_spec` change this session).
- `Eq Cons` field access: Cons is a recursive struct.  The
  pure-Turmeric loop already exists from M4c-pre-ext; verifying
  the `.head` / `.tail` field accesses work on the spec param is
  the same spot-check.
- Snapshot churn: a stdlib rewrite touches many fixtures.  Per the
  fixture STRICT RULE, regen and commit alongside.

## Out of scope (deferred to M5 proper)

- Option C's inline-C body rewriter for arbitrary stdlib helpers.
  That's the worklist generalization that lets ANY carrier helper
  fall out of a by-value spec body automatically.  Useful for
  the wider M5 north-star but not required to retire the
  M4c-pre-ext-specific straddle.
- HKT (`Functor`, `Monad`) instance method dispatch: M6 / M7.
- Closure-env typed-thunk flow for Path A spec captures
  (`(fn [a b] (eq? a b))` inside `Eq Vec`'s body): the inner
  closure-spec system handles this today; revisit if D.2 surfaces
  a regression.

## Estimated effort

- D.1-D.2: ~1 hour (stdlib rewrites + one elab spot-check).
- D.3: ~30 minutes (fixture + emitted-C inspection).
- D.4: ~30 minutes (deleting branch + bridge predicate cleanup).
- D.5: out of scope for this doc.
- Total: 2-3 hours for D.1-D.4.

## North star

This doc retires only the M4c-pre-ext straddle.  The broader M5
ambition -- a constrained-polymorphic defn over `(Vec A)` /
`(Map K V)` / etc. monomorphizing through helpers without any
carrier round-trip -- still needs Option C (or equivalent) to
land properly.  Option D is a tactical clearance that unblocks
M3 and shrinks the bridge predicate surface; it does not by
itself complete M5.
