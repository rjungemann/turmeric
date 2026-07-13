# Explicit `(rc/drop (.field o))` plus the scope-exit auto-drop double-drops a by-value struct's owning field

**Severity:** medium (memory-unsafe: use-after-free / double-decrement), but the
shape is unusual -- user code rarely drops a struct's owning field by hand.
Handler-independent; surfaced while resolving the consuming-aggregate handler
capture (`docs/reported/cps-consuming-aggregate-capture-hardfails.md`, now
resolved for the compile error; this is the orthogonal memory-safety gap it
exposed).

## Summary

When a by-value struct/record local has an owning (`rc`/`ref`) field, the
elaborator injects a scope-exit auto-drop of that field (`(defer (rc/drop
(.field o)))`). If the body ALSO drops the same field explicitly, the field is
decremented twice: once by the explicit drop, once by the scope-exit auto-drop.
The auto-drop injection is not move-aware for an explicit field drop, so it does
not suppress itself. `rc/of` starts the strong count at 1, so the first drop
frees the control block and the second decrements freed memory -> use-after-free
(and the count wraps, so it does not double-free-and-crash without a sanitizer).

This is entirely straight-line -- no effects, handlers, or continuations
involved.

## Minimal repro

```turmeric
(defstruct Own [r : rc<int> tag : int])
(defn f [] : int
  (let [o (make-struct Own :r (rc/of 7) :tag 9)]
    (do (rc/drop (.r o)) (.tag o))))   ; o.r decremented HERE and again at scope exit
(defn main [] : int (println (f)) 0)
```

`tur build` succeeds and the program prints `9`, but `o.r` is decremented twice.

## Root cause (file:line)

Emitted C for `f` (from `tur emit-c`) shows both decrements of the count-1 rc:

```c
static int64_t f() {
    ...
    rc_strong_decrement((RcControlBlock *)(o_1282).r);   // explicit (rc/drop (.r o)) -> count 0, freed
    rc_free_queue_drain();
    struct __defer_env_178 __t180 = {.o = o_1282};
    tur_frame_push_defer(&__frame_177, __defer_179, &__t180);  // scope-exit auto-drop
    ...
    tur_frame_fire_lifo(&__frame_177);   // fires __defer_179 -> rc_strong_decrement on freed block (UAF)
}
```

The scope-exit auto-drop is injected in `elab_let` (`src/compiler/elab_forms.c`)
for a by-value struct with owning fields: `(defer (rc/drop (.field o)))` per
owning field, unconditionally. There is no move/consume tracking that would
notice the body already dropped `(.field o)` and suppress the corresponding
auto-drop.

## Fix directions

- **Move-aware auto-drop suppression:** when the ownership pass sees an explicit
  `(rc/drop (.field o))` / `(drop! (.field o))` on a path, mark that field of
  `o` as moved-out and skip the per-field scope-exit auto-drop for it. This is
  the principled fix and matches how a full move checker would behave.
- **Cheaper interim (diagnostic):** if move-awareness is too large, at least
  emit a warning when a by-value struct's owning field is explicitly dropped in
  a scope that will also auto-drop it, so the double-drop is not silent.

## Notes

- A bare `rc` local (not a struct field) does not hit this: dropping it
  explicitly and letting it fall out of scope is the ordinary consuming path,
  which the ownership pass already reconciles. The gap is specific to the
  per-owning-field auto-drop of a by-value aggregate.
- The consuming-aggregate handler-capture note
  (`cps-consuming-aggregate-capture-hardfails.md`) now compiles its repro (the
  capture completeness fix landed); that repro then exhibits exactly this
  double-drop, because the handler path was made to match this straight-line
  behavior. Fixing the auto-drop here fixes both.
