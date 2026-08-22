# `rc/of` does not release a multi-variant ADT payload

**Severity:** medium. `rc<T>` means "shared ownership, released when the last
reference goes". For a multi-variant ADT payload it does not release the
payload, so code doing exactly the documented thing leaks.

**Status:** OPEN. Not fixed.

## Repro

```turmeric
(defdata Plain :copy (PA :int) (PB :int))
(defn main [] : int
  (let [r (rc/of (PA 7))]
    (println (rc/strong-count r))
    (rc/drop r)
    0))
```

```
$ valgrind --leak-check=full ./rcleak
==2542== 16 bytes in 1 blocks are definitely lost in loss record 1 of 3
```

16 bytes is `tur_adt_Plain` -- the tag plus one `int64_t`.

## Cause

`rc/of` does not put the ADT box in the control block. It allocates a SECOND,
8-byte box to hold the carrier word and registers that:

```c
int64_t __ps_161 = (ctor_PA(INT64_C(7)));            /* the ADT box       */
int64_t *__t162  = (int64_t *)malloc(sizeof(int64_t)); /* a second box     */
*__t162 = __ps_161;                                   /* holding the carrier */
RcControlBlock *__t163 = rc_cb_alloc(0, 19, NULL);
rc_set_value(__t163, __t162, NULL);
...
rc_strong_decrement(r_1333);   /* drop_fn = default_rc_drop_fn -> free(__t162) */
```

`default_rc_drop_fn` frees the value pointer it was given, which is the 8-byte
wrapper. Nothing ever frees `__ps_161`.

Two defects in one, worth separating:

1. **Correctness.** The payload is never released. `rc/drop` on the last
   reference reclaims the wrapper and the control block, not the thing the user
   put in.
2. **Cost.** Two allocations where one would do. The wrapper exists only to give
   the control block a pointer to point at; the carrier could be stored inline
   (`inline_scalar_drop_fn` already exists for exactly the inline case).

## Why this is coupled to the ADT slab allocator -- read before fixing

`docs/reported/multi-variant-adts-always-heap-allocate.md` proposes bump-
allocating never-freed ADT boxes, and the safety argument for it is precisely
that nothing frees them -- including this path. **If this bug is fixed so that
`rc` releases its ADT payload, and the slab allocator is on, that free() receives
slab memory and corrupts the heap.**

So the two must move together: either fix this first and narrow the slab's
predicate to exclude anything reachable from an `rc`, or fix this by making the
carrier inline (no free of the box at all), which leaves the slab's argument
intact. The second is both cheaper and the better design.

Any slab work must state this dependency in the code, not just here.

## Scope not established

Checked for a multi-variant `:copy` ADT. Not checked: single-variant/by-value
payloads (which do not box at all, so probably fine), `:heap` ADTs, parametric
monomorphs, or whether `rc/clone` + partial drops change the picture. Someone
fixing this should establish the full set rather than trusting the one case
measured here.
