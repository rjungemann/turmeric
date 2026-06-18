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
status: PARTIAL 2026-06-18. `option-eq?` and `option-map` retyped to by-value
  `(Option A)` (see prior PARTIAL notes for step 1/2 detail). **2026-06-18
  follow-up: `some?` retyped to `[A] [o : (Option A)] : bool (.is-some o)`
  -- pure-Turmeric by-value body.** Carrier-context callers (`(some? (lookup
  ...))` against an inline-C carrier-int Option producer, `(some? r)` in
  `kleisli.tur`/`refined.tur` where `r` is a carrier-int from
  `k-apply-raw`/`ne-from?`/`bidx-of?`) bridge via `(:: r (Option int))`
  ascription -- the layout is uniform across element types so a single
  `(Option int)` ascription is safe at carrier-erased boundaries. Native
  interpreter override `native_some_pred` is unchanged. Full suite green
  (1676/0). `unwrap-or` retype attempted and reverted: it requires the
  default-value type `A` to be inferable at carrier-context call sites
  (e.g. `(unwrap-or opt 0)` where opt is `:int`), and the cascade spans
  ~10 stdlib modules (`zipper`, `seq/*`, `json`, `safe`, `env`, `serial`,
  ...) that all produce carrier-int Options -- a separate PR is warranted.
  Rewriting `ne-from?`/`bidx-of?` bodies to pure-Turmeric by-value Option
  uncovered two new compiler-side bugs filed as their own reports:
    - [zero-arg-construct-ground-byvalue-return.md](zero-arg-construct-ground-byvalue-return.md)
      (0-arg `(none)` in a ground `(Option BoundedIdx)` return emits the
      carrier handle into the by-value slot)
    - [parametric-option-return-clone-struct-app-leak.md](parametric-option-return-clone-struct-app-leak.md)
      (doubly-nested parametric `(Option (NonEmpty A))` return leaks
      304 bytes from `clone_struct_app_type` per compile)
  Step 4 (refined.tur smart-constructor + unwrapper retype) and step 5
  (kleisli.tur `comp`/`k-apply-raw` retype) are blocked on those two
  compiler bugs plus the broader carrier-Option-producer cascade.
  `result-map` remains (a deliberate carrier-ABI regression test backs its
  `:int` signature -- see below); `unwrap-or` remains cascade-coupled.
  One niche residual on `option-map` is filed under
  `docs/archive/option-map-literal-none-unannotated-fn-no-A-inference.md` (resolved 2026-06-18; emit-side guards in PR #421 route the under-determined-`A` call to the carrier-context spec). A separate spill-bridge regression at the by-value-producer -> carrier-consumer boundary inside a `let`/`do`/`if` arg slot is tracked in `docs/reported/option-map-byvalue-result-into-carrier-consumer-let-inside-arg.md`.
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

## Done: `option-map` (construct-in-by-value-return + 0-arg `(none)`)

`option-map` returns `(Option B)` and builds it with `(some ...)`/`(none)` in
**return position**. The `(some X)` arm was unblocked by step 1
(`construct_recovered_byvalue` generalized to non-instance generic specs). The
`(none)` arm was the step-2 residual: a 0-arg `#{Construct}` got no
`abi_bindings`, so `emit_abi_register_call` early-exited and `(none)` stayed on
the carrier base, which then failed to assign to the by-value `Option__B`
return. Resolved 2026-06-18:

- **elab (`elab_call.c`):** a 0-arg `#{Construct}` callee whose declared result
  is a parametric TY_APP (`none : (Option A)`), sitting in the non-ground return
  position of an enclosing generic body, now records the
  constructor-result-tyvar -> caller-tyvar mapping (none's `A` -> the body's
  `B`). emit composes it through the active spec (`B -> int`) so
  `construct_recovered_byvalue` mints a by-value `none__spec__Option__int`.
- **emit (`emit_core.c`/`emit_expr.c`):** a 0-arg call carries no arg types to
  disambiguate one spec from another, so `emit_call_name` /
  `find_matched_abi_spec` now require the per-Expr* recording for 0-arg
  constructors (the structural by-args match would otherwise route every
  `(none)` -- including carrier-context `(some? (none))` -- to the first
  by-value `none__spec`). A construct in a carrier-returning spec body is forced
  to the carrier base. `call_returns_byvalue_aggregate` /
  `expr_emits_byvalue_carrier_abi` now consult the matched spec's return ABI: a
  spec whose result element stayed unresolved (e.g. an opaque `ptr<void>`
  closure left `B` unresolved, so the spec returns the bare carrier handle even
  though its declared return Type is still `(Option B)`/TY_APP) is NOT a by-value
  producer, so the caller does not spill `&temp`.

`option-map` now has a pure-Turmeric by-value body and no inline-C carrier base.
`result-map` is NOT in the same boat: see below.

## Done: `some?` (by-value `(Option A) -> bool`)

`some?` (`stdlib/option.tur`) retyped to:

```turmeric
(defn some? [A] [o : (Option A)] : bool (.is-some o))
```

Pure-Turmeric, reads `.is-some` by value. The native interpreter override
`native_some_pred` (`src/main.c`) is unchanged. Carrier-context callers --
where `some?` previously consumed a carrier-int Option directly from
inline-C producers like `lookup`, `zipper-move-right`, `ne-from?`,
`bidx-of?`, or the closure result threaded through `k-apply-raw` -- bridge
with `(:: r (Option int))`. The layout is uniform regardless of A
(`{bool is_some, int64 value}`), so a single `(Option int)` ascription is
safe even where the actual element type is erased (kleisli Arrow).

Callers updated:

- `stdlib/kleisli.tur` `comp` body: `(some? (:: r (Option int)))`.
- `stdlib/refined.tur` `ne-unwrap` / `bidx-unwrap`: bridge `o : int`
  through `(:: o (Option int))` to read `.value` by-value.
- `tests/fixtures/refined-nonempty/input.tur`,
  `tests/fixtures/refined-bounded-idx/input.tur`,
  `tests/fixtures/kleisli-arrow-instance/input.tur`,
  `tests/fixtures/option-result-c-abi/input.tur`: ascribe carrier-int
  Option producers at the `(some? ...)` site.

## Cascade-coupled: `unwrap-or` (deferred)

`unwrap-or` is used internally by many carrier-`:int` designs that would all
have to retype together (the `some?` retype is no longer in this cascade --
it bridges via call-site ascription):

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

## Blocked: `result-map` (deliberate carrier-ABI regression test)

`result-map` is *not* in the same boat as `option-map`. Retyping it to
`[A B C] [r : (Result A B) ^fat f : (fn [A] C)] : (Result C B)` would break
`tests/fixtures/typed-slots/coerce-carrier-to-struct/`, a **deliberate
carrier-ABI regression test**: its `(defn double-if-ok [r : int] : int
(result-map r (fn [x] (* x 2))))` passes a bare `:int` carrier into
`result-map`, then ascribes the carrier result via `(:: r (Result int int))`.
The carrier->concrete `::` bridge it exercises depends on `result-map`'s
`:int` signature. Retyping `result-map` makes `(result-map r ...)` with
`r : int` a type error, regressing that test's premise -- and per the
project rule, a deliberate carrier-bridge regression fixture is not rewritten
to dodge the breakage. `result-map` therefore stays on its inline-C carrier
body until that fixture is intentionally migrated (or a by-value twin is added
alongside the carrier surface).

## `option-free`

Unused outside docstrings; for a by-value Option there is no heap box to free.
Left on `:int` (pure churn to change a dead helper). Revisit when none-as-NULL
is fully retired and the carrier producers (`some`/`none`) stop heap-allocating.

## Sequencing for the remainder

1. ~~Generalize `construct_recovered_byvalue` to non-instance generic specs
   (construct-in-by-value-return)~~ -- **2026-06-17: done** (the `(some x)`
   arm). Fixture `tests/fixtures/option-construct-byvalue-return-spec/`.
2. ~~**0-arg constructor `abi_bindings`.**~~ -- **2026-06-18: done.** elab
   attaches the constructor-result-tyvar -> caller-tyvar binding on a 0-arg
   `#{Construct}` (`(none)`/`(err)`) in non-ground return position; emit
   composes it through the active spec to mint a by-value `none__spec`.
   Structural-match guards (`emit_call_name`/`find_matched_abi_spec`) and the
   spec-return-ABI consult (`call_returns_byvalue_aggregate`/
   `expr_emits_byvalue_carrier_abi`) keep carrier-context and
   unresolved-element-spec cases correct. Fixtures:
   `option-construct-byvalue-return-spec` (none arm), `option-map-capturing-closure`.
3. ~~Rewrite `option-map` body to pure Turmeric~~ -- **2026-06-18: done.**
   `result-map` deferred (deliberate carrier-ABI regression test, above).
   Niche `option-map` residual:
   `docs/archive/option-map-literal-none-unannotated-fn-no-A-inference.md` (resolved 2026-06-18; emit-side guards in PR #421 route the under-determined-`A` call to the carrier-context spec). A separate spill-bridge regression at the by-value-producer -> carrier-consumer boundary inside a `let`/`do`/`if` arg slot is tracked in `docs/reported/option-map-byvalue-result-into-carrier-consumer-let-inside-arg.md`.
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
