---
title: Plan M3 ("delete `emit_carrier_bridge`") is gated on M4, not just M2
category: Codegen / ABI — monomorphization plan refinement
severity: Low. M3 is presented in `docs/upcoming/end-to-end-monomorphization-plan.md` as a 1-2-session phase to retire the carrier-bridge machinery after M2 lands. Empirical audit (this session) shows that claim was over-optimistic: even with the full M2b stdlib migration in tree, the bridge is still load-bearing for typeclass-method-dispatch call sites. The actual deletion needs M4 (per-method typeclass ABI) first.
description: I shipped instrumentation (`TUR_M3_AUDIT=1`) inside `emit_carrier_bridge` so the per-call-site cost of removing it is measurable. Running the full suite under the audit shows only 2 crossings (and both inside already-FAILing pre-existing fixtures), which initially looked like the bridge was dead. Attempting the M3 deletion, the suite regressed by ~6 fixtures with cc errors of the form "passing 'int64_t' to parameter of incompatible type 'Result__int__cstr'". Direct per-fixture re-audit then showed the bridge IS firing for those — `tests/run.sh` was swallowing per-fixture stderr in the snapshot/test phase, so the suite-wide audit count was misleadingly low. The bridge's remaining role is `carrier→concrete` at `EX_ASCRIBE` (when a typeclass-method return like `(:: (decode …) (Result int cstr))` produces an int64 carrier but the ascription's outer context wants the by-value struct) and `concrete→carrier` at the typeclass-instance dispatch arg-cast site (when a by-value `Tuple2__int__int` is passed to a dict-dispatched `__inst_Eq_eq_qu_Tuple2(int64_t, int64_t)`).
status: MOSTLY UNBLOCKED -- 2026-06-15 (see "Resolution 2026-06-15" below). On the current merged tree (`origin/main`, with M5 Option C `#364` and the M5 emit fixes landed) the bridge fired for **1188 fixtures / 2429 crossings**, but **2376 of those (98%) were a single pattern**: the `Eq [Cons]` carrier base (`stdlib/list.tur`) recursing through `(:: t1 (Cons A))` with `A` abstract, emitted into every program that links stdlib. That instance was also **silently miscompiled** (invoking `(eq? cons cons)` via spec or abstract dispatch produced hard cc errors) and **never tested** -- `list-basic` exercises the separate `list-eq?` function, not the instance. **Fixed this session** by rewriting `Eq [Cons]` to delegate to carrier-based `list-eq?` with an element-comparison closure (the proven `Eq [Vec]` pattern): empty `main` crossings 2 -> 0, the instance now compiles + returns correct results, full suite 1637/0. Remaining: a **~53-crossing long tail** (abstract-element `Vec`, multi-param `MutableMap`, and the pre-existing `concrete->carrier` dispatch-arg-bridge gap that fails identically for `Vec` and `Cons`). The bridge cannot be deleted outright until those clear -- M4/M5 work -- but M3 is no longer dominated by `Eq [Cons]`.
NOTE-on-earlier-status: an earlier 2026-06-15 status line on this report claimed the M4c Path A / M5 work "was never merged" and that the tree was blocked on 1186 fixtures. That was WRONG -- it came from auditing a stale checkout that predated M5 Option C (`#364`) and from looking up squash-merged commits by their pre-merge short SHAs (which no longer exist after squash). M5 Option C IS merged; the accurate status is above.
historical-status: PARTIALLY RESOLVED 2026-06-13. Path A landed for arg-side AND return-side substitution (commits `0fd565fe` and `c6088488`). **Suite-wide bridge audit** (`TUR_M3_AUDIT=1` per-fixture) reveals 14 fixtures still cross the bridge with ~82 total crossings — but those are NOT residuals from Path A's mechanism. They are **legitimate, essential bridge uses**: stdlib `Eq` instances on collection types (`Vec`, `Map`, `MutableMap`, `Set`, `Cons`) consult inline-C carrier helpers (`vec-eq?`, `map-eq?`, etc.) that fundamentally require the int64 carrier ABI to iterate opaque memory. With Path A specializing the instance methods to by-value param types, the bridge fires (correctly) at the helper call sites to spill the by-value back to int64. Additional bridge-essential cases: `generic-relay-aggregate-result` (SChan), `tuplen-struct-param-passing`, `data-literal-typed-empty`, `serial-composite-instances`. **The bridge is not dead code; it's load-bearing for any dispatch path that bottoms out in an inline-C carrier helper.** Deleting it requires rewriting every such stdlib helper to be ABI-agnostic — a substantial parallel effort that would touch `vec-eq?`, `map-eq?`, `mutmap-eq?`, `option-eq?`, `set-eq?`, `list-eq?`, `vec-fmap`, `map-fmap`, etc., reimplementing each in pure Turmeric without inline-C helpers that take int64. That's strictly beyond the M4 scope as originally written.
---

# Plan M3 deletion is blocked on M4's per-method typeclass ABI

## The design's claim

`docs/upcoming/end-to-end-monomorphization-plan.md` §M3:

> The work is removing the carrier-bridge machinery (Prereq 2's
> `emit_carrier_bridge` CK_CARRIER -> CK_CONCRETE path) that exists to deref
> carrier-shaped accessors. With M2 in tree the accessors operate on real
> by-value structs and the bridge becomes dead code; M3 deletes it.

"Becomes dead code" turns out to be true for stdlib accessor wrappers
themselves (`ok-val`, `err-val`, `unwrap`, `pair-fst`, `pair-snd` — all already
`(.field x)`), but the bridge is still called at call sites those accessors
appear inside, when the receiver's runtime ABI is int64 carrier (because the
producer is a typeclass-method dispatch, not a monomorphized constructor).

## How the audit misled me

I added a `TUR_M3_AUDIT=1` env-var probe inside `emit_carrier_bridge` and ran
the full suite under it. The audit reported only 2 crossings:

```
1 bridge concrete->carrier  type=(type-app SChan (type-app (type-app SRecv int) ptr<void>))
1 bridge carrier->concrete  type=(type-app (type-app Pair int) int)
```

Both inside fixtures that already FAIL on main (`scheduler-multithread`,
`schan-worker-pool` etc.). That looked like a clear path to deletion.

**The audit was wrong**, but in a subtle way: `tests/run.sh` redirects
per-fixture compile stderr to `actual.stderr` and only echoes a summary on
PASS. So `fprintf(stderr, "[m3-audit] …")` from inside the compiler doesn't
reach the audit log when the fixture compiles cleanly — it lands in the
fixture's `actual.stderr` and gets thrown away.

Direct per-fixture audit (running `tur build` outside the harness) tells the
real story. For example:

```
$ TUR_M3_AUDIT=1 ./build/tur build tests/fixtures/typeclass-return-dispatch-result-wrapped/input.tur 2>&1 | grep m3-audit
[m3-audit] bridge carrier->concrete type=(type-app (type-app Result int) cstr)
[m3-audit] bridge carrier->concrete type=(type-app (type-app Result cstr) cstr)

$ TUR_M3_AUDIT=1 ./build/tur build tests/fixtures/emit-abi-trace/input.tur 2>&1 | grep m3-audit
[m3-audit] bridge concrete->carrier type=(type-app (type-app Tuple2 int) int)
[m3-audit] bridge concrete->carrier type=(type-app (type-app Tuple2 int) int)
```

So at minimum these three fixtures (plus the related
`typeclass-method-parameterized-result-decode`) need the bridge to compile.

## Why these crossings can't be eliminated by M2 alone

The remaining crossings sit at the **typeclass-method dispatch boundary**.

`carrier→concrete` example: `(:: (decode doc handle) (Result int cstr))`.
`decode` is a typeclass method; the dispatch table's slot is uniformly
`int64_t (*)(int64_t, …)`. The ascription pins the static type to `Result int
cstr` (a by-value struct after M2). To bridge: deref the int64 handle as
`Result__int__cstr *` and load the by-value struct. That's exactly the
`CK_CARRIER → CK_CONCRETE` path the bridge implements.

`concrete→carrier` example: `__inst_Eq_eq_qu_Tuple2(t1, t2)` where t1/t2 are
by-value `Tuple2__int__int` values. The dict-resolved instance method's C
signature is `bool __inst_Eq_eq_qu_Tuple2(int64_t, int64_t)` because the
dispatch dict slot is uniform. To bridge: spill the by-value to a local and
pass its address as `int64_t`. That's the `CK_CONCRETE → CK_CARRIER` path.

Both go away only when the dispatch dict's slot type matches the instance's
real signature — i.e. when the dict carries `bool (*)(Tuple2__int__int,
Tuple2__int__int)` for the Tuple2 instance. That is exactly **Plan M4** in
the monomorphization plan ("non-HKT typeclass instances switch to per-method
ABI"). Until M4 lands, the carrier ABI is the dict's contract, and the
bridge is the only thing keeping by-value and dict-uniform views consistent
across the same call.

## What got shipped this turn

- `src/compiler/emit_core.c` — `TUR_M3_AUDIT=1` env-var probe inside
  `emit_carrier_bridge`. Off by default; one `fprintf(stderr, …)` per
  crossing when enabled. Permanent diagnostic — useful when M4 lands and
  someone wants to re-verify that the bridge has actually gone dead.
- This finding doc.
- No emit/runtime change. Suite unchanged: 172 FAIL, exact pre-M3 baseline.

## Validation when M4 lands

1. Apply the M4 dict-slot rework so non-HKT instance dict slots use the
   per-instance C signature (e.g. `bool (*)(Tuple2__int__int,
   Tuple2__int__int)` rather than `bool (*)(int64_t, int64_t)`).
2. Run the full suite under `TUR_M3_AUDIT=1` directly (not through
   `tests/run.sh`, or with the harness patched to surface per-fixture
   stderr). Expected: zero crossings.
3. Delete `emit_carrier_bridge` (the impl in `emit_core.c`, the
   `CarrierKind` enum + decl in `emit_internal.h`, the 4 call sites in
   `emit_expr.c`, the unit test
   `tests/compiler/test_emit_carrier_bridge.c`, and the CMake target
   `tur_codegen_carrier_bridge`).
4. Full suite must remain at baseline.

## Re-validation 2026-06-15 (SUPERSEDED -- contains a wrong "never merged" claim; see "Resolution 2026-06-15" below)

> **This section is wrong and is kept only as a record of the mistake.** It
> was written against a stale checkout that predated M5 Option C (`#364`) and
> concluded the M4c/M5 work "was never merged" because it looked the commits
> up by their pre-squash short SHAs. After merging `origin/main` (which DOES
> contain M5 Option C) and rebuilding, the real story is in "Resolution
> 2026-06-15". Do not trust the numbers or the "blocked on M4+M5 actually
> landing" verdict in this section.

Ran step 2 of the validation harness on a fresh Debug build of the current
tree. Verdict: **the bridge is more load-bearing than ever; deletion is
firmly blocked.** Details:

- **Suite-wide audit** (`TUR_M3_AUDIT=1 ./build/tur build <fixture>` per
  fixture, stderr captured directly so the `tests/run.sh` swallow described
  above can't mislead): **1186 fixtures cross the bridge, 2426 total
  crossings.** That is ~85x the fixture count and ~30x the crossing count
  this report originally recorded.

- **Even an empty program crosses.** `(defn main [] : int 0)` emits **2**
  `carrier->concrete type=(type-app Cons tyvar)` crossings. They come from
  `stdlib/list.tur:165`'s `(definstance Eq [Cons] ...)` carrier base, whose
  body recurses with `(eq? (:: t1 (Cons A)) (:: t2 (Cons A)))`. With `A`
  abstract in the carrier base, each `(:: tN (Cons A))` ascription lowers to
  `(*(Cons__... *)(intptr_t)(tN))` -- the `CK_CARRIER -> CK_CONCRETE`
  accessor-side path M3 targets. Every program that links stdlib list (i.e.
  every program) inherits these two crossings, which is why the suite-wide
  count is so high. This is exactly M5's Finding 2/3 (the residual hard case
  is *abstract* element types in the instance carrier base; the fix is
  ABI-aware field-access lowering, which M5 documents as designed but **not
  landed**).

- **The M4c Path A / M5 work was never merged.** `git fetch origin main`
  then `git log` for the commits this report and
  `docs/upcoming/m5-residual-straddle-retirement.md` cite as "landed"
  (`0fd565fe`, `c6088488`, `78589845`, `a45ff6c1`, `a301229e`) finds them on
  **no ref**. The M5 plan's own session logs end with "all experiments
  reverted" / "the field-access change and the stdlib rewrite are reverted
  from the tree." So the by-value instance specialization that this report's
  original audit measured against was experimental and is not in the tree;
  the merged tree only ever had the carrier-base `Eq Cons` / `Eq Vec` shape,
  which crosses the bridge universally.

- **`origin/main` is 3 commits ahead** of this branch, all unrelated cfnptr
  ABI tweaks (`#363`/`#362`/`#365` lineage); they do not touch the bridge or
  the monomorphization path, so the bridge state is identical on both.

Net: no code change is warranted here. Deleting `emit_carrier_bridge` now
would break 1186 fixtures. M3 stays blocked on M4 (per-method typeclass ABI)
**and** M5 (residual-straddle retirement / ABI-aware field-access lowering)
actually landing on `main` -- neither has. The audit probe at
`src/compiler/emit_core.c` `emit_carrier_bridge` remains the correct
re-verification hook for whenever that work does land; re-run this same
suite-wide audit and expect the count to fall to 0 before attempting the
deletion in step 3.

## Resolution 2026-06-15 (Eq[Cons] retired -- 98% of crossings gone, suite green)

After merging `origin/main` (M5 Option C `#364` + the M5 emit fixes present),
rebuilding, and running the audit *with per-fixture stderr captured directly*:

- **1188 fixtures crossed, 2429 total crossings.** Categorized by type:
  **2376 (98%)** were `carrier->concrete (type-app Cons tyvar)`; the rest a
  long tail (`Vec int` 27+6, `MutableMap` 4, `Result Device` 3, `SChan` 2,
  `Option Device` 2, `Tuple3..8` ~1 each, `Pair` 1, `Result int cstr` 1).

- **The dominant crosser was `Eq [Cons]` and it was a latent miscompile.**
  Its body recursed via `(eq? (:: t1 (Cons A)) (:: t2 (Cons A)))`. Because
  `Cons`'s `tail` field is typed `:int` (a stand-in for `(Cons A)` -- exactly
  the "No Lazy `:int`" defect), `(.tail x)` loses its type and the ascription
  forced the `CK_CARRIER -> CK_CONCRETE` bridge. The instance is emitted into
  every binary (for the dict singleton), so it crossed twice in *every*
  program -- including an empty `main`. Worse, invoking `(eq? cons cons)`
  emitted by-value `Cons__int` into the `int64_t` carrier dict slot (cc error)
  and a bogus `*(int64_t*)tail` deref; it compiled into binaries but **no
  fixture ever called it** (`list-basic` tests the standalone `list-eq?`
  function), so the breakage was invisible.

- **Fix** (`stdlib/list.tur`): rewrite `Eq [Cons]` to delegate to the
  carrier-based `list-eq?` with an element-comparison closure
  `(fn [a b] (eq? (:: a A) (:: b A)))` -- the same element-ascription pattern
  that makes `Eq [Vec]` work. The receiver stays an int64 carrier
  end-to-end (`(:: x :int)` is a relabel, no spill), no by-value Cons spec is
  minted, and no carrier-bridge fires. The carrier base now reads
  `return list_hyeq_qu(x, y, <elem-closure>);`.

- **Validation**: empty `main` crossings 2 -> 0; the previously-broken
  `(eq? cons cons)` compiles and returns correct element-wise results
  (`a==b` true, `a==c` false); **full suite 1637 passed, 0 failed**. 75
  `expected.c` snapshots regenerated in the same commit (the `Eq [Cons]`
  body is emitted into every program).

### What is left before `emit_carrier_bridge` can be deleted

The ~53-crossing long tail, none of which is `Eq [Cons]`:

1. **`Vec int` (27 carrier->concrete + 6 concrete->carrier)** -- abstract-
   element `Vec` dispatch and the `vec-*-byval` carrier bases (M5 Finding 2).
2. **`MutableMap` (4)** -- the multi-param instance whose unconstrained
   type-ctor param `K` records as `TY_STRUCT` not `TY_TYVAR` (documented
   blocker in `#364`'s message and the M5 docs).
3. **`concrete->carrier` dispatch-arg-bridge gap** -- a user-defined
   polymorphic fn specialized to a by-value concrete type (e.g.
   `poly-eq [A] [(Eq A)] (eq? x y)` with `A = (Cons int)` / `(Vec int)`)
   calls the carrier dict slot with by-value args and fails to compile.
   This reproduces identically for `Vec` and `Cons`, so it is a pre-existing
   general gap, **orthogonal to `Eq [Cons]`** and to the `carrier->concrete`
   accessor path M3 deletes. It is an M4/M5 (per-method dict ABI) item.
4. The handful of `Result Device` / `Option Device` / `SChan` / `TupleN`
   crossings sit in already-FAIL-prone or HKT/typeclass paths (audit §7).

Re-run the suite-wide `TUR_M3_AUDIT=1` audit after each of 1-3 lands; when
the count reaches 0, delete `emit_carrier_bridge` per the step-3 checklist
above and confirm the suite stays green.

## Update 2026-06-15 (post-#369 re-audit; by-value direction chosen; M3 -> M7 sequencing)

Re-ran the per-fixture audit on the tree with **M5 D.4 (this branch) + #369
(`Eq [Cons]` carrier rewrite)** both present, and settled the direction question
that the "what is left" list above left open. Recording the result so the next
engineer does not re-derive it.

### New baseline (methodology: `TUR_M3_AUDIT=1 tur emit-c <fixture>` per fixture, with and without `-Xdata-literals`, counting every probe line)

| | pre-#369 | post-#369 (now) |
|---|---|---|
| fixtures crossing | 1217 | **21** |
| total crossings | 2576 | **142** (141 carrier->concrete + 1 concrete->carrier) |

(The 142 vs #369's "~53" is a counting-scope difference: this sweep counts
every crossing in the 21 fixtures including the by-value-spec *call-boundary*
derefs and the `-Xdata-literals` variants, not just the carrier bases.)

**Every remaining crossing now has a CONCRETE element type** (no abstract
`tyvar`) -- the universal `Cons tyvar` crossings are gone. Distribution:

- **`Vec int` -- 114 (dominant).** Two sources, both from the **by-value**
  `Eq [Vec]` (`stdlib/vec.tur`, which uses `vec-len-byval`/`vec-eq-loop-byval`
  over `(:: x (Vec A))`):
  1. the `Eq [Vec]` carrier base deref'ing `(:: x (Vec A))` -> `*(Vec__int*)x`
     to feed the by-value `vec-*-byval` specs;
  2. call sites deref'ing carrier `vec-of` handles to feed the by-value
     `__inst_Eq_eq_qu_Vec__spec__Vec__int` directly.
- **long tail (~28):** `MutableMap` (4), `Result Device` (3), `Tuple3..8`,
  `Pair`, `Option Device`, `SChan`, `Result int cstr` -- HKT / dispatch-arg /
  FAIL-prone paths.

### The blocker is a genuine M5-vs-#369 direction conflict (confirmed empirically)

`Eq [Cons]` (#369) is **carrier-based**: body delegates to carrier `list-eq?`
with an element closure, receiver stays int64 (`(:: x :int)` is a relabel), no
by-value Cons spec, no bridge. `Eq [Vec]` (M5) is **by-value**: receiver becomes
`Vec__int`, helpers are the `*-byval` twins.

Prototyped rewriting `Eq [Vec]` carrier-based to match `Eq [Cons]`. It **reduces
crossings but breaks the build** (`incompatible type for argument 1 of
'vec_hyeq_qu'`): dispatching `Eq [Vec]` on a concrete `(Vec int)` mints a
by-value spec (receiver `Vec__int`), and the carrier-based body's `(:: x :int)`
then has to *widen* that struct back to the carrier -- **exactly the EX_ASCRIBE
`CK_CONCRETE -> CK_CARRIER` bridge that M5 D.4 deleted** (this report's sibling,
`m5-residual-straddle-retirement`). `Eq [Cons]` only works carrier-based because
Cons stays a carrier (no by-value spec is minted). So the two directions are
**incompatible**, and the carrier route is closed on this branch.

### Direction chosen: by-value (M7-aligned)

Keep the by-value collection direction (consistent with the plan's north star
and M5 D.4). Do **not** revert `Eq [Vec]` to carrier-based. The 142 crossings
clear when the carrier *producers* stop handing int64 to by-value consumers --
i.e. when collection construction/ops are monomorphized -- not by another
stdlib instance rewrite.

### Roadblocks for the by-value direction (record before starting)

1. **Mutable collections need reference semantics -> "by-value" means a TYPED
   POINTER, not a by-value struct.** `Vec`/`Map`/`Set`/`MutableMap` are
   heap-backed and mutated in place (`vec-push!` reallocs `data`, bumps
   `len`/`cap`). A literal by-value `Vec__int {int64_t* data; size_t len, cap;}`
   passed by value copies the header, so an in-place push on the copy is invisible
   to the caller -- a **silent mutation miscompile**. The correct monomorphic ABI
   for these is `Vec__int*` (a typed pointer): still fully clang-checked, kills
   the `*(Vec__int*)int64` bridge, and preserves identity/mutation. Only the
   genuinely-immutable value types (Option, Result, Pair, Tuple, immutable Cons)
   are by-value structs. **Settle this per-type distinction before editing
   stdlib.**
2. **The dispatch dict slot is still int64-uniform.** Confirmed
   `dict_Eq_Vec { bool (*eq_qu)(int64_t, int64_t); }`. Abstract dispatch (a
   generic `(defn all-eq [A] [(Eq A)] ...)` through the dict) therefore still
   forces the value through int64 regardless of a typed-pointer Vec. The M3
   report calls M4 "landed" but the dict-slot rework is **not** done for these
   instances; the lone remaining `concrete->carrier` crossing is this gap.
   Full deletion needs typed dict slots (or per-(instance,element) dict
   monomorphization) -- M4 dict-ABI work, not stdlib edits.
3. **Producers are int64-returning inline-C.** `vec-new`/`vec-of`/`vec-push!`/
   `vec-get`/`vec-len`/`vec-free` have hardcoded `int64_t` C signatures. The 114
   `Vec int` crossings are these producers feeding by-value consumers. Removing
   them requires converting every primitive to the typed-pointer signature per
   element type -- the **inline-C-body monomorphization** the M5 docs deferred as
   its own infra. Sequencing is therefore producers-first, bridge-deletion-last.
4. **The carrier cannot fully die; it relocates.** The plan keeps the carrier for
   existentials, `tur_poly_fn_t`, and the heterogeneous HAMT (`Vec @Any`, a vec
   inside a type-erased map). So Vec is not *uniformly* typed-pointer -- there is
   both a `Vec__int*` path and a carrier path, with a bridge at the pack/open
   boundary. **Realistic M3 goal: delete the bridge from the MONOMORPHIC paths;
   `emit_carrier_bridge` itself survives for the type-erased boundary.** The
   step-3 "delete `emit_carrier_bridge` outright" checklist above should be
   re-scoped accordingly.
5. **MutableMap multi-param** (`K` recorded `TY_STRUCT` not `TY_TYVAR`) -- partly
   addressed in #364 but the audit still shows 4 crossings; the multi-param path
   needs finishing.
6. **Snapshot blast radius.** A Vec ABI change regenerates a large fraction of
   the ~1640 `expected.c` snapshots -- one coordinated regen per the fixture
   STRICT RULE; coordinate timing with in-flight vec-touching branches.

### Sequencing (this is really starting M7 + M4 dict-ABI, with M3 as the payoff)

1. **Per-type ABI decision matrix** -- classify every parameterized stdlib type
   as by-value-struct (Option/Result/Pair/Tuple/Either/Slice), typed-pointer
   (Vec/MutableMap mutable; Map/Set/Cons/GVec immutable-but-heap-linked), or
   type-erased carrier (the opaque/HKT handles). **LOCKED 2026-06-15:**
   [docs/upcoming/parametric-type-abi-matrix.md](../upcoming/parametric-type-abi-matrix.md).
   Key correction this produced: Cons is typed-pointer, not by-value -- it is a
   linked node chain, so the receiver is `Cons__A *`, not a by-value cell.
2. **Vec typed-pointer vertical slice** -- convert the Vec primitives
   (`vec-new`/`-of`/`-push!`/`-get`/`-len`/`-free`/...) to the `Vec__int*` ABI
   per element type (inline-C-body monomorphization), and type the `Eq [Vec]`
   dict slot. Target: the 114 `Vec int` crossings drop to 0; suite green; one
   coordinated snapshot regen. Proves the pattern before Map/Set/MutableMap.
3. **Replicate for Map/Set/MutableMap** (incl. the MutableMap multi-param fix).
4. **M4 dict-ABI** -- typed dict slots / per-(instance,element) dict
   monomorphization so abstract dispatch stops forcing int64; clears the
   `concrete->carrier` dispatch-arg gap.
5. **Re-audit; delete the bridge from monomorphic paths** -- when the audit shows
   only the existential/`@Any`/`tur_poly_fn_t` carrier boundary remaining, scope
   `emit_carrier_bridge` down to that boundary (do not delete the function
   wholesale; see roadblock 4) and confirm the suite stays green.

### Status

Direction settled (by-value). No code changed in this update -- the deliverable
is the corrected baseline, the confirmed incompatibility, and the sequencing.
The tractable first increment is step 2 (the Vec typed-pointer vertical slice).

## Related

- [docs/upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md)
  §M3 / §M4 — the plan this report refines.
- [docs/reported/m2b-stdlib-migration-blocked-on-carrier-fallback.md](m2b-stdlib-migration-blocked-on-carrier-fallback.md)
  — the M2b finding that pointed at M4 as the deeper unblocker.
- `src/compiler/emit_core.c` `emit_carrier_bridge` — the bridge function
  (now carrying a permanent audit hook).
