# :heap ADT field read at by-value aggregate element -- the nested-by-value-monomorph cascade

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a
default-path bug). 2 fixtures direct, ~4 in the blast radius.

## One-line summary

A direct field read on a `:heap` record-ADT receiver whose element is a by-value
aggregate -- `(.head xs)` / `(.tail xs)` where `xs : (Cons (Option int))` and
`Cons` is `(defstruct Cons :heap [A] (head A) (tail :int))` -- mislowers because
the compiler does not consistently treat `(Cons (Option int))` (and its nested
`(Option int)` element) as a concrete by-value monomorph.  Making it consistent
is NOT a leaf fix: the "is this a concrete by-value ADT-app" decision is consumed
by a CASCADE of codegen sites, and flipping a NESTED ADT-app element to by-value
exposes each one in turn.

## Minimal repro

`tests/fixtures/list-length-byvalue-aggregate-element`,
`tests/fixtures/list-homog-byvalue-aggregate-element` under the force-lower probe.

```turmeric
(defn opt-or [o : (Option int) d : int] : int (if (.is-some o) (.value o) d))
(defn main [] : int
  (let [xs (:: (list (some 42) (some 7)) (Cons (Option int)))]
    (println (.head xs)) ...)            ;; conversion to non-scalar type
  0)
```

Both pass at default (where `Cons`/`Option` are int64-carrier heap handles).

## The cascade (mapped 2026-06-27)

The root knob is `adt_app_is_byvalue_product` / `type_app_is_concrete_adt`
rejecting a nested ADT-app ARG: `type_has_concrete_codegen_layout((Option int))`
is false (its `TY_APP` arm only handles struct-apps), so `(Cons (Option int))` is
not a by-value product and `type_c_name` collapses it to int64.  The instinct is
to teach the predicates to accept a nested by-value ADT-app element.  Doing so
fixes the two list fixtures but cascades:

1. **Constructor selection** (emit_expr.c N-arg ctor branch via
   `type_app_is_concrete_adt`): without the nested-arg fix the `tcons-of` spec
   calls the GENERIC `ctor_Cons` (a 16-byte `{int64 head; int64 tail}` cell)
   while the reader uses the monomorph `tur_adt_Cons__Option__int` (whose `tail`
   is at offset 16) -> **heap-buffer-overflow** (confirmed via ASan: read past
   the 16-byte `ctor_Cons` allocation).  Fix: `type_app_is_concrete_adt` accepts
   a nested by-value ADT-app arg.

2. **Field read** (emit_expr.c EX_GET_FIELD): the ADT branch has no
   `:heap`-ADT-receiver case (the `adt_recv_byvalue` branch is gated on
   `emit_type_is_byvalue_adt`, false for `:heap`), so it falls to the generic
   carrier cast.  Fix: a `heap_adt_recv` branch that casts to the monomorph
   pointer (`tur_adt_Cons__Option__int *`) and reads the inline aggregate field.

3. **Nested typedef ordering** (types.c `emit_registered_adt_app_rec`): the
   monomorph cell `tur_adt_Cons__Option__int { ... tur_adt_Option__int _0; ... }`
   references the nested monomorph typedef, which `adt_field_c_type` registers
   only WHILE this typedef's field loop runs -- too late to precede it ->
   `unknown type name 'tur_adt_Option__cstr'` / `'tur_adt_Tuple2__cstr__int'`.
   Fix: a dependency pre-pass (mirroring the struct-app emitter) that recursively
   emits each nested by-value ADT-app/struct-app field typedef first.  (This one
   is a pure improvement and fixed `tuple-type-bracket-sugar`.)

4. **Canonical carrier-box readback, named-field path** (emit_core.c
   `emit_carrier_bridge`, the `tur_option_t`/`tur_result_box_t` reconstruction):
   a nested WIDE by-value field (`(Result (Option int) cstr)`'s ok_val holding a
   16-byte boxed Option) was C-cast int64->aggregate.  Fix: deref-unbox
   (`*(tur_adt_Option__int *)(intptr_t)(box->ok_val)`).  NOTE this path is
   struct-app-only (gated on `type_extract_struct_app` + Option/Result name), so
   it does NOT cover a LOWERED ADT Result.

5. **Carrier-box readback, ADT positional path** (NOT yet located): a lowered
   record-ADT Result reconstructed in a `#{Construct}`/instance spec body emits
   `(tur_adt_Result__Option__int__cstr){.as.Result._1 = (tur_adt_Option__int)
   (__t->ok_val), ...}` -- the same int64->aggregate cast, but via the positional
   `.as.<Ctor>._N` form, not the named canonical readback in (4).  This is the
   remaining build error for `nested-construct-byvalue-decode`.

6. **`constrained-loop-vec-push-byvalue-result-element`**: a SEPARATE deeper
   fixture (its header documents three combined defects -- nested return-dispatch
   redirect mistyping `dec`'s seed, the return-only-poly accessor, and the
   vec-push carrier bridge).  Flipping the nested app to by-value turns its
   prior runtime segfault into a build error; fully fixing it needs the
   return-dispatch work, out of scope here.

Sites 1-4 are implemented and correct in isolation; 5 and 6 remain.  Because the
nested-by-value flip is global (it changes `type_c_name` / `adt_app_is_byvalue_
product` for EVERY consumer), a partial landing regresses `nested-construct-
byvalue-decode` (site 5) -- which passes today -- so the cluster must land all of
1-5 atomically (and accept 6 as a separately-tracked residual).

## Why NOT broaden `type_has_concrete_codegen_layout` globally

The tempting one-liner -- give its `TY_APP` arm an ADT-app case -- regresses 9
DEFAULT fixtures: a MULTI-variant recursive-functor app `(ExprF Expr)` then gets
named `ExprF__Expr` by `type_c_name`'s `register_struct_app` fallback but is never
emitted (`unknown type name`).  Gating on `adt_app_is_byvalue_product`
(single-variant flat product) avoids the default regressions but still drives the
force-lower cascade 1-6 above; measured net was 30 -> 30 (cleared list-length /
list-homog / tuple-type-bracket-sugar, regressed nested-construct-byvalue-decode /
constrained-loop) until sites 4+5 are both done.

## Fix direction (for a dedicated pass)

Land sites 1-5 together: the two predicate fixes (`adt_app_is_byvalue_product`
and `type_app_is_concrete_adt` accept a nested by-value ADT-app arg/field), the
`heap_adt_recv` EX_GET_FIELD branch, the `emit_registered_adt_app_rec` dependency
pre-pass, and BOTH carrier-box readback paths (named + ADT-positional)
deref-unboxing a nested wide by-value field.  Verify the default suite (esp.
`hkt-cata-*`, which must stay carrier) and re-sweep force-lower.  Track
`constrained-loop-vec-push-byvalue-result-element` (site 6) separately under its
own return-dispatch report.

## Notes

- Default suite is unaffected by the bug (only the lowered representation triggers
  it); the danger is entirely in the fix's blast radius.
- Distinct from the landed vec-element carrier<->by-value read bridge (an
  inline-C generic-result deref) and the by-value-ADT `any` box/unbox.
