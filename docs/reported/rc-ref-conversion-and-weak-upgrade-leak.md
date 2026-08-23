# `ref/from-rc` and `upgrade` leak at the ownership handoff

**Severity:** medium. Two unrelated call sites, one shape: a heap allocation is
handed across an ownership boundary to something that never frees it.

**Status:** OPEN. Both root-caused with minimal repros; neither fixed.

Found by widening `requires.leak-check` coverage from 2 fixtures to 54 (thread
#3 of the leak-checking work). Four fixtures failed the gate; they reduce to
these two bugs.

## Bug 1 -- `ref/from-rc` orphans the payload

```turmeric
(defn main [] : int
  (let [c (rc/of 1)]
    (let [r (ref/from-rc c)]
      (println @r)))
  (let [q (rc/of 9)] (println (rc/strong-count q)))   ; any trailing work
  0)
```

```
SUMMARY: AddressSanitizer: 8 byte(s) leaked in 1 allocation(s).
```

`tur_ref_from_rc` (`src/runtime/rc.c:227`) transfers the value out and frees
only the control block:

```c
void *value = cb->value;
cb->value = NULL;
gc_unregister_block(cb);
free(cb);
return value;          /* caller now owns `value` -- and nothing frees it */
```

The emitted code binds the result to a bare `void *` with no drop:

```c
void *__t169 = tur_ref_from_rc(rc2_1332);
void * r2_1333 = __t169;
```

`rc/from-ref` (the inverse) is clean -- verified with the same trailing-work
probe.

**Note the reporting asymmetry, it cost time here.** Alone, this repro is
reported CLEAN: `r2` still holds the pointer at exit, so LSan sees the block as
reachable. Add any trailing statement and it is reported. The leak is present
either way -- the emitted C for the leaking block is byte-identical between the
two -- so a clean LSan run on a small program is not evidence of no leak.

## Bug 2 -- `upgrade` orphans its boxed `option<rc<T>>`

```turmeric
(defn opt-some? [o : ptr<void>] : bool
  ```c
  struct { bool is_some; int64_t value; } *opt = (void*)o;
  return opt != NULL && opt->is_some;
  ```)

(defn main [] : int
  (let [rc1 (rc/of 42)]
    (let [w1   (weak rc1)
          opt1 (upgrade w1)]
      (println (opt-some? opt1))))
  0)
```

```
SUMMARY: AddressSanitizer: 16 byte(s) leaked in 1 allocation(s).
    #1 tur_box_some
    #2 tur_some_ptr
    #3 weak_slupgrade
```

`upgrade` returns `option<rc<T>>`. Crossing it into a `ptr<void>` parameter
heap-boxes it via `tur_some_ptr` -> `tur_box_some`, and that box has no owner.

The trigger is the crossing, not `upgrade` itself: consuming the option by
value (`(.is-some (upgrade w))`) is clean. It leaks when the option is bound
and passed to a `ptr<void>`.

## Same family as an already-fixed bug

Both are the shape of
[rc-of-adt-leaks-the-payload](../archive/rc-of-adt-leaks-the-payload.md)
(resolved 2026-08-22): a box minted at a carrier crossing that no drop path
owns. That one was one over-narrow condition in `emit_expr.c`. These two are
worth checking against the same seam -- `tur_some_ptr` / `tur_box_*` call sites
that have no matching release.

## Affected fixtures

Marked `known-leak` against this report so the gate stays green and honest;
delete the markers when this is fixed and the gate will catch a regression:

| fixture | bug |
|---|---|
| `rc-ref-conversion` | 1 |
| `rc-auto-drop-test` | 1 |
| `weak-upgrade-option` | 2 |
| `weak-upgrade-after-drop` | 2 |

Reproduce with `bash tests/run-leak-check.sh` (or `ctest -R tur_leak_check`).
