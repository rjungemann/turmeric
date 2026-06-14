---
title: Instance-method Path A spec doesn't propagate constraint-var bindings to callee specs
severity: FIXED 2026-06-14 (session 4) -- a constrained-poly helper called from a typeclass-instance method body now monomorphizes per receiver element type; pinned by tests/fixtures/m5-instance-spec-constraint-var/
date: 2026-06-14
---

## Update 2026-06-14 (session 4): FIXED

The two-part remediation in the session-3 update turned out to be a
*three*-part coordinated fix, and the hamt-delete regression was an
artifact of doing part A at the wrong site (the dispatch-call's
abi_bindings, which flow into *every* instance method's own spec).
Doing the equivalent derivation **emit-side, scoped to the active
instance-method spec only**, sidesteps that regression entirely.

The three coordinated changes:

1. **`elab_typeclasses.c` (definstance constraint parse)**: record each
   constraint var's `Symbol` in `TypeConstraint.tyvar` (both the
   vector-of-lists and the flat constraint-vector branches).  Previously
   `.tyvar` was left NULL, so the emit side had no name to key the
   constraint var on.

2. **`elab_types.c` + `elab_internal.h` + `elab_typeclasses.c` (pass 2)**:
   add an ambient `inst_body_type_params` scope on `Elab`, populated with
   the instance's constraint vars during method-body pass 2, and consult
   it in `type_expr_from_form`'s symbol fallback.  This makes an
   ascription `(:: x (Vec A))` inside an instance body resolve `A` to a
   NAMED `TY_TYVAR` rather than the anonymous `TY_STRUCT{def=NULL}` that
   the session-3 trace flagged (`call.bindings[0]: kind=18 TY_STRUCT`).
   That named identity is what lets the emit composition substitute by
   name.

3. **`emit_module.c` (`emit_abi_register_call` composition pass)**: when
   the active specialization is an instance-method spec
   (`typeclass_inst` set, `fn->owner_instance` available), derive the
   constraint vars' concrete resolutions from the receiver's resolved
   `TY_APP` (the class-var binding) -- `param_idx` indexes its elem
   types -- and splice `{A -> int}` onto the spec's bindings *for the
   composition pass only*.  It does not change the spec's clone name or
   identity, so it cannot collide with sibling instance specs (the
   hamt-delete regressor).

With all three in place the call's binding `{A -> TY_TYVAR("A")}`
composes against the augmented spec `{a -> (Vec int), A -> int}` to
yield `Vec__int`, and the callee's by-value spec is interned.

**Validation**: `/tmp/g4-instance.tur` (the motivating repro) compiles
and exits 0; the regular-outer control still works; full suite is
`1621 passed, 4 failed` with the same 4 pre-existing failures as
baseline (hkt-stdlib-option-result-instances, rt-return-dispatch-basic,
and two errors/rt-* diagnostic-text mismatches -- all reproduce with the
change reverted).  **hamt-delete passes** (the session-2/3 regressor).
Pinned by `tests/fixtures/m5-instance-spec-constraint-var/`.

---

(Historical session-1..3 material below.)

## Update 2026-06-14 (session 3): regressor isolated AND fix shown insufficient

Re-attempted the principled fix today: at
`elab_typeclasses.c:4123` (receiver-dispatch spec-intern site), extend
`abi_bindings[]` to also record `{constraint.tyvar->name -> elem-type}`
for each `TypeConstraint` with `param_idx >= 0`, by walking
`obj_orig_type`'s TY_APP arg chain.

**Suite baseline vs patched** (compare `/tmp/m5-baseline.fails`,
`/tmp/m5-gap4.fails`):

- baseline: 1568 passed, 87 failed
- patched: 1567 passed, 88 failed
- diff: **+1 hamt-delete** (the SAME regressor as the prior attempt --
  not a different test, and not a build-ordering artifact).

**End-to-end check on the motivating case (g4-instance.tur)**:
still fails with the same cc error -- `callee(int64_t, ...)` called
with `Vec__int` args.  The principled fix at the elab site does NOT
unblock the case on its own.

### Why the fix is insufficient

Trace evidence from the prior session:

```
call.bindings[0]: name=A type.kind=18 (TY_STRUCT, def=NULL -- abstract tyvar)
```

The call to `callee` from inside `myeq?`'s spec body has its
abi_bindings recorded by `elab_call.c` (not by my elab_typeclasses
fix).  callee's binding type is `TY_STRUCT(NULL, name="A")`, NOT
`TY_TYVAR(name="A")`.

`emit_module.c:570 emit_abi_instantiate_type` only substitutes by name
on the `TY_TYVAR` case.  For `TY_STRUCT(NULL)` it returns the type
unchanged.  So even when the outer spec's bindings carry the correct
`{A -> int}` (as my fix arranges), the composition pass fails to use
it because the call's binding type doesn't match the substitution
shape.

The principled fix at the outer spec-intern site is **necessary but
not sufficient**.  To actually unblock g4-instance, either:

1. **Elab side**: fix `elab_call.c`'s binding capture for calls inside
   instance-method bodies so the call's binding type is recorded as
   `TY_TYVAR(name="A")` rather than `TY_STRUCT(NULL, name="A")`.  Find
   where the constraint var `A` is parsed as a TY_STRUCT instead of a
   TY_TYVAR -- probably the elab type-name resolution at the instance
   body scope doesn't have `A` in the type-var environment.

2. **Emit side**: extend `emit_abi_instantiate_type`'s TY_STRUCT case
   to substitute by-name when the struct has NULL def and a non-NULL
   name string.  Broader change, higher regression surface.

(1) is more principled.  (2) is more localized.  Either needs to be
combined with the elab_typeclasses fix to land both halves coherently.

### hamt-delete regressor: not yet root-caused

Despite hamt-delete using only primitive-typed Eq instances (no
constraint vars), the extended outer bindings still affect it -- likely
through a downstream spec-name collision when the extra binding makes
`emit_abi_clone_name` produce a different mangled name for some HAMT
internal.  Pinning the exact mechanism requires per-spec emit tracing
that exceeds this session's budget.

### Conclusion

Gap 4 is real, the proposed fix is structurally correct, and a single
end-to-end attempt is NOT enough to land it.  The work is at minimum a
two-part change (elab_call + elab_typeclasses, OR emit_module +
elab_typeclasses) plus regression triage for hamt-delete.  Estimated
4-8 hours of focused trace + iterate.

The fix is reverted; the report stays open as the canonical place to
resume from.  Prior attempt material from session 2 below for context.

## Update 2026-06-14 (session 2): Likely subsumed by bridge-side fix; attempted fix had marginal regression

Subsequent investigation (same session) showed that the original
repro shapes (gap2b and g4-instance) actually compile and run
correctly with just the earlier bridge-side EX_ASCRIBE strip fix
in place.  The "no spec interned for callee" symptom may have been
specific to a partial state or build ordering.

An attempted fix that extended `elab_typeclasses.c`'s instance-method
abi_bindings to also record constraint-var bindings (derived from
`recv_def->type_params[param_idx]`) was implemented and reverted:

- Compiled cleanly, didn't change emission for any tested case.
- Caused a net regression of 1 in the suite (1566/88 with the fix
  vs 1567/87 without) -- one test that was passing started failing.

The patch is principled (records info that's logically in scope at
the spec-intern site) but apparently affects something subtler than
predicted in the spec-matching / interning chain.  Future fix
attempts should isolate the regressing test first.

## Summary

When an instance method's Path A spec body calls a sibling constrained-
polymorphic defn, no per-receiver-type spec gets interned for the
callee.  The callee's int64 carrier base ends up called with by-value
spec params (e.g. `callee(Vec__int, ...)` against `callee(int64_t,
...)`) — cc type error.

The same pattern works correctly when the OUTER caller is a regular
polymorphic defn (not an instance method) — confirmed by direct
side-by-side trace.

## Repro

```turmeric
(defn callee [A]
  [(Eq A)]
  [x : (Vec A) y : (Vec A) i : int len : int]
  : bool
  (if (= i len)
    true
    (if (eq? (:: (vec-get (:: x :int) i) A) (:: (vec-get (:: y :int) i) A))
      (callee x y (+ i 1) len)
      false)))

(defclass MyEq [a] (myeq? [x y] : bool))

(definstance MyEq [Vec]
  [(Eq A)]
  (myeq? [x y]
    (callee (:: x (Vec A)) (:: y (Vec A)) 0 (vec-len (:: x :int)))))

(defn main [] : int
  (let [a (:: (vec-of) (Vec int))
        b (:: (vec-of) (Vec int))]
    (vec-push! a 1) (vec-push! b 1)
    (if (.myeq? a b) 0 1)))
```

Build: `error: passing 'Vec__int' to parameter of incompatible type 'int64_t'`

Control (works):

```turmeric
(defn callee ...same as above...)

(defn outer [A]                          ;; regular outer, not instance
  [(Eq A)]
  [x : (Vec A) y : (Vec A) len : int]
  : bool
  (callee x y 0 len))

(defn main [] : int
  ...callers identical...
  (if (outer a b 1) 0 1))                ;; compiles + runs
```

## Trace (instrumented `emit_abi_register_call`'s composition pass)

**WORKING (regular outer)** — when callee is called from outer's
Path A spec body:

```
DBG g4 callee: outer_spec=outer__spec__bool_Vec__int_Vec__int_int64_t
  call.bindings[0]: name=A type.kind=36 (TY_TYVAR)
  outer.bindings[0]: name=A type.kind=3 (TY_INT)
```

Names line up (`A` == `A`), composition substitutes `A → int`,
arg_types[0] becomes Vec__int, by-value spec interned.

**BROKEN (instance method outer)** — when callee is called from
MyEq[Vec]'s Path A spec body:

```
DBG g4 callee: outer_spec=__inst_MyEq_myeq_qu_Vec__spec__bool_Vec__int_Vec__int
  call.bindings[0]: name=A type.kind=18 (TY_STRUCT, def=NULL — abstract tyvar)
  outer.bindings[0]: name=a type.kind=21 (TY_APP, (Vec int) — the class var resolution)
```

The outer spec tracks the CLASS VAR `a` resolution (= the full
receiver type `(Vec int)`), NOT the CONSTRAINT VAR `A` (= the element
type `int`).  When callee's `A` (which corresponds to the constraint
var) is composed against the outer's bindings, no match exists --
the call's `A → TY_STRUCT(NULL)` representation carries no name to
look up, and even if it did, the outer doesn't have a binding for
the constraint var.

## Root cause

The instance-method Path A spec interner only records the class
var(s) in its `bindings[]` array, with values like `a → (Vec int)`.
Constraint vars introduced by `[(Eq A)]` on the instance are NOT
recorded -- they're class-local tyvars that are supposed to be
resolved through the class-var's type arguments (`(Vec int)` →
element is `int`, so constraint var A in `(Eq A)` resolves to int).

But `emit_abi_register_call`'s composition pass doesn't know to
peek into the class var's type to extract constraint-var bindings.
It just looks up the call's binding name in the outer's bindings,
finds no match, and returns the call's binding type unchanged.

## Proposed fix directions

1. **At spec-intern time**: when interning an instance-method Path A
   spec, ALSO record constraint-var bindings derived from the
   class-var's resolved type args.  E.g. when interning
   `__inst_MyEq_myeq_qu_Vec__spec__bool_Vec__int_Vec__int`, record
   `{a → (Vec int), A → int}` instead of just `{a → (Vec int)}`.

2. **In the composition pass**: when the call's binding is
   `TY_STRUCT(NULL)` and the outer's only binding is the class var
   resolved to a TY_APP, walk the TY_APP's arg-chain to extract
   constraint-var values by position.  Heuristic; fragile.

(1) is the principled fix.  It needs identifying where instance-
method specs are interned in `emit_module.c` and extending the
binding recording to walk the constraint structure.  Estimated
2-4 hours of focused trace + code.

## Workaround

Don't call a constrained-polymorphic helper from inside an instance
method body.  The original `Eq Vec` body (using `(:: x :int)` +
existing carrier `vec-eq-loop` with a `^fat cmp` lambda) sidesteps
this because the carrier helpers don't need by-value specs.

## Validation under fix

`/tmp/g4-instance.tur` should compile and exit=0.
The Eq Vec rewrite that I attempted (which mirrors g4-instance's
shape) would also compile.

## Related

- `docs/upcoming/m5-residual-straddle-retirement.md` — Option D plan
  that this gap blocks.
- `docs/reported/m5-constrained-poly-wrong-instance-on-tyvar-receiver.md`
  — fixed earlier this session; addressed a different but adjacent
  dispatch bug in the same area.
