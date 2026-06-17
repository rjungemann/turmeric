---
title: Retype stdlib Option consumers from `o : int` to by-value `(Option A)` (No-Lazy-`:int` cleanup)
category: Stdlib / ABI -- Option none-as-NULL retirement (Track A)
severity: Low-medium ergonomics + audit hygiene. The stdlib Option consumers
  (`some?`, `unwrap-or`, `option-map`, `option-eq?`, `option-free`) declare their
  handle as `o : int` -- the historical carrier ABI, a No-Lazy-`:int` stand-in
  for `(Option A)`. Passing a by-value `(Option A)` into them spills
  concrete->carrier at every call site (the 8 spills the audit flags in
  `option-consumers-byvalue-arg`). Retyping them to `(Option A)` honours the
  No-Lazy-`:int` rule and removes those spills.
status: PARTIAL 2026-06-17. `option-eq?` retyped to `[A] [o1 : (Option A)
  o2 : (Option A) ^fat cmp-fn] : bool` with a pure-Turmeric by-value body;
  audit `option-consumers-byvalue-arg` 12 -> 8 crossings. `construct_recovered_byvalue`
  generalised to fire for non-instance generic specs (gate previously locked
  to instance-method spec bodies) -- proven by
  `tests/fixtures/option-construct-byvalue-return-spec/`. The `option-map` /
  `result-map` rewrites still pend on the 0-arg constructor binding follow-up
  (see step 2 in "Sequencing for the remainder"). The rest are blocked or
  cascade-coupled; see below.
---

# Retyping the stdlib Option consumers to by-value `(Option A)`

## Context

`docs/reported/option-none-as-null-byvalue-param-segfault.md` made by-value
`(Option A)` params callable from carrier `#{Construct}` results (with NULL-safe
none). That unblocked retyping the Option consumers from the carrier `:int`
stand-in to honest `(Option A)`. This report tracks that retype.

## Done

`option-eq?` (`stdlib/option.tur`): retyped to

```turmeric
(defn option-eq? [A] [o1 : (Option A) o2 : (Option A) ^fat cmp-fn : (fn [A A] bool)] : bool
  (if (.is-some o1)
    (if (.is-some o2)
      (cmp-fn (.value o1) (.value o2))
      false)
    (not (.is-some o2))))
```

Pure-Turmeric, reads `.is-some`/`.value` by value; none (`{is_some=false}`) is
never deref'd. No construct in the body, so no construct-in-return blocker (see
below). The interpreter keeps its `native_option_eq` override (`src/main.c`), so
this is a compiled-path-only change. Callers pass either `#{Construct}` results
(`(some 1)`/`(none)`, bridged by the by-value-param fix) or by-value producers
(`(g)`), so no caller cascade. Audit: `option-consumers-byvalue-arg` 12 -> 8.

## Blocked: `option-map` (construct-in-by-value-return)

`option-map` returns `(Option B)` and builds it with `(some ...)`/`(none)` in
**return position**. Inside a monomorphized by-value spec
(`option-map__spec__...` returning `Option__B`), `(some X)` emits the carrier
int64 box (`tur_some`), which then fails to assign to the by-value `Option__B`
return: `incompatible types when assigning to type 'Option__int' from type
'int64_t'`.

The fix is the construct-in-by-value-return path: generalize
`construct_recovered_byvalue` (`src/compiler/emit_module.c`, currently GATED to
instance-method spec bodies via `current_abi_specialization->fn->owner_instance`)
to also fire for a **non-instance generic spec** whose declared return is a
concrete by-value Option/Result struct of the same family the body's construct
produces. That is the broad M2-completion change deliberately deferred to avoid a
wide snapshot blast (the gate comment says so). `option-map` therefore stays on
its inline-C carrier body for now. `result-map` is in the same boat.

## Cascade-coupled: `some?` / `unwrap-or`

`some?` and `unwrap-or` are used internally by carrier-`:int` designs that would
all have to retype together:

- **`stdlib/refined.tur`**: `ne-from?` / `bidx-of?` deliberately return the
  carrier `:int` Option (inline-C builds the box) and `ne-unwrap` / `bidx-unwrap`
  consume it via `(unwrap-or o 0)` with `o : int`. Retyping `unwrap-or` to
  `(Option A)` makes `(unwrap-or o 0)` a type error (`int` is not `(Option A)`).
  Honest fix: retype the whole refined smart-constructor surface to `(Option X)`
  (itself a No-Lazy-`:int` item in `spices-int-stand-in-audit`).
- **`stdlib/kleisli.tur`** `comp`: threads the Kleisli arrow result as the
  carrier int64 (`k-apply-raw` returns `:int`) and calls `(some? r)` /
  `(unwrap-or r 0)` on it. The Arrow abstraction erases the element type, so
  retyping `some?`/`unwrap-or` cascades into `k-apply-raw` and the Arrow
  machinery -- a larger change.

Both have **native interpreter overrides** (`native_some_pred`,
`native_option_unwrap_or`), so the interpreter is unaffected by a signature
change; the cascade is purely the compiled-path callers above.

## `option-free`

Unused outside docstrings; for a by-value Option there is no heap box to free.
Left on `:int` (pure churn to change a dead helper). Revisit when none-as-NULL
is fully retired and the carrier producers (`some`/`none`) stop heap-allocating.

## Sequencing for the remainder

1. ~~Generalize `construct_recovered_byvalue` to non-instance generic specs
   (construct-in-by-value-return)~~ -- **2026-06-17: gate generalized;
   regression fixture `tests/fixtures/option-construct-byvalue-return-spec/`
   proves a non-instance generic helper returning `(some x)` now mints a
   spec returning `Option__A` by value (zero bridge crossings on that
   fixture). The 0-arg `(none)` / `(err)` constructor leg stays on the
   carrier base because `emit_abi_register_call` early-exits when the
   call carries no `abi_bindings` -- elab does not attach bindings on a
   pure type-arg-only call. Tracked as the follow-up below.
2. **0-arg constructor `abi_bindings` (follow-up).** Wire elab to
   attach `abi_bindings` on a 0-arg constructor call whose declared
   result type involves a tyvar bound by the enclosing generic's
   active spec, OR weaken `emit_abi_register_call`'s
   `n_bindings == 0` early-exit to synthesize the missing bindings
   from the active spec when the callee is a `#{Construct}` template
   and the call type composes with the spec's bindings. This is what
   `option-map` / `result-map` need to compile a pure-Turmeric body
   that returns `(none)` / `(err)` in the false arm.
3. Rewrite `option-map` / `result-map` bodies to pure Turmeric (drop
   the carrier inline-C), unblocked by step 2.
4. Retype `refined.tur`'s `ne-from?`/`bidx-of?`/`ne-unwrap`/`bidx-unwrap`
   to `(Option X)`, then retype `some?`/`unwrap-or` together with them.
5. Retype `kleisli.tur` `comp` / `k-apply-raw` to thread a by-value
   Option (or keep Arrow on the carrier and bridge at its boundary).

## Related

- [docs/reported/option-none-as-null-byvalue-param-segfault.md](option-none-as-null-byvalue-param-segfault.md)
  -- the by-value-param bridge fix this builds on.
- [docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  -- the audit (`option-consumers-byvalue-arg` was the standout fixture).
- `src/compiler/emit_module.c` `construct_recovered_byvalue` -- the gate to
  generalize for `option-map`.
