---
title: Carrier <-> Concrete ABI crossing audit + unification plan -- turning the spice-fix whack-a-mole into a closeable list
category: Planning -- ABI / Codegen (constrained / parametric / HKT instance bodies)
description: The recent run of spice-fix PRs (#437-#481) are not independent
  bugs; they are the SAME defect -- a parametric payload's concrete element
  type collapsing to the int64 carrier -- surfacing at a different emit site
  each time a spice happens to exercise a not-yet-audited crossing. This doc
  enumerates every carrier<->concrete crossing site (file:line), names the one
  shared recovery routine they should all route through, classifies which are
  covered, and sequences the remaining work. It also records two NEW bugs found
  PROACTIVELY by a composition stress matrix rather than reactively by a spice.
status: OPEN -- audit produced; routing + stress-matrix promotion sequenced below
---

# Carrier <-> Concrete ABI crossing audit + unification plan

## TL;DR

We have been **shooting fish in a barrel**. PRs #437-#481 each fixed one
emit site where a parametric/constrained/HKT value crosses between Turmeric's
two value ABIs and the concrete element type had collapsed to the int64
carrier. They are siblings of a single defect, not separate bugs -- the
reports say so in their own words (#479: *"all three are the same carrier-ABI
machinery not resolving a generic payload against the current spec's concrete
element type at a carrier boundary"*; #481: *"Same family as #475/#479/#480,
but on the return/recursion side"*).

This plan replaces the reactive loop with a proactive one:

1. **Enumerate** every crossing site up front (the audit table below).
2. **Route** them all through the one shared recovery routine
   (`emit_var_spec_arg_type` + the dispatch re-resolver) so a new site is
   correct by construction instead of being the next fish.
3. **Stress** the composition surface (nested parametric instances) with a
   fixture matrix, because composition is what multiplies crossings and is
   exactly what the spices hit organically.

Step 3 already paid off: a first pass of the composition matrix found **two
new, currently-open bugs** (filed under `docs/reported/`, summarized in
section 5) -- found by us, not by a downstream spice.

## 1. The two ABIs (recap)

From `docs/archive/aggregate-carrier-abi-plan.md`. Turmeric carries every
aggregate value through one of two conventions:

- **Carrier ABI** (`CK_CARRIER`): the value is an `int64_t` slot holding an
  inline 8-byte payload or a heap pointer cast to `intptr_t`. Generic stdlib
  code and typeclass dispatch shims (`ok`, `err`, `vec-new`, `tcons-of`,
  `__inst_*`) produce values this way.
- **Concrete ABI** (`CK_CONCRETE`): a monomorphized specialization uses the
  real C type by value (`Result__int__int`, `Option__cstr`, `Cons__double`).

A bug exists wherever a value produced under one ABI flows into a sink typed
for the other **without a bridge**, AND the emitter failed to recover the
concrete element type because `emit_resolve_type` leaves a spec's parametric
param type (e.g. receiver `x : (Vec A)`) unsubstituted even though the active
specialization is `(Vec int)`.

## 2. The one shared recovery routine

Every fix in this family ultimately calls the **same** helper to recover the
concrete type from the active specialization:

- `emit_var_spec_arg_type` (`src/compiler/emit_expr.c:1474`) -- given an
  `EX_VAR` that is a parameter of the active ABI spec, returns
  `current_abi_specialization->arg_types[pi]` (the concrete monomorphized
  type) instead of the unsubstituted parametric param type.
- `emit_reresolve_disp_type` / `emit_reresolve_method_call` /
  `emit_reresolve_method_fndef` (`src/compiler/emit_core.c:1141`, `:1318`,
  `:1308`) -- the dispatch-side companion: re-resolves which concrete
  `__inst_*` to call (and marks it live) when a method receiver's element
  type was erased to the carrier.

The defect is **not** that this routine is wrong. It is that it is invoked
**ad hoc, one emit site at a time** -- each PR discovers another expression
context that forgot to consult it.

**Making these routines mandatory chokepoints -- so a new emit site is correct
by construction instead of being the next fish -- is the structural follow-up
tracked in
[docs/carrier-crossing-recovery-routing-plan.md](carrier-crossing-recovery-routing-plan.md).**
That plan exists precisely so the unification is not lost between one-off gap
closures (the failure mode that produced this whole audit). Close each gap below
by *adding to a chokepoint*, not by adding another site-local branch.

## 3. Crossing-site audit

Every site below is a place where a parametric payload can appear inside a
constrained/parametric/HKT spec body and must consult the recovery routine.
`[OK]` = routes through recovery today; `[GAP]` = does not (or only partially).

| # | Site (file:line) | Expr context | Recovery call | Status | Landed by |
|---|---|---|---|---|---|
| 1 | `emit_expr.c:2116` | `EX_CALL` arg-type typing (avoid `int`<-ptr) | `emit_var_spec_arg_type` | [OK] | end-to-end-mono |
| 2 | `emit_expr.c:2704` | `EX_CALL` poly-HOF constrained arg cast sig | `emit_var_spec_arg_type` | [OK] | poly-hof |
| 3 | `emit_expr.c:2863` | `EX_ASCRIBE` arg inside call (carrier bridge) | `emit_var_spec_arg_type` | [OK] | carrier-bridge |
| 4 | `emit_expr.c:3432` | `EX_CALL` receiver arg0, by-ptr dispatch | `emit_var_spec_arg_type` | [OK] | #439/#440 |
| 5 | `emit_expr.c:4807` | `EX_GET_FIELD` `:heap` field (`Cons` head) | `emit_var_spec_arg_type` | [OK] | #479 |
| 6 | `emit_expr.c:4941` | `EX_GET_FIELD` value-struct field (`Option` value) | `emit_var_spec_arg_type` | [OK] | #475 |
| 7 | `emit_expr.c:5325` | `EX_ASCRIBE` heap concrete cast | `emit_var_spec_arg_type` | [OK] | M7 |
| 8 | `emit_core.c:1141`/`1318` | method-call dispatch re-resolution | `emit_reresolve_*` | [OK] for scalar/value-struct/`:heap` single-level | #475-#480 |
| 9 | `emit_core.c:1308` | scan-time liveness companion | `emit_reresolve_method_fndef` | [OK] | #480 |
| ~~G1~~ | `emit_stmt.c` `EX_CALL` | `(list ...)` homogeneity helper `tur-list-homog__` dead call elided for by-value aggregate args | n/a (elide) | **[FIXED]** | this branch |
| ~~G2~~ | site 8, recursive case | method dispatch where the receiver element is **itself a parametric container** (`(.value x) : (Cons A)`): mint the inner instance's by-value spec + route to it | `emit_reresolve_disp_type` + `emit_abi_try_nested_instance_dispatch_redirect` | **[FIXED]** | this branch |
| ~~G3~~ | `emit_expr.c` `expr_emits_byvalue_carrier_abi` vs site 6 | instance method whose HEAD is a by-value applied struct (`Enc [(Option cstr)]`) takes the carrier param, but a by-value struct-**field** receiver passed the aggregate -- now bridged (spill + address-of) like a local | `expr_emits_byvalue_carrier_abi` (EX_GET_FIELD) + `emit_carrier_bridge` | **[FIXED]** | this branch |
| ~~G4~~ | int-carrier list helpers (`list-length`, ...) vs `(:: xs :int)` coercion | generic carrier walk of a `:heap` `Cons` whose head is a **by-value aggregate** reads `tail` at the wrong offset and **segfaults** (consumer side of G1) -- both supported consumer paths now correct: the concrete `(Cons A)` walk via the element-aware pure-Turmeric `tlength`, and the phantom `(List A)` view via phantom-opaque element specialization (a per-element clone minted only when the phantom's tyvar is a by-value aggregate, zero churn otherwise). The bare `(:: xs :int)` carrier escape-hatch stays unsafe BY DESIGN (explicit erasure of a layout-bearing element) | `tlength` (`stdlib/list.tur`) + `type_phantom_hides_aggregate` (`emit_module.c`) | **[FIXED]** | this branch |
| ~~G5~~ | `Option`'s legacy `tur_option_t` special-casing vs #482 | (S1) a struct-field-read `Option` passed to a typeclass method inserts a stale `tur_option_t *`->aggregate reconstruction; (S2) `Result__T` typedef emitted before `T` once `T` embeds an `(Option ...)` field -- **BOTH fixed in self-contained repros**, pending real `json/encode` derive-json confirmation | `field_read_emits_byvalue_aggregate` (S1); forward typedef in `emit_registered_struct_app_rec` (S2) | **[FIXED*]** | this branch |
| **G6** | HKT `fmap` instance layout (carrier vs by-value) + closure-thunk register class | a generic `cata = alg . fmap (cata alg) . unroll`: (a) return-differentiated sibling spec -- **FIXED** (`emit_spec_result_mismatch`); (b) **direct `fmap` over a parametric SUM type miscompiled** (carrier `tur_adt_ReF` int64 fields vs by-value `tur_adt_ReF__bool` narrow fields) -- **FIXED (this branch):** by-value HKT instance-method spec now minted for parametric ADT results (`type_app_is_concrete_adt`; `EX_MATCH`/ADT-ctor bodies recognized as by-value-constructible; ctor suffix recovered from the active spec's result family). bool/float/int/cstr round-trip. (c) **recursive cata** (`fmap`'s element is the enclosing generic's ungrounded tyvar; recursive closure lifted to one int64 carrier fn) -- **still open**, needs per-spec cloning of a captured passed closure + symbolic method->generic tyvar binding | (a) `emit_spec_result_mismatch`; (b) `type_app_is_concrete_adt` + `emit_hkt_spec_ctor_suffix`; (c) passed-closure spec clone: open | **[PARTIAL]** | (a)+(b) this branch |
| ~~G7~~ | site 8, return/decode side, sum field | a `defdata`-sum struct field's `(decode ... (Result Cmd cstr))` dispatched to the generic `decode_T` at the ENCLOSING struct's result type instead of the field's `Decode [Cmd]` instance -- the `EX_ASCRIBE` scan now registers a return-polymorphic dict-less call with the ascription's concrete result as `result_type_override` (instead of the plain inner scan), and the two result-recovery heuristics bail when an override is supplied | `emit_abi_scan_expr` (EX_ASCRIBE) + `result_type_override` | **[FIXED]** | this branch |
| ~~G9~~ | witness-side, parametric-container element | the MIRROR of G2: a single-level `Enc [Cons]` over `(Cons (Option int))`. G2 already minted the `Cons__Option__int` cell + inner Option by-value spec; the remaining defect was the dispatch arg `(.head xs)` (an embedded `Option__int`) reconstructed via a stale `tur_option_t *` carrier cast | `field_read_emits_byvalue_aggregate` (suppresses the spurious carrier->concrete bridge) | **[FIXED]** | this branch |
| ~~G10~~ | instance selection, applied-struct heads | two instances of one class over applied structs differing only in the element (`Enc [(Option cstr)]` vs `Enc [(Option int)]`) conflated -- the element-discrimination only treated struct/ADT elements as concrete, ignoring a primitive (cstr vs int) difference (NOT an ABI bug; instance keying) | `typeclass_type_arg_concrete` (primitive elements discriminate) | **[FIXED]** | this branch |

Sites 1-9 are the fish already caught. **G1, G2, G3, and G4 are the gaps the
composition stress matrix exposed** (section 5); **G1, G2, G3, G4, G5, G7, G9, and
G10 are now closed** (this branch; G5 and G7 verified end-to-end against the
real `json/encode` derive-json; G6 spec-selection half closed). **G5**
is a #482 follow-up filed by the maintainer (Option-specific residual
special-casing, two codegen sites, both fixed); **G7** is the return/decode-side
dispatch gap for a `defdata`-sum struct field (now fixed); **G9** is the witness-side
mirror of G2 and **G10** the applied-struct instance-selection conflation, both
found while closing G2/G3 and now both fixed. G3 was filed independently on
`main` by the maintainer (`docs/reported/instance-method-byvalue-struct-field-receiver-abi-mismatch.md`,
#482-era) -- same family, same fix direction; it is the single-level
struct-field-receiver companion of G2's nested-container dispatch, and the two
should close together under P2. G4 was the **consumer-side** crossing that
closing G1 exposed: the same `(Cons (Option int))` value that now *builds*
segfaulted when walked through the generic int-carrier list API. Now closed
(this branch) -- the concrete `(Cons A)` walk via `tlength` and the phantom
`(List A)` view via phantom-opaque element specialization. It was tracked here as
a first-class row -- not buried in prose -- precisely because this family of bug,
left only as a side note, gets lost and resurfaces.

## 4. Why composition is the multiplier (the user's HKT/compose intuition)

A plain `(Enc int)` instance never crosses the boundary -- one concrete type,
no carrier. A **composed** instance crosses it once per layer:

- `(definstance Enc [Cons] [(Enc A)] (enc (.head xs)))` crosses twice per
  element: extract `A` out of the `:heap` carrier (site 5), then re-dispatch
  `enc` on the recovered `A` (site 8).
- Nest it -- `Enc` over `(Cons (Option A))` or `(Option (Cons A))` -- and the
  inner `(.value x)` / `(.head xs)` is *itself* a parametric container, so the
  recovery must fire **recursively at each layer**. Site 8 handles the
  single-level (scalar / value-struct / `:heap`) receiver; the recursive case
  (G2) is where it stops.

HKTs are not inherently buggy; they are simply the constructs that generate
the most crossings, and **composition multiplies them**. That is why the
spice work felt like roadblock-after-roadblock: every new composed instance a
spice wrote hit a fresh, un-audited crossing of the same defect.

## 5. Composition stress matrix -- results (two new open bugs)

Baseline (single level) is green on this build:

```
constrained-instance-element-dispatch:   42 | 3.25 | "hi" | 99   (Option over int/float/cstr/Box)
constrained-instance-heap-field-dispatch: 42 | 7.1  | "hi"        (Cons over int/float/cstr)
```

Two-level composition breaks:

### G1 -- `(Cons (Option A))`: list-homogeneity helper typed at the carrier -- **FIXED (this branch)**

Building `(:: (list (some 42) (some 7)) (Cons (Option int)))` was a **hard C
compile error**: the homogeneity helper `tur-list-homog__ [A] [a :A b :A]`
(`stdlib/list.tur:196`) emits a single **carrier** C function
`tur_hylist_hyhomog_un_un(int64_t, int64_t)` (its inline-C body fixes the
signature at the carrier; it is not monomorphized), but the elements are
emitted as by-value `some__spec__Option__int(...)` aggregates (`Option__int`):

```
incompatible type for argument 1 of 'tur_hylist_hyhomog_un_un'
note: expected 'int64_t' but argument is of type 'Option__int'
```

**Fix:** the call is a compile-time-only homogeneity assertion (a heterogeneous
list is still rejected with `TUR-E0001` at elaboration) whose inline-C body is a
no-op -- the emitted runtime call is dead. `emit_stmt.c` now elides it when an
argument is a by-value (non-`:heap`) aggregate; scalar/float/cstr/heap-pointer
elements keep their existing carrier-coerced call (zero snapshot churn). The
list of Options now constructs and round-trips through typed accessors. Fixture:
`tests/fixtures/list-homog-byvalue-aggregate-element`. Suite green (1738/0).

Resolved report archived at
`docs/archive/list-homog-helper-carrier-typed-byvalue-aggregate-element.md`.
Closing G1 exposed two distinct downstream crossings on the same example:
the int-carrier `list-length` over a by-value-aggregate-headed `:heap` cons
segfaulted (gap G4, now fixed, archived:
`docs/archive/heap-cons-byvalue-aggregate-head-breaks-int-carrier-list-helpers.md`),
and the nested `enc` dispatch is gap G2.

### G2 -- `(Option (Cons A))`: inner instance method stays at the carrier signature -- **FIXED (this branch)**

Encoding `(:: (some (:: (list 7.1 2.5) (Cons float))) (Option (Cons float)))`
**compiled with a warning and silently miscompiled**. The `Enc [Option]` spec
body called `__inst_Enc_enc_Cons((x).value)` where `(x).value` is a concrete
`Cons__float *`, but the inner instance was **not** re-resolved per element --
it kept the generic carrier signature `__inst_Enc_enc_Cons(int64_t)`:

```
warning: passing argument 1 of '__inst_Enc_enc_Cons' makes integer from pointer without a cast
note: expected 'int64_t' but argument is of type 'Cons__float *'
...
output: 4619679907765970534      (want: 7.1 -- a double's bit pattern read as int64)
```

**Fix:** the dispatch-type chokepoint `emit_reresolve_disp_type` already
recovered the concrete receiver `(Cons float)`; the gap was (1) the instance-head
matcher (`emit_inst_head_matches`) not matching a bare type-constructor instance
head (`Enc [Cons]`) against the applied `(Cons float)`, and (2) no by-value spec
being minted/routed. `emit_abi_try_nested_instance_dispatch_redirect`
(`emit_module.c`) now mints the inner instance method's per-instantiation
by-value spec (`__inst_Enc_enc_Cons__spec__..._Cons__float`, taking
`Cons__float *`) and records the call so the emit side routes to it; the
single-level `@Cons` path mints the identical spec and `emit_abi_intern_spec`
dedupes. A pre-existing owned-clone leak in the field-substitution branch of
`emit_reresolve_disp_type` (surfacing only once the recovered element is a
TY_APP) is freed so the leak-checked codegen path stays clean. The float line
now prints `7.1`; int/float/cstr round-trip. Fixture:
`tests/fixtures/constrained-instance-dispatch-nested-parametric-element`. Suite
green (1740/0).

Resolved report archived at
`docs/archive/constrained-instance-dispatch-nested-parametric-element-carrier-collapse.md`.
Closing G2 confirmed a distinct, still-open **mirror** -- the inner-parametric
nesting `(Cons (Option A))`, where a single-level `Enc [Cons]` collapses its
parametric-container element at the witness -- tracked as gap G9
(`docs/reported/constrained-instance-dispatch-parametric-container-element-collapse.md`).

### G3 -- `Enc [(Option cstr)]` over a struct field: instance-method param stays at the carrier (maintainer-filed on `main`) -- **FIXED (this branch)**

Independently surfaced by the maintainer on `main` (PR #482 era) and filed at
`docs/archive/instance-method-byvalue-struct-field-receiver-abi-mismatch.md`.
An instance whose head is a by-value applied struct (`Enc [(Option cstr)]`)
emits its method with the int64 carrier param (`enc(int64_t)`); dispatch over a
*local* works, but after the #482 field-layout fix a by-value struct **field**
read (`(.nick r) : Option__cstr`) passed the real aggregate, so:

```
error: incompatible type for argument 1 of '__inst_Enc_enc_Option_cstr'
note: expected 'int64_t' but argument is of type 'Option__cstr'
```

**Fix (option 2 -- bridge the call site, not the method signature):**
`expr_emits_byvalue_carrier_abi` (`emit_expr.c`) now recognizes a by-value
(non-`:heap`) aggregate **field read** as a by-value carrier producer, so the
existing `emit_carrier_bridge` spills it to a temp and passes
`(int64_t)(intptr_t)(&tmp)` -- exactly what a by-value *local* of the same type
already did. The instance method keeps its uniform carrier ABI (so the dict slot
is unchanged); only the call-site materialization is bridged. A `:heap` field
stays the int64 pointer. The residual `-Wint-conversion` the report noted is
also gone. Fixture
`tests/fixtures/instance-method-byvalue-struct-field-receiver`. Suite green
(1741/0).

Resolved report archived at
`docs/archive/instance-method-byvalue-struct-field-receiver-abi-mismatch.md`.
Writing the fixture surfaced a distinct, still-open neighbor -- two
applied-struct instances of one class differing only in the element conflate at
dispatch -- tracked as gap G10
(`docs/reported/multiple-applied-struct-instances-same-class-conflate.md`).

### G4 -- `(Cons (Option int))` consumed via the int-carrier list API: segfault (consumer side of G1) -- **FIXED (this branch)**

Exposed by closing G1. `Cons` is `(defstruct Cons :heap [A] (head A) (tail :int))`,
and the generic int-carrier list helpers walk a chain as a fixed
`struct { int64_t head; int64_t tail; }`. With a by-value aggregate head the real
cell is `Cons__Option__int { Option__int head; int64_t tail; }`, so `tail` no
longer sits at offset 8 -- the carrier walk reads it from inside the head
aggregate, follows a bogus pointer, and crashes:

```turmeric
(let [xs (:: (list (some 42) (some 7)) (Cons (Option int)))]
  (println (list-length (:: xs :int))))   ;; segfault (the list builds; the walk crashes)
```

The *typed* path (`.head` / ascribed `.tail`) works; only the generic
`(:: xs :int)`-coerced carrier walk breaks. The producer (`list-build__` via
`tcons-of`) already specializes the cell; the generic consumer still reads it at
the carrier layout.

**Fix (audit Option 2, monomorphization direction -- concrete `(Cons A)` consumer
path):** the element-aware walker `tlength [A] [xs : (Cons A)] : int`
(`stdlib/list.tur`) is the sibling of the carrier `list-length`, written in **pure
Turmeric** over the typed `(.tail xs)` read (no inline-C). The compiler lowers
`(.tail xs)` at the concrete `(Cons A)` stride and monomorphizes `tlength` per
element type, so a by-value aggregate head no longer misplaces the tail link and
the scalar/pointer path keeps the identical concrete layout (no allocation, no
carrier collapse). The G1 example now counts and round-trips correctly:

```turmeric
(let [xs (:: (list (some 42) (some 7) (some 100)) (Cons (Option int)))]
  (println (tlength xs)))   ;; 3 -- was a segfault through carrier list-length
```

Fixture `tests/fixtures/list-length-byvalue-aggregate-element` (Option/int/float
elements + empty list).

**Phantom `(List A)` view -- also FIXED (the follow-up).** `(defopaque List [A]
:int)` lowers to the int64 carrier regardless of `A`, so a helper typed
`[A] [xs : (List A)]` had the SAME C lowering for every element and was emitted
once with `A` erased; a layout-dependent body collapsed the element back to the
generic `Cons *` and segfaulted on an aggregate. The emit-side spec gate
(`type_phantom_hides_aggregate` + the owned-path arg loop in
`emit_abi_register_call`, `src/compiler/emit_module.c`) now mints a
per-element-type clone **only** when the phantom opaque's tyvar is bound to a
by-value aggregate (a non-opaque, non-`:heap` struct element). The clone's
`arg_types` retain the concrete element (`(List (Option int))`) so the spec dedup
+ clone name distinguish it from the carrier base (the C param still lowers to
`int64_t`), its body's `(:: ... (Cons A))` resolves to `Cons__Option__int`, and
the inner `tlength` call composes to `tlength__spec__...Cons__Option__int`.
Scalar/pointer/`:heap` elements carry no inline aggregate, so the gate does not
fire and emission is byte-identical -- **zero snapshot churn** across the corpus.
New helper `list-count` (`stdlib/list-typed.tur`); fixture
`tests/fixtures/list-count-phantom-opaque-aggregate-element`. Suite green
(1746/0).

**Remaining caveat (by design, not an open gap):** the bare `(:: xs :int)`
escape-hatch fed to the carrier `list-length`/`list-head`/... still walks the
fixed `{int64 head; int64 tail}` layout. This is inherent to explicitly erasing a
layout-load-bearing element type -- with no runtime type tag there is nothing to
recover the stride. The supported routes (`tlength`/`thead`/ascribed `.tail` on
`(Cons A)`, or `list-count`/the typed `(List A)` API) all retain `A` and are
correct; a raw `(:: xs :int)` on an aggregate-element list is an unchecked
reinterpret.

Resolved report archived at
`docs/archive/heap-cons-byvalue-aggregate-head-breaks-int-carrier-list-helpers.md`.

### G5 -- `(Option T)` struct field: `Option`'s `tur_option_t` special-casing not reconciled with #482 (maintainer-filed) -- **BOTH SITES FIXED + VERIFIED (this branch)**

Follow-up to #482. The field *layout* is now an embedded `Option__cstr`
aggregate, but two **Option-specific** codegen paths still assume the old
"struct field holds an Option as a `tur_option_t *` heap box":

- **Site 1 (typeclass-arg lowering):** a struct-field-read `Option` `(.field x)`
  passed into a typeclass method (`(encode (:: (.o b) (Option int)))`) inserts a
  `tur_option_t *`->aggregate reconstruction; post-#482 the field is already an
  `Option__cstr`, so `(tur_option_t *)(intptr_t)((x).field)` casts an aggregate
  to a pointer -- `error: aggregate value used where an integer was expected`.
  An `Option` *local* into the same method, and a field-read `Option` into a
  *plain* `defn`, both work; the bug is the intersection field-read + typeclass
  dispatch. `Pair` field-read into `encode` works (no `tur_pair_t *` round-trip),
  so the reconstruction is hard-wired to `Option`.
- **Site 2 (type-emission ordering):** once `T` embeds an `(Option ...)` field,
  the emitter pushes `T` after `Option__cstr` but does **not** push
  `Result__T` (which embeds `T *`) after `T` -- `error: unknown type name 'T'`
  plus an `ok_val` int-fallback cascade. The transitive order
  `Result__T -> T -> Option__cstr` is not honored. The identical struct with a
  `Pair`/`Cons`/user-struct field orders correctly.

This blocks `derive-json` over structs with `(Option T)` fields in
rjungemann/turmeric-spices spices/json. Verified on `tur` from `main` at
`99cc8b3` / post-#482 (v0.22.0). G5's Site 1 is the **Option-only caller side**
of G3's struct-field-receiver dispatch boundary (G3 is the carrier-param callee
side, reproducing for any applied-struct head); Site 2 is a typedef
topological-sort gap that #482 exposed, riding along because it blocks the same
use case. Filed:
`docs/reported/option-tur-option-special-casing-stale-post-482.md`.

**FIXED (this branch, self-contained repros).** Site 1 is closed by the G9 fix
(`field_read_emits_byvalue_aggregate` suppresses the `tur_option_t *`
reconstruction). Site 2 is closed by `emit_registered_struct_app_rec`
(`types.c`) emitting a guarded forward `typedef struct User User;` when a
struct-app field references a user struct by pointer, so `Result__User__cstr`
no longer precedes the `User` typedef (verified-redundant typedef under
`-std=c99`). Fixture
`tests/fixtures/result-over-struct-with-option-field-typedef-order`; Site 1's
mechanism is also covered by the G9 fixture. **Verified end-to-end against the
real `json/encode` derive-json**: the turmeric-spices sibling was cloned into the
environment and yyjson built; a `User { id : int  nick : (Option cstr) }`
derive-json round-trips `{"id":7,"nick":"al"}` -- the emitted C compiles under
`-std=c99 -Wall`, links against `libturi.a` + yyjson, and runs correctly. Report
archived: `docs/archive/option-tur-option-special-casing-stale-post-482.md`
(suite green 1744/0).

### G6 -- generic `cata` via `Functor` `fmap`: closure-thunk ABI + boxed cata result (spice-filed)

Found by the turmeric-spices Track C U5 regex prototype
(`spices/regex/src/regex/tree.tur`). The textbook generic catamorphism
`cata alg = alg . fmap (cata alg) . unroll` over a `Fix`-style sum functor
type-checks but miscompiles per carrier `B`:

- **cstr (pointer) carrier -> segfault.** The recursive
  `(fn [c : Re] : B (re-cata alg c))` handed to `fmap` is stored into the
  thunk's `__fn` slot at the wrong function-pointer type:
  `__t80->__fn = (tur_thunk_const_char___int64_t_t)..._fn_1061;` -- a
  `-Wint-conversion` warning, then a crash.
- **int / bool carrier -> boxed result, wrong equality.** It prints the right
  value, but `(= 4 (re-cata size-alg e))` is **false** and a `(ReF bool)`
  algebra folds the wrong branch (`or false true -> false`): the
  `(:: (fmap ...) (ReF B))` result threads through the int64 carrier (boxed)
  instead of by value.

Direct structural recursion (no `fmap`) is correct, localizing the defect to
the `fmap`-driven path.

**Sharpened (branch claude/g6-carrier-concrete-abi-audit-pb3gqo).** The remaining
half is NOT a closure/cata problem -- it is a **`Functor`-instance layout**
problem that reproduces with no cata, no recursion, and no closure capture. A
single direct `fmap` whose result element is narrower than the int64 carrier is
already wrong:

```turmeric
(definstance Functor [ReF] (fmap [c g] (match c ... (AltF x y) (AltF (g x) (g y)) ...)))
(let [r (:: (fmap (:: (AltF 0 5) (ReF int)) to-bool) (ReF bool))]
  (match r ... (AltF x y) (if y 100 200) ...))   ;; EXPECT 100 (y=true); ACTUAL 200
```

Root cause: the instance is emitted once as the int64 **carrier** representative
`__inst_Functor_fmap_T`, whose body builds `tur_adt_ReF` with `int64_t` union
fields (`AltF._1` at offset 8); the consumer reads the **by-value** monomorphized
`tur_adt_ReF__bool` with `bool` fields (`AltF._1` at offset 1). `fmap` writes
`_1` at offset 8, the reader loads it at offset 1 -> `false`. `int`/`cstr` are
correct only because they are exactly 8 bytes wide; `bool`/`float` mismatch. (The
cstr "segfault" and the `-Wint-conversion` thunk cast in the cata example are the
*float/pointer* register-class facet of the same instance not being
monomorphized -- the closure `g` is called via the int64 carrier signature.)

The fix is therefore to **monomorphize the HKT instance method per concrete
result element** so producer and consumer share the ADT layout. The emit side
already has the by-value HKT instance-method spec machinery (M7 "layer-4",
`g_m7_hkt_enabled`: `emit_fns.c:566` returns the by-value ADT C name for the
instance-method return once an ABI spec is active; `emit_module.c:2636` notes the
carrier base, and `ctor_*__bool` twins already exist). The GAP is that **no
by-value `fmap` spec is interned/selected** at the `(:: (fmap ...) (ReF bool))`
call site -- `emit_abi_register_call` (`emit_module.c:1812`) needs
`call->as.call_.abi_bindings`, and elab does not attach the result-element
binding (`b -> bool`) to an HKT method call, so it falls through to the carrier
`__inst_Functor_fmap_T`. The originally-described closure-thunk register-class
half then falls out for `float` once `g` is finally called at `double` rather
than via the int64 carrier. Step (1) (elab attaching the binding) touches every
HKT method call, so it is snapshot/regression sensitive and must be gated to fire
only for a concrete element that does not round-trip through the int64 carrier
(`int`/`cstr` stay byte-identical). Verified on turmeric 0.22.0, main @ `99cc8b3`
(build-release), and re-confirmed on this branch. Filed:
`docs/reported/hkt-fmap-cata-carrier-miscompile.md`.

### G7 -- `defdata` sum as a `derive-json` struct field decodes to the wrong instance (peripheral to #482) -- **FIXED + VERIFIED (this branch)**

A struct field whose type is a `defdata` sum has its `derive-json` `Decode` body
dispatch the field decode to the **generic `decode` method specialized at the
enclosing struct's result type** rather than the field type's own `Decode [Cmd]`
instance:

```c
.cmd = ok_val__spec__int64_t_Result__Cmd__cstr(
         __inst_Decode_decode_T__spec__Result__Event__cstr_int64_t_int64_t(   // (!) decode_T, Event's result
           doc, json_obj_get(doc, val, "cmd")))
```

`error: incompatible type for argument 1 of 'ok_val__spec__int64_t_Result__Cmd__cstr'`.
A nested **struct** field resolves correctly (`__inst_Decode_decode_Point`), so
this is specific to sum-typed (ADT) fields. Standalone `Decode [Cmd]` works, so
the instance exists -- it just is not selected from inside another instance's
body for a sum field.

This is the **return/decode side** of the same dispatch family. The root cause
turned out NOT to be `emit_reresolve_disp_type` -- the decode call carries **no
dict_arg** (it is return-dispatched), so the re-resolver early-returns. The field
decode `(:: (decode doc val) (Result Cmd cstr))` carries its class var in
`call->type` (`(Result a cstr)`); inside the enclosing `Decode [Event]` spec, the
result-recovery heuristics in `emit_abi_register_call` (the `family_elem_rehydrated`
/ return-tyvar recovery) ground that class var to the **enclosing** element, so
the call interned + recorded at `decode_T__spec__Result__Event` -- an ill-typed
sibling whose body is actually the `Cmd` decode.

**FIXED (this branch).** In `emit_abi_scan_expr`'s `EX_ASCRIBE` case: when the
inner is a return-polymorphic (result mentions a tyvar), dict-less global call
with a concrete result ascription, register that call with the ascription's
concrete result as `result_type_override` -- and *instead of* the plain inner
scan, so only the correct spec is interned + recorded. The two result-recovery
blocks in `emit_abi_register_call` now also bail when a `result_type_override` is
supplied, so the concrete result is not clobbered back to the enclosing element.
The `.cmd` field now decodes via `decode_T__spec__Result__Cmd` (type-consistent
with its `ok_val__spec__...Result__Cmd` wrapper). **Verified end-to-end against
the real `json/encode` derive-json** (turmeric-spices cloned, yyjson built): an
`Event { id : int  cmd : Cmd }` over `{"id":7,"cmd":{"Quit":{}}}` compiles, links,
and runs -- `(decode doc root) : (Result Event cstr)` yields `ok`. Report
archived: `docs/archive/derive-json-sum-field-decodes-wrong-instance.md`
(suite green 1744/0).

### G9 -- `(Cons (Option A))`: the witness-side mirror of G2 (parametric-container element collapses at the witness)

Surfaced while closing G2. G2 fixed the case where a constrained instance
*body* dispatches on a parametric-container receiver (`(Option (Cons A))`).
This is the **mirror**: a single-level `Enc [Cons]` whose *element* is itself a
parametric container, `(Cons (Option int))`. The `@Cons` witness collapses the
`(Option int)` element to `int` (the minted spec is named `..._Cons__int`, not
`..._Cons__Option__int`, with a `Cons__int` cell), so the spec body's
`(.head xs)` -- a by-value `Option__int` -- is dispatched on the carrier
representative `__inst_Enc_enc_int(int64_t)`:

```
error: incompatible type for argument 1 of '__inst_Enc_enc_int'
note: expected 'int64_t' but argument is of type 'Option__int'
```

Verified **pre-existing** (fails identically against the pre-G2 build), so G2
neither caused nor fixed it.

**FIXED (this branch).** The G2 changes already minted the `Cons__Option__int`
cell + the inner Option by-value spec (so the "collapse to int" framing was
cured by G2); the remaining defect was the dispatch *argument*: `(.head xs)` is
an embedded by-value `Option__int`, but its elaborated type is the erased int64
carrier, so the matched-spec arg bridge reconstructed it through a stale
`(tur_option_t *)(intptr_t)((xs)->head)` cast. `field_read_emits_byvalue_aggregate`
(`emit_expr.c`) now resolves the field type through the receiver's concrete
(active-spec) type, recognizes the field read as already-concrete, and suppresses
the bridge -- `(xs)->head` passes directly. int prints `5`, float `7.1`. Fixture
`tests/fixtures/constrained-instance-dispatch-parametric-container-element`.
Resolved report archived at
`docs/archive/constrained-instance-dispatch-parametric-container-element-collapse.md`.
This is the **same `tur_option_t *`-reconstruction mechanism as G5 Site 1**, so
that site is very likely closed too (self-contained repro passes; awaiting real
`derive-json` verification).

### G10 -- two applied-struct instances of one class conflate (instance selection, not an ABI crossing) -- **FIXED (this branch)**

Surfaced while writing the G3 fixture. Defining `Enc [(Option cstr)]` **and**
`Enc [(Option int)]` -- two instances of one class whose heads are applied
structs sharing the constructor `Option` but differing in the element -- and
dispatching on a concrete `(Option cstr)` vs `(Option int)` selected the
**same** instance for both (the last defined won):

```turmeric
(println (enc (:: (some "hi") (Option cstr))))   ;; want "s"
(println (enc (:: (some 7)   (Option int))))     ;; want "i"
;; output (pre-fix): i / i   -- both pick Enc [(Option int)]
```

**Fix:** the method-call instance selection in `elab_typeclasses.c` (NOT the
`typeclass_env_lookup_instance` path, which this dispatch never reaches)
discriminated applied-head elements but treated only **TY_STRUCT/TY_ADT** as
concrete -- so a concrete **primitive** element (`cstr` vs `int`) was ignored
and both instances matched. A new helper `typeclass_type_arg_concrete` now
treats a concrete primitive element as discriminating too; a tyvar element stays
a wildcard so parametric instances (`Enc [Option]`) still match any element.
Output is now `s` / `i`. Fixture
`tests/fixtures/applied-struct-instance-element-discrimination` (locals + struct
fields). Suite green (1743/0). Resolved report archived at
`docs/archive/multiple-applied-struct-instances-same-class-conflate.md`. This
also lets the two-instance form of the G3 example (a `Rec` with both
`(Option cstr)` and `(Option int)` fields) work end to end.

## 6. Plan (phased)

### P0 -- Audit (this document). DONE.

Crossing sites enumerated, shared recovery routine named, gaps G1/G2 found
and filed (plus G3 and G5, filed independently as #482 follow-ups). This
converts the open surface from "unknown number of fish" to a **closeable
list**. **G1, G2, G3, G4, G5, G7, G9, and G10 are now closed** (this branch; G5
and G7 verified end-to-end against the real `json/encode` derive-json;
**G6**'s spec-selection half is fixed, its closure-thunk half
open; **G4** is fully closed -- the concrete `(Cons A)` walk via `tlength` and
the phantom `(List A)` view via phantom-opaque element specialization, with the
bare `(:: xs :int)` carrier escape-hatch left unsafe by design); **G6-closure-thunk**
remains
and is tracked as a table row under P2 (G5 is the Option-specific residual special-casing #482 left
stale, now fixed; G6 is the HKT `fmap` closure-thunk/cata-result crossing on the
fn-value spec path; G7 was the return/decode-side dispatch gap for a
`defdata`-sum struct field, now fixed; G9 is the witness-side mirror of G2, found
while closing it). The
structural follow-up that routes all of these through mandatory recovery
chokepoints is
[docs/carrier-crossing-recovery-routing-plan.md](carrier-crossing-recovery-routing-plan.md).

### P1 -- Promote the composition stress matrix to fixtures (on green)

A self-contained fixture family that is the cartesian product the spices hit
organically:

- containers: `Option`, `Cons`, `Vec`
- elements: `int`, `cstr`, `float`, value-struct (`Box`)
- nestings: `Cons (Option A)`, `Option (Cons A)`, `Vec (Option A)`,
  `Option (Option A)`

The single-level cells already exist (`constrained-instance-element-dispatch`,
`constrained-instance-heap-field-dispatch`); the `Option (Cons A)` nested cell
is now `constrained-instance-dispatch-nested-parametric-element` (G2). The
remaining nested cells are added as each gap closes -- a fixture is only
committed once it PASSES, so the gate stays green (per CLAUDE.md). The minimal
repros live in the reported docs until then.

### P2 -- Route the remaining sites through the shared recovery (close G6-closure-thunk)

Each gap closes by *adding to a chokepoint* per
[docs/carrier-crossing-recovery-routing-plan.md](carrier-crossing-recovery-routing-plan.md),
not by adding another site-local gated branch.

- **G1**: DONE (this branch). The `tur-list-homog__` call is a dead
  compile-time-only assertion (the homogeneity it enforces fires at
  elaboration; its inline-C body is a no-op), so rather than monomorphize it,
  `emit_stmt.c` elides it for by-value aggregate args. See the FIXED row in the
  table and the G1 result subsection.
- **G4** (consumer side of G1,
  `docs/archive/heap-cons-byvalue-aggregate-head-breaks-int-carrier-list-helpers.md`):
  DONE (this branch). The maintainer chose Option 2 (monomorphize, not box).
  Two changes close every supported consumer path: (a) the concrete `(Cons A)`
  walk via the element-aware pure-Turmeric `tlength [A] [xs : (Cons A)] : int`
  (`stdlib/list.tur`), which the compiler lowers at the concrete stride and
  monomorphizes per element type; and (b) the phantom `(List A)` view via
  **phantom-opaque element specialization** -- `type_phantom_hides_aggregate` +
  the owned-path arg loop in `emit_abi_register_call` (`emit_module.c`) mint a
  per-element clone of a `(List A)` helper ONLY when the phantom's tyvar is bound
  to a by-value aggregate, so the body's `(:: ... (Cons A))` resolves to the
  concrete cell and cascades to the concrete `tlength` spec. Scalar/pointer/`:heap`
  elements do not trigger the gate -- byte-identical emission, zero churn. New
  helper `list-count` (`stdlib/list-typed.tur`). Fixtures
  `tests/fixtures/list-length-byvalue-aggregate-element` and
  `tests/fixtures/list-count-phantom-opaque-aggregate-element`. The bare
  `(:: xs :int)` carrier escape-hatch is left unsafe by design (explicit erasure
  of a layout-bearing element, no runtime tag to recover the stride). See the
  FIXED row + the G4 result subsection.
- **G2**: DONE (this branch). The dispatch-type chokepoint
  `emit_reresolve_disp_type` already recovers the parametric-container receiver;
  `emit_abi_try_nested_instance_dispatch_redirect` (`emit_module.c`) mints the
  per-instantiation inner spec (`__inst_Enc_enc_Cons__spec__..._Cons__float`,
  taking `Cons__float *`) and records the call so the emit side routes to it
  instead of the generic carrier `__inst_Enc_enc_Cons`. `emit_inst_head_matches`
  now matches a bare type-constructor instance head against the applied type so
  the FnDef lookup agrees with the name resolver. See the FIXED row + the G2
  result subsection.
- **G3**: DONE (this branch). Rather than re-emit the instance method with a
  by-value parameter (which would break the uniform-carrier dict slot), the
  call-site bridge was extended: `expr_emits_byvalue_carrier_abi`
  (`emit_expr.c`) recognizes a by-value aggregate field read, so
  `emit_carrier_bridge` spills it and passes its address into the method's
  carrier parameter -- the same path a by-value local already used. See the
  FIXED row + the G3 result subsection.
- **G5** (maintainer-filed,
  `docs/archive/option-tur-option-special-casing-stale-post-482.md`): both sites
  DONE and VERIFIED end-to-end against the real `json/encode` derive-json (this
  branch). **Site 1** -- the G9 fix (`field_read_emits_byvalue_aggregate`)
  suppresses the `tur_option_t *` reconstruction for a struct-field-read Option
  passed to a typeclass method. **Site 2** -- `emit_registered_struct_app_rec`
  (`types.c`) emits a guarded forward `typedef struct User User;` when a
  struct-app field references a user struct by pointer, so
  `Result__User__cstr { User *ok_val; }` no longer precedes the `User` typedef
  (fixture `tests/fixtures/result-over-struct-with-option-field-typedef-order`). A
  `User { id : int  nick : (Option cstr) }` derive-json round-trips
  `{"id":7,"nick":"al"}` against the cloned turmeric-spices + yyjson.
- **G6** (spice-filed,
  `docs/reported/hkt-fmap-cata-carrier-miscompile.md`): **sharpened (this
  branch)** -- the remaining half is a `Functor`-instance LAYOUT gap, not a
  closure/cata gap. The carrier representative `__inst_Functor_fmap_T` builds the
  int64-field `tur_adt_ReF`, but a sub-int64 result element reads the narrow
  by-value `tur_adt_ReF__bool` -- reproducible with a *direct* `fmap` (no cata,
  no closure). Fix = mint+select a by-value HKT instance-method spec at the
  `fmap` call site so the instance is monomorphized per result element: the M7
  layer-4 emit path (`emit_fns.c:566`, `emit_module.c:2636`) already returns the
  by-value ADT once an ABI spec is active and the `ctor_*__bool` twins exist; the
  gap is that no by-value spec is interned because elab attaches no
  result-element binding (`b -> bool`) to the HKT call, so
  `emit_abi_register_call` (`emit_module.c:1812`, which requires
  `call->as.call_.abi_bindings`) never fires. Closing it: (1) elab attaches the
  HKT method call's concrete result-element binding (gated so `int`/`cstr` stay
  byte-identical -- it touches every HKT call); (2) `emit_abi_register_call`
  mints the by-value instance-method spec for a concrete sub-int64 `TY_APP`
  result; (3) the recursive-closure register-class clone (the original
  closure-thunk framing) then falls out for `float` because `g` is finally called
  at `double` rather than via the int64 carrier. Closes independently of
  G2/G3/G5 (it is the fn-value/instance-layout corner, not the dispatch
  re-resolver).
- **G7**: DONE + VERIFIED (this branch,
  `docs/archive/derive-json-sum-field-decodes-wrong-instance.md`). The decode call
  is return-dispatched with no dict_arg, so `emit_reresolve_disp_type`
  early-returns; the real fix is in the ABI scan. `emit_abi_scan_expr`'s
  `EX_ASCRIBE` case now, for a return-polymorphic dict-less global call under a
  concrete result ascription, registers the call with that ascription as
  `result_type_override` *instead of* the plain inner scan, and the two
  result-recovery heuristics in `emit_abi_register_call` bail when an override is
  supplied -- so the `.cmd` field decodes via `decode_T__spec__Result__Cmd`, not
  the enclosing `Result__Event` sibling. Verified end-to-end: an
  `Event { id : int  cmd : Cmd }` over `{"id":7,"cmd":{"Quit":{}}}` decodes to
  `ok` against the cloned turmeric-spices + yyjson.
- **G9**: DONE (this branch). G2 already minted the `Cons__Option__int` cell +
  inner Option by-value spec; the remaining defect was the dispatch arg
  `(.head xs)` (an embedded `Option__int` whose elaborated type is the erased
  carrier) being reconstructed via a stale `tur_option_t *` cast.
  `field_read_emits_byvalue_aggregate` (`emit_expr.c`) resolves the field type
  through the receiver's concrete type and suppresses that bridge -- the same
  mechanism that closes G5 Site 1. See the FIXED row + the G9 result subsection.
- **G10**: DONE (this branch). The method-call instance selection
  (`elab_typeclasses.c`) now discriminates on a concrete PRIMITIVE element, not
  only struct/ADT (`typeclass_type_arg_concrete`), so `Enc [(Option cstr)]` and
  `Enc [(Option int)]` are no longer conflated. A tyvar element stays a wildcard
  for parametric instances. See the FIXED row + the G10 result subsection.

### P3 -- Audit-as-regression-guard

Keep this table current: any future PR that adds a recovery call site adds a
row here and a stress-matrix cell, so the audit stays the single source of
truth for "which crossings are covered." A crossing without a row is a fish
waiting to be found by a spice -- the exact loop this plan exists to end.

The **runtime enforcement** of this guard is R3/R4 of
[docs/carrier-crossing-recovery-routing-plan.md](carrier-crossing-recovery-routing-plan.md):
a Debug-build assertion that a carrier value / method dispatch reaching emission
with an unresolved parametric param type *without* passing a chokepoint is a
hard ICE -- turning "forgot to route" from a silent downstream miscompile into a
local, immediate failure.

## 7. Validation

- `bash tests/run.sh` (10-minute timeout) must stay green throughout; failing
  composition repros stay in `docs/reported/` (not in `tests/fixtures/`) until
  their gap closes.
- Each gap closure flips its repro FAIL->PASS, promotes it to a fixture, and
  moves its report to `docs/archive/`.

### Progress (branch claude/g2-carrier-concrete-abi-audit-3yzkhm)

Closed this branch, suite green at each step (final **1744 passed, 0 failed**):

| Gap | Result | Fixture |
|---|---|---|
| G1 | FIXED (prior) | `list-homog-byvalue-aggregate-element` |
| G2 | FIXED | `constrained-instance-dispatch-nested-parametric-element` |
| G3 | FIXED | `instance-method-byvalue-struct-field-receiver` |
| G5 | FIXED (both sites; verified vs real `json/encode`) | `result-over-struct-with-option-field-typedef-order` (+ G9 fixture for S1) |
| G6 | PARTIAL (spec-selection fixed; closure-thunk open) | -- |
| G7 | FIXED (verified vs real `json/encode`) | spice round-trip (needs yyjson) |
| G9 | FIXED | `constrained-instance-dispatch-parametric-container-element` |
| G10 | FIXED | `applied-struct-instance-element-discrimination` |

Found while closing the above (filed, sequenced, then closed): **G9** (mirror of
G2), **G10** (applied-struct instance-selection conflation -- an instance-keying
defect, not a carrier crossing).

### Progress (branch claude/carrier-concrete-abi-audit-94dbi2)

Closed **G4** in full (the maintainer chose Option 2, monomorphize). Suite green
(**1746 passed, 0 failed**):

| Gap | Result | Fixture |
|---|---|---|
| G4 | FIXED (concrete `(Cons A)` walk via `tlength`; phantom `(List A)` view via phantom-opaque element specialization; bare `(:: xs :int)` left unsafe by design) | `list-length-byvalue-aggregate-element`, `list-count-phantom-opaque-aggregate-element` |

**Step 1 -- concrete `(Cons A)`.** The element-aware walker
`tlength [A] [xs : (Cons A)] : int` (`stdlib/list.tur`) is the pure-Turmeric
sibling of the carrier `list-length`: it walks `(.tail xs)` at the concrete
`(Cons A)` stride and monomorphizes per element type, so a by-value aggregate
head (`Cons__Option__int`) no longer misplaces the tail link. The carrier
`list-length` inline-C is left untouched for the scalar/pointer bulk (the dual
"by-value-body + carrier-shim" shape landmine #7 endorses). The one-time snapshot
churn here is shifted gensym counters from the new stdlib defn, regenerated in
the same change.

**Step 2 -- phantom `(List A)` view (the follow-up).** `(defopaque List [A] :int)`
lowers to the int64 carrier regardless of `A`, so a `(List A)` helper had the same
C lowering for every element and was emitted once with `A` erased; a
layout-dependent body collapsed the element to the generic `Cons *` and
segfaulted on an aggregate. `type_phantom_hides_aggregate` + the owned-path arg
loop in `emit_abi_register_call` (`src/compiler/emit_module.c`) now force a
per-element-type clone of such a helper -- but ONLY when the phantom opaque's
tyvar is bound to a by-value aggregate (a non-opaque, non-`:heap` struct element).
The clone's `arg_types` retain the concrete element so the spec dedup + clone name
distinguish it from the carrier base, its body's `(:: ... (Cons A))` resolves to
`Cons__Option__int`, and the inner `tlength` call composes to the concrete
`tlength` spec. Scalar/pointer/`:heap` elements do not trigger the gate, so their
emission is byte-identical -- **zero snapshot churn** across the corpus. New
helper `list-count` (`stdlib/list-typed.tur`).

**Still open**:

- **G6 (remaining half)** -- the HKT `fmap` closure-thunk per-carrier ABI. The
  spec-selection half is FIXED (int no longer miscompiles, cstr no longer
  segfaults); the recursive `fmap` closure is still lifted to a single
  int64-returning C function shared across carriers, so a sub-word `bool`
  recursive closure folds wrong. Closing it means generalizing the
  inner-closure-spec machinery (today only for closure-*returning* defns) to
  *passed* closures -- a deep closure-lifting change.

### Progress (branch claude/g6-carrier-concrete-abi-audit-pb3gqo) -- M6 HKT-dispatch monomorphization over sum types LANDED

**M6 core / G6(b) -- FIXED (this branch, suite green 1746/0, zero churn).**
Implemented by-value HKT instance-method monomorphization for parametric ADT
(`defdata` sum) results -- the case the existing M7 layer-4 machinery did not
cover (it handled parametric STRUCTS like Option/Result/Pair). A `Functor [ReF]`
over a sum, whose method body `match`es the receiver and constructs the result
family per arm, now mints a per-(f, A) by-value spec so the producer builds the
same `tur_adt_ReF__<elem>` layout the consumer reads:

- elab (`elab_typeclasses.c`): `m7_body_constructs_byvalue` /
  `m7_body_returns_byvalue_element` recurse through `EX_MATCH` and accept an ADT
  constructor call as by-value-constructible.
- types (`types.c`/`.h`): `type_app_is_concrete_adt`, `type_adt_app_def`.
- emit (`emit_module.c`): a concrete parametric-ADT HKT instance result is an
  ABI change -> by-value spec interned.
- emit (`emit_expr.c`): `emit_hkt_spec_ctor_suffix` recovers the monomorphized
  ctor suffix from the active spec's result family, so the body emits
  `ctor_AltF__bool` not the carrier `ctor_AltF`.

Fixture `hkt-fmap-byvalue-sum-element` (bool/float/int/cstr round-trip through a
DIRECT `fmap`). This closes the direct-`fmap` miscompile (which reproduced with
no cata/closure).

**G6(c) recursive cata -- still open (deep closure-lifting).** `cata` via `fmap`
still folds bool/float wrong because inside the GENERIC `re-cata` body `fmap`'s
element `B` is the enclosing generic's ungrounded tyvar (so no by-value `fmap`
spec is minted there), and the recursive closure `(fn [c] : B (re-cata alg c))`
is lifted to ONE int64-carrier `__fn` calling the carrier base `re_hycata`,
shared across return-specs. Closing it needs three coupled changes: (A) capture
the symbolic method->generic tyvar binding `b -> B` (which `m7_collect_tyvar_-
bindings` deliberately SKIPS when the actual is a tyvar, for the Applicative
`ap` case); (B) reconstruct the by-value result `(ReF bool)` at emit from the
method's declared `(f b)` instantiated through the composed spec bindings; and
(C) per-spec clone the CAPTURED closure PASSED as a call argument (the existing
`inner_closure_spec_idx` machinery clones only closures a generic defn RETURNS),
resolving the clone's recursive `re-cata` call to the active return-spec. The
EX_CLOSURE emit already consults `inner_closure_spec_idx` generically, so (C) is
register-side wiring -- but the set is a research-grade change best landed
separately from the green core above.

### Earlier note (diagnosis, superseded by the FIXED status above)

Investigating the "closure-thunk" half
showed it was mischaracterized: the bug is a **`Functor`-instance layout**
mismatch, not a closure/cata problem. It reproduces with a single *direct*
`fmap` over a sub-int64 element -- no cata, no recursion, no closure capture:

```turmeric
(let [r (:: (fmap (:: (AltF 0 5) (ReF int)) to-bool) (ReF bool))]
  (match r ... (AltF x y) (if y 100 200) ...))   ;; EXPECT 100 (y=true); ACTUAL 200
```

The carrier representative `__inst_Functor_fmap_T` builds `tur_adt_ReF` with
`int64_t` union fields (`AltF._1` at offset 8); the `(ReF bool)` reader uses
`tur_adt_ReF__bool` with `bool` fields (`AltF._1` at offset 1). The write/read
offsets disagree, so `y` (true, written at offset 8) is read as `false`.
`int`/`cstr` survive by 8-byte width coincidence; `bool`/`float` do not.

Fix direction (recorded in the report + section 5 + P2): monomorphize the HKT
instance method per concrete result element. The emit side already has the
by-value HKT spec machinery (M7 layer-4: `emit_fns.c:566`, `emit_module.c:2636`,
`ctor_*__bool` twins); the gap is that **no by-value `fmap` spec is interned** at
the call site because elab attaches no result-element binding to the HKT call
(`emit_abi_register_call`, `emit_module.c:1812`, requires
`call->as.call_.abi_bindings`). The implementation is bounded but
snapshot/regression sensitive (elab change touches every HKT method call), so it
must be gated to leave `int`/`cstr` byte-identical -- deferred, not landed, this
branch. Suite untouched (docs-only).
