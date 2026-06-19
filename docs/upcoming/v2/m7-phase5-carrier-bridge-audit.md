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

**2026-06-19 deep re-investigation -- corrected attribution.** The earlier draft
of this section pinned the crossings primarily on the dispatch-DICT path. A
per-fixture breakdown (count of `[m3-audit] bridge` lines per fixture) shows the
DOMINANT source is actually **plain generic stdlib helpers**, not dict dispatch:

- `option-basic` produces **7** crossings with **zero** typeclass/dict dispatch
  -- they all come from `(some 42)`, `(none)`, `(unwrap ...)`, `(unwrap-or ...)`,
  `(option-eq? ...)`. Each is a generic `(defn some [A] [x : A] : (Option A))`
  etc. -- quantified over a type variable `A`.

The carrier producers feeding ALL ~78 crossings are, uniformly, **element-poly-
morphic code compiled once over a type variable** and then met by a concrete
by-value value:

1. **Generic Option/Result stdlib helpers** -- `some`/`none`/`unwrap`/
   `unwrap-or`/`option-eq?`/`ok`/`err`/`result-map`/... Each is
   `(defn f [A ...] ... : (Option A))` (or `(Result ...)`). It is compiled
   ONE time; at that point `A`'s size/layout is unknown, so the body MUST use
   the uniform `int64` carrier representation. A concrete call site like
   `(some 42)` then bridges `carrier->concrete (Option int)`.
2. **The per-instance dispatch DICT path (the M6/M7 carve-out).** Indirect /
   constrained-polymorphic HKT dispatch (`(defn f [^m] [^&: Monad m] ...)`
   calling `.bind` through the dict singleton) keeps the same uniform `int64`
   carrier ABI -- the dict's function-pointer slots are `int64_t (*)(int64_t,
   ...)`, polymorphic over the erased element type. Same root cause: a function
   compiled once over an unknown element type.
3. **Genuinely-erased runtime helpers** (HAMT `tur_hamt_*`) -- carrier by
   construction.

### The fundamental limit

This is the classic separately-compiled-parametric-polymorphism trade-off
(cf. Java/Go generics erasure): a polymorphic function compiled ONCE cannot be
by-value, because the value's size is unknown at its compile time. There are
exactly two ways to remove a carrier crossing, and no third:

- **Monomorphize** -- emit a per-`(f, A)` clone of the generic function with
  `A` concrete (the existing `__spec` machinery), so no carrier is ever
  produced; or
- **keep the carrier** -- the uniform `int64` representation that erases `A`.

The direct typeclass-instance-method call sites already take the first path via
`__spec` clones (Phase 4). Everything still crossing does so because it is NOT
yet monomorphized: the generic stdlib helpers and the dict/indirect path are
both compiled once over an erased element type.

### What "delete the bridge" (5.3/5.5) therefore requires

Deleting the bridge is EQUIVALENT to monomorphizing every remaining element-
polymorphic Option/Result function per concrete element type at each call site
-- i.e. extending the per-`(f, A)` `__spec` path from "direct typeclass-instance
methods" to "all generic Option/Result-typed functions," driven by elab
attaching `abi_bindings` (the `A -> int` substitution) to EVERY such call, plus
the emit-side spec-interning gates. The specialization decision is gated on
`call->as.call_.abi_bindings` (emit_module.c:1186); `option-basic`'s `(some 42)`
carries none today, which is exactly why the carrier `some()` is called and
bridged.

That is the **core of the end-to-end-monomorphization-plan** (its Phases 1-3 +
the monomorphization engine), not a bounded follow-on. It changes elab binding
attachment for all generic calls and the emit spec gates, regenerates hundreds
of snapshots, and has a broad fixture blast radius. It cannot be landed
partially without breaking the by-value gate suite (the v1 hard requirement),
so it is tracked as its own phase. Until then the bridge is genuinely
load-bearing and the 5.1 tripwire guards its scope.

### Prototype + the sharp remaining blocker (2026-06-19 experiment)

A bounded emit-side prototype was built and measured to pin down exactly where
the work lives. In `emit_module.c`'s `emit_abi_register_call`, the
`construct_recovered_byvalue` block (which today only grounds a `#{Construct}`
result inside an *active spec*) was extended to also fire at a plain call site
when the call's own `abi_bindings` ground the constructor's parametric result to
a concrete by-value struct (`(some 42)`: `A -> int` => `(Option int)`), plus a
matching `emit_abi_note_carrier_call` so the auto-preloaded HKT instance carrier
bodies still find the carrier base.

Result (measured): `option-basic` dropped from **7 crossings to 1**, only **8**
snapshots churned (not the feared hundreds), and the fixture built and ran with
correct output. So monomorphizing the *direct-argument* construct case is real,
bounded, and effective.

But it broke **4 fixtures** (`option-control-form-construct`,
`kleisli-arrow-instance`, `hkt-stdlib-option-result-instances`,
`option-of-tvec-eq`), and the failures isolate the true blocker precisely:

- In `(if b (some 11) (none))` the arg-bearing `(some 11)` monomorphizes to
  `some__spec` (returns `Option__int`) but the **0-arg `(none)`** stays on the
  carrier (`int64_t`), because elab only attaches 0-arg-`#{Construct}` bindings
  in a **return position** with an `expected_return` push (elab_call.c:3996-4035,
  the "step 2 / ground" branches). A `(none)` sitting in an `if`/`cond`/`let`
  branch gets no `expected_return`, so it reaches emit with `call->type`
  collapsed to the carrier and cannot be grounded emit-side either. The two `if`
  arms then disagree (`Option__int` vs `int64_t`) and `cc` rejects the merge.

So the sharp next increment is **elaborator-side, not emit-side**: push the
expected type into `#{Construct}` sites inside control-form branches
(`if`/`cond`/`let`/`do`) so a 0-arg `none`/`err` receives `abi_bindings`
consistent with its arg-bearing `some`/`ok` siblings. Once construct siblings
agree, the emit-side construct-monomorphization prototype above deletes the bulk
of the Option/Result crossings without the carrier/by-value branch-merge
mismatch. That elab change is the first concrete, testable step of the larger
monomorphization engine; the prototype is reverted (gate kept green at 1685/0)
pending it.

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
