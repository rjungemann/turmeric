---
title: fat_captures_borrowed was read uninitialized
category: History
description: A _Bool guarding a fat-closure drop was never initialized at any of its three allocation sites; 60 in-tree fixtures tripped UBSan on every suite run and nothing failed, because UBSan here prints and continues.
---

# `fat_captures_borrowed` was read uninitialized

**Severity:** medium. Genuine undefined behaviour on a flag whose only job is to
prevent a heap-use-after-free. Benign on today's corpus by luck, not by design.

**Status:** RESOLVED 2026-08-25. Three-line fix. Found while probing
`stdlib/logic.tur` for the arena thread -- the UBSan line appeared in the middle
of unrelated output and did not belong to what was being measured.

## What was wrong

`struct Closure` (`expr.h:967`) is allocated with `arena_alloc`, which does not
zero. Two of its three allocation sites set only some fields; `elab_call.c:3936`
even carries the comment `/* arena mem is not zeroed */` next to two sibling
flags it initializes explicitly. None of the three initialized
`fat_captures_borrowed`, and it is only ever assigned `true`
(`elab_concurrent.c:435`) -- never `false`.

Two sites read it: `emit_fns.c:3131` and `emit_expr.c:125`.

```
src/compiler/emit_fns.c:3131:66: runtime error: load of value 190,
    which is not a valid value for type '_Bool'
```

190 is 0xBE -- arena garbage, consistently truthy in practice.

## Why it mattered

The flag's own struct comment says what it is for: a `^fat` handle captured by a
catch-site thunk is BORROWED from the enclosing frame, so releasing it in the
thunk env's drop glue frees a handle the enclosing closure still owns --
"the first call through a captured handle worked and the second read freed
memory (`heap-use-after-free ... freed by drop_glue___env_NNNN`)".

So the flag exists to suppress exactly one use-after-free, and it was being read
out of uninitialized arena memory. Garbage reading truthy suppresses a drop that
should happen (a leak); garbage reading falsy performs a drop that must not
happen -- reinstating the UAF the flag was added to fix, nondeterministically,
depending on what previously occupied that arena byte.

## Exposure, measured

**60 of 2092 in-tree fixtures** trip the read on every `emit-c`:

| area | fixtures |
|---|---:|
| httpd | 33 |
| arrow | 9 |
| logic | 8 |
| hkt | 3 |
| stdlib | 2 |
| other | 5 |

Nothing failed, because the Debug build uses `-fsanitize=address,undefined`
without `-fno-sanitize-recover`: UBSan prints to stderr and execution continues,
and `tests/run.sh` compares stdout. So this has been firing on ~3% of the corpus,
on every run, invisibly.

## What the fix changed, and what it did not

Initializing the flag to `false` at all three sites takes the UBSan hits from
**60 to 0**, and changes the emitted C on **0 of the 60** fixtures.

That second number is the honest scope of it. The read sits third in a
short-circuit chain:

```c
} else if (cap && cap->is_fat && !fd->closure->fat_captures_borrowed &&
           !(cap->closure_fn_binding && fd->binding &&
             cap->closure_fn_binding == fd->binding)) {
```

On every fixture that reaches the read, the fourth condition independently
excludes the branch, so the garbage never decided the outcome. The bug is real
UB and the guard is one arena byte from mattering, but no current program was
miscompiled by it -- which is why a corpus-wide snapshot diff is clean and why
this is filed as history rather than as a regression.

## Worth carrying forward

**UBSan is non-aborting here, so the suite could not see this class of bug.** A
`_Bool` holding 190 is as loud as a diagnostic gets and it still cost nothing to
ignore for as long as it existed.

**That gap is now closed.** `tests/run.sh` scans each phase's captured stderr
for `: runtime error:` and reports the findings after the summary; they do not
fail the run unless `TUR_SANITIZER_GATE=1` is set. Non-fatal by default was
deliberate -- arming it on discovery would have turned 60 silent findings into
60 red fixtures at once, which is how a gate gets switched off rather than
fixed. The tree is at zero findings, so CI can arm it whenever it wants.

The gate was verified by putting this bug back: deleting the three initializers
and re-running takes the count from 0 to 63 across 59 fixtures, and
`TUR_SANITIZER_GATE=1` then exits 1. That check earned its keep -- the first
version of the gate reported a clean tree *with the bug present*, because
`note_sanitizer` was not in `run.sh`'s `export -f` list and so did not exist
inside the xargs workers. A gate that has never been seen to fire is not a
gate. Details in
[test-suite-portability-guide.md](../../guides/test-suite-portability-guide.md#7b-ubsan-findings-in-the-compiler-are-collected-not-fatal).
