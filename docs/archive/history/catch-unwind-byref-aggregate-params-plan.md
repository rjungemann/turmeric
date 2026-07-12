---
title: Stackless catch-unwind -- by-const-pointer aggregate params -- Plan
category: Planning
description: Let a trampolined function take a by-const-reference aggregate param (a C `const tur_adt_* *`, e.g. a `Result` param), whose pointee lives in the caller's C frame -- a frame the trampoline does not have. Follow-on to the completed general lowering (archived).
---

# Stackless catch-unwind -- by-const-pointer aggregate params -- Plan

> **ARCHIVED -- COMPLETE (BR1-BR2).** By-const-pointer aggregate params are
> trampolined behind `--enable=stackless-catch-unwind`. The residual widening
> work (BR3: relax the pure-accessor eligibility gate) proceeded in two steps:
> BR3a (in-place `match`/`.field`/`@` reads) landed -- see the archived
> [BR3 (BR3a) plan](./catch-unwind-byref-aggregate-br3-plan.md); the remaining
> BR3b/BR3c (pass the param to a pure const-by-ref reader) is tracked in
> [docs/upcoming/v1/catch-unwind-byref-aggregate-br3b-plan.md](../upcoming/v1/catch-unwind-byref-aggregate-br3b-plan.md).

## Status

BR1 + BR2 landed. A by-const-pointer aggregate param (a large by-value
product passed as `const <struct> *`, e.g. a `(Result int int)` param) is now
trampolined: `gs_param_class` classifies it via `type_struct_pass_by_ptr` and
flags it `is_ref`; a descend materializes the pointee on the heap
(`memcpy(box, cname, sizeof)` -- reading THROUGH the pointer, not `&cname`);
a resume re-homes the box into a stable function-scope buffer `<cname>__agg`
and re-points the param at it (`gs_restore`), freeing the box; the descend
arg-pass (`gs_self_descend` and the tail-call backedge in `cps_emit`) re-homes
each new arg's pointee into the same buffer with `memmove` (tolerating the
`arg == param` aliasing case). Eligibility stays conservative: the existing
pure-accessor gate (`ok?`/`err?`/`ok-val`/`err-val`/`some?`/`none?` only, plus
member calls) already enforces the read-only, identity-agnostic borrow the copy
requires, and the group path still bails on any aggregate param. Validated by
`tests/fixtures/stackless-catch-unwind-byref-aggregate-param` (matches native
at small depth, runs flat where native SIGSEGVs) and valgrind (the aggregate
box balances; the only residual leak is the pre-existing documented
catch-result box leak, identical to the by-value path).

Note: before this change the by-ref case did NOT bail as the "why" section
below assumed -- `type_c_name` yields the bare struct name for both the small
(by-value) and large (by-ref) product, so `gs_param_class` misclassified the
by-ref param as by-value and emitted `memcpy(box, &acc, ...)` (copying from the
address of the pointer variable). That silent miscompile is what BR1 fixes.

BR3 (widen eligibility past the pure-accessor gate) proceeded in two steps:
BR3a (in-place `match`/`.field`/`@` reads) landed --
[archived BR3 (BR3a) plan](./catch-unwind-byref-aggregate-br3-plan.md); the
remaining BR3b/BR3c is planned in
[docs/upcoming/v1/catch-unwind-byref-aggregate-br3b-plan.md](../upcoming/v1/catch-unwind-byref-aggregate-br3b-plan.md).

## Why this exists

The general lowering
([archived plan](../archive/compiled-catch-unwind-general-lowering-plan.md), G6)
handles **by-value** aggregate params (a C struct passed by value, ctype a bare
struct name) by heap-boxing them across a descend. It explicitly excludes
**by-const-pointer** aggregate params: `gs_param_class`
(`src/compiler/emit_fns.c`) only accepts a ctype with no `*`. A `Result` param,
for example, is passed as `const tur_adt_Result__int__int *` -- a pointer, so it
bails to native.

The reason is lifetime, not width. A by-const-ref param is a **borrow**: the C
signature receives `const T *acc`, and the pointee lives in the **caller's C
frame** (the argument was `&some_temp` at the call site). In native recursion
that temp lives on the caller's stack for the duration of the recursive call.
The trampoline has no per-level C frame -- the "caller" is a heap continuation
node -- so a saved bare pointer would dangle: the temp it points at is gone by
the time the resume reads it.

## The core question

Where does the pointee live across a descend?

- **Save the pointer only** -- WRONG. The pointee is in a C frame that the
  trampoline collapses; the pointer dangles. This is exactly why the by-value
  path copies the struct rather than a reference.
- **Materialize the pointee on the heap and save that** -- the fix. On descend,
  the param's pointee is copied to a heap box (`malloc(sizeof(T)) +
  memcpy(box, acc, sizeof(T))`), the slot holds the box pointer, and on resume
  the param C-var is re-pointed at the box: `acc = (const T *)box`. This is the
  by-value heap-box mechanism, one indirection removed -- save `*acc`, restore a
  pointer to the boxed copy.

This changes the borrow into an owned heap copy for the trampolined region. That
is observationally identical to native for a `const` (read-only) borrow with no
interior mutation and no identity/aliasing dependence -- which is the common case
for a recursion-carried `Result`/`Option`/aggregate. It is NOT safe if the callee
relies on pointer identity (comparing `acc` addresses) or mutates through a
non-const alias; both must be excluded.

## Design

- **Detection.** Extend `gs_param_class` to a third accepted category:
  by-const-ptr aggregate. Recognize a ctype of the form `const <struct> *`
  (and/or the underlying `Type` being an ADT/struct passed by-ref -- prefer the
  `Type` over string-matching the ctype). Emit `struct_ctype` = the pointee
  type name (strip `const` and `*`) so `sizeof(struct_ctype)` works.
- **Save (`gs_save`).** For a by-ref-aggregate var: `void *__ab =
  malloc(sizeof(<pointee>)); memcpy(__ab, <cname>, sizeof(<pointee>));
  node->saved[i] = (int64_t)(intptr_t)__ab;` -- note `memcpy(__ab, cname, ...)`
  (cname is already the pointer) vs the by-value form's `memcpy(__ab, &cname,
  ...)`.
- **Restore (`gs_restore`).** `<cname> = (<const-ctype>)(intptr_t)__k->saved[i];`
  -- re-point the param at its heap box. Do NOT free here: the box IS the
  pointee for the resumed segment's lifetime. Free it when the node is done with
  it (see ownership below).
- **Ownership / free.** The by-value path frees the box in `gs_restore` (it
  memcpys the value out, then the box is dead). The by-ref path keeps the box
  alive as the pointee -- so it must be freed when the resumed segment finishes
  (its value delivered), or arena'd per call-tree. Simplest correct: free the
  box at the end of the resume segment, after the continuation no longer reads
  the param. This needs the resume to know it owns a box -- track a per-node
  "aggregate box slots" set, or free in the same spot the node is freed.
- **Eligibility guard.** Reject if the param could be mutated (non-const path)
  or if pointer identity is observed. Conservatively: only accept a param typed
  as an immutable/`const` borrow of an aggregate, used only through read
  accessors. If that is hard to prove, keep the feature behind a stricter gate
  than the by-value case.

## Interaction with existing pieces

- **Group path.** Same restriction as by-value aggregates: the shim marshals
  through int64 `__a` slots, so a by-ref aggregate param forces group bail. Land
  single-function first (see the aggregate-followups plan for the group combo).
- **Result params specifically.** A `Result` param plus the whitelisted
  `ok?/err?/ok-val/err-val` accessors is the motivating case -- it would let a
  recursion thread a `Result` and inspect it, fully trampolined.

## Phases

- **BR1** -- detection + save/restore (materialize pointee), single-function,
  const-only, with a conservative eligibility gate. Differential-check a
  `Result`-param recursion against native.
- **BR2** -- box ownership/free discipline; valgrind for balance (happy path).
- **BR3** -- widen the eligibility once the identity/mutation guards are solid.

## Validation

- Default byte-identical; existing by-value + scalar cases unchanged.
- New fixture: a `(Result int int)` (or by-ref struct) param carried through
  recursion crossing `catch-unwind`, inspected via `ok?`/`ok-val`, matching
  native at small depth and running deep flat where native SIGSEGVs.
- valgrind: pointee boxes freed on the happy path.

## Risks

- Turning a borrow into an owned copy is only sound for read-only,
  identity-agnostic use. The eligibility gate is the load-bearing safety check --
  get it conservative first, widen later.
