---
title: Stackless catch-unwind -- by-ref aggregate param passed to a pure reader (BR3b/BR3c) -- Plan
category: Planning
description: Admit passing a by-const-pointer aggregate param to a NON-member pure const-by-ref reader inside the stackless catch-unwind trampoline. This is the remaining half of the BR3 eligibility widening: BR3a (read-in-place -- match/field/deref) landed; BR3b adds the one-level callee const-borrow proof, which -- unlike BR3a -- is not near-free because a native reader call can panic mid-segment. BR3c is an optional transitive escape/mutation summary. Follow-on to BR3a (archived).
---

# Stackless catch-unwind -- by-ref aggregate param passed to a pure reader (BR3b/BR3c) -- Plan

> **Status:** Not started. Split out from the BR3 plan when BR3a landed --
> see the archived
> [BR3 (BR3a) plan](../../archive/catch-unwind-byref-aggregate-br3-plan.md).
> BR3a widened the read gate to admit pure in-place reads (`match`
> destructure, `.field` projection, `@`-deref) of a by-ref aggregate param
> in the single-function trampoline. BR3b/BR3c cover the one remaining
> rejected read-only use: **passing the param to another function** that
> takes it `const`-by-ref and only reads it.
>
> **Prerequisites:** BR1+BR2 (by-ref param save/restore + box ownership) and
> BR3a (the read-gate widening). `gs_param_class` classifies a by-ref
> aggregate param (`type_struct_pass_by_ptr`, `is_ref`); save materializes
> the pointee on the heap, restore re-homes it into `<cname>__agg`, and the
> descend arg-pass re-homes each new arg's pointee with `memmove`. BR3a's
> gate (`gs_value_ok` admits pure `EX_MATCH`/`EX_GET_FIELD`/`EX_DEREF` when
> `g_gs_nmem == 1`) is the structural predecessor this plan extends.
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
- **BR3c (optional transitive summary).** A "does not escape / does not
  mutate" summary computed over the callee (and its callees) for
  deeper-than-one-level proof. Only build this if a real fixture needs it;
  otherwise leave BR3b's syntactic check as the documented ceiling.

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

- **BR3b** -- (1) the one-level callee const-borrow proof in `gs_value_ok`
  (admit a non-member call whose aggregate arg is the by-ref param and whose
  callee param is `const`-by-ref + the callee passes a cheap escape check);
  (2) the codegen: hoist the admitted call into a segment temp and emit
  `if (tur_panicking) break;` after it, in `cps_emit` / the value-hole path.
  Fixture: a single-function recursion that threads a by-ref aggregate and
  hands it to a non-member pure const-by-ref reader each level, read after a
  `catch-unwind` descend, running flat where native SIGSEGVs.
- **BR3c** -- (optional) a transitive escape/mutation summary if a real
  fixture needs deeper-than-one-level proof; otherwise leave BR3b's syntactic
  check as the ceiling and document it.

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
