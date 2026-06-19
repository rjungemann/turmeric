# Phase 5 re-audit (M10): carrier-bridge inventory after the HKT stdlib migration

**Snapshot:** 2026-06-19, after all six HKT classes
(Functor/Applicative/Monad/Alternative/Bifunctor/Foldable) migrated to by-value.

## Method

`emit_carrier_bridge` (emit_core.c) prints `[m3-audit] bridge <dir> type=<T>`
under `TUR_M3_AUDIT=1`. Ran `TUR_M3_AUDIT=1 tur emit-c` over every
`tests/fixtures/*/input.tur` and aggregated.

## PROGRESS UPDATE (2026-06-19, later same day)

Two levers landed, both green at 1685/0:
- **Construct-monomorphization half** (commit 5c8b725): constructs build by-value
  at plain call sites; user-construct-site crossings eliminated.
- **Dead-instance elimination**: skip unused auto-preloaded HKT instance
  dicts+bases. **Suite-wide raw crossings dropped 1338 -> 102 (92%)**; the
  bridge now fires in only **24 fixtures** (down from ~1300), all of which
  genuinely call the stdlib Option/Result HKT methods directly.

The remaining 102 are direct HKT method calls spilling by-value Option/Result
args to the carrier base (e.g. `__inst_Monad_bind_Option((int64_t)(intptr_t)(
&__t54), ...)`).  Eliminating them is lever 2 (HKT method-call monomorphization).
The original "~78" figure below is the pre-work historical baseline.

### Lever 2 -- FULLY root-caused (2026-06-19, deep dive): fmap/bimap/pure already monomorphize; bind/ap are closure-ABI-bound

A deep dive corrected the lever-2 picture substantially.  The emit path
**already monomorphizes HKT instance methods by value** -- e.g. in
`hkt-stdlib-option-result-instances` the `fmap` call routes to
`__inst_Functor_fmap_Option__spec__Option__int_Option__int_int64_t(Option__int,
tur_poly_fn_t)` taking the container BY VALUE with NO crossing.  `fmap`, `bimap`,
and `pure` all monomorphize cleanly today.  So lever 2 is NOT the broad
carve-out reversal earlier feared.

The remaining 102 crossings are **only `bind` and `ap`**, and the root cause is
specific and deep:

- `fmap`'s continuation `g` returns an ELEMENT (a scalar `b`) that flows into a
  `#{Construct}` (`(some (g x))`) -- the construct takes the carrier scalar
  directly, no crossing.
- `bind`'s continuation `k` returns a CONTAINER `(m b)` (a carrier `Option`)
  that IS the result (`(k (.value ma))` in tail position).  `ap` is analogous
  (`ff` carries a function whose result is the container element).  Two
  consequences:
  1. **Grounding fails:** the element `b` lives in the continuation's RESULT
     type, which is collapsed to a tyvar at dispatch time (the continuation
     lambda types as `(fn [int] (Option ?))`), so `m7_collect` can't ground `b`
     and the by-value spec is not committed (`m7_byvalue_grounded` stays false).
  2. **Even if grounded, the crossing persists:** `k` is a `tur_poly_fn_t`
     (closure), and closures use the UNIFORM CARRIER result ABI by design.  So
     `k` returns a carrier `Option`; `bind` returning it by value needs a
     `carrier->concrete` bridge regardless.

So the bind/ap residue is tied to the **closure-result carrier ABI for monadic
continuations** -- the deepest layer (poly_fn results are uniformly carrier).
Eliminating it requires by-value closure-result ABI for these continuations,
which is a larger change than method monomorphization and is genuinely the floor
for HKT monadic dispatch under the current closure model.  `fmap`/`bimap`/`pure`
are already done; `bind`/`ap` are the closure-ABI-bound remainder.

### (Historical) Lever 2 -- earlier diagnostic (what blocks it; two suppression points)

A probe of `emit_abi_register_call` over the live fixture
`hkt-stdlib-option-result-instances` shows the direct HKT method calls split
into two cases, and BOTH must be fixed to reach zero:

1. **`bind` / `ap` get no `abi_bindings` from elab** (`n_bindings=0`), so
   `emit_abi_register_call` early-returns and they stay on the carrier base.
   This is an ELAB gap: the dispatch lowering for `bind`/`ap` does not attach the
   element-type substitution the way `fmap`/`bimap`/`pure` do (`fmap` n=5,
   `bimap` n=8, `pure` n=2).  Fix: attach `abi_bindings` to `bind`/`ap` dispatch
   in elab_typeclasses.c (mirror the fmap path).
2. **Even `fmap`/`bimap`/`pure` (which HAVE bindings) still emit only the carrier
   base, no `fmap__spec`** -- so a second, EMIT-side suppression keeps HKT
   instance-method specs from being minted/routed by value.  This is the M6/M7
   carve-out (emit_module.c:785 leaves `typeclass_inst` NULL for HKT instances,
   and the by-value method spec is not interned/routed).  Fix: for a DIRECT HKT
   method call with concrete element bindings, mint a by-value method clone
   (by-value container param + `tur_poly_fn_t` for the function args) and route
   the call to it -- safe here because no fixture uses indirect dispatch.

The body of such a clone is already handled by the construct half
(construct-recovery for its `(some ...)`/`(ok ...)` tails) + the boundary
bridges landed this session; lever 2 is purely about *minting and routing* the
by-value method spec.  It is a two-sided (elab + emit) reversal of the carve-out
for the direct-call case -- bounded (24 fixtures, 102 crossings) but touching the
most delicate part of the HKT dispatch ABI, so it warrants its own focused pass
rather than being rushed against the green gate.

## (Historical) Result: the bridge is still LOAD-BEARING (~78 crossings), all by-value Option/Result

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

### What still blocks full deletion: the auto-preloaded HKT instance dict bases

A per-fixture breakdown after the construct half is sharper than "consumers":
**almost every fixture now has exactly ONE crossing, and it is the same one** --
`carrier->concrete type=(Option (fn [int] int))`. Tracing it (defn-basic, which
uses no Option at all, still has it) lands on a single function:

```c
static int64_t __inst_Applicative_ap_Option(int64_t ff, int64_t fa) {
    tur_option_t *__t12 = (tur_option_t *)(intptr_t)(ff);   // <-- the crossing
    ...
}
```

This is the **Option Applicative `ap` carrier base**, wired into
`dict_Applicative_Option_singleton.ap`.  It is emitted in EVERY fixture because
(a) the Option/Result HKT instances are auto-preloaded and (b)
`emit_abi_fn_skip_generic` never skips an HKT instance carrier base (the dict
references it -- the M6/M7 carve-out).  Its `ff` param is the int64 carrier (the
dict slot is `int64_t (*)(...)`), so reading the `Option<fn>` payload requires
the carrier->concrete deref.  The same applies to the `fmap`/`bind`/`ap` bases
of the preloaded Option/Result instances.

So the construct half eliminated the genuinely-deletable crossings (user
construct sites + generic consumers monomorphize at concrete call sites); the
~1319 raw total is dominated by this ONE function re-emitted per fixture, not by
1319 distinct deletable crossings.  The irreducible remainder is exactly the
**dict carve-out**.  Two independent levers remain, neither yet pulled:

1. **Dead-instance elimination** (REDUCES, never fully removes) -- do not emit an
   auto-preloaded HKT instance's dict + carrier base in a program that never
   dispatches through it (defn-basic needs no Option Applicative).  This drops
   the crossing from the ~1300 fixtures that don't use it, but keeps the bridge
   machinery for those that do.  Sound-use-analysis optimization; tracked
   separately.
2. **HKT method-call monomorphization** -- for the handful of fixtures that
   genuinely use the stdlib Option/Result HKT instances, the method calls are
   emitted as DIRECT calls to the carrier base with concrete element types known
   at the call site (e.g. `__inst_Monad_bind_Option((int64_t)(intptr_t)(&__t54),
   ...)` in `hkt-stdlib-option-result-instances` -- it spills a by-value
   `Option__int` back to the carrier for the base).  Because the element type is
   concrete at the call site, these can be monomorphized to by-value
   `bind__spec`/`ap__spec` clones exactly like the construct half did for
   `some`/`ok`, after which the base is dead too.

### Correction (2026-06-19, later): full deletion IS achievable -- earlier "impossible" claim retracted

An earlier revision of this section argued the dict carve-out made full deletion
impossible.  That is WRONG for this codebase, and the evidence is direct:

- **No fixture uses genuinely-indirect / constrained-polymorphic HKT dispatch**
  (`(defn f [^m] [^&: Applicative m] ...)`).  A grep for the constraint-dispatch
  syntax across `tests/fixtures/` returns nothing.
- **The dict singletons are never dispatched.**  `dict_*_singleton.<method>`
  slot accesses are 0 even in HKT-heavy fixtures (`hkt-stdlib-suite`,
  `hkt-do-m-option`, `hkt-monad-laws`).
- Every actual stdlib-HKT use is a DIRECT method call with a concrete element
  type (monomorphizable), and in the vast majority of fixtures the preloaded
  `__inst_*_Option`/`_Result` bases are **dead code** -- defined (carrying the
  crossing) but never called and never dispatched.

The impossibility argument only applies to *genuinely element-polymorphic
indirect dispatch through the dict* -- which the carrier is indeed the erasure
mechanism for -- but that pattern does not occur here.  So the carve-out is not
irreducible in practice: it is dead preloaded code plus direct calls that
monomorphize.  Full deletion (zero crossings) is reachable via lever 1
(dead-instance elimination of the unused preloaded dicts+bases, ~1300 fixtures)
plus lever 2 (HKT method-call monomorphization for the ~handful of genuine-use
fixtures).  Both are real, bounded implementations gated behind a coordinated
instance-liveness analysis (skip the dead dict singleton and its base together)
and lifting the M6/M7 carve-out for the direct-call case.  The construct half
(landed) is the same technique applied to constructors.

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
