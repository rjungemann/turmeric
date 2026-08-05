# A bare fn passed to a `^fat` sink allocates a shim box per call and never frees it

**Severity:** medium (unbounded memory growth in a loop; no wrong answers, no
crash). Pre-existing -- not introduced by fn-value fat normalization, though
that campaign made the neighbouring shape common enough to notice.

Found 2026-08-01 while measuring the static-box hoist for normalized nominal
fn params (fn-value-fat-normalization; that half is fixed, see
[the plan](../upcoming/fn-value-fat-normalization-plan.md)).

## Summary

`EX_FN_TO_FAT` boxes a bare fn into a `{ shim, orig }` fat handle by
`malloc`ing a fresh box **every time the bridge executes**. At a `^fat`
parameter nothing frees it: the box is handed to the callee, the callee
returns, and the allocation is unreachable. In a loop the process grows
without bound.

## Repro

```turmeric
(defn callit [^fat f : (fn [int] int) x : int] : int (f x))
(defn add3 [n : int] : int (+ n 3))
(defn loop [n : int acc : int] : int
  (if (= n 0) acc (loop (- n 1) (callit add3 acc))))
(defn main [] : int (println (loop 5000000 0)) 0)
```

```
$ tur build fatleak.tur -o fatleak && ./fatleak    # Release
15000000
peak RSS: 1002 MiB          # ~200 bytes per iteration, never released
```

Answer is correct; only the memory grows. 5e5 iterations peaks near 100 MiB,
5e6 near 1 GiB -- linear in trip count, as an un-freed per-iteration
allocation is.

## Why the normalized-param fix does not cover it

The same bridge at a **normalized nominal** fn param had the identical leak
(122 MiB / 5e6 iterations) and was fixed by hoisting the box to a file-scope
static, because such a box is a constant when the boxed value is a global fn.
That fix is opt-in per shim site (`fn_to_fat_.static_ok`), and `^fat` sinks
deliberately do not opt in:

- A `^fat` callee **may take ownership and drop its argument**.
  `tests/fixtures/closure-drop-glue-fatshim/` does exactly that --
  `TUR_CLOSURE_DROP(f)` inside the callee -- and the fixture exists to pin
  that a shimmed bare fn is releasable that way.
- Whether a given `^fat` callee drops is not visible at the call site, so the
  caller cannot choose between "share one static box forever" and "hand over
  a fresh heap box".
- Handing a static box to a dropping callee is *harmless at runtime* (the
  static box's drop glue is a no-op), but GCC cannot see that through the
  inlined `tur_closure_drop` and reports
  `'free' called on unallocated object` (-Wfree-nonheap-object) at every such
  site. Shipping that warning on correct code is worse than the leak.

So the missing piece is not a boxing strategy -- it is that **`^fat` has no
ownership contract**. The callee's droppedness has to become part of the
signature before the caller can pick a representation.

## Fix directions

1. **Make ownership explicit at the `^fat` parameter.** A callee that drops
   its callback says so (`^fat ^owned`, or the inverse -- borrowing is the
   default and dropping is opt-in). Callers into a borrowing sink take the
   static box; callers into an owning sink keep the heap box. This is the
   principled fix and it generalizes: the same annotation answers the
   struct-field store, where the field's owning-ness is already declared.
2. **Cache the box per (call site, fn) at the caller.** A function-local
   `static` box filled on first execution keeps the allocation count at one
   per site without changing any contract -- but it is only sound if the
   callee never drops, so it needs the same information as (1). Not a
   shortcut around it.
3. **Free the box at the call site after the call returns**, when the callee
   is known not to drop. Correct but pointless work compared to (1), and it
   needs the same droppedness fact.

Direction 1 is the one worth doing; 2 and 3 both dead-end on the same missing
contract.

## Scope

Every `^fat` boxing site with a bare fn argument. The struct fn-field store
has the same shape (a fresh box per store) but not the same defect: those
boxes ARE owned and released by the struct's drop glue
(`tests/fixtures/local-struct-fnfield-drop/` pins it, valgrind-clean), so
they are recycled, not leaked.

## Guide upkeep

When this is resolved, update the fn-value section of
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md):
the `^fat` row gains an ownership column, and the static-box note under the
nominal row should stop saying `^fat` is excluded for want of a contract.
