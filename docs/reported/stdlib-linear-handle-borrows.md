---
title: Linear Resource Handles Have No Borrow Form for Non-Consuming Accessors
category: Reported Bug
description: Promoting a stdlib resource handle to `:linear` (or `:affine`) per stdlib-linearity-affinity-plan makes its construct/consume pair safe (double-free is TUR-E0101, drop is TUR-E0100), but there is no borrow form for the handle's *non-consuming* accessors. Under -Xlinear, any read accessor (fs/tmpfile-path, fs/tmpfile-fd, promise-pending?, chan ops, mutex-lock/unlock, ...) consumes the handle, so a read followed by a free is a use-after-consume. This blocks promoting the multi-use handles in the inventory (Mutex, Chan, ...) and leaves the promoted handles' read accessors unusable under -Xlinear. Surfaced while executing stdlib-linearity-affinity-plan (L1 foundation + TmpFile representative).
---

# Linear Resource Handles Have No Borrow Form for Non-Consuming Accessors -- Reported Bug

> **Status:** OPEN (found 2026-06-04).
> **Found:** while executing
>   [stdlib-linearity-affinity-plan](../upcoming/stdlib-linearity-affinity-plan.md)
>   -- the foundational slice (defopaque `:linear`/`:affine` support +
>   inline-C linear-param consumption + `TmpFile` as the representative handle).
> **Severity:** Medium -- ergonomics / expressiveness gap, not a miscompile.
>   It blocks the bulk of the linearity-plan inventory from being promoted,
>   and it half-promotes the handles that *are* promoted (their read
>   accessors cannot be called under `-Xlinear`).

---

## Summary

`stdlib-linearity-affinity-plan` wants nine concurrency / I/O resource handles
promoted from plain `defopaque` to `:linear` / `:affine` so the type checker
catches double-free, use-after-free, missing-wait, and double-fulfill at
compile time.

The foundational compiler support now exists (see "What was implemented"
below) and the **construct -> consume** path works exactly as the plan wants.
But every one of these handles also exposes **non-consuming accessors** --
operations that read a handle and hand it back for further use:

| Handle      | Consuming op              | Non-consuming op(s) that re-use the handle |
|-------------|---------------------------|--------------------------------------------|
| `TmpFile`   | `fs/tmpfile-free`         | `fs/tmpfile-path`, `fs/tmpfile-fd`         |
| `Mutex`     | `mutex-free`              | `mutex-lock`, `mutex-unlock`, `mutex-try-lock` (called repeatedly) |
| `Chan`      | (close/free)              | `chan-send`, `chan-recv` (called repeatedly) |
| `Promise`   | `promise-fulfill`/`-free` | `promise-of-cell`, `future-of-cell`, status reads |
| `TaskGroup` | `task-group-wait`         | `task-group-spawn` (called repeatedly)     |

Under the linear discipline, **any use of a linear binding consumes it**
(`elab_toplevel.c` marks the binding `is_linear_consumed` on the first
`EX_VAR` reference). So:

```turmeric
(let [t (fs/tmpfile)]
  (fs/tmpfile-path t)   ;; consumes t
  (fs/tmpfile-free t))  ;; TUR-E0101: linear value 't' used after being consumed
```

There is no way to *borrow* `t` for the `fs/tmpfile-path` read while leaving
the single consumption obligation for `fs/tmpfile-free`. The language has the
ingredients (`&T` / `TY_REF_IMMUT`, `&mut T` / `TY_REF_MUT`, and the borrow
checker in `elab_call.c`), but:

1. The handle accessors are not declared to take borrows (`fs/tmpfile-path
   [t : TmpFile]`, not `[t : &TmpFile]`), so a normal call consumes.
2. There is no surface syntax / story for "borrow this linear value for the
   duration of one call" that has been wired through the resource-handle
   accessors and documented for stdlib authors.

## Impact on the plan

- **Multi-use handles cannot be promoted at all.** `Mutex` (lock/unlock/lock)
  and `Chan` (send/recv in a loop) reference the same handle many times; the
  second reference is always a `TUR-E0101`. These are L1 items in the plan but
  are blocked until borrows land.
- **Promoted handles are half-promoted.** `TmpFile` was promoted as the
  representative handle (it has a clean `tmpfile -> tmpfile-free` pair and zero
  existing fixtures, so the blast radius is contained). Its `new -> free` path
  is fully checked, but `fs/tmpfile-path` / `fs/tmpfile-fd` cannot be called
  under `-Xlinear` without consuming the handle. The module comment in
  `stdlib/fs.tur` points here.
- The plan's own "Risks" section anticipates this ("Provide an `unsafe-dup`
  escape hatch") but `unsafe-dup` is a duplication hatch, not a borrow; it does
  not give you "read without consuming, still owe exactly one free."

## Minimal repro

```turmeric
;; -Xlinear
(load "stdlib/fs.tur")
(defn main [] : int
  (let [t (fs/tmpfile)]
    (println (fs/tmpfile-path t))  ;; consumes t here ...
    (fs/tmpfile-free t))           ;; TUR-E0101 here
  0)
```

Observed: `TUR-E0101: linear value 't' used after being consumed`.
Expected (desired): the `fs/tmpfile-path` read borrows `t` immutably and does
not discharge the consume obligation, so the later `fs/tmpfile-free` is the
single legal consumption.

## Proposed fix directions

1. **Borrow-taking accessor signatures.** Declare non-consuming accessors as
   `[t : &TmpFile]` (immutable borrow) / `[t : &mut Mutex]` (mutable borrow for
   lock/unlock). Calls then borrow rather than consume, and the existing
   borrow-conflict machinery (`scope_borrow_conflicts`, `BK_MUT`) enforces
   exclusivity. This is the cleanest fix and reuses Phase-12 borrows.
2. **Auto-borrow at the call site.** When a linear/affine argument is passed to
   a parameter declared as `&T`, take an implicit borrow instead of consuming
   (the inverse of today's "any use consumes"). Requires the accessors from (1).
3. **`with-handle` scoping form.** A macro that opens a linear handle, exposes a
   non-linear borrow inside its body, and threads the single consume to the end.
   Sugar over (1)/(2).

Direction (1)+(2) is the right long-term answer and unblocks the whole
inventory. Until then, only construct/consume-only usage of a promoted handle
is expressible under `-Xlinear`.

## How to validate a fix

- `tests/fixtures/tmpfile-linear/` should be extendable to call
  `fs/tmpfile-path` / `fs/tmpfile-fd` between `fs/tmpfile` and
  `fs/tmpfile-free` and still type-check under `-Xlinear`.
- A `mutex-linear` fixture doing `lock; ...; unlock; ...; free` should
  type-check, and a `free; lock` ordering should be `TUR-E0101`.
- The negative fixtures (`errors/tmpfile-linear-double-free`,
  `errors/tmpfile-linear-dropped`) must keep failing exactly as today.

---

## Appendix: what was implemented in this slice (not a bug)

The foundational compiler support landed alongside this report and is working:

- `defopaque Name :base :linear` / `:affine` is now parsed and enforced
  (previously the attribute was silently dropped -- the elaborator only read
  name + base type). The forward-declaration stub is reused in place so
  `: Name` annotations pick up the discipline (`elab_structs.c`).
- A `defopaque ... :affine` handle is `CK_UNIQUE` + `SK_AFFINE` (drop allowed,
  duplication rejected); `:linear` is `CK_LINEAR` (exactly-once).
- Inline-C function bodies no longer spuriously report `TUR-E0100` on their own
  linear parameters: an inline-C body is opaque to the checker, so the
  exactly-once obligation is enforced at the Turmeric call site (matching the
  `linear-ffi` model). Without this, *every* resource-handle accessor written
  in inline-C (`file-close`, `promise-fulfill`, `fs/tmpfile-free`, ...) would
  fail to compile under `-Xlinear`. (`elab_fns.c`)
- `TmpFile` promoted to `:linear` as the representative handle, with positive
  (`tmpfile-linear`) and negative (`errors/tmpfile-linear-double-free`,
  `errors/tmpfile-linear-dropped`) fixtures.

## Appendix: unrelated pre-existing finding regenerated in the same commit

While running the full suite, two codegen snapshots --
`tests/fixtures/instance-closure-return-bool/expected.c` and
`tests/fixtures/instance-closure-return-float/expected.c` -- were found to
**fail on a pristine tree and on `origin/main`**, independent of this work.
Their `expected.c` encodes the old `?`->`_` / `!`->`_` identifier mangling,
whereas the current compiler (and ~1015 other fixtures) uses the two-letter
mnemonic scheme in `src/compiler/mangle.c` (`?`->`qu`, `!`->`ex`, `'`->`qt`).
The entire diff is that mnemonic substitution with no semantic change, so the
snapshots are stale -- they were missed when the mnemonic mangling landed
upstream. They were regenerated to restore a green suite; this is snapshot
maintenance, not a compiler change.
