---
status: resolved
resolved: 2026-06-23
severity: high
discovered: 2026-06-23
discovered-by: fat-closure-abi-audit-plan Phase 0
fix: src/compiler/elab_concurrent.c catch_thunk_to_fat
---

## Resolution (2026-06-23)

`catch_thunk_to_fat` was wrapping every `TY_FN` thunk through `EX_FN_TO_FAT`,
including capturing closures whose type is `TY_FN { boxed: true }`. A boxed
TY_FN already places its lifted thunk at slot 0 of the env box, so wrapping
it again produced a `{ __tur_fatshim0, env_ptr }` outer box whose
`__tur_fatshim0` read slot 1 as a bare fn pointer and jumped to the env
pointer. Fix: only auto-shim when `!thunk->type.as.fn.boxed`. Regression
locked in by `tests/fixtures/panic-catch-unwind-captures/`.

# `catch-unwind` SEGVs when its thunk captures locals

## Summary

A `(catch-unwind (fn [] : int <body that captures locals>))` call segfaults at
runtime. The captured environment is dropped and the env pointer is invoked as
a no-arg C function pointer.

This is the same bug class flagged in the archived
`docs/archive/httpd-middleware-plan.md` (row 55) — the archive's claim that
`EX_CATCH_UNWIND` mishandles env-bearing thunks was **right**; the survey in
`docs/upcoming/fat-closure-abi-audit-plan.md` that called it stale was wrong.

## Minimal repro

```turmeric
(defn report [n : int] : int
  (if (= n 42)
    (println "captured-survived")
    (println "captured-dropped"))
  0)

(defn main [] : int
  (let [captured 42
        _        (catch-unwind (fn [] : int (report captured)))]
    (println "done"))
  0)
```

```
$ ./build/tur emit-c repro.tur > /tmp/repro.c && cc -o /tmp/repro /tmp/repro.c
$ /tmp/repro; echo "exit=$?"
exit=138        # SIGSEGV
```

The dispatch path itself (`tur_catch_unwind_box` →
`TUR_APPLY0(thunk)`, `src/compiler/emit_module.c:5716-5733`) is correct. The
defect is upstream, at the elab/emit boundary.

## Root cause

`catch_thunk_to_fat` in `src/compiler/elab_concurrent.c:349` only wraps the
thunk through `EX_FN_TO_FAT` when its type is `TY_FN` (bare fn pointer). The
capturing closure in the repro reaches that site with type `TY_FN` rather
than `TY_PTR_VOID` (the capturing-closure carrier type), so it goes through
the auto-shim branch.

The auto-shim path in `src/compiler/emit_expr.c:5552-5599` then emits:

```c
struct __env_N *__t50 = malloc(...);          // EX_CLOSURE: real fat closure
__t50->__fn = __fn_NNN;
__t50->captured = 42;
void *__t51 = __t50;
int64_t *__t52 = malloc(2 * sizeof(int64_t)); // EX_FN_TO_FAT: double-box
__t52[0] = (int64_t)__tur_fatshim0;
__t52[1] = (int64_t)__t51;
tur_catch_unwind_box((int64_t)__t52);
```

`__tur_fatshim0(__e)` (`emit_module.c:4907`) reads `((int64_t*)__e)[1]` and
calls it as a no-arg fn pointer — but slot 1 is the closure env pointer, not
a bare function pointer, so the program SEGVs jumping to a heap address (or
to the captured value treated as code).

The bug is that EITHER:

1. The capturing closure is reaching `catch_thunk_to_fat` mistyped as `TY_FN`
   (so the inner type-classifier needs to be corrected), or
2. `catch_thunk_to_fat` needs to detect "this is already a fat closure" via
   a richer check (presence of an `EX_CLOSURE` ancestor under any `EX_ASCRIBE`
   wrap, not just the surface `type.kind`).

## Fix direction

Carried to Phase 2 of `docs/upcoming/fat-closure-abi-audit-plan.md` —
investigate whether the closure is being mistyped (fix the type) or whether
`catch_thunk_to_fat` needs to look through `EX_CLOSURE` / `EX_ASCRIBE`
wrappers before deciding to auto-shim.

A regression fixture (`tests/fixtures/panic-catch-unwind-captures/`) was
prototyped and pulled out of the tree pending the fix — the repro above is
the same shape.

## Adjacent risks

The same `EX_FN_TO_FAT`-wraps-a-fat-closure pattern can fire anywhere
`catch_thunk_to_fat`-shaped guards exist (`elab_typeclasses.c:354, 381`,
`elab_call.c:1952, 3420, 4061`, `elab_fns.c:762`). A focused sweep is worth
budgeting after the catch-unwind fix lands.

## Links

- Plan: `docs/upcoming/fat-closure-abi-audit-plan.md`
- Archive (now corrected): `docs/archive/httpd-middleware-plan.md`
