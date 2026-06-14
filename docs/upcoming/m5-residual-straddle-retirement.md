---
title: M5 Residual Straddle Retirement
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: Retire the two CK_CONCRETE -> CK_CARRIER bridges introduced by M4c-pre-ext so the bridge call count can drop and M3 (delete `emit_carrier_bridge`'s accessor-side path) becomes mechanical. Per audit §10.8 and the end-to-end monomorphization plan's M5 phase.
---

# M5 Residual Straddle Retirement -- Plan

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

## Update 2026-06-14 (session 2): Bridge-side trace, partial progress

After landing the M5 elab fix for wrong-instance dispatch on
EX_ASCRIBE-to-tyvar receiver
(`docs/reported/m5-constrained-poly-wrong-instance-on-tyvar-receiver.md`),
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
