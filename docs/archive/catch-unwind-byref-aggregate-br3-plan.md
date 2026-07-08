---
title: Stackless catch-unwind -- by-const-pointer aggregate params, widened eligibility (BR3) -- Plan
category: Planning
description: Widen the by-const-pointer aggregate param support in the stackless catch-unwind trampoline past the conservative pure-accessor gate BR1 shipped behind. A by-ref aggregate param currently rides the trampoline only when the body reads it through the whitelisted pure accessors; BR3 admits richer read-only, identity-agnostic uses while keeping the borrow-becomes-owned-copy soundness contract. Follow-on to BR1+BR2 (archived).
---

# Stackless catch-unwind -- by-const-pointer aggregate params, widened eligibility (BR3) -- Plan

> **ARCHIVED -- COMPLETE (BR3a).** The read-only use widening
> (`match`/destructure, `.field` reads, `@`-deref of a by-ref aggregate param
> in the single-function trampoline) landed; details below. The remaining BR3
> work -- BR3b (passing the param to a non-member pure const-by-ref reader,
> which needs the mid-segment `tur_panicking` panic route, not just a gate
> relaxation) and BR3c (the optional transitive summary) -- is split out into
> [docs/upcoming/v1/catch-unwind-byref-aggregate-br3b-plan.md](../upcoming/v1/catch-unwind-byref-aggregate-br3b-plan.md).
> BR1 (detection + save/restore) and BR2 (box ownership/free + valgrind
> balance) landed earlier -- see the archived
> [by-const-pointer aggregate params plan](./catch-unwind-byref-aggregate-params-plan.md).
>
> **BR3a landing (single-function only):** `gs_value_ok` in
> `src/compiler/emit_fns.c` now admits `EX_MATCH` / `EX_GET_FIELD` /
> `EX_DEREF` in value position when they are fully pure (no suspension, no
> inline `panic`) and the trampoline is single-function (`g_gs_nmem == 1`).
> `gs_suspends`, `gs_suspends_live`, and `gs_has_panic` were taught to
> descend into those three forms so the purity check is sound. The unsound
> uses (address observation via `(& p)`, borrow escape, mutation via
> `set!`, raw-pointer inline-C) stay at `gs_value_ok`'s `default: return
> false` and bail to native, unchanged. The cross-function GROUP path is
> deliberately excluded (see BR3b/group note below): a `match`/`.field` on a
> group member's by-ref param mis-lowers today (the pointee int64 slot is
> read as a by-value struct), so those cases bail to native exactly as they
> did before BR3a. Fixtures:
> `tests/fixtures/stackless-catch-unwind-byref-aggregate-field` (Result read
> via `.is-ok`, `is_ref` re-home across a descend),
> `.../stackless-catch-unwind-byref-aggregate-match` (`(Box int)`
> destructured via `match`, `is_ref`), and
> `.../stackless-catch-unwind-byref-aggregate-group-bail` (mutual `match`
> regression guard -- must compile + run, proving the group path bails).
>
> **BR3b/BR3c split out.** BR3b admits passing the param to a non-member
> pure const-by-ref reader. That is NOT a near-free change: native codegen
> hoists a fallible call into a temp and emits `if (tur_panicking) return;`
> after it, so admitting such a call mid-segment requires the trampoline's
> `cps_emit` to hoist the call and emit an `if (tur_panicking) break;`
> unwind route -- a departure from the current "only pure values in value
> position" invariant. Without a callee totality/no-panic proof, admitting
> the call is unsound. This (and the optional BR3c transitive summary) now
> lives in its own plan:
> [catch-unwind-byref-aggregate-br3b-plan.md](../upcoming/v1/catch-unwind-byref-aggregate-br3b-plan.md).
>
> **Prerequisites:** BR1+BR2. `gs_param_class` already classifies a by-ref
> aggregate param (`type_struct_pass_by_ptr`) and flags it `is_ref`; save
> materializes the pointee on the heap, restore re-homes it into a stable
> function-scope buffer `<cname>__agg`, and the descend arg-pass re-homes each
> new arg's pointee with `memmove`. All of that is unchanged by BR3.
>
> **Flag:** `--enable=stackless-catch-unwind` (unchanged; BR3 does not add a
> new gate, it relaxes an existing structural one).

## Why this exists

BR1 turns a `const <struct> *` borrow into an owned heap copy for the
trampolined region. That is observationally identical to native **only** for a
read-only, identity-agnostic const borrow -- no interior mutation, no
dependence on the pointer's address. BR1 bought that soundness cheaply by
leaning on the eligibility gate the general lowering already enforces: the body
may call only trampoline-group members and the whitelisted **pure accessors**
(`ok?`/`err?`/`ok-val`/`err-val`/`some?`/`none?`; see `gs_is_pure_accessor` in
`src/compiler/emit_fns.c`). A body that reads a `Result`/`Option` param through
exactly those accessors cannot mutate it or observe its address, so the copy is
sound.

That gate is conservative. It rejects legitimate read-only uses a by-ref
aggregate param would want:

- **Passing the param to another (non-member) pure function** that takes it
  `const`-by-ref and only reads it -- e.g. a user `defn describe [r : (Result
  int int)] : cstr` -- forces the whole function to bail to native.
- **Pattern-matching / destructuring** the param (a `match` on the ADT) rather
  than going through the `ok?`/`ok-val` accessor pair.
- **Field reads on a by-value product** that is not a `Result`/`Option` (a
  large record ADT) -- there are no whitelisted accessors for its fields, so
  any read disqualifies it.

BR3 admits these while preserving the contract.

## The core question

Which uses of a by-ref aggregate param are safe once its borrow is silently
replaced by an owned heap copy?

Safe (BR3 should admit):
- Any **read** of the pointee or its fields (accessor, field projection,
  `match`/destructure, deref-and-copy-out).
- Passing the param **by const-ref to another pure function** that itself only
  reads it (the callee receives a `const <struct> *` to the buffer; the buffer
  outlives the call).
- Copying the pointee **by value** out of the borrow.

Unsafe (BR3 must still reject):
- **Mutation** through any non-const alias (there is no `&mut` path to an
  immutable param today, but a future `defmut`/interior-mutability escape must
  keep the param out).
- **Pointer-identity** observation: comparing `&param` against another address,
  hashing the pointer, or storing the raw pointer somewhere that outlives the
  buffer and later compares it. The copy has a *different* address than the
  native borrow, so any identity dependence diverges from native.
- **Escaping the borrow**: returning the raw `const <struct> *` (or a reference
  into the pointee) so it outlives the trampoline frame's buffer. The buffer is
  reused per resume, so an escaped pointer dangles / aliases the wrong value.

## Design

- **Reclassify the gate as read-only, not accessor-whitelist.** Replace the
  "calls must be members or whitelisted accessors" check with a proper
  **use-analysis** over the by-ref param: walk every occurrence of the param
  binding in the body and classify it as READ (admitted), ADDRESS-OBSERVED
  (reject), or ESCAPES (reject). A call that passes the param to a `const`-by-ref
  parameter of a pure callee is a READ; a call that passes it to a `&mut` /
  non-const slot, or returns it, is a reject.
- **Callee const-borrow proof.** For "passes the param to another function",
  admit only when the callee's matching parameter is itself a by-const-ptr
  aggregate (`type_struct_pass_by_ptr` + immutable) *and* the callee does not
  leak it. Start with a one-level syntactic check (callee param is `const`-by-ref
  and the callee is not itself trampolined against this arg); deepen to a
  transitive "does not escape / does not mutate" summary only if a real fixture
  needs it.
- **Identity/escape rejection.** Reject if the param (or `&param`) flows into:
  an equality/comparison against a pointer, a store into a heap/struct field
  that outlives the call, a return position typed as the borrow, or any
  `#fx{Unsafe}` inline-C body (inline-C can do anything to the pointer -- treat
  it as an automatic reject for a by-ref param, matching the existing
  "no inline-C in variadic bodies" caution).
- **Keep the copy mechanics identical.** BR3 changes *which functions qualify*,
  not how a qualified param is saved/restored. `is_ref`, `<cname>__agg`, the
  `memcpy(box, cname, …)` save, and the `memmove` arg-pass are all unchanged.

## Interaction with existing pieces

- **Group path.** Still bails on any aggregate param (`|| aggr` in the group
  eligibility). BR3 is single-function only, exactly like BR1. Widening the
  group path to carry a by-ref aggregate through the shared int64 `__a` shim is
  a separate follow-up (see the aggregate-followups plan).
- **BR1 fixtures.** The existing pure-accessor fixture
  (`stackless-catch-unwind-byref-aggregate-param`) must stay byte-identical --
  its use is a strict subset of what BR3 admits, so the widened gate must not
  perturb its codegen.

## Phases

- **BR3a** -- DONE. Widen the read gate: admit pure (non-suspending,
  non-panicking) `match` / `.field` / `@`-deref reads of the by-ref param in
  the single-function trampoline; the address-observe / escape / mutation /
  inline-C forms stay at `default: return false` and bail. Implemented as an
  enumerate-and-admit relaxation of `gs_value_ok` rather than a separate
  occurrence classifier -- the existing whitelist structure means only the
  three explicitly-added pure-read forms are newly admitted, so an
  identity/mutation/escape use cannot be a false ADMIT (it is not one of the
  three). Verified: `.field`-read on a `Result` param and a `match` on a
  `(Box int)` param both run flat where native SIGSEGVs; the BR1 pure-accessor
  fixtures stay byte-identical.
- **BR3b / BR3c** -- SPLIT OUT to
  [catch-unwind-byref-aggregate-br3b-plan.md](../upcoming/v1/catch-unwind-byref-aggregate-br3b-plan.md).
  BR3b (admit passing the param to a pure `const`-by-ref non-member reader)
  is not a gate-only change like BR3a -- a native reader call can panic
  mid-segment, so it needs `cps_emit` to hoist the call and emit
  `if (tur_panicking) break;`. BR3c is the optional transitive
  escape/mutation summary. See the split-out plan for the full design.

## Validation

- Default byte-identical; BR1's pure-accessor fixture unchanged.
- New fixtures: (a) a by-ref aggregate param destructured via `match` across
  `catch-unwind`; (b) a by-ref aggregate param passed to a non-member pure
  const-by-ref reader across `catch-unwind`. Both match native at small depth
  and run flat where native SIGSEGVs.
- Negative fixtures: a param whose address is compared, one that is returned as
  the borrow, and one read inside `#fx{Unsafe}` inline-C -- each must **bail to
  native** (compile + run correctly, just not trampolined), proving the gate
  still rejects the unsound cases.
- valgrind: pointee boxes still balance (no change from BR2).

## Risks

- The use-analysis is the new load-bearing safety check. A false ADMIT on an
  identity- or mutation-dependent use silently diverges from native. Get the
  classifier conservative first (reject on anything not provably a read), widen
  only against real fixtures.
- The callee const-borrow proof is easy to over-trust. A one-level syntactic
  "callee param is `const`-by-ref" check does **not** prove the callee refrains
  from stashing the pointer; pair it with an escape check on the callee, or keep
  the proof shallow and the admitted set small until a transitive summary
  exists.
