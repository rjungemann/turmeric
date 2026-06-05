---
title: A ^borrow parameter forwarded (or left unused) in a Turmeric body spuriously reports TUR-E0100
category: Reported
description: The LT1 linear-drop check exempted inline-C-bodied accessors but not ^borrow parameters themselves, so a Turmeric-bodied function that merely forwards its borrowed handle to another ^borrow parameter was wrongly flagged as dropping a linear value. Fixed by exempting ^borrow params from the consumption obligation.
---

# `^borrow` parameter forwarded/unused in a Turmeric body spuriously reports TUR-E0100

**Status:** RESOLVED (fixed in the stdlib-linearity-affinity L1/L2 pass).

**Summary:** Under `-Xlinear`, a function with a `^borrow` parameter whose body
is *not* inline-C was required to "consume" that parameter, even though
`^borrow` means non-consuming. Any thin delegating wrapper that forwarded its
borrowed handle to another `^borrow` parameter -- or simply did not mention it
-- reported `TUR-E0100: linear parameter '<p>' dropped without being consumed`.

**Severity:** expressiveness gap / false-positive hard error. It blocked
promoting `TaskGroup` to `:linear`: the convenience wrappers
`task-group-cancel-panic` / `-timeout` / `-error` delegate to
`task-group-cancel-with-reason` (a `^borrow` accessor), and each wrapper's
`^borrow group` was flagged on the delegation.

## Minimal repro

```turmeric
;; flags: -Xlinear
(defopaque H :ptr<void> :linear)
(defn h-read  [^borrow x : H] : nil ```c (void)x; ```)   ;; OK (inline-C body)
(defn h-read2 [^borrow x : H] : nil (h-read x))          ;; ERROR (forwarded borrow)
(defn h-unused [^borrow x : H] : nil 0)                  ;; ERROR (unused borrow)
```

- **Observed:** `h-read2` and `h-unused` both emit `TUR-E0100`.
- **Expected:** both are fine -- a `^borrow` parameter carries no consumption
  obligation; the caller retains ownership and the borrow auto-releases at scope
  exit.

## Root cause

`src/compiler/elab_fns.c`, the LT1 "verify all linear params were consumed"
check at function-scope exit. It carved out inline-C bodies
(`body_is_inline_c`) -- which is why every existing inline-C resource accessor
(`mutex-lock`, `fs/tmpfile-path`, ...) was unaffected -- but it did **not**
skip parameters flagged `is_borrow`. So a `^borrow` param only escaped the
check by accident of having an inline-C body. A Turmeric-bodied borrow accessor
fell through to the drop diagnostic.

The same omission existed in the lambda-scope-exit LT1 check (CC4.4).

## Fix

Exempt `^borrow` parameters from the consumption obligation in both LT1 checks:

```c
if (params[_li]->is_borrow) continue;
```

added ahead of the `is_linear && !is_linear_consumed && !is_moved` test at both
the defn-scope and lambda-scope sites in `elab_fns.c`.

## Validation

- Repro above type-checks cleanly.
- `tests/fixtures/taskgroup-linear` (positive) exercises cancel/wait/cancelled?
  borrows plus the delegating cancel wrappers, then a single `task-group-free`.
- `tests/fixtures/errors/taskgroup-linear-dropped` still correctly reports
  `TUR-E0100` for a *value* (let-bound `g`) that is borrowed but never freed --
  the fix narrows the exemption to borrow *parameters*, leaving the
  drop-a-real-owner diagnostic intact.
- `bash tests/run.sh` green with leak detection on.
