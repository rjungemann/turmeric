---
title: TCO follow-up -- pure-Turmeric `Eq [Map]` / `Eq [Set]` instances
status: Resolved -- shipped pure-Turmeric `map-eq-loop` / `set-eq-loop` (option 1)
---

# Resolution

Implemented option 1 (TCO'd accumulator walk).  The `Eq [Map]` and
`Eq [Set]` instances now dispatch through pure-Turmeric loops
(`map-eq-loop` / `set-eq-loop`) that drive HAMT iteration via a thin
heap-allocated iter-box (`tur_hamt_iter_alloc` / `_advance` /
`_destroy` plus `_cur_hash` / `_cur_key` / `_cur_val` accessors) and
look up keys content-correctly through `tur_hamt_get_dynamic` /
`tur_hamt_has_dynamic`, which thread m1's stamped key comparator
(`tur_hamt_keyeq`) so `cstr` and other content-keyed maps stay
correct.  The self-tail-recursive specs lower to goto loops under the
existing TCO-in-ABI-specs gate, mirroring `Eq [Vec]` /
`Eq [MutableMap]`.

The inline-C `map-eq?` / `map-eq-raw?` / `map-eq-dynamic` and
`set-eq?` / `set-eq-cmp?` helpers stay for direct and abstract-K-V
callers (same compromise the Vec / MutableMap rewrites made).

Validation:

- Full suite green (`bash tests/run.sh`: `1676 passed, 0 failed`).
- 82 codegen snapshots regenerated alongside the instance change.
- Bridge audit: crossing-neutral (Map / Set remain `:heap`).

---

# Pure-Turmeric `Eq [Map]` / `Eq [Set]` instances

This is the residual scope of the now-archived
[tco-in-abi-specs-for-stdlib-iteration](../archive/tco-in-abi-specs-for-stdlib-iteration.md)
plan. The umbrella plan shipped its three TCO restriction lifts plus the
`Eq [Vec]` (PR #400) and `Eq [MutableMap]` rewrites; the post-#400
bridge audit floor sits at 34 crossings / 10 fixtures with zero
monomorphic deref-copies (see the archived
[m3-carrier-bridge-deletion-blocked-on-typeclass-abi](../archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
report's "Update 2026-06-17 (post-#400 audit floor)" section).

`Map` (HAMT) and `Set` (HAMT) currently keep their inline-C carrier
`Eq` instance bodies. Both types are `:heap`, so they **do not cross
the bridge today** -- this follow-up is type-hygiene cleanup, not
audit reduction.

## Scope

- Rewrite `Eq [Map]` to dispatch through a pure-Turmeric HAMT walk
  with a TCO'd accumulator, mirroring the `Eq [Vec]` /
  `Eq [MutableMap]` shape from PR #400 et seq.
- Rewrite `Eq [Set]` the same way (Set wraps HAMT).
- Keep the existing inline-C `map-eq?` / `set-eq?` helpers for direct
  / abstract-A callers (the same compromise `vec-eq?` and
  `mutmap-eq?` made).

## Likely shape

HAMT iteration in pure Turmeric needs either:

1. An accumulator-threaded recursive walk over the trie (TCO'd),
   matching the `vec-eq-loop` pattern but over child slots; or
2. An inline-C `hamt-fold` primitive taking a per-element fat-closure
   callback. The element callback would still use carrier ABI at the
   element level, but not the whole-map level. The archived plan
   flagged this as a possibility worth exploring.

Option 1 is the closer mirror to the Vec/MutableMap pattern and keeps
the iteration core in Turmeric; option 2 is the easier port if the
recursive walk hits TCO eligibility edges.

## Why this is not urgent

- **No audit pressure.** Map/Set do not cross the bridge today
  (`:heap` types; the inline-C body is invoked, not the bridge).
- **No spice / Track C dependency.** No spice surface is blocked on
  these instances being pure-Turmeric.
- **No new ABI work required.** The three TCO restriction lifts the
  umbrella plan needed are already in tree; this is application of
  the established pattern, not new compiler work.

## When to revisit

- If a future round of bridge auditing surfaces a downstream
  `:heap`-element Map/Set crossing that the inline-C body is hiding.
- If a spice author wants a pure-Turmeric `Eq` body for ergonomic
  reasons (e.g. composing it with constraint-driven derivation).
- Opportunistically, alongside other stdlib hygiene work, if the
  HAMT walk-with-accumulator pattern is needed for an unrelated
  reason (e.g. a pure `hamt-fold`).

## Validation when it lands

- Full suite green with the regenerated codegen snapshots that pick
  up the new `map-eq-loop` / `set-eq-loop` defns.
- Interpreter parity: `map-eq` / `set-eq` style fixtures should pass
  via the pure-Turmeric path (the umbrella plan saw this improve for
  MutableMap; the same lift is expected here).
- Bridge audit: crossing-neutral (Map/Set are `:heap` today; they
  should remain at zero crossings).

## Related

- [docs/archive/tco-in-abi-specs-for-stdlib-iteration.md](../archive/tco-in-abi-specs-for-stdlib-iteration.md)
  -- the now-archived umbrella plan with the full TCO restriction
  lift design, the Vec rewrite, and the MutableMap follow-up.
- [docs/archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  -- the now-archived M3 report whose audit baseline this work
  would extend.
