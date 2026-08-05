# `set!` on an `^mut` binding holding `rc<T>` never releases the old value

**Status: FIXED 2026-07-26.** The investigation found the reported leak was one
of three defects on the same seam, all now closed -- see *Resolution* at the
bottom. Pinned by `tests/fixtures/set-bang-releases-old-rc`.

**Severity:** high -- an ordinary, idiomatic loop leaks one rc block per
iteration. Acyclic, so the cycle collector cannot reclaim it either: the
overwritten blocks keep `strong_count > 0` forever and are simply lost.

Found while running a real-shape workload through the CG6 counters for
[docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../../upcoming/v1/gc-cycle-collection-followup-plan.md)
(CG8 item 3). The collector handled every *cyclic* structure in that workload
correctly (7940 blocks freed, 60 live at exit); the entire residue came from
this, on the acyclic half.

## Minimal repro

```turmeric
(defstruct Node :move [next : rc<Node>])
(defn null-rc-node [] : ptr<void>
  ```c
  return NULL;
  ```)
(defn live [] : int
  ```c
  extern uint32_t gc_all_blocks_count;
  return (int64_t)gc_all_blocks_count;
  ```)
(defn main [] : int
  (gc-enable!)
  (println (live))                     ; 0
  (let [^mut h (rc/of (make-struct Node (null-rc-node)))
        ^mut i 0]
    (while (< i 10)
      (set! h (rc/of (make-struct Node (null-rc-node))))
      (set! i (+ i 1))))
  (println (live))                     ; expected 0, actual 10
  0)
```

```
$ ./build/tur build repro.tur -o repro && ./repro
0
10
```

11 blocks are allocated (1 initial + 10 assignments). The binding goes out of
scope before the second `println`, so all 11 should be released. Exactly the 10
*overwritten* values survive -- the one still bound at scope exit is released
correctly, so scope-exit drop works and it is specifically the assignment path
that is missing its decrement.

## Scale

A `build-chain` loop of 4 assignments per round, 4000 rounds, under `GC_AUTO`:

| | collections | objects freed | live blocks at exit |
|---|---|---|---|
| acyclic chains only | 31 | **0** | **16000** |
| cyclic families only | 63 | 7940 | 60 |

16000 = 4000 rounds x 4 `set!`s, exactly. Nothing is reclaimed on the acyclic
path at all, and the collector correctly does not try -- these are not cycles.

## Root cause

The scope-exit drop is emitted and works; the assignment path lacked the
release. `emit_set_stmt` (`src/compiler/emit_stmt.c`) was a bare `bn = v;`.
The struct-field write next to it (`emit_set_field_stmt`) had always released
the old value, so the two sides of `set!` disagreed.

## Related

Not a collector bug, and not fixed by enabling the collector: these blocks are
acyclic with a positive strong count, which is precisely the case refcounting
alone is supposed to handle.

---

## Resolution (2026-07-26)

Adding the missing decrement alone would have turned a leak into a
use-after-free. Probing every ownership shape of `set!` first turned up **two
further defects** on the same seam, both live before this change:

| shape | emitted before | defect |
|---|---|---|
| `(set! h (.next h))` | `h = h->value->next;` | no `rc_strong_increment` -- the binding released a reference it never took, and the value reads `h` INLINE, so a naive release-then-store is a UAF |
| `(set! (.next a) (.next b))` | `release a.next; a.next = b->value->next;` | same missing increment: both fields alias one block that BOTH later release -- a double free |
| `(set! (.next a) (.next a))` | `release a.next; a.next = a->value->next;` | reads the field after releasing it -- a UAF |

So the fix is an ownership normalization, not a decrement:

1. **The value must carry its own +1.** A fresh `(rc/of ...)` and an explicit
   `(rc/clone x)` do; a bare variable does because the elaborator treats it as a
   **move** (the source binding's auto-drop is suppressed). A bare rc **field
   read** carries nothing, so it is now wrapped in `EX_RC_CLONE` -- exactly the
   clone-on-read the let-binding init path already performed for the same borrow
   shape. Applied on both the variable path (`elab_set_rc_release`, new in
   `elab_core.c`) and the field path (`elab_forms.c`, beside the existing
   move-at-set scan).
2. **The value must be fully evaluated before the old one is released**, since
   it may read the very slot being overwritten. Both `emit_set_stmt` and
   `emit_set_field_stmt` now spill to a temp first, making the order
   [evaluate new] -> [release old] -> [store] regardless of the value's shape.
   This is also what keeps `(set! h (rc/of ... (rc/clone h) ...))` correct: the
   clone's +1 is taken while the old value is still alive.
3. **The release is gated on exactly the scope-exit auto-drop predicate**
   (`!binding_moved_during_init && !is_moved && !is_binding_consumed`), so the
   two can never disagree. A binding whose ownership is hand-managed gets
   neither an auto-drop nor a release here.

`(set! (@ r) v)` through a `&mut` borrow needed nothing: assigning an rc that
way is **rejected by the type checker** (`set! type mismatch: cannot assign
rc<...> through &mut rc<?> borrow`), so the case is closed by rejection rather
than by codegen.

Measured on the workload that surfaced this -- 4000 rounds, acyclic half:

| | live blocks at exit |
|---|---|
| before | 16000 |
| after | **0** |

and the full mixed workload's live-at-exit went 16050 -> 62.

### Deliberately still leaking

Two degenerate shapes keep their pre-existing leak, because both suppress the
auto-drop entirely and releasing would be worse than leaking:

- `(set! h h)` -- self-assignment lowers to `h = h` with no auto-drop; a release
  would leave the binding dangling. Pinned at `live=1` in the check matrix so a
  future change here surfaces rather than passing silently.
- `(rc/drop h)` then `(set! h v)` -- the explicit drop already suppressed the
  auto-drop, so `v` is never released. Releasing the old value here would
  double-free instead. A genuine (if unusual) remaining gap.
