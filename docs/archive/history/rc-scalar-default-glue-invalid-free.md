# `rc_cb_alloc` with a scalar type and no drop_fn invalid-frees its own payload

**RESOLVED 2026-07-26.** The `rc_cb_alloc*` entry points now default scalar
payloads to a no-op glue (`inline_scalar_drop_fn`) -- the inline `(cb + 1)`
payload is part of the header's own allocation, so `free(cb)` in `rc_cb_free`
reclaims it and there is nothing separate to free. The separate-payload entry
points (`rc_set_value`, `tur_rc_from_ref`) keep the `free()`-ing default.
Mirrored in the emitted replica (`emit_module.c`); `inline_scalar_drop_fn` and
`rc_set_value` are byte-identical between the copies per `gc-copy-diff.py`.

**The "worth checking first" caveat below was the whole fix.** A caller relying
on the defaulted `free()` for a repointed payload did exist, and it was the
CODEGEN: `EX_RC_OF` allocated with `rc_cb_alloc(0, <kind>, NULL)` and then
repointed `cb->value` at a separately malloc'd cell by RAW ASSIGNMENT -- so the
"codegen does not take this path" claim below was wrong, and the narrow
scalar-no-op fix alone turned `hkt-fmap-rc-result-droppable` /
`hkt-instance-rc-construct-result` red at +160000 leaked bytes -- every fmap's
payload cell stranded. `EX_RC_OF` now repoints through
`rc_set_value(cb, val, <glue>)`, passing the alloc's explicit struct drop glue
through unchanged: a bare `NULL` there re-derives the free-capable default, but
it also CLOBBERS an explicit struct drop glue, which an intermediate draft
learned from five more red rc fixtures (`weak-breaks-parent-child-cycle`
leaked ~1 MB when its parent/child glue was stomped). `rc_set_value` is added
to the emitted replica and the prototype block, since generated code now calls
it on every replica path.

Pinned by `test_scalar_default_glue_drop` in
`tests/turi/gc-runtime-copy-parity.c`: the report's repro shape drained (the
old glue is an ASan bad-free there), plus the two separate-payload shapes that
must KEEP freeing. Suite: 2397 passed, 0 failed. Verified on both rc/GC
linkage modes (archive and `TUR_RCGC_FROM_ARCHIVE=0` replica).

The original report follows.

---

**Severity:** medium -- crashes (`free(): invalid pointer`, SIGABRT) on the plain
RC path with the collector disabled. Reachable only from the C API, not from
codegen'd Turmeric today, so it is a latent footgun on the embedder/interop
surface rather than a live compiler bug.

Found while measuring CG5's payload zeroing for
[docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../../upcoming/v1/gc-cycle-collection-followup-plan.md)
(CG8 item 2); unrelated to the collector.

## Minimal repro

```c
#include "rc.h"
#include "gc.h"
#include "rc_free_queue.h"
int main(void) {
    gc_init(); gc_disable();                                     /* collector OFF */
    RcControlBlock *cb = rc_cb_alloc(sizeof(int64_t), 0, NULL);  /* scalar, default glue */
    *(int64_t *)cb->value = 7;
    rc_strong_decrement(cb);
    rc_free_queue_drain();
    return 0;
}
```

```
$ cc -I src/runtime -o repro repro.c src/runtime/{rc,gc,rc_free_queue}.c && ./repro
free(): invalid pointer
Aborted
```

## Root cause

The payload is allocated *inline*, immediately after the header --
`src/runtime/rc.c:105`:

```c
size_t total_size = sizeof(RcControlBlock) + value_size;
RcControlBlock *cb = (RcControlBlock *)malloc(total_size);
cb->value = (void *)(cb + 1);
```

so `cb->value` is an interior pointer into the single `malloc` block, never an
allocation of its own.

But `default_drop_fn_for_type` (`src/runtime/rc.c:66`) returns `default_drop_fn`
for every type that is not `RC_VT_REF` / `RC_VT_RC` / `RC_VT_WEAK` -- i.e. for
all the scalar ordinals 0..7 -- and `default_drop_fn` is
(`src/runtime/rc.c:27`):

```c
static void default_drop_fn(void *value) { free(value); }
```

`rc_cb_free` (`src/runtime/rc.c:246`) then does:

```c
if (cb->value) { cb->drop_fn(cb->value); }   /* free(cb + 1)  <-- invalid */
free(cb);
```

So the default glue for a scalar payload frees an interior pointer, and then
the block is freed again through its real base pointer.

The three non-scalar cases are all fine: `drop_ref_payload` /
`drop_rc_payload` / `drop_weak_payload` are for payloads that own a *separate*
allocation, and `free(value)` is correct there.

## Why it has not been hit

- Codegen does not take this path -- it either supplies an explicit `drop_fn`
  or allocates through `rc_cb_alloc_struct` / the kinded entry points.
- `tests/turi/gc-runtime-copy-parity.c:284` allocates exactly this shape
  (`rc_cb_alloc(sizeof(int64_t), 0, NULL)`) but the blocks never reach a drain,
  so the invalid free never executes.

## Fix directions

The narrow fix is to give scalar payloads a no-op drop rather than `free`:

```c
static void inline_payload_drop_fn(void *value) { (void)value; }  /* payload is inline */
...
default: return inline_payload_drop_fn;
```

Worth checking first whether any caller relies on `default_drop_fn` for a block
whose `value` was *repointed* away from the inline slot by `rc_set_value` --
that is the one shape where `free(value)` is the right behaviour. If such
callers exist, the dispatch should key off `cb->value == (void *)(cb + 1)`
inside `rc_cb_free` instead of off the type ordinal.

The emitted replica in `src/compiler/emit_module.c` needs the same treatment if
it shares the defaulting logic.
