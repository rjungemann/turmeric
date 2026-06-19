# Phase 5 re-audit (M10): carrier-bridge inventory after the HKT stdlib migration

**Snapshot:** 2026-06-19, after all six HKT classes
(Functor/Applicative/Monad/Alternative/Bifunctor/Foldable) migrated to by-value.

## Method

`emit_carrier_bridge` (emit_core.c) prints `[m3-audit] bridge <dir> type=<T>`
under `TUR_M3_AUDIT=1`. Ran `TUR_M3_AUDIT=1 tur emit-c` over every
`tests/fixtures/*/input.tur` and aggregated.

## Result: the bridge is still LOAD-BEARING (~78 crossings), all by-value Option/Result

| shape | crossings (approx) |
| --- | --- |
| `carrier->concrete (Option int)` | 39 |
| `concrete->carrier (Option int)` | 10 |
| `carrier->concrete (Result int int)` | 6 |
| `carrier->concrete (Result cstr cstr)` | 6 |
| `concrete->carrier (Result int int)` | 4 |
| `carrier->concrete (Option Device)` | 4 |
| `carrier->concrete (Result int cstr)` | 4 |
| `carrier->concrete (Option float)` | 3 |
| `carrier->concrete (Result Device int)` | 3 |
| `carrier->concrete (Result bool cstr)` | 2 |
| `carrier->concrete (Option cstr)` | 1 |

Every remaining crossing is a by-value `Option`/`Result` value meeting a carrier
producer/consumer. There are NO stray crossings over other types -- the audit
floor is exactly the Option/Result family.

## Why the bridge cannot be deleted yet (the genuine carrier-essential set)

The carrier producers feeding these crossings are:

1. **The per-instance dispatch DICT path (the M6/M7 carve-out).** Indirect /
   constrained-polymorphic HKT dispatch (a `(defn f [^m] [^&: Monad m] ...)`
   calling `.bind` through the dict singleton) keeps the uniform `int64` carrier
   ABI -- the dict's function-pointer slots are `int64_t (*)(int64_t, ...)`. A
   by-value `Option`/`Result` consumer of such a dispatch result must bridge the
   carrier int64 back to the by-value struct. The direct (monomorphic) call sites
   already go fully by-value via the per-(f,A) `__spec` clones; only the
   dict/indirect path produces a carrier result.
2. **Genuinely-erased helpers** (HAMT `tur_hamt_*`, `option-map`/`result-map`
   carrier shims used by carrier-context callers) -- documented carrier-essential
   in `v2/phase4-carrier-helper-inventory.md`.

So Phase 5's "delete the bridge" (5.3/5.5) is gated on a FURTHER migration:
lower the **dispatch-dict ABI itself** from the int64 carrier to per-instance
by-value slots (so the indirect path matches the direct `__spec` path), after
which the dict-fed crossings disappear and the bridge collapses to only the
genuinely-erased helpers. That is an M9-follow-on (dict-ABI monomorphization),
distinct from the stdlib instance-body migration that Phase 4 completed.

## Phase 5 status

- **5.4 (re-audit): DONE** -- inventory above; audit floor is the Option/Result
  family only, all traceable to the dict/indirect-dispatch carrier carve-out plus
  the erased helpers.
- **5.1 (tighten predicates): tripwire LANDED.** The bridge predicates already
  fire ONLY on real carrier<->by-value crossings (the audit shows no spurious
  firings), so they are effectively scoped to the carrier-essential set. A
  compile-audit **tripwire** now backs that empirically: `emit_carrier_bridge`
  (emit_core.c), under `TUR_M3_AUDIT=1`, prints
  `[m3-audit] WARNING non-essential carrier crossing type=<T>` for any crossing
  outside the carrier-essential family (Option/Result/heap-tagged/inline-scalar/
  pointer-leaf). The per-fixture sweep over all ~1685 fixtures reports **zero**
  such warnings, so the audit floor is now machine-checked, not just eyeballed --
  a future generic-instance body that leaks a by-value aggregate through the
  carrier surfaces immediately. The remaining step for 5.1 (promoting the
  tripwire from a non-fatal audit-mode warning to a hard, always-on abort) waits
  on the dict-ABI migration, since that is what removes the dict-fed crossings
  that today legitimately keep the carrier path live.
- **5.2 (rename `tur_ok`->`tur_box_ok`): DONE.** Renamed the carrier-bridge box
  constructors to the `tur_box_*` namespace across the emit paths
  (emit_core/emit_fns/emit_module/types), `stdlib/result.tur`, and the 8 fixtures
  that hand-roll the old names in inline-C. Snapshots regenerated; suite green at
  1685/0.
- **5.3 / 5.5 (delete bridge):** BLOCKED on the dict-ABI monomorphization above.
  The bridge is genuinely load-bearing today. The 5.1 tripwire is the standing
  guard until then.
