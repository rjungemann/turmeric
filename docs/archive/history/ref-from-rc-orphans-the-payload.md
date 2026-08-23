# Fix: `ref/from-rc` results are owning refs, so they auto-drop

Resolves bug 1 of
[rc-ref-conversion-and-weak-upgrade-leak](../../reported/rc-ref-conversion-and-weak-upgrade-leak.md).
Bug 2 (`upgrade`) is still open there.

Found by the widened `requires.leak-check` gate (2 -> 54 fixtures), which is
what made it visible at all.

## The bug

A ref obtained from `ref/from-rc` was never freed. `tur_ref_from_rc` takes the
payload out of the control block and destroys the block:

```c
void *value = cb->value;
cb->value = NULL;
gc_unregister_block(cb);
free(cb);
return value;
```

so the returned ref is the payload's **only** owner. The elaborator believed
the opposite:

```c
/* Theme 1: mark a ref obtained from `ref/from-rc` as non-owning so
 * elab_deref leaves its deref consuming (it shares the rc payload and
 * cannot auto-drop -- see Binding.is_nonowning_ref). */
if (b->type.kind == TY_REF && init && init->kind == EX_REF_FROM_RC) {
    b->is_nonowning_ref = true;
}
```

Nothing shared it, because the rc it supposedly shared with had been freed one
line earlier. So the auto-drop every other ref binding gets was suppressed and
the payload leaked.

`rc.h` documents the contract the code contradicted -- "tur_ref_from_rc is the
inverse and requires a UNIQUE rc ... since it destroys the control block" -- and
so does the header of the very fixture that guards this path:

> ownership transfers to the caller ... the caller owns the resulting ref

What that fixture actually asserts is that the **consumed rc** gets no
auto-drop. That is a different binding and still holds; the fix does not touch
it.

## The fix

Three guards, all resting on the same wrong premise:

1. `elab_forms.c` -- drop the `is_nonowning_ref = true` marking. The result is
   an ordinary owning ref: deref is non-consuming (via `elab_memory.c`'s
   existing check of that flag) and the scope-exit auto-drop discharges the
   linear obligation.
2. `elab_forms.c` -- the `has_ref_bindings` gate that decides whether to wrap a
   `let` body in a `do` so defers can be appended. This one ran **first**, so
   until it was fixed the change to (3) had no observable effect at all.
3. `elab_forms.c` -- the two `EX_REF_FROM_RC` exclusions in the auto-drop count
   and injection loops.

Left alone deliberately: the `EX_REF_FROM_RC` exclusion in the **uniqueness**
marking (`elab_forms.c:887`) rests on the same wrong premise, but uniqueness is
a separate axis from ownership -- changing it risks new TUR-E0200/E0201
diagnostics without affecting the leak.

## Verification

- Repro clean: the 8-byte leak is gone, program output unchanged.
- `tests/run-leak-check.sh`: 52 passed, 0 failed, 2 known-open (both now bug 2).
  `rc-ref-conversion` and `rc-auto-drop-test` lost their `known-leak` markers,
  so the gate fails if either regresses.
- `bash tests/run.sh`: 2694 passed, 0 failed.
- One snapshot regenerated (`rc-auto-drop-test/expected.c`), and its diff is
  exactly the intended change -- a `free` defer now appears for the
  `ref/from-rc` result. No other fixture's codegen moved.

## The thing that cost the most time

The isolated repro reports **CLEAN**. LSan reports what is unreachable, and in
a `main` that ends right after the allocation the binding still holds the
pointer, so the block looks live. Add one trailing statement and the same
program reports the leak -- with byte-identical emitted C for the leaking
block. Nearly concluded "no leak" from the small probe. Recorded as trap 3 in
the test-suite portability guide: when probing a suspected leak, always put
work after it.
