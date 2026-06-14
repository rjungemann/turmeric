# `task-group-new` under-allocates TaskGroupBlock; cancel writes `cancel_reason` out of bounds

**One-line summary:** `stdlib/taskgroup.tur`'s `task-group-new` allocates a
`TaskGroupBlock` whose local `typedef struct` **omits** the trailing
`int64_t cancel_reason` field, but `task-group-cancel` /
`task-group-cancel-with-reason` (and `task-group-cancel-reason`) cast the same
pointer to a struct that **includes** `cancel_reason` and write/read it -- a
heap-buffer-overflow of one `int64_t` past the allocation.

**Severity:** Medium-high -- a real **out-of-bounds heap write** on the compiled
path. Currently latent: it "works by luck" because `malloc` rounds the chunk up
(the struct already carries `pthread_mutex_t`/`pthread_cond_t` and tail padding),
so the extra 8 bytes usually land in slack. That is exactly the
"works-by-luck-because-the-allocation-happens-to-be-big-enough" hazard CLAUDE.md
calls a bug, not a non-issue. A tighter allocator, a layout change, or ASan with
a tight redzone could turn it into a crash or corruption.

## Repro / evidence

`stdlib/taskgroup.tur`:

- Documented canonical layout (comment, lines ~26-33) **includes**
  `int64_t cancel_reason`.
- `task-group-new` (line ~78-86): the local `typedef struct { ... bool done; }
  TaskGroupBlock;` has **no** `cancel_reason`, then
  `malloc(sizeof(TaskGroupBlock))` -- i.e. allocates the smaller size.
- `task-group-cancel` (line ~305-318): local struct ends `... bool done;
  int64_t cancel_reason; }` and executes `g->cancel_reason = 0;` -- writing 8
  bytes past what `new` allocated.
- `task-group-cancel-with-reason` (~346-353) writes `g->cancel_reason = reason;`
  and `task-group-cancel-reason` (~424-429) reads `g->cancel_reason` -- same
  over-read.

The per-function local `typedef` (rather than one shared struct) is what let the
two sizes drift without a compile error.

## Observed vs expected

- Observed: no crash today (slack absorbs the write), but the program writes/reads
  outside the allocated object -- undefined behavior.
- Expected: every accessor of a `TaskGroupBlock` agrees on its size, and the
  allocation covers `cancel_reason`.

## Proposed fix

Make `task-group-new` allocate the **full** layout (add `int64_t cancel_reason;`
to its local struct, matching cancel/cancel-with-reason/cancel-reason and the
documented layout). Best long-term: hoist the struct to a single shared C
declaration (e.g. an `extern-c` typedef or a runtime header) so the four bodies
cannot drift again. Initialise `cancel_reason = 0` in `new`.

## Validation

- Rebuild Debug (ASan); `tests/fixtures/taskgroup-*` stay green with the field
  now in-bounds.
- The interpreter shim (`wk_register_taskgroup_natives`, `src/main.c`) already
  allocates the full layout including `cancel_reason`, so `--interpret` is
  unaffected by the fix and stays correct either way.

## Notes

Found while implementing the R1 taskgroup interpreter native shims
([turi-interpret-flip-residual-plan.md](../archive/history/turi-interpret-flip-residual-plan.md)).
The shim deliberately allocates the full struct to avoid inheriting this bug.
