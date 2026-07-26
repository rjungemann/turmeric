# `rc_cb_alloc` with a scalar type and no drop_fn invalid-frees its own payload

**Severity:** medium -- crashes (`free(): invalid pointer`, SIGABRT) on the plain
RC path with the collector disabled. Reachable only from the C API, not from
codegen'd Turmeric today, so it is a latent footgun on the embedder/interop
surface rather than a live compiler bug.

Found while measuring CG5's payload zeroing for
[docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../upcoming/v1/gc-cycle-collection-followup-plan.md)
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
