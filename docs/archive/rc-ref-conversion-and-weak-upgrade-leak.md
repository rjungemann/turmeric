# `ref/from-rc` and `upgrade` leak at the ownership handoff

**Severity:** medium. Two unrelated call sites, one shape: a heap allocation is
handed across an ownership boundary to something that never frees it.

**Status:** RESOLVED 2026-08-23. Both bugs fixed; history entries
[bug 1](history/ref-from-rc-orphans-the-payload.md) and
[bug 2](history/upgrade-option-box-has-no-owner.md).

Fixing bug 2 left `stdlib/weak.tur`'s `weak/upgrade` FUNCTION still leaking --
a different bug with a wider blast radius, filed separately as
[inline-c-option-carrier-box-leaks](../reported/inline-c-option-carrier-box-leaks.md).
This report covers the `(upgrade w)` builtin only.

Found by widening `requires.leak-check` coverage from 2 fixtures to 54 (thread
#3 of the leak-checking work). Four fixtures failed the gate; they reduce to
these two bugs.

## Bug 1 -- `ref/from-rc` orphans the payload -- FIXED

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

**Fixed.** The elaborator marked a `ref/from-rc` result `is_nonowning_ref`
("it shares the rc payload and cannot auto-drop"), which contradicted the
runtime contract, and two further guards excluded it from the scope-exit
auto-drop. It is an ordinary owning ref; treating it as one closes the leak.
Details in the history entry linked above.

**Note the reporting asymmetry, it cost time here.** Alone, this repro is
reported CLEAN: `r2` still holds the pointer at exit, so LSan sees the block as
reachable. Add any trailing statement and it is reported. The leak is present
either way -- the emitted C for the leaking block is byte-identical between the
two -- so a clean LSan run on a small program is not evidence of no leak.

## Bug 2 -- `upgrade` orphans its boxed `option<rc<T>>` -- FIXED

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

### Root cause

`EX_WEAK_UPGRADE` in `src/compiler/emit_expr.c:8391` mints the box during
**emit**, not elaboration:

```c
buf_printf(body, "struct { bool is_some; int64_t value; } *%s = NULL;\n", opt_tmp);
buf_printf(body, "if (%s) { %s = malloc(sizeof(*%s)); ... }\n", ...);
```

That is why bug 1's fix does not reach it. The ref auto-drop is injected by the
elaborator onto a *binding*; this allocation does not exist until codegen, so no
binding owns it and nothing can be attached to it.

### Why the obvious fixes are wrong -- all three were checked

- **Stack local instead of `malloc`.** Unsafe: the result can escape. A
  `(defn get-opt [w : weak<int>] : ptr<void> (upgrade w))` elaborates fine, so
  the returned pointer would dangle.
- **A per-call-site `static`.** Unsafe: `tests/fixtures/gc-deterministic`
  binds two upgrade results live at once (`ua`, `ub`), which a static would
  alias.
- **Free it right after the call.** The emit site cannot know the use site;
  the box is bound and read later in the enclosing body.

### The fix

Option 2, in its narrowest form. Two further designs were tried and rejected
first, which is worth recording:

- **Return the option by value** (`option<rc<T>>` rather than `ptr<void>`):
  rejected. All five in-tree callers read the box through inline C as
  `struct { bool is_some; int64_t value; } *`, so the surface breaks.
- **Type the result `ref<...>`** so it picks up the ref auto-drop, since both
  lower to `void *`: rejected. `ref<ptr<void>>` does not coerce to a
  `ptr<void>` parameter -- every caller gets `TUR-E0001`.

What landed keeps the `ptr<void>` type and keys the scope-exit drop on the
INIT instead: a `let` binding whose init is `EX_WEAK_UPGRADE` gets the same
auto-drop a `ref` binding gets. The disposal is the existing `drop!` builtin,
which is `BS_PREFIX_UNARY_FREE` and emits a plain `free` -- exactly right for
this malloc. (`drop!`'s "requires ref<T>" check lives in `elab_drop`, the
surface form; the injection builds the builtin directly and bypasses it.)

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
