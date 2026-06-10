---
title: Bare `^fat` Result-Kind Monomorphization Plan (Phase B)
category: Planning
description: Make a signature-less `^fat g` parameter return a non-int register-class result (e.g. :float) in ANY position -- including non-tail -- by monomorphizing the bare-^fat callee over the incoming closure's result kind. Subsumes the Phase A tail-only retype pass shipped in the predecessor plan.
---

# Bare `^fat` Result-Kind Monomorphization -- Plan (Phase B)

> **Status:** **Implemented** (intra-module, non-recursive, float register
> class). The non-tail gap Phase A structurally cannot reach -- a bare-`^fat`
> non-int result consumed off the tail -- now compiles via per-call-site
> monomorphization. Deferred sub-cases (recursion, exports, cross-module dedup,
> a single combinator shared by `:int` *and* `:float`) raise a **clean error**
> or are simply not triggered today; see "Deferred sub-cases" below.
> **What landed (2026-06-10):**
> - `diag_push_capture()/diag_pop_capture()` (diag.c) -- speculative
>   elaboration: errors are swallowed + counted, warnings pass through.
> - A bare-`^fat` defn whose canonical (int) body does not typecheck is
>   *deferred* (no canonical FnDef); the binding retains its `(defn ...)` Form
>   (`Binding.defn_form`, `bare_fat_lazy`).
> - `elab_specialize_bare_fat` (elab_call.c) re-elaborates that Form per
>   incoming closure result kind, mangled `<callee>__bf_<kind>`, memoized by
>   `(callee, kind)`; the call site is redirected to the clone.
> - CY2 fat-dispatch types `(g x)` from `Binding.bare_fat_result_kind`.
> - End-of-pass sweep (`elab_sweep_bare_fat_lazy`) re-surfaces the deferred
>   diagnostic for a lazy binding no call site specialized.
> - Phase A's tail retype is **kept** (it still covers its non-lazy tail cases
>   with zero churn); B2 only handles the bodies Phase A could never compile,
>   so there was nothing to retire.
> - Fixtures: `bare-fat-nontail-float-noannot` (the gate, stdout 7),
>   `bare-fat-float-result-dedup` (one clone across two float call sites).
>   No existing fixture/snapshot churned (only deferred bodies specialize).
> **Re-open trigger (for the deferred sub-cases):** a real caller needing a
> recursive bare-`^fat` non-int result, a cross-module/exported bare-`^fat`
> float combinator, or one bare-`^fat` combinator shared by `:int` and `:float`.
> **Last updated:** 2026-06-10
> **Type:** compiler -- closure ABI / monomorphization
> **Supersedes Phase B of:**
> - [bare-fat-result-type-inference-plan.md](../../archive/history/bare-fat-result-type-inference-plan.md) (Phase A shipped; this is its deferred Phase B)
> **Builds on:**
> - [closure-representation-unification-plan.md](../../archive/closure-representation-unification-plan.md)
> - [closure-first-class-type-plan.md](../../archive/closure-first-class-type-plan.md)
> - [closure-typed-invocation-abi-plan.md](../../archive/closure-typed-invocation-abi-plan.md)

---

## The remaining gap (what Phase A cannot do)

Phase A re-stamps a bare-`^fat` int64 call's result type **in tail positions
only** (do-last, let-body, both if-branches), inferring it from the enclosing
function's declared return. That is tail-precise *by construction* and sound
under an honest signature, but it leaves exactly one case open:

```turmeric
;; `y` carries no annotation, and `(g x)` is NOT the function's tail -- it
;; feeds `use-float`.  Phase A's tail-only walk never visits it, so the call
;; stays int64 and the :float result is read from rax instead of xmm0.
(defn run-with [^fat g x : float] : float
  (let [y (g x)]          ;; non-tail bare-^fat float call -- left int64
    (use-float y)))
```

Phase A's design note explains why this is *deliberately* out of scope: the
function body is compiled **once**, the closure's concrete result kind only
exists at each *call site* of `run-with`, and float cannot share a uniform
integer-register representation. Extending the contextual return-type heuristic
to non-tail positions would be **unsound** -- the declared return need not equal
the closure's result type off the tail.

## Why a value-typed binding does not work (recap)

The original "thread the incoming closure's `result_kind` onto the parameter
binding" sketch fails because a bare `^fat g` is **generic over any closure
signature** and the body is compiled once. There is no honest in-body source for
`g`'s result kind, so any "store it on the binding" scheme degenerates back to
the Phase A contextual heuristic -- which is tail-only and unsound if naively
extended. The only sound general mechanism specializes the body **per call
site**.

## The mechanism: monomorphize over closure result-kind

To type `(g x)` as `:float` in *any* position, a bare-`^fat` function must be
**specialized per call site** to the incoming closure's result kind.

### B1'. Specialize the bare-`^fat` callee per closure result-kind

At a call `(run-with <closure> ...)`:

- Read the boxed-`TY_FN` argument's `result_full_type` / `result_kind` (CRU B-1
  already carries these on a first-class closure value).
- Clone + mangle a specialized body `run-with$float` whose `^fat g` binding
  records `result_kind = TY_FLOAT`.
- **Dedup** specializations by `(callee, result_kind)` so each distinct kind is
  emitted once. Reuse the existing binding-name mangling
  (`elab_mangle_binding_name`, `elab_core.c`) rather than inventing a parallel
  scheme.

Model the clone+dedup on the per-use-site **ADT** monomorphization already in
`elab_call.c` (TY_APP path); this is the closest existing precedent.

### B2. Type the direct call from the binding's own result kind

In the `elab_call.c` CY2 `TY_PTR_VOID` fat-dispatch branch, prefer, in order:

1. the binding's recorded result kind (set by B1' in the specialized clone),
2. the Phase A contextual retype (`retype_bare_fat_tail_calls`),
3. the `TYPE_INT` default.

Because (1) is established at the call site, the call types correctly
**regardless of position** -- delivering the non-tail case. Codegen needs no
change: `emit_expr.c` already reads `e->type` and builds the typed thunk via
`ensure_typed_thunk_typedef`, so a `:float` result lands in xmm0 the moment
`e->type` is `TY_FLOAT`.

### B3. Retire the Phase A tail-retype pass where B2 covers it

Remove `retype_bare_fat_tail_calls` for any call whose binding now carries a
result kind (B2 handles it from the call site). Keep the pass only for the
residual "opaque int64 carrier with no result type" case (e.g. a closure handed
in from inline-C as a raw `int64`), or drop it entirely if no such case remains.

### B4. Generalize beyond `:float`

With the result kind on the specialized binding, every register-class-distinct
return works uniformly through `ensure_typed_thunk_typedef`. Add a fixture per
distinct carrier as it arises. Drive the target set from
`kind_is_non_int_register_class` (`elab_fns.c`), the predicate Phase A already
extracted.

## Open issues this mechanism must handle

These are the reason Phase B is *large* and was deferred:

- **Recursive / mutually-recursive** bare-`^fat` callees (a specialization that
  calls itself).
- Interaction with **exported bindings** and the existing poly-call /
  `is_poly_fn` paths.
- **Specialization-set dedup across modules** (the same `(callee, kind)` reached
  from two translation units).
- **Snapshot churn** from newly emitted specialized C functions -- regenerate
  `tests/fixtures/*/expected.c` in the same PR (see CLAUDE.md "Fixture
  Snapshots").

## Deferred sub-cases (raise a clean error or are not triggered today)

- **Recursion / mutual recursion.** A specialized clone that calls itself (or a
  partner) through the bare-`^fat` param cannot recover the param's result kind
  (the param carries no signature), so the call site emits a clear error --
  "cannot specialize bare-`^fat` function '%s' ... recursive ... not yet
  supported" -- instead of a body-less symbol that fails at link time. An
  in-progress cache marker also guards against an infinite specialization loop.
- **Exports / cross-module dedup.** A clone is a fresh `static` C symbol in the
  TU that specializes it; it is never exported, and two TUs may each emit their
  own copy. No stdlib bare-`^fat` defn is float-only (they are all int carriers,
  hence non-lazy), so this cannot arise in-tree today. An *exported* float-only
  bare-`^fat` defn that is also called externally is unsupported.
- **One combinator shared by `:int` and `:float`.** The minimal-churn design
  only specializes bodies that fail the canonical int elaboration (float-only).
  A body that typechecks as int is non-lazy and keeps the canonical path, so a
  single lazy combinator cannot also serve an int closure. Supporting both from
  one combinator needs the non-lazy specialization path (future work).

## Validation

- [x] Every Phase A fixture still passes, **byte-identical** -- non-lazy bodies
      keep the canonical path + Phase A tail retype; only deferred (lazy) bodies
      specialize, so `bare-fat-float-result` / `bare-fat-int-and-float-combinator`
      / `bare-fat-nontail-int-roundtrip` / `bare-fat-let-float-binding` and their
      `expected.c` snapshots are untouched.
- [x] **New gate (the deliverable):** `bare-fat-nontail-float-noannot` --
      `(let [y (g x)] (use-float y))` with a `:float` closure round-trips to 7.
- [x] Dedup: `bare-fat-float-result-dedup` -- one bare-`^fat` combinator across
      two `:float` call sites emits exactly one clone (`expected.c` shows a
      single `run_..._unbf_unfloat`). (The mixed `:int`+`:float` variant is a
      deferred sub-case above.)
- [x] Arrow `>>>` / `option-map` / `fat-param-*` fixtures stay green (int carrier
      is non-lazy, unchanged shape -- verified by zero snapshot churn).
- [x] `bash tests/run.sh`: 1541 passed, 0 failed, leak detection on.
- [x] Captureless and capturing `:float` closures both round-trip; a recursive
      or unrecoverable-kind call errors cleanly (no link-time failure).

## Risks

- **New ABI risk.** This is a whole new monomorphization pass on the closure
  ABI; the open issues above (recursion, exports, cross-module dedup) are where
  miscompiles would hide. Gate aggressively with the non-tail fixture before
  retiring any Phase A path.
- **Snapshot churn.** Specialized C functions change emitted output; regenerate
  snapshots in the same PR.

## Implementation order (single PR)

B1' specialize callee per result-kind -> B2 call-site typing -> non-tail gate
fixture -> B3 retire the tail pass where covered -> B4 generalize -> regenerate
snapshots -> Phase B validation.

---

## Implementation slice (concrete touch points)

> **Added 2026-06-10** after a scoping pass that reproduced the gap (probe
> `(let [y (g x)] (use-float y))` -> `TUR-E0001: arg 1: expected float, got
> int`) and verified the elaborator has no pre-existing scaffold for deferred
> body elaboration. This slice is what the eventual PR must do; it is **not**
> yet implemented.

### Why deferred elab is unavoidable

The probe body **cannot elaborate at defn time** with `g`'s result stamped
TY_INT: `(g x)` is int, so `y` is int, so `(use-float y)` fails TUR-E0001
before the body ever produces an EX_FN_DEF to clone. So we cannot start from
an elaborated body and retype call sites (Phase A's approach extended
whole-body); we must **retain the pre-elab `Form`** and re-run elaboration
under each (callee, result_kind) specialization.

### Step 1 -- detect "needs deferred elab" defns

In `elab_defn` (`src/compiler/elab_fns.c:552`), after parsing the param vector
and **before** elaborating body forms:

- If any param has `is_fat == true` AND no fn-type annotation
  (`param_full_types[i] == NULL` or its kind is not TY_FN), set a new
  `bool needs_lazy_body` on the synthesized `FnDef`.
- For such defns, also store `const Form **body_forms` + `uint8_t
  n_body_forms` on `FnDef` -- a slice of `call->as.list.items[body_start ..]`
  (the post-`:return-type` tail). Arena-owned pointers into the parser's form
  tree, no deep copy needed.

This keeps the legacy path (no bare-`^fat` param) byte-identical: no Form
retention, no behavior change. Only bare-`^fat` defns pay the cost.

The defn's **canonical** body elaboration still happens (with the bare-fat
param's `bare_fat_result_kind = TY_INT`), so existing `:int`-result callers
keep working. The clone is only created on demand for non-int kinds.

### Step 2 -- carry result-kind on the bare-^fat Binding

Add to `struct Binding` (`src/compiler/expr.h:43`):

```c
/* B1' bare-fat-result-monomorphization: kind stamped on a bare ^fat param's
 * call result during a specialized re-elab.  TY_INT for the canonical body
 * (current behavior); set to e.g. TY_FLOAT in a specialized clone before
 * elab_call's CY2 fat-dispatch path (elab_call.c:2034) stamps the call. */
TypeKind bare_fat_result_kind;
```

Read it in `elab_call.c:2034`:

```c
TypeKind rk = (fn_binding->is_fat && fn_binding->bare_fat_result_kind != TY_UNKNOWN)
              ? fn_binding->bare_fat_result_kind : TY_INT;
Expr *out = expr_new(e->arena, EX_CALL, type_from_kind(rk), call->span);
```

This subsumes the Phase A tail-retype pass for **any** call inside a
specialized clone -- not just tail positions.

### Step 3 -- per-call-site specialization at the caller

In the caller-side argument check loop (`elab_call.c` ~line 2403, the
fn-arg-coercion zone where `EX_FN_TO_FAT` shimming already lives):

When the formal slot is bare-`^fat` (i.e. `fn_type.as.fn.arg_fat[i]` true AND
`arg_full_types[i]` lacks a TY_FN), inspect the actual arg's closure result
kind. Reachable via `args[i]->type.as.fn.result_kind` for a TY_FN literal,
or via the closure's `closure_fn_binding->type.as.fn.result_kind` for a
TY_PTR_VOID capturing-closure value.

If that kind is `kind_is_non_int_register_class(k)` (the predicate already in
`elab_fns.c:370`):

1. Look up `(callee_binding, k)` in a new per-Elab cache
   `bare_fat_specs[]` (small array, dedup by linear scan -- specs per defn
   should be few).
2. On miss: invoke a new `elab_defn_specialize(e, callee_fndef, k)` that:
   - Clones the callee's `Binding *` with a mangled name
     (`<callee>__bf_<kind-suffix>`, e.g. `run_with__bf_float`). Use a small
     hand-rolled suffix (kind char) rather than `elab_mangle_binding_name`,
     which is reserved for export-name mangling.
   - Clones the param bindings; on the bare-`^fat` param sets
     `bare_fat_result_kind = k`.
   - Re-runs the body elaboration loop on the retained `body_forms`, with the
     new param scope. The CY2 bare-fat dispatch path (step 2) now stamps the
     correct kind, so `(use-float y)` typechecks.
   - Synthesizes a new `FnDef` and registers it with `elab_register_file_def`
     (same registration EX_FN_DEF uses; see `elab_fns.c:3429`).
3. Redirect `out->as.call_.fn_binding` to the specialized binding.

### Step 4 -- non-tail gate fixture (the regression seed)

Add `tests/fixtures/bare-fat-nontail-float-noannot/`:

```turmeric
;; Phase B gate: bare-^fat :float result consumed in non-tail position
;; with NO annotation on the let binding -- the case Phase A cannot reach.
(defn use-float [y : float] : float y)
(defn run-with [^fat g x : float] : float
  (let [y (g x)]
    (use-float y)))
(defn make-scale [k : float] : ptr<void>
  (fn [x : float] : float (* x k)))
(defn main [] : int
  (println (run-with (make-scale 2.0) 3.5))   ;; 7
  0)
```

Expected stdout: `7\n`. Until B1'+B2 lands, the fixture fails elaboration
(verified 2026-06-10: `TUR-E0001: arg 1: expected float, got int` at the
`(use-float y)` callsite). Land the positive fixture in the same PR as the fix.

> **Landed 2026-06-10 (implemented).** The positive fixture
> `tests/fixtures/bare-fat-nontail-float-noannot/` now compiles and prints 7.
> (The interim negative seed under `tests/fixtures/errors/` was removed when the
> mechanism landed, as planned.) The int-carrier analogue compiles via the
> canonical path; only the non-int register-class body is deferred + specialized.

### Step 5 -- dedup + int-only caller of a bare-^fat combinator

Add `tests/fixtures/bare-fat-int-and-float-no-annot/`: one combinator, two
call sites passing an `:int` closure and a `:float` closure respectively.
Assert in `expected.c` that exactly two specializations are emitted
(`run_with` and `run_with__bf_float`) and the `:int` path emits the
unspecialized name (canonical body unchanged).

### Open issues this slice still defers

- **Self-recursion**: a specialized clone calling itself must redirect to the
  same specialized binding (lookup in the cache by `(callee_fndef, k)` during
  the re-elab, keyed off the cloned binding's `source_binding` pointer).
- **Mutual recursion**: same scheme but the cache must be seeded **before**
  starting re-elab so the partner can find it.
- **Exports**: a specialization of an exported function is itself a fresh
  internal symbol; do not export the clone. Block specialization of imported
  bindings from other modules (no body to re-elab).
- **Cross-module dedup**: out of scope for the first PR. Each TU may emit its
  own copy of `run_with__bf_float`; if the linker complains, make the
  specialization `static` in the emitted C.

### Estimated size

~600-1000 LOC across `expr.h`, `elab_fns.c`, `elab_call.c`, plus 2 fixtures
and a ~1442-fixture snapshot regen. Single PR is feasible but takes a
multi-session commitment, not one execution turn. The fixture-only seed
(step 4) is a safe sub-PR to land first so the gap is visible in CI **(done
2026-06-10, as the negative fixture noted in step 4)**.

### Two enabling primitives the core PR must build first (confirmed missing 2026-06-10)

A scoping pass through the elaborator confirmed neither piece of infrastructure
the mechanism depends on exists yet -- both must be built as part of the core PR:

1. **Function-body re-elaboration (clone + re-elab).** There is no existing
   "clone a defn body and re-elaborate it under a new param scope" helper. The
   "ADT monomorphization" precedent the plan cites (`elab_call.c`, TY_APP path)
   is *type-level* struct/constructor codegen, not body re-elaboration -- it is
   not reusable here. The cheapest route is to retain the defn's `Form` (step 1)
   and re-invoke the signature+body machinery of `elab_defn` under a
   specialization context (mangled name, `bare_fat_result_kind` set, no
   re-export, no forward-decl rematch) rather than hand-factoring its ~2000-line
   body.
2. **Speculative elaboration with diagnostic rollback.** `diag.c` emits straight
   to stderr (only a `had_error_` bool; no capture/count/restore). The probe's
   canonical (int) body genuinely *fails* to elaborate (`(use-float y)` ->
   `TUR-E0001`), so deciding "this body is non-int-only, defer it" cannot be done
   without a `diag_push_capture()/diag_pop_capture(emit?)` primitive that
   suppresses + counts errors and restores `had_error_`. Without it, the lazy-body
   detection in step 1 spews spurious errors on every non-int-only bare-`^fat`
   defn. Build this primitive first; the common (int-carrier) case must stay
   byte-identical so existing fixtures/snapshots do not churn.
