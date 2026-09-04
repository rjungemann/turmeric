# Fix: `(upgrade w)`'s option box gets an owner

Resolves bug 2 of
[rc-ref-conversion-and-weak-upgrade-leak](../rc-ref-conversion-and-weak-upgrade-leak.md).

## The bug

`(upgrade w)` returns `option<rc<T>>` as a heap box. The allocation is minted
in **emit** (`emit_expr.c`, `EX_WEAK_UPGRADE`), not elaboration:

```c
buf_printf(body, "struct { bool is_some; int64_t value; } *%s = NULL;\n", opt_tmp);
buf_printf(body, "if (%s) { %s = malloc(sizeof(*%s)); ... }\n", ...);
```

and the elaborator types the result `TYPE_PTR_VOID` -- a bare pointer with no
ownership. So no binding owned the box and nothing freed it. Bug 1's fix (the
ref auto-drop) could not reach it: that drop is injected onto a *binding* by
type, and this allocation does not exist until codegen.

## Four designs, three rejected

Recording these because each looks right until tested:

1. **Stack local instead of `malloc`.** Unsafe -- the result can escape.
   `(defn get-opt [w : weak<int>] : ptr<void> (upgrade w))` elaborates fine, so
   the returned pointer would dangle.
2. **Per-call-site `static`.** Unsafe -- `tests/fixtures/gc-deterministic`
   binds two upgrade results live at once (`ua`, `ub`), which a static aliases.
3. **Type the result `ref<...>`.** Both lower to `void *`, so this looked free.
   It is not: `ref<ptr<void>>` does not coerce to a `ptr<void>` parameter, and
   all five in-tree callers take it as one -- `TUR-E0001` for every caller.
   (Returning a by-value `option<rc<T>>` fails for the same reason: all five
   read the box through inline C as `struct { bool is_some; int64_t value; } *`.)
4. **Key the drop on the INIT, not the binding's type.** What landed.

## The fix

The binding keeps `ptr<void>`, so no caller changes. A `let` binding whose init
is `EX_WEAK_UPGRADE` now gets the same scope-exit auto-drop a `ref` binding
gets, via one predicate threaded through the three places that decide it:

- the `has_ref_bindings` gate (whether to wrap the body in a `do` so defers can
  be appended -- this one runs first, so missing it makes the other two inert),
- the auto-drop count loop,
- the auto-drop injection loop.

The disposal is the existing `drop!` builtin, which is `BS_PREFIX_UNARY_FREE`
and emits a plain `free` -- exactly right for this malloc. `drop!`'s "requires
ref<T>" check lives in `elab_drop`, the surface form; the injection builds the
builtin directly and bypasses it.

## What it did NOT fix

`stdlib/weak.tur`'s `weak/upgrade` FUNCTION still leaks. It is an ordinary
Turmeric function whose inline-C body builds the Option with `tur_some_ptr`, so
it is a different bug -- and a wider one, because that is the form the inline-C
results guide recommends. Filed as
[inline-c-option-carrier-box-leaks](../../reported/inline-c-option-carrier-box-leaks.md);
`tests/fixtures/weak-upgrade-after-drop` stays `known-leak` against it.

## Verification

- Repro clean; `tests/fixtures/weak-upgrade-option` lost its `known-leak`
  marker and now passes the gate.
- `bash tests/run.sh`: 2694 passed, 0 failed.
- Leak gate: 53 passed, 0 failed, 1 known-open (the separate bug above).
