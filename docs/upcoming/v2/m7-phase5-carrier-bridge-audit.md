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

### Construct-monomorphization layer -- LANDED (2026-06-19, commit 5c8b725)

The construct-monomorphization step is now implemented and green (1685/0). In
`emit_module.c`'s `emit_abi_register_call`, the `construct_recovered_byvalue`
block (which previously grounded a `#{Construct}` result only inside an *active
spec*) now also fires at a plain call site when the call's own `abi_bindings`
(or already-grounded `call->type`) resolve the constructor's parametric result
to a concrete by-value non-heap struct (`(some 42)`: `A -> int` => `(Option
int)`), with a matching `emit_abi_note_carrier_call` so the auto-preloaded HKT
instance carrier bodies still find the carrier base. `option-basic` drops from
**7 carrier crossings to 1**.

The control-form homogeneity blocker (a by-value `(some 11)` arm meeting a
carrier `(none)` arm) was resolved **emit-side** -- not via the elaborator
change earlier predicted -- by teaching every boundary site to bridge between
the carrier and by-value representations:

- **concrete->carrier RETURN bridge** (emit_fns.c, both the main fn path and
  `emit_tail`): a closure/thunk whose C return is the uniform int64 carrier but
  whose body tail is now a by-value construct spec is heap-spilled
  (`malloc` -- a stack spill would dangle) back to the carrier.
- **by-value if-merge** (emit_expr.c `emit_if_value`): when either arm is a
  by-value producer the merge temp is declared by-value and each carrier arm is
  bridged carrier->concrete (a deref).
- **control-form by-value detection** (`expr_emits_byvalue_carrier_abi` now
  delegates `if`/`do`/`let` to the tail walker; new
  `fn_body_tail_byvalue_carrier_type` recovers the concrete type from the
  matched spec, since the construct's `e->type` is carrier-collapsed).
- **construct cross-spec guard** (`find_matched_abi_spec` never applies its
  cross-spec fallback to a `#{Construct}` callee, mirroring `emit_call_name`),
  so a shared `(some ...)` Expr* is not reported by-value under a sibling
  int64-result spec.
- **generic-carrier-param arg bridge** fires when the callee param resolves to
  the `int64_t` carrier C type (abstract `(Option A)`), leaving concrete
  by-value sinks (`(Option BoundedIdx)`, `(Pair int int)`) alone.

**Measured outcome:** raw suite-wide bridge-call count `1319 -> 1338` (neutral;
the audit doc's earlier "~78" was a deduped Option/Result subset, the raw
per-fixture total is ~1319 dominated by stdlib-internal crossings). The change
trades consumer-side `carrier->concrete` crossings for construct-side
`concrete->carrier` ones while eliminating them at user construct sites
(option-basic 7->1). So this lands the construct *half* of the deletion and the
reusable boundary-bridge infrastructure, but does not itself delete the bridge.

### What still blocks full deletion: the generic Option/Result CONSUMERS

The ~1319 raw crossings are dominated by the generic stdlib helper bodies
(`unwrap`/`some?`/`unwrap-or`/`option-eq?`/`result-map`/...), compiled ONCE over
a type variable `A` and replicated per importing fixture. They carry the carrier
internally and only monomorphize when a concrete call site grounds the whole
relay chain. Deleting the bridge requires monomorphizing these CONSUMERS (and
their generic-to-generic "relay" calls) per element type -- the second half of
the engine, building on the construct half and the boundary bridges landed here.

### The conclusive architectural constraint (why this is genuinely structural)

Tracing the prototype's failures one level deeper lands on a single load-bearing
assumption in the emitter: **control-form branches are homogeneous in
carrier-kind.** The codegen for `if`/`cond`/`let`/`do` results is built on it:

- `fn_body_tail_is_carrier_producer` for an `if` requires BOTH arms to be carrier
  producers (emit_expr.c:218-221).
- `fn_body_tail_emits_byvalue_carrier_abi` for an `if` returns true if EITHER arm
  is by-value (emit_expr.c:340-341).
- `emit_control_result_temp_decl` (emit_expr.c:528) picks ONE representation for
  the whole result temp from those two predicates.

When both arms agree (the status quo: both carrier, or both by-value via a shared
enclosing spec) this is correct. The construct-monomorphization prototype breaks
the agreement -- arg-bearing `some` goes by-value while 0-arg `none` stays
carrier -- so the single result temp is declared `Option__int` but one arm
assigns the `int64_t` carrier, and `cc` rejects it. The same homogeneity
assumption underlies the function-return bridge
(`fn_body_tail_is_carrier_producer` keying the `emit_fns.c` return unbox), which
is the `kleisli` "returning Option__int but int64_t expected" failure.

So full bridge deletion is not a localized edit; it requires **either**
homogenizing every control-form's construct leaves at elab time (cross-branch
expected-type / binding propagation so all arms pick the same representation)
**or** teaching every control-form result site (if/cond/let/do temps + function
returns) to bridge each arm to the chosen representation -- i.e. dismantling the
homogeneity assumption that the emitter currently relies on throughout. That is
the monomorphization engine, confirmed from three independent angles (the
generic-helper crossings, the 0-arg construct binding gap, and this control-form
homogeneity assumption). It is tracked as its own phase; the bridge is genuinely
load-bearing until it lands, and the 5.1 tripwire guards its scope.

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
