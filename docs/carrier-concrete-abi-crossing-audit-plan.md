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
| **G2** | site 8, recursive case | method dispatch where the receiver element is **itself a parametric container** (`(.value x) : (Cons A)`) | partial | **[GAP]** | -- |
| **G3** | `elab_typeclasses.c` instance-method ABI vs site 6 | instance method whose HEAD is a by-value applied struct (`Enc [(Option cstr)]`) takes the carrier param, but a by-value struct-**field** receiver passes the aggregate | none | **[GAP]** | -- |
| **G4** | int-carrier list helpers (`list-length`, ...) vs `(:: xs :int)` coercion | generic carrier walk of a `:heap` `Cons` whose head is a **by-value aggregate** reads `tail` at the wrong offset and **segfaults** (consumer side of G1) | none | **[GAP]** | -- |

Sites 1-9 are the fish already caught. **G1, G2, G3, and G4 are the gaps the
composition stress matrix exposed** (section 5). G3 was filed independently on
`main` by the maintainer (`docs/reported/instance-method-byvalue-struct-field-receiver-abi-mismatch.md`,
#482-era) -- same family, same fix direction; it is the single-level
struct-field-receiver companion of G2's nested-container dispatch, and the two
should close together under P2. G4 is the **consumer-side** crossing that
closing G1 exposed: the same `(Cons (Option int))` value that now *builds*
segfaults when walked through the generic int-carrier list API. It is tracked
here as a first-class row -- not buried in prose -- precisely because this
family of bug, left only as a side note, gets lost and resurfaces.

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
segfaults (filed:
`docs/reported/heap-cons-byvalue-aggregate-head-breaks-int-carrier-list-helpers.md`),
and the nested `enc` dispatch is gap G2.

### G2 -- `(Option (Cons A))`: inner instance method stays at the carrier signature

Encoding `(:: (some (:: (list 7.1 2.5) (Cons float))) (Option (Cons float)))`
**compiles with a warning and silently miscompiles**. The `Enc [Option]` spec
body calls `__inst_Enc_enc_Cons((x).value)` where `(x).value` is a concrete
`Cons__float *`, but the inner instance was **not** re-resolved per element --
it kept the generic carrier signature `__inst_Enc_enc_Cons(int64_t)`:

```
warning: passing argument 1 of '__inst_Enc_enc_Cons' makes integer from pointer without a cast
note: expected 'int64_t' but argument is of type 'Cons__float *'
...
output: 4619679907765970534      (want: 7.1 -- a double's bit pattern read as int64)
```

The `int` case prints `42` by luck (pointer width happens to align); the
`float` case is a denormal-class silent miscompile -- precisely the failure
mode CLAUDE.md flags. This is the *dispatch-on-nested-element* (encode/read)
sibling of #480's *nested-construct* (decode/write) fix.

Filed: `docs/reported/constrained-instance-dispatch-nested-parametric-element-carrier-collapse.md`

### G3 -- `Enc [(Option cstr)]` over a struct field: instance-method param stays at the carrier (maintainer-filed on `main`)

Independently surfaced by the maintainer on `main` (PR #482 era) and filed at
`docs/reported/instance-method-byvalue-struct-field-receiver-abi-mismatch.md`.
An instance whose head is a by-value applied struct (`Enc [(Option cstr)]`)
emits its method with the int64 carrier param (`enc(int64_t)`); dispatch over a
*local* works, but after the #482 field-layout fix a by-value struct **field**
read (`(.nick r) : Option__cstr`) passes the real aggregate, so:

```
error: incompatible type for argument 1 of '__inst_Enc_enc_Option_cstr'
note: expected 'int64_t' but argument is of type 'Option__cstr'
```

Verified still-open against a fresh `origin/main` build (2026-06-21). This is
the **single-level struct-field-receiver** companion of G2's nested-container
dispatch -- same carrier-vs-byvalue defect on the instance-method-receiver ABI
side, same fix direction (emit the method with the concrete by-value parameter,
monomorphized per type, and bridge carrier-ABI call sites). Closes with G2
under P2.

### G4 -- `(Cons (Option int))` consumed via the int-carrier list API: segfault (consumer side of G1)

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
the carrier layout. Filed:
`docs/reported/heap-cons-byvalue-aggregate-head-breaks-int-carrier-list-helpers.md`.

## 6. Plan (phased)

### P0 -- Audit (this document). DONE.

Crossing sites enumerated, shared recovery routine named, gaps G1/G2 found
and filed (plus G3, filed independently on `main`). This converts the open
surface from "unknown number of fish" to a **closeable list**. **G1 is now
closed** (this branch); G2/G3/G4 remain and are tracked as table rows under P2
(G4 is the consumer-side crossing that closing G1 exposed).

### P1 -- Promote the composition stress matrix to fixtures (on green)

A self-contained fixture family that is the cartesian product the spices hit
organically:

- containers: `Option`, `Cons`, `Vec`
- elements: `int`, `cstr`, `float`, value-struct (`Box`)
- nestings: `Cons (Option A)`, `Option (Cons A)`, `Vec (Option A)`,
  `Option (Option A)`

The single-level cells already exist (`constrained-instance-element-dispatch`,
`constrained-instance-heap-field-dispatch`). The nested cells are added as
each gap closes -- a fixture is only committed once it PASSES, so the gate
stays green (per CLAUDE.md). The minimal repros live in the two reported docs
until then.

### P2 -- Route the remaining sites through the shared recovery (close G2/G3/G4)

- **G1**: DONE (this branch). The `tur-list-homog__` call is a dead
  compile-time-only assertion (the homogeneity it enforces fires at
  elaboration; its inline-C body is a no-op), so rather than monomorphize it,
  `emit_stmt.c` elides it for by-value aggregate args. See the FIXED row in the
  table and the G1 result subsection.
- **G4** (consumer side of G1,
  `docs/reported/heap-cons-byvalue-aggregate-head-breaks-int-carrier-list-helpers.md`):
  either box the by-value aggregate head into the carrier inside `tcons-of` for
  aggregate elements (cell stays `{ int64 head; int64 tail; }`, one alloc per
  element) -- the smaller local change -- or monomorphize the int-carrier list
  helpers per element type at the `(:: xs :int)` coercion point (keeps the
  by-value thread end-to-end, consistent with P2). Until fixed, the typed
  accessor path is the supported way to consume such a list.
- **G2**: the dispatch re-resolver (`emit_reresolve_disp_type`,
  `emit_core.c:1141`) must recurse: when the receiver expression is a field
  extraction whose recovered type is *itself* a parametric container, mint /
  select the per-instantiation inner spec (`__inst_Enc_enc_Cons__spec__Cons__float`)
  rather than the generic carrier `__inst_Enc_enc_Cons`. This is the natural
  extension of sites 5+8 to the recursive case.
- **G3** (maintainer-filed,
  `docs/reported/instance-method-byvalue-struct-field-receiver-abi-mismatch.md`):
  the single-level form of the same dispatch-ABI fix -- emit the instance
  method for a by-value applied-struct head with the concrete by-value
  parameter (`Option__cstr x`) and bridge carrier-ABI call sites (locals) into
  the aggregate. Same code change as G2, minus the recursion; do them together.

### P3 -- Audit-as-regression-guard

Keep this table current: any future PR that adds a recovery call site adds a
row here and a stress-matrix cell, so the audit stays the single source of
truth for "which crossings are covered." A crossing without a row is a fish
waiting to be found by a spice -- the exact loop this plan exists to end.

## 7. Validation

- `bash tests/run.sh` (10-minute timeout) must stay green throughout; failing
  composition repros stay in `docs/reported/` (not in `tests/fixtures/`) until
  their gap closes.
- Each gap closure flips its repro FAIL->PASS, promotes it to a fixture, and
  moves its report to `docs/archive/`.
