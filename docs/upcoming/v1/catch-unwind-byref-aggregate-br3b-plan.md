---
title: Stackless catch-unwind -- by-ref aggregate param passed to a pure reader (BR3b/BR3c) -- Plan
category: Planning
description: Admit passing a by-const-pointer aggregate param to a NON-member pure const-by-ref reader inside the stackless catch-unwind trampoline. This is the remaining half of the BR3 eligibility widening: BR3a (read-in-place -- match/field/deref) landed; BR3b adds the one-level callee const-borrow proof, which -- unlike BR3a -- is not near-free because a native reader call can panic mid-segment. BR3c is an optional transitive escape/mutation summary. Follow-on to BR3a (archived).
---

# Stackless catch-unwind -- by-ref aggregate param passed to a pure reader (BR3b/BR3c) -- Plan

> **Status:** BR3b LANDED. BR3c LANDED. Split out from the
> BR3 plan when BR3a landed -- see the archived
> [BR3 (BR3a) plan](../../archive/catch-unwind-byref-aggregate-br3-plan.md).
> BR3a widened the read gate to admit pure in-place reads (`match`
> destructure, `.field` projection, `@`-deref) of a by-ref aggregate param
> in the single-function trampoline. BR3b -- **passing the param to a
> non-member pure const-by-ref reader** -- is done; BR3c -- **a reader may
> now hand that borrow on to a FURTHER pure reader, proven read-only
> transitively** -- is also done.
>
> **BR3b landing (single-function only):** `gs_value_ok` in
> `src/compiler/emit_fns.c` now admits a call that hands fd's by-const-ptr
> aggregate param to a pure reader -- `gs_is_br3b_reader_call`: the callee
> resolves to a top-level defn whose matching param is `const`-by-ref, whose
> return is a scalar slot (never the borrow), and whose body only READS that
> param (`gs_reader_use_ok`: `.field` / `match` / `@` / pure-accessor uses,
> a `panic` is allowed, no inline-C, no borrow/store/return of the param, no
> handing it to a non-accessor callee -- that transitive case is BR3c). The
> call is FALLIBLE, so emission (`cps_emit_br3b` / `gs_preemit_br3b`) hoists
> it into a temp and emits `if (tur_panicking) break;` after it -- the
> segment analogue of native's post-call `if (tur_panicking) return;`,
> routing a reader panic to the driver unwind instead of a driver-escaping
> return. The break is produced by flipping `emit_panic_signal_return` via
> the new `ctx->panic_signal_is_break` flag, set only around the reader call.
> Guards keep it sound: single-function only (`g_gs_nmem == 1`, like BR3a); a
> reader call sharing a value expression with a suspension bails (it would be
> reordered after the suspension's descend -- observable since a reader can
> panic); and a reader call inside a value-position conditional bails (it
> would be hoisted unconditionally). Fixtures:
> `tests/fixtures/stackless-catch-unwind-byref-aggregate-reader` (Result
> handed to a `.is-ok` reader, read after a descend, flat at 200000) and
> `.../stackless-catch-unwind-byref-aggregate-reader-panic` (the reader
> panics in the base case; the panic routes via the `break` to the enclosing
> catch-unwind).
>
> **BR3c landing (transitive read proof).** `gs_reader_use_ok` no longer
> rejects a reader that hands its borrow to a non-accessor callee: the new
> `gs_callee_reads_arg_only` recurses through the callee to prove that
> callee also only READS the borrow. When the callee receives the arg
> by-const-ref it borrows the same pointer, so the proof recurses into the
> callee body (`gs_reader_use_ok` on the callee's matching param); when it
> receives the arg by-value an independent aggregate copy is made at the
> boundary, so the copy cannot alias the trampoline buffer and no recursion
> is needed. The chain is bounded by `GS_READER_MAX_DEPTH` and a
> visited-FnDef stack (seeded with the BR3b reader itself), so a
> (mutually) recursive reader is rejected conservatively rather than
> looping. **No new codegen:** the transitive callees are native-emitted and
> propagate a panic through their own post-call return-signal checks up to
> the top-level reader, where BR3b's hoisted `if (tur_panicking) break;`
> already routes it to the driver unwind. Fixtures:
> `.../stackless-catch-unwind-byref-aggregate-reader-transitive` (`step` ->
> `describe` -> `inner`; the borrow threads two levels of pure reader, flat
> at 200000) and
> `.../stackless-catch-unwind-byref-aggregate-reader-transitive-escape-bail`
> (the deeper callee RETURNS the borrow -- an escape -- so the chain is
> rejected and `step` bails to native, runs correctly at small depth).
>
> **Prerequisites (landed):** BR1+BR2 (by-ref param save/restore + box
> ownership) and BR3a (the read-gate widening). `gs_param_class` classifies a
> by-ref aggregate param (`type_struct_pass_by_ptr`, `is_ref`); save
> materializes the pointee on the heap, restore re-homes it into
> `<cname>__agg`, and the descend arg-pass re-homes each new arg's pointee
> with `memmove`.
>
> **Flag:** `--enable=stackless-catch-unwind` (unchanged; graduated to
> always-on -- BR3b relaxes an existing structural gate, it adds no new one).

## Why this exists

BR3a made the trampoline admit reads of a by-ref aggregate param *in place*
(`match`, `.field`, `@`). It still rejects the one read-only use left on the
BR3 list:

- **Passing the param to another (non-member) pure function** that takes it
  `const`-by-ref and only reads it -- e.g. a user `defn describe [r : (Result
  int int)] : cstr` called as `(describe acc)` -- forces the whole function
  to bail to native.

Observationally, `(describe acc)` where `describe` only reads its borrow is a
READ: the callee receives a `const <struct> *` into the trampoline's stable
`<cname>__agg` buffer, which outlives the call. Admitting it is exactly the
borrow-becomes-owned-copy contract BR1 established. The blocker is not
soundness of the *borrow* -- it is soundness of the *call*.

## The core problem BR3a did not have to solve: a native call can panic

BR3a's admitted forms (`match`/`.field`/`@`) are pure, total reads: they call
no user code and cannot panic. So `gs_value_ok` could admit them and let the
`emit_value` fast path emit the whole read atomically -- no mid-segment
control flow.

`(describe acc)` is different. `describe` is an ordinary function; it can
`panic`. Native codegen already accounts for this: a fallible call is hoisted
into a temp with a following `tur_panicking` check. For

```turmeric
(defn caller [x : int] : int (+ 10 (describe x)))
```

native emits (verified against the current backend):

```c
static int64_t caller(int64_t x) {
    __auto_type __ps_182 = (describe(x));
    if (tur_panicking) return (int64_t){0};   // <-- prompt unwind
    return (INT64_C(10)) + (__ps_182);
}
```

The trampoline's value-position fast path emits the *whole* enclosing
expression as one C expression (`(10 + describe(acc))`) with **no** place to
insert a statement-level `if (tur_panicking) …`. That is exactly why the
general lowering rejects every non-member, non-accessor call today: the
trampoline invariant is "only pure values in value position." Admitting a
fallible native call in a segment breaks that invariant.

So BR3b is **not** a gate-only relaxation like BR3a. It needs codegen:

- **Hoist the admitted callee call into a segment-local temp**, mirroring the
  native `__ps_NNN` pattern, instead of letting `emit_value` inline it.
- **Emit an `if (tur_panicking) break;` after the call**, routing to the
  driver's `for(;;)` unwind loop (which pops boundary-less nodes to the
  nearest catch, or to DONE -> propagate). A `break` here is the segment
  analogue of native's `if (tur_panicking) return;`.
- Continue the enclosing expression in the resume/continuation only after the
  panic check, with the temp standing in for the call (the existing
  `sub_holes` hole mechanism already re-emits an enclosing expression with a
  suspension replaced by a temp; BR3b reuses that shape for a *non-suspending
  but fallible* call).

Without this, admitting `(describe acc)` would compute `10 + garbage` and
deliver it before the next driver iteration notices `tur_panicking` -- which
*happens* to match native's own "compute then check" ordering in simple
cases, but is fragile (a garbage value used as a pointer, or a delivery with
a side effect, diverges) and must not be relied on. Get the panic route
explicit.

## The callee const-borrow proof

Even with the panic route, admitting `(describe acc)` requires proving the
callee only *reads* the borrow -- it must not stash the pointer, mutate
through it, or return it (the buffer is reused per resume, so an escaped
pointer dangles / aliases the wrong value).

- **BR3b (one-level syntactic check).** Admit only when the callee's matching
  parameter is itself a by-const-ptr aggregate (`type_struct_pass_by_ptr` +
  immutable) *and* the callee is not itself trampolined against this arg. This
  is shallow: it proves the callee *receives* a const borrow, not that it
  *refrains from stashing* it. Pair it with a cheap escape check on the callee
  body (no `EX_INLINE_C`, no return-position use of the param, no store of the
  param into a heap/struct field), or keep the admitted set small until BR3c.
- **BR3c (transitive summary) -- LANDED.** `gs_callee_reads_arg_only`
  extends the escape check across a call: when the reader body hands the
  borrow to a further callee, resolve that callee and, if it takes the arg
  by-const-ref, recurse `gs_reader_use_ok` on its matching param (a by-value
  arg is an independent copy and needs no recursion). The recursion is
  bounded by `GS_READER_MAX_DEPTH` and a visited-FnDef stack so a
  (mutually) recursive callee is rejected rather than looping. This turns
  BR3b's "no return-position use of the param, no store" escape check into a
  transitive proof over the whole reader chain, not just one level.

## Also out of scope here (tracked separately): the group path

BR3a is single-function only (`g_gs_nmem == 1`). The cross-function GROUP
driver carries a by-ref aggregate param through the int64 `__a` shim, and its
pbp-deref is only wired for the accessor path a group already admitted -- a
`match`/`.field` on a group member's by-ref param mis-lowers today (the
pointee int64 slot is read as a by-value struct; it once emitted a by-value
struct init from an int64 pointer, a C compile error). BR3a's gate keeps
these out of the group path, and the
`stackless-catch-unwind-byref-aggregate-group-bail` fixture guards that they
bail to native. Widening the group path to carry BR3a/BR3b reads is a
**separate follow-up** (see the archived
[aggregate-followups plan](../../archive/catch-unwind-aggregate-followups-plan.md)),
independent of BR3b; do not fold it in here.

## Phases

- **BR3b** -- DONE. (1) the one-level callee const-borrow proof in
  `gs_value_ok` (`gs_is_br3b_reader_call` + `gs_reader_use_ok`: the by-ref arg
  goes to a `const`-by-ref reader param, scalar return, param used only as a
  read, no inline-C); (2) the codegen: `gs_preemit_br3b` hoists the call into a
  temp and `emit_panic_signal_return` emits `if (tur_panicking) break;` via
  `ctx->panic_signal_is_break`. Guards: single-function only; no reader call
  sharing a value expression with a suspension (reorder hazard); no reader call
  in a value-position conditional (unconditional-hoist hazard). Verified: a
  reader-fed recursion runs flat at 200000 where native SIGSEGVs, a reader
  panic in the trampolined region is caught by the enclosing catch-unwind, and
  the negative shapes (aggregate return / transitive escape / inline-C reader /
  reader-left-of-a-suspension) all bail to native. BR3a/BR1 fixtures stay
  byte-identical; full suite 1983 passed, 0 failed.
- **BR3c** -- DONE. The transitive escape/mutation summary:
  `gs_callee_reads_arg_only` lets a reader hand its borrow to a further
  callee and proves that callee reads-only too (recurse on a by-const-ref
  arg; accept a by-value copy without recursion), bounded by
  `GS_READER_MAX_DEPTH` + a visited-FnDef cycle guard. No new codegen -- the
  transitive callees are native and route panics through BR3b's existing
  hoist-and-break. Verified: a two-level reader chain runs flat at 200000; a
  chain whose deeper callee escapes the borrow bails to native and runs
  correctly; the whole byref-aggregate family and full suite (1985 passed, 0
  failed) stay green.

## Validation

- Default byte-identical; BR3a's and BR1's fixtures unchanged.
- New positive fixture: a by-ref aggregate param passed to a non-member pure
  const-by-ref reader across `catch-unwind`; matches native at small depth and
  runs flat where native SIGSEGVs. (Today this program is the
  `stackless-catch-unwind-byref-aggregate-*` "pass-to-non-member" shape that
  BR3a deliberately bails to native; BR3b flips it to trampolined.)
- Negative fixtures: a callee that stashes / returns / mutates the borrowed
  pointer, and one whose reader is `#fx{Unsafe}` inline-C -- each must **bail
  to native** (compile + run correctly, just not trampolined), proving the
  callee proof rejects the unsound cases.
- A panic fixture: the admitted reader `panic`s inside the trampolined
  region; the trampoline must unwind to the enclosing `catch-unwind` (or
  propagate) identically to native -- this exercises the new
  `if (tur_panicking) break;` route.
- valgrind: pointee boxes still balance (no change from BR2).

## Risks

- **The panic route is the new load-bearing piece.** A missing or misplaced
  `if (tur_panicking) break;` lets a segment compute and deliver a garbage
  value on the panic path, diverging from native in exactly the cases that are
  hard to reproduce. Add the panic fixture first and confirm the unwind before
  widening the admitted set.
- **The callee const-borrow proof is easy to over-trust.** A one-level
  syntactic "callee param is `const`-by-ref" check does **not** prove the
  callee refrains from stashing the pointer; pair it with the escape check, or
  keep the proof shallow and the admitted set small until BR3c exists.
- **Do not entangle the group-path widening.** It is a separate follow-up with
  its own ABI (int64 `__a` shim) concerns; folding it into BR3b would couple
  two independent soundness stories.
