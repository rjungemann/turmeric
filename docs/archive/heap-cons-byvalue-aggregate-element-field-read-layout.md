# :heap ADT field read at by-value aggregate element -- the nested-by-value-monomorph cascade

**RESOLVED (2026-06-27).**  The fix turned out far narrower than the cascade
below feared, once a TESTING ERROR was corrected: every "fixture X passes at
baseline" measurement in the earlier write-ups had `git stash`-ed the
force-lower probe along with the code, so it was silently testing the
NON-lowered (struct) path.  The TRUE force-lower baseline shows
`nested-construct-byvalue-decode` and `constrained-loop-vec-push-byvalue-result-
element` were ALREADY runtime-segfaulting -- they are not regressions, just
pre-existing breakage in the constrained-instance-body monomorphization (their
own separate work).

So the actual cluster is only the two `:heap` list fixtures, and the fix is
`:heap`-SCOPED: a nested by-value-product element (`(Cons (Option int))`'s
`(Option int)`) is accepted by `adt_app_is_byvalue_product` /
`type_app_is_concrete_adt` ONLY when the outer ADT is `:heap`.  A non-heap nested
aggregate (`(Result (Option cstr) cstr)`) already round-trips via the struct-app
monomorph path, and flipping it to the ADT-app path is what double-represented
the type and broke the constrained-instance specs -- the `:heap` gate avoids it
entirely.  Plus the EX_GET_FIELD `:heap`-ADT-receiver branch reads the field off
the monomorph cell (its c-name lookup gated on the cheap `is_heap_adt` check so
it never registers monomorphs for unrelated non-heap receivers).  No global
`type_has_concrete_codegen_layout` change; the typedef-ordering dep pass (old
"site 3") and the carrier-box readback (old "site 5") turned out unnecessary.

Cleared `list-length-byvalue-aggregate-element` and
`list-homog-byvalue-aggregate-element` (build + run); default suite 1863/0;
force-lower real build failures 30 -> 29.  `constrained-loop-...` shifts from a
runtime segfault to a build error (still failing -- the `:heap` Vec element flip
surfaces its latent 3-defect monomorphization as a build error); it and
`nested-construct-...` remain tracked separately as constrained-instance-body
monomorphization work.  Original report below.

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

5. **Carrier-box readback, ADT positional path** (LOCATED + fixed:
   `emit_carrier_bridge`'s lowered-Option/Result ADT readback, emit_core.c
   ~L3458): a lowered record-ADT Result reconstructed from the canonical box
   emitted `(tur_adt_Result__Option__int__cstr){.as.Result._1 =
   (tur_adt_Option__int)(__t->ok_val), ...}` -- an int64->aggregate cast.  Fix:
   deref-unbox a nested by-value aggregate field UNCONDITIONALLY (the canonical
   box always stores aggregates boxed via the concrete->carrier bridge; the
   `type_is_wide_byval_adt` gate is both wrong here AND only handles bare `TY_ADT`
   not the `TY_APP` element -- and that predicate must NOT be broadened, or the
   cons cell at site 2 flips to boxed element storage).

6. **The flip regresses two currently-WORKING fixtures with deeper defects --
   the actual blocker.**
   - `nested-construct-byvalue-decode` **passes today** (correct output `42 hi
     3.25 99`) because the whole Result/Option chain rides the int64 carrier
     consistently.  With sites 1-3,5 applied its readback is correct, but two
     DEEPER defects that carrier masked then surface: (a) the `ok__spec__...`
     return is treated as a carrier box by its caller while it now returns a
     by-value aggregate, and (b) a genuine monomorphization type error -- the
     Dec-over-Option instance builds `Option__int` where the cstr arm needs
     `Option__cstr` (`some__spec__Option__int` passed to a `_Option__cstr`
     callee).  Both are int64-identical at carrier; neither is in sites 1-6's
     scope.
   - `constrained-loop-vec-push-byvalue-result-element`: its header documents
     three combined defects (nested return-dispatch redirect mistyping `dec`'s
     seed, the return-only-poly accessor, the vec-push carrier bridge).  The flip
     turns its prior runtime segfault into a build error.

Sites 1-5 are implemented and verified -- they cleanly clear `list-length-
byvalue-aggregate-element`, `list-homog-byvalue-aggregate-element`, and
`tuple-type-bracket-sugar`.  But the nested-by-value flip is GLOBAL (it changes
`type_c_name` / `adt_app_is_byvalue_product` for every consumer), so it also
regresses the two fixtures in (6) -- one of which PASSES today.  Net build count
would be 30 -> 29, but turning a passing fixture red is a bad trade, so the
implemented sites 1-5 were **reverted pending the prerequisite work**: resolve
the carrier-masked return-ABI mismatch and the Dec-over-Option monomorphization
type error in `nested-construct-byvalue-decode`, and the three-defect
`constrained-loop-vec-push-byvalue-result-element`, FIRST; then land sites 1-5
atomically on top.  The five site fixes are recorded above in full so the
prerequisite pass can re-apply them verbatim.

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

**Prerequisite first** (these block the cluster because the global flip regresses
them): in `nested-construct-byvalue-decode`, fix the carrier-masked `ok__spec__`
return-ABI mismatch and the Dec-over-Option monomorphization type error
(`Option__int` built where the cstr arm needs `Option__cstr`); and resolve the
three-defect `constrained-loop-vec-push-byvalue-result-element`.  Both pass/build
today only because carrier makes every layer int64-identical.

**Then land sites 1-5 atomically**: the two predicate fixes
(`adt_app_is_byvalue_product` and `type_app_is_concrete_adt` accept a nested
by-value ADT-app arg/field), the `heap_adt_recv` EX_GET_FIELD branch, the
`emit_registered_adt_app_rec` dependency pre-pass, and the ADT-positional
carrier-box readback deref-unboxing a nested by-value field unconditionally.
Verify the default suite (esp. `hkt-cata-*`, which must stay carrier) and
re-sweep force-lower.

## Notes

- Default suite is unaffected by the bug (only the lowered representation triggers
  it); the danger is entirely in the fix's blast radius.
- Distinct from the landed vec-element carrier<->by-value read bridge (an
  inline-C generic-result deref) and the by-value-ADT `any` box/unbox.
