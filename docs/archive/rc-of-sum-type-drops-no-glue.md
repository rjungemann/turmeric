# `rc/of` over a multi-variant ADT emits no drop glue and no walker -- its `rc` fields leak

**Severity:** high (silent leak; also a cycle-collector blind spot)
**Status:** RESOLVED 2026-07-25
**Found by:** CG3 residue (gc-cycle-collection-followup-plan), while checking
whether the planned "struct with rc fields boxed without a walker" lint could
ever fire

## Summary

`(rc/of v)` attaches drop glue and walk glue only when `v`'s type is a
**by-value product** ADT. A multi-variant sum carrying an `rc<T>` payload gets
neither -- `NULL` drop function, no `walk_fn` -- so the payload's reference is
never released and the collector cannot trace through the box.

```turmeric
(defstruct S :move [tag : int])
(defdata Holder (Empty) (Full [inner : rc<S>]))

(defn main [] : int
  (gc-enable!)
  (let [a (rc/of (make-struct S 7))
        h (rc/of (Full (rc/clone a)))]
    (println (rc/strong-count a)))     ; 2
  (gc!)
  (println (gc-live-blocks))           ; 1  <-- should be 0
  0)
```

The control -- the same shape as a single-variant product -- reclaims correctly:

```turmeric
(defstruct Holder :move [inner : rc<S>])   ; product, not sum
...
  (println (gc-live-blocks))           ; 0
```

| shape | boxed as | live blocks after `(gc!)` |
|---|---|---|
| by-value product with an `rc` field | `rc_cb_alloc_struct(..., drop_glue, walk_glue)` | **0** |
| multi-variant sum with an `rc` field | `rc_cb_alloc(0, 19, NULL)` | **1** |

## Root cause

`src/compiler/emit_expr.c`, the `EX_RC_OF` arm. The glue is selected by:

```c
if (adef && adef->needs_drop_glue && adt_is_byvalue_product(adef)) {
    ... drop_fn_name = drop_glue_tur_adt_<name>;
        struct_with_rc_fields = true;
}
if (struct_with_rc_fields) {
    ... rc_cb_alloc_struct(0, kind, drop_fn_name, walk_glue_...);
} else {
    ... rc_cb_alloc(0, kind, drop_fn_name);   /* drop_fn_name is still "NULL" */
}
```

`adt_is_byvalue_product(adef)` excludes sums, so a sum falls to the `else`
branch **with `drop_fn_name` never assigned** -- it keeps its initial `"NULL"`.
The `needs_drop_glue` half of the condition is satisfied (the ADT does have an
owning field); only the product test fails.

## Impact

Two distinct problems from one branch:

1. **A plain refcount leak, with the collector off.** The `rc` field of a boxed
   sum is never decremented when the box dies. This is not cycle-specific and
   does not need the GC enabled to bite.
2. **A cycle-collector blind spot, with it on.** No `walk_fn` means the walker
   cannot follow the box's `rc` children, so a cycle routed through a boxed sum
   is not collected -- the `RCK_OPAQUE` case the GC guide documents, reached
   here by an ordinary program rather than by a raw C handle.

`option<T>` and `result<T,E>` are multi-variant, so `rc<option<rc<S>>>` and
friends are in scope. Not measured how far the blast radius goes.

## Fix (landed 2026-07-25)

Two changes, and the second is the one that was easy to miss.

**1. Glue for sums.** `emit_adt_byval_drop_glue` now dispatches on the tag,
emitting a `case` per ctor that releases that variant's owning fields, and the
emission is no longer gated on `byval`. The `rc/of` site drops its
`adt_is_byvalue_product` requirement. A single-variant ADT keeps the original
emitted text exactly, so no snapshot moved.

**2. The boxing had the same double-indirection as the `:heap` bug.** Emitting
glue alone did *not* fix the leak. A sum's ctor mallocs the tag+union record and
returns a pointer, so the generic "malloc a cell and store the value" boxing
left `cb->value` pointing at a cell that *held* the pointer rather than at the
record. The glue then cast the cell, read the pointer bits as `s->tag`, and
matched no case -- so the fields still were not released. The boxing now adopts
the ctor's pointer directly, exactly as
[gc-heap-struct-rc-not-a-control-block](gc-heap-struct-rc-not-a-control-block.md)
did for `:heap`.

Verified both halves: the owning field is released at scope exit (live blocks
0), and a cycle routed *through* a boxed sum is now collected (live blocks 0) --
so the walker traces through it too. Pinned by
`tests/fixtures/rc-of-sum-type-releases-fields`.

## Original fix directions

1. **Emit drop glue for sums.** The glue has to switch on the variant tag and
   release the owning fields of that arm. `drop_glue_tur_adt_<name>` already
   exists for products; the sum version is the same idea with a tag dispatch.
2. **Emit walk glue for sums**, the same shape, so the collector can trace
   through. Worth doing together with (1) -- they share the per-variant field
   walk.
3. **Failing that, diagnose it.** If glue for sums is deferred, `(rc/of v)`
   where `v` is a sum with owning fields should at minimum warn, since the
   current behaviour is a silent leak.

## Note on the planned lint

The CG3 phase called for a lint warning when "a `defstruct` with rc fields is
boxed without a walker". This is that shape -- but a lint is the wrong response
to it, because the code is not a questionable-but-valid pattern the user should
reconsider. It is an ordinary program that the compiler miscompiles into a leak.
Fix the glue; the lint then has nothing left to warn about.
