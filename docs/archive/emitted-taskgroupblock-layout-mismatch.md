# Emitted `TaskGroupBlock` shims declare the wrong struct layout

**RESOLVED 2026-07-29**: all three emitted `TaskGroupBlock` typedefs (the
report found two; a third in `tur_task_group_notify_done` had correct offsets
but a misleading trailing field name) now spell the ONE canonical layout,
verbatim from `stdlib/taskgroup.tur:78`.  Verified on Linux: `taskgroup-async`
passes under `tur jit` and cc, suite 2399/0, product sweep byte-identical.
The macOS silent-empty-stdout repro should be confirmed fixed on the next
macOS run.  The report's layered-luck analysis (glibc zero byte x clang bool
masking) is preserved below -- it is the reason this survived every Linux
sweep.

---

**Severity: high** (host-independent latent memory corruption; currently masked
on the `cc` path by luck, and already causing a silent wrong answer under
`tur jit` on arm64 macOS).

## One-line summary

`emit_module.c` emits **two different, mutually inconsistent** local `typedef`s
for `TaskGroupBlock`, and **neither matches** the real layout that
`stdlib/taskgroup.tur` allocates. Both therefore read and write the wrong
offsets of a live `TaskGroupBlock` -- including taking `pthread_mutex_lock` on
an address that is not the mutex.

## The three layouts

Canonical, as allocated by `task-group-new` (`stdlib/taskgroup.tur:78-86`, and
documented identically at `stdlib/taskgroup.tur:27-33`):

```c
typedef struct {
    pthread_mutex_t lock;        /* offset 0   (64 bytes on arm64 macOS) */
    pthread_cond_t  done_cond;   /* offset 64  (48 bytes)                */
    int64_t task_count;          /* offset 112 */
    int64_t completed_count;     /* offset 120 */
    bool    cancelled;           /* offset 128 */
    bool    done;                /* offset 129 */
    int64_t cancel_reason;       /* offset 136 */
} TaskGroupBlock;
```

What the emitter claims, in `tur_fiber_block_resume`
(`src/compiler/emit_module.c:8497`):

```c
typedef struct TaskGroupBlock { bool cancelled; } TaskGroupBlock;
if (((TaskGroupBlock *)f->task_group)->cancelled) { f->cancelled = 1; f->done = 1; return 0; }
```

-> reads `cancelled` at **offset 0**, i.e. the first byte of the initialized
`pthread_mutex_t`. On this box that byte is `0x5A`.

And what it claims in `tur_fiber_shim` (`src/compiler/emit_module.c:8448`):

```c
typedef struct TaskGroupBlock { bool cancelled; bool done; int64_t cancel_reason;
                                pthread_mutex_t lock; pthread_cond_t done_cond; } TaskGroupBlock;
TaskGroupBlock *g = (TaskGroupBlock *)task_group;
pthread_mutex_lock(&g->lock);
g->cancelled = true;
g->done = true;
```

-> writes `cancelled`/`done` over bytes 0-1 **of the mutex**, writes
`cancel_reason` at offset 8 (still inside the mutex), and calls
`pthread_mutex_lock` on **offset 16** -- the middle of the real mutex, not the
mutex itself. This is the panic-propagation path.

The two shims do not even agree with each other, which is the tell that neither
was checked against `task-group-new`.

## Observed impact

Under `tur jit` on arm64 macOS, `tests/fixtures/taskgroup-async` exits **rc=0
with completely empty stdout** (expected `10 / 20 / 30 / done`), deterministically
across runs. Correct under `tur run`.

Trace of the resume path, jit vs cc:

```
JIT: AW pre done=0 cancelled=0 tg=... tgbyte=90  ->  AW post done=1 cancelled=1   (fiber entry never runs)
cc : AW pre done=0 cancelled=0 tg=... tgbyte=90  ->  ENTRY ... result=10, done=1 cancelled=0
```

Every fiber spawned into a TaskGroup is bogusly marked cancelled before its
entry function runs. The futures stay pending, the AW-004 await lowering parks
the continuation, nothing ever resumes it, `main` never reaches its `println`s,
and the process exits 0 having printed nothing -- a silent wrong answer, which
is worse than the crash in
[jit-xxh64-missing-prototype.md](jit-xxh64-missing-prototype.md).

Counterfactual confirming the mechanism: setting `fiber->task_group = NULL` in
an otherwise identical copy of the fixture makes it print `10 / 20 / 30 / done`
under `tur jit`.

## Why the `cc` path has been getting away with it

Reading a byte that is neither 0 nor 1 through a `bool` lvalue is undefined, and
the two front ends resolve it differently -- clang masks the load to bit 0,
c2mir tests the whole byte:

```turmeric
(defn probe [] : int
  ```c
  unsigned char raw = 0x5A; struct B { bool f; };
  fprintf(stderr, "truthy=%d as_int=%d\n",
          ((struct B*)(void*)&raw)->f ? 1 : 0, (int)((struct B*)(void*)&raw)->f);
  fflush(stderr); return 0;
  ```)
(defn main [] : int (probe))
```

JIT prints `truthy=1 as_int=90`; cc prints `truthy=0 as_int=0`. (With `0xA7`
both say 1.)

So on macOS the `cc` path survives only because a freshly-initialized mutex's
first byte happens to be `0x5A` and `0x5A & 1 == 0`. On Linux glibc leaves that
byte `0`, so `cancelled` reads false and the layout bug is invisible there
regardless of front end -- which is why the whole Linux JIT baseline is clean on
this fixture. **Nothing about this is a JIT bug or an arm64 bug.** The JIT is
just the first thing to stop being lucky.

The `tur_fiber_shim` variant is worse than the resume variant and is not
protected by any similar luck: locking a `pthread_mutex_t` at the wrong offset
is corruption on every platform. It is simply on a path (panic inside a
task-group fiber) that the corpus exercises rarely.

## Fix direction

Both emitted shims should use the canonical layout. The real fix is to stop
hand-rolling the struct in three places: `TaskGroupBlock` should have a single
definition the emitter and `stdlib/taskgroup.tur` both reference -- a real
runtime header (this is the same "one declared runtime boundary" that plan item
**S2** exists to create), not a `typedef` retyped per call site with whatever
fields that site happens to need.

If a narrow fix is wanted first, replace both `typedef`s at
`emit_module.c:8448` and `emit_module.c:8497` with the full canonical field
list. That is behavior-preserving on Linux and fixes `taskgroup-async` under
`tur jit` on macOS.

Fixing c2mir's `bool` truthiness would also mask the symptom; do not do that
instead of fixing the layout.

## Provenance

Found during the arm64 macOS re-validation of the JIT engine (Apple M2, Darwin
27.0.0, Apple clang 21.0.0), `tur` v0.32.2, MIR pin `41ff4d94`. See section 20
of [../upcoming/jit-engine-j0-findings.md](../upcoming/jit-engine-j0-findings.md).
