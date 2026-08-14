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

## Resolution (2026-08-13)

Fixed by **fix direction 2** -- the one the report calls "not a shortcut around"
direction 1. It is, because the information direction 1 was going to add as an
annotation is already inferred.

### The missing ownership fact already exists

The report's argument for needing a new annotation is:

> Whether a given `^fat` callee drops is not visible at the call site, so the
> caller cannot choose between "share one static box forever" and "hand over a
> fresh heap box".

The first clause is true and the second does not follow. The only way to drop a
fat handle is `TUR_CLOSURE_DROP`, which is a **C macro** -- reachable only from
an inline-C body. And `nonretain_param_mask` (`elab_fns.c`) is zeroed outright
for any body containing inline-C, for an unrelated reason recorded there: "an
inline-C body can STORE a fn-param invisibly to the AST escape analysis."

So a set nonretain bit already implies the callee neither retains **nor drops**
the argument. That is precisely the fact direction 1 proposed to add. The
call-site test is one line:

```c
bool sink_is_nonretaining =
    fn_binding && i < 32 && (fn_binding->nonretain_param_mask & (1u << i)) != 0;
```

It also disposes of the `-Wfree-nonheap-object` objection: that warning was about
handing a static box to a *dropping* callee, and a callee that can drop has the
bit clear and keeps its heap box. `tests/fixtures/closure-drop-glue-fatshim` --
whose whole point is a `^fat` callee that drops -- is inline-C and therefore
untouched, and still passes.

### The report's measurement conflates two allocations

Worth recording, because the numbers do not otherwise add up. The report's repro
recurses (`(loop (- n 1) (callit add3 acc))`) and attributes ~200 bytes/iteration
to the shim box. Measured on that repro the fix moves 4e6 iterations from 822 MiB
to 697 MiB -- real, but only ~31 bytes/iteration.

The shim box is ~28 bytes. The rest is a **CPS continuation env**
(`go_j0_env *__ce_go_j0 = malloc(...)` in the emitted C), allocated per call
because the recursive shape routes through the DK trampoline. Rewriting the same
loop as a `while` with `set!` -- no CPS -- isolates the shim exactly:

| 4e6 iterations | before | after |
| --- | --- | --- |
| recursive (report's repro: shim + CPS env) | 821,620 kB | 696,616 kB |
| `while` loop (shim only) | 109,420 kB | **1,296 kB** |

Flat, which is the actual claim. Anyone re-measuring on the report's original
program will see ~15% and conclude the fix barely worked; the `while` form is the
one that measures this defect.

Whether the CPS-join env is itself a leak or is reaped later (the call site
threads `__dk_reap_node`) was not investigated here -- it is a different
allocation in a different subsystem and does not belong to this report.

### Coverage

`tests/run-fat-shim-leak.sh` + `tests/fixtures/fat-sink-shim-no-leak`, modelled
on the existing `run-closure-env-leak.sh`: emit C, compile it with
`-fsanitize=address,undefined`, run under LeakSanitizer, assert no leak. A
per-call box that is never freed is an LSan leak, so this fails loudly rather
than requiring a trip count big enough to see in RSS. Registered as the
`tur_fat_shim_leak` ctest target; the fixture carries
`requires.dedicated-runner` because `tests/run.sh` compiles spawned programs
without ASan and could not see this.

Verified against a deliberately-reverted build: `LeakSanitizer: detected memory
leaks`, nonzero exit.

### Guide upkeep

Done, and not as the report anticipated. The `^fat` row is removed from the
open-cells table in
[value-representations-guide.md](../guides/value-representations-guide.md), and
the static-box note no longer says `^fat` is excluded for want of a contract --
it explains the inference instead. No ownership column was added, because no
annotation was needed.

### What is still true

Direction 1 (an explicit `^fat ^owned` / borrowing-by-default annotation) remains
the more general answer, and would cover the case this does not: a `^fat` sink
with an inline-C body that does **not** drop still keeps its heap box, because
the inline-C guard is conservative. That is a leak-avoidance opportunity, not a
correctness gap, and nothing in the tree currently needs it.

### Verification

`tests/run.sh`: 2597 passed, 0 failed. `tests/run-turi.sh`: 1782 passed, 0
failed. `tests/run-fat-shim-leak.sh`, `tests/run-closure-env-leak.sh`, and
`tests/run-leak-gate.sh` all pass.
