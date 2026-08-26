# `rc/of` does not release a multi-variant ADT payload

**Severity:** medium. `rc<T>` means "shared ownership, released when the last
reference goes". For a multi-variant ADT payload it did not release the
payload, so code doing exactly the documented thing leaked.

**Status:** RESOLVED 2026-08-22. One condition removed in `emit_expr.c`; both
defects (the leak and the redundant allocation) went with it. Verified under
LeakSanitizer by `tests/fixtures/rc-of-adt-payload-released`, which carries
`requires.leak-check`.

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

## The fix

The sibling path for sums WITH an owning field already did the right thing --
`payload_is_boxed_adt` in `emit_expr.c` adopts the ctor's pointer directly
instead of wrapping it, and its comment (`rc-of-sum-type-drops-no-glue`)
explains exactly why. It was gated on `def->needs_drop_glue`, so it applied only
to sums carrying an owning field.

A sum without one has the identical shape. Drop glue is not what makes the
pointer adoptable; being a boxed record is. Removing that one condition makes
`cb->value` the record itself, `rc_set_value` re-derives `default_rc_drop_fn`,
and that frees exactly the record. The wrapper allocation disappears with it.

Note this is an ownership change for the affected types: `rc/of` now MOVES the
box into shared ownership rather than copying the carrier word and leaking the
original. That matches the semantics the `:heap` and drop-glue paths already
had, so it is a consistency fix rather than a new convention.

## Confirmed coupling to the ADT slab allocator

The report predicted that fixing this while `TUR_ADT_SLAB=1` was on would hand
slab memory to `free()`. Checked after the fix, and it does, exactly:

```
==31286==ERROR: AddressSanitizer: attempting free on address which was not
malloc()-ed
```

The slab therefore stays off, and its blocker is now WORSE rather than resolved
-- see [multi-variant-adts-always-heap-allocate.md](multi-variant-adts-always-heap-allocate.md).

## Scope not established

Checked for a multi-variant `:copy` ADT. Not checked: single-variant/by-value
payloads (which do not box at all, so probably fine), `:heap` ADTs, parametric
monomorphs, or whether `rc/clone` + partial drops change the picture. Someone
fixing this should establish the full set rather than trusting the one case
measured here.
