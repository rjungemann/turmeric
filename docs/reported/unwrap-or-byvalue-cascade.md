---
title: Retype `unwrap-or` to by-value `(Option A)` and migrate the carrier-int Option producers in stdlib
category: Stdlib / ABI -- Option none-as-NULL retirement (Track A, step 4 cascade)
severity: Medium ergonomics + audit hygiene. `unwrap-or`'s `[o : int dflt : int] : int`
  signature is the last large `:int`-stand-in surface in `stdlib/option.tur`.
  Retyping it to `[A] [o : (Option A) dflt : A] : A` honours the No-Lazy-`:int`
  rule and removes the audit's remaining carrier->concrete spills at
  `unwrap-or` call sites. The blocker is not codegen -- by-value primitive-
  payload Options round-trip through `unwrap-or` shapes today -- it is the
  cascade: ~10 stdlib modules currently produce carrier-int Options from
  inline-C bodies and pass them straight into `(unwrap-or r dflt)`. Retyping
  `unwrap-or` without first retyping (or bridging) those producers turns every
  call site into a hard type error.
status: OPEN, NOT YET STARTED. `unwrap-or` remains on
  `[o : int dflt : int] : int` in `stdlib/option.tur`. The native interpreter
  override (`native_option_unwrap_or` in `src/main.c`) is unaffected by a
  compiled-path signature change; the cascade is purely compiled-path callers.
  **No caller-ascription workaround (`(:: r (Option int))`) is in the tree
  for `unwrap-or`, and none will be added** -- same reasoning as the NonEmpty
  plan ([ne-from-byvalue-option-nonempty-element-type-uninferable](
  ne-from-byvalue-option-nonempty-element-type-uninferable.md)): propagating
  carrier-`:int` into every consumer is the No-Lazy-`:int` defect this retype
  exists to remove, and an `(Option int)` ascription on a `(Option float)`
  producer would silently truncate.
---

# Retyping `unwrap-or` to by-value `(Option A)`

## Context

`docs/reported/option-consumer-retype-byvalue.md` step 4 names `unwrap-or` as
the cascade-coupled tail of the Option-consumer retype. `option-eq?`,
`option-map`, `some?`, and the BoundedIdx half of step 4 have landed.
`unwrap-or` is the only consumer left that touches a large set of stdlib
producers.

Current signature:

```turmeric
(defn unwrap-or [o : int dflt : int] : int
  ```c return tur_is_some(o) ? tur_opt_value(o) : dflt; ```)
```

Target signature (mirrors `unwrap`):

```turmeric
(defn unwrap-or [A] [o : (Option A) dflt : A] : A
  (if (.is-some o) (.value o) dflt))
```

Pure-Turmeric body; no inline-C. The native interpreter override stays.

## Cascade scope

A `grep -rln "unwrap-or" stdlib/` enumerates the producers that have to be
either retyped to by-value `(Option A)` or have a bridge installed at the
`unwrap-or` boundary:

| Module | Producer | Notes |
|---|---|---|
| `stdlib/zipper.tur` | `zipper-move-right` and siblings (`zipper-move-left`, `zipper-down`, `zipper-up`) | Inline-C; return the carrier-int Option for an out-of-range move. Migrate to by-value `(Option (Zipper A))`. |
| `stdlib/seq/*.tur` | `seq-head?`, `seq-tail?`, generator-step Options | Carrier-int Options from inline-C iteration primitives. |
| `stdlib/json.tur` | `json-get?` / `json-field?` lookup helpers | Carrier-int Options over a parsed-JSON handle. |
| `stdlib/safe.tur` | safe-arith helpers (`safe-div`, `safe-mod`, ...) | Return carrier-int Option for divide-by-zero / overflow guard. |
| `stdlib/env.tur` | `getenv?` | Carrier-int Option over a `:cstr` payload. |
| `stdlib/serial.tur` | deserializer-step Options | Carrier-int Options from inline-C parse step. |
| `stdlib/kleisli.tur` | `k-apply-raw`'s return | Tracked separately under [kleisli-byvalue-option-cascade](kleisli-byvalue-option-cascade.md); not retired by this plan. |
| `stdlib/refined.tur` `ne-from?` | Carrier-int Option | Tracked under [ne-from-byvalue-option-nonempty-element-type-uninferable](ne-from-byvalue-option-nonempty-element-type-uninferable.md); the typed-list retype subsumes it. |

(The grep produces additional matches in `stdlib/docstrings.tur` and inside
fixtures; those are not producers, just call-site/doc text.)

**Verify the table before starting**: re-run the grep against current HEAD
and reconcile any new producers added since 2026-06-19.

## Strategy

The cascade is too large for a single PR, but each row is independently
landable. Land them in this order, one PR per row, each ending with a
green `bash tests/run.sh`:

1. **`unwrap-or` itself** -- retype in `stdlib/option.tur` to the by-value
   shape above, but keep a compatibility shim under a different name
   (`unwrap-or-carrier`) for the duration of the cascade. The shim's body is
   the current inline-C `unwrap-or`. Every existing stdlib caller flips to
   `unwrap-or-carrier` in this same PR, so the suite stays green at the
   producer boundary while individual modules migrate at their own pace.
2. **Per-module migration** (one PR per row in the table). Each PR:
   a. Retypes the module's Option producers to by-value `(Option T)` with a
      pure-Turmeric body where possible. Where the producer must stay
      inline-C (e.g. `getenv?` reading C `getenv`), it constructs the
      `(Option T)` via `(some ...)` / `(none)` from a fixed-arity helper
      and returns by value.
   b. Flips that module's `unwrap-or-carrier` call sites back to
      `unwrap-or`.
   c. Drops any caller-ascription bridge in that module (there shouldn't
      be one, but grep `(:: .* (Option int))` to confirm).
   d. Regenerates affected fixture snapshots (`expected.c`) in the same PR.
3. **Retire `unwrap-or-carrier`** -- after the last producer migrates, the
   shim has zero callers; delete it. Native override (`src/main.c`) stays
   on the by-value entry point.

### Why a shim, not caller-ascription

The shim is a temporary single-symbol surface that disappears at the end
of the cascade. It does not leak the carrier-`:int` convention into the
type signature of any new code: a caller of `unwrap-or-carrier` is
explicitly opting into the legacy ABI by name. Once a producer migrates,
the consumer flips back to `unwrap-or` in the same PR. There is no window
where new code sees `unwrap-or-carrier` as the "normal" choice.

Compare a caller-ascription pattern (`(unwrap-or (:: r (Option int)) 0)`):
the call-site ascription would become the dominant idiom for months, every
new consumer would see and copy it, and at `Option float` / `Option cstr`
producers it would silently truncate / mis-cast. The shim avoids both.

## Cross-cutting compiler dependencies

By-value `(Option A)` calls into a `[A] [o : (Option A) dflt : A] : A`
target rely on the ABI work that landed in PRs #414, #421, #425, #426:
NULL-safe `.is-some` deref, by-value-producer -> by-value-consumer call,
spill bridge around `let`/`do`/`if` wrappers whose tail produces a
by-value aggregate. The `unwrap-or` retype itself does not need new
codegen; each producer migration may surface a residual gap (see the
`option-map` cascade for precedent), in which case file a fresh report
and pause that row until it lands.

## Validation

- `stdlib/option.tur`: `unwrap-or` is pure-Turmeric, no inline-C. The
  shim `unwrap-or-carrier` exists during the cascade and is gone after.
- Each per-module PR: green `bash tests/run.sh` (~1442 fixtures) plus
  any new regression fixtures added for that producer.
- No new `(:: .* (Option int))` strings introduced in `stdlib/` or
  `tests/fixtures/` across the cascade. Grep before opening each PR.
- Native interpreter (`turi`) regression suite passes on each step --
  the override stays bound to the by-value entry point so the
  interpreter path is unaffected.
- Audit `option-consumers-byvalue-arg` fixture count drops to **0**
  after the last row migrates.

## Out of scope (explicitly)

- The kleisli `comp` / `k-apply-raw` retype -- tracked under
  [kleisli-byvalue-option-cascade](kleisli-byvalue-option-cascade.md).
- The NonEmpty / `ne-from?` retype -- tracked under
  [ne-from-byvalue-option-nonempty-element-type-uninferable](
  ne-from-byvalue-option-nonempty-element-type-uninferable.md).
- `result-map` -- deferred; its `:int` signature is a deliberate
  carrier-ABI regression test (see option-consumer-retype-byvalue).
- `option-free` -- dead helper; revisit after none-as-NULL is fully
  retired.
- Any caller-ascription bridge as a permanent solution.

## Related

- `docs/reported/option-consumer-retype-byvalue.md` -- the umbrella;
  this file is its `unwrap-or` row broken out for independent tracking.
- `docs/reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md`
  -- sibling NonEmpty cascade.
- `docs/reported/kleisli-byvalue-option-cascade.md` -- sibling Arrow
  cascade.
- `stdlib/option.tur` `unwrap-or` (the retype target).
- `src/main.c` `native_option_unwrap_or` (interpreter override; stays).
