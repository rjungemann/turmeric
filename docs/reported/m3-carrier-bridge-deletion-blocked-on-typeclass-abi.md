---
title: Plan M3 ("delete `emit_carrier_bridge`") is gated on M4, not just M2
category: Codegen / ABI — monomorphization plan refinement
severity: Low. M3 is presented in `docs/upcoming/end-to-end-monomorphization-plan.md` as a 1-2-session phase to retire the carrier-bridge machinery after M2 lands. Empirical audit (this session) shows that claim was over-optimistic: even with the full M2b stdlib migration in tree, the bridge is still load-bearing for typeclass-method-dispatch call sites. The actual deletion needs M4 (per-method typeclass ABI) first.
description: I shipped instrumentation (`TUR_M3_AUDIT=1`) inside `emit_carrier_bridge` so the per-call-site cost of removing it is measurable. Running the full suite under the audit shows only 2 crossings (and both inside already-FAILing pre-existing fixtures), which initially looked like the bridge was dead. Attempting the M3 deletion, the suite regressed by ~6 fixtures with cc errors of the form "passing 'int64_t' to parameter of incompatible type 'Result__int__cstr'". Direct per-fixture re-audit then showed the bridge IS firing for those — `tests/run.sh` was swallowing per-fixture stderr in the snapshot/test phase, so the suite-wide audit count was misleadingly low. The bridge's remaining role is `carrier→concrete` at `EX_ASCRIBE` (when a typeclass-method return like `(:: (decode …) (Result int cstr))` produces an int64 carrier but the ascription's outer context wants the by-value struct) and `concrete→carrier` at the typeclass-instance dispatch arg-cast site (when a by-value `Tuple2__int__int` is passed to a dict-dispatched `__inst_Eq_eq_qu_Tuple2(int64_t, int64_t)`).
status: BRIDGE DOWN-SCOPE COMPLETE for the non-HKT collection-Eq cascade -- 2026-06-17 (see "Update 2026-06-17 (post-#400 audit floor)" below). The audit floor is **34 crossings / 10 fixtures** with **zero monomorphic deref-copy crossings**: 22 are fat-closure comparator `:heap` reinterpret casts (bucket A'), 10 are blessed inline-C `tur_ok`/`tur_some` construction (bucket C), 2 are the type-erased SChan path (bucket E) -- all three are the by-design boundaries `emit_carrier_bridge` is kept for. The original "delete the bridge wholesale" goal is superseded; the realistic re-scoped goal (delete it from the monomorphic paths, keep it for the type-erased/cast/blessed boundary) is met. The remaining 22 clear only via fat-closure-element monomorphization (a large M5/M7-adjacent ABI change), NOT M4 dict-slot typing. EARLIER STATUS (kept for history): MOSTLY UNBLOCKED -- 2026-06-15 (see "Resolution 2026-06-15" below). On the current merged tree (`origin/main`, with M5 Option C `#364` and the M5 emit fixes landed) the bridge fired for **1188 fixtures / 2429 crossings**, but **2376 of those (98%) were a single pattern**: the `Eq [Cons]` carrier base (`stdlib/list.tur`) recursing through `(:: t1 (Cons A))` with `A` abstract, emitted into every program that links stdlib. That instance was also **silently miscompiled** (invoking `(eq? cons cons)` via spec or abstract dispatch produced hard cc errors) and **never tested** -- `list-basic` exercises the separate `list-eq?` function, not the instance. **Fixed this session** by rewriting `Eq [Cons]` to delegate to carrier-based `list-eq?` with an element-comparison closure (the proven `Eq [Vec]` pattern): empty `main` crossings 2 -> 0, the instance now compiles + returns correct results, full suite 1637/0. Remaining: a **~53-crossing long tail** (abstract-element `Vec`, multi-param `MutableMap`, and the pre-existing `concrete->carrier` dispatch-arg-bridge gap that fails identically for `Vec` and `Cons`). The bridge cannot be deleted outright until those clear -- M4/M5 work -- but M3 is no longer dominated by `Eq [Cons]`.
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
   [docs/parametric-type-abi-matrix.md](../parametric-type-abi-matrix.md).
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

## Update 2026-06-15 (Vec vertical-slice execution plan written)

Step 2 now has a grounded execution plan:
[docs/upcoming/v2/vec-typed-pointer-vertical-slice-plan.md](../upcoming/v2/vec-typed-pointer-vertical-slice-plan.md).
Baseline re-confirmed green (`bash tests/run.sh`: **1639 passed, 0 failed**).

The plan settles the mechanism question the sequencing left open ("inline-C-body
monomorphization the M5 docs deferred as its own infra"). Inspecting the emitted
C for `vec-eq-ascribed` produced the load-bearing insight: the Vec element buffer
(`data`) stays `int64`-carried for **every** element type -- `Vec__int`,
`Vec__cstr`, `Vec__bool` differ only in the *type label on the 24-byte header
pointer*, the memory is byte-identical. So "typed pointer" types the **header**,
not the elements, and the producers' inline-C bodies are identical across `A`
except for the struct type *name*. That collapses "inline-C-body
monomorphization" from a general C-rewrite problem to a single primitive
(`heap-box [A] [v : A] : ptr<A>`) whose body substitutes one concrete C name --
auditable token replacement, not a C parser.

It also re-confirms (from the emitted C) that today's `Eq [Vec]` passes
`Vec__int` **by value** (24-byte header copy via `*(Vec__int *)(intptr_t)(h)`),
which is correct-by-luck only because Eq is read-only -- a CLAUDE.md "works by
luck" latent miscompile the moment a by-value Vec reaches a mutator. The
typed-pointer ABI (`Vec__A *`) fixes that and converts the ~114 `Vec int`
deref-copy crossings to (eventually zero) casts.

Next implementation increment: plan sub-step 1 (the inert `#{Heap}`/`:heap`
struct attribute + pointer lowering, gated so the suite stays byte-identical
until a type is tagged).

## Update 2026-06-15 (post-#377 audit refresh; Set + MutableMap `:heap` landed)

### Refreshed audit baseline (Vec slice #377 is now merged)

The "142 crossings / 21 fixtures" and "~53 long tail" figures above predate the
Vec slice (#377) merging. Re-ran the per-fixture sweep
(`TUR_M3_AUDIT=1 tur emit-c <fixture>`, stderr captured directly) on the current
tree. **Accurate post-#377 baseline: 14 fixtures, 48 crossings.** Distribution:

| Type | crossings | nature |
|---|---|---|
| `Vec int` | 28 | now reinterpret CASTS (post-#377 `:heap`), concentrated in the M5 constrained-poly / instance-spec residual (`m5-constrained-poly-vec-eq` 12, `m5-instance-spec-constraint-var` 8) -- the M4 dict-ABI residual |
| `Result`/`Option`/`Pair`/`Tuple3..8` | 14 | by-value-struct types at the *typeclass-dispatch* boundary (the dict slot is still `int64_t (*)(...)`) -- the **M4 dict-ABI** item, not a per-type-rep issue |
| `MutableMap int int` | 4 | dispatch-arg crossings via `(:: a (MutableMap int int))` -> `.eq?` |
| `SChan` | 2 | type-erased channel path (FAIL-prone) |

The dominant `Cons tyvar` crossings (2376 of the old 2429) are long gone -- they
were retired by the #369 `Eq [Cons]` rewrite.

### Set + MutableMap migrated to `:heap` (matrix step 3, this change)

Tagged `Set` and `MutableMap` `:heap` (the same flip #377 did for `Vec`). The Vec
slice already landed all the enabling compiler support (`:heap` lowering,
`emit_carrier_bridge` reinterpret-cast for heap types, `type_struct_pass_by_ptr`
skip, ABI-aware field access), so both types are a one-line `defstruct` flip --
no producer retyping, no compiler change. Both keep their existing inline-C
carrier-base producers (which take/return the int64 carrier = the header pointer,
exactly like Vec).

- **MutableMap**: the load-bearing fix. `mutmap-set!` reallocs `storage` and
  writes back `m->storage` (`stdlib/mutmap.tur:139`), so the pre-`:heap` by-value
  `MutableMap__K__V` header copy at the dispatch boundary held a **stale storage
  pointer after a resize** -- a CLAUDE.md "works by luck" hazard that was benign
  only because `Eq`'s dispatch is read-only. `:heap` makes `(MutableMap K V)` a
  shared `MutableMap__K__V *`, so the 4 `MutableMap int int` deref-copy crossings
  become reinterpret casts and the staleness hazard is gone.
- **Set**: matrix conformance. `Set` has a single immutable HAMT pointer field, so
  the by-value copy was harmless, but `:heap` brings it in line with the matrix and
  is prerequisite to the eventual `*-byval` twin retirement / bridge down-scoping.

Validation: zero snapshot drift (neither type's C name appears in any
`expected.c`); full compiled suite green; spice roundtrip
(`../turmeric-spices/spices/{ecs,json,frame}`) shows only the documented
pre-existing failures (ecs `poly-call-row`/`query-typed`/`sized-dense-rt`; json
`derive-{decode,encode}-struct`; frame `group_test`/`interop_test`/`reshape_test`
ld errors -- all identical with the change reverted).

`Map` migrated too (same commit): its producers were already typed `(Map K V)`
and its `(carrier :int)` field is internal-only (never accessed structurally; the
real C struct is `{void* hamt}`), so the `:heap` flip needs no field retyping --
`(Map K V)` -> `Map__K__V *`. Suite green (1646/0), zero snapshot drift, spice
roundtrip clean (frame is the heavy Map user). Map is immutable/persistent so the
by-value copy was harmless; the flip is matrix conformance + prerequisite to the
eventual bridge down-scoping.

`Cons` migrated too (subsequent commit): tagged `:heap`, so `(Cons A)` ->
`Cons__A *`. This is the pervasive/risky one (#369 deliberately left it
carrier-based), so it was validated harder: compiled suite 1646/0, zero snapshot
drift, spice roundtrip clean, **interpreter gate 1205 passed / 2 failed** (the 2
are the documented pre-existing `eq-carrier-capturing-comparator` / `mutmap-eq`).
The #369 carrier-based `Eq [Cons]` keeps working: its body's `(:: x :int)`
relabels the `Cons__A *` handle to the int64 carrier (a cast, not a by-value
spill), so no by-value Cons spec is minted -- the typed-pointer direction is
compatible with the carrier-based instance, unlike the by-value-*struct* direction
that #369 found "closed." The `(tail :int)` field stays an internal next-cell link
(analogous to Map's `carrier :int`); fully typing it `(Cons A)` is a further
No-Lazy-`:int` step, not required for the `:heap` flip. NOTE: run-turi.sh
correctly SKIPS user-inline-C fixtures (e.g. the `variadic-*` set, which build
their `& rest` cons lists with inline-C), so those are not an interpreter gate for
this change -- they pass identically in compiled mode.

Remaining typed-pointer migrations (matrix step 3 / 4): `GVec` (niche GADT demo).
The 48 crossings clear when **M4 dict-ABI** (typed dict slots) lands -- that is the
dominant remaining item, not further stdlib `:heap` flips.

## Update 2026-06-15 (full-tree audit refresh + crossing-by-ROOT-CAUSE categorization; "M4 dict-ABI is the dominant item" is WRONG)

Re-ran the per-fixture sweep on the current branch head (suite **1647 passed,
0 failed**), this time **using each fixture's own `flags` file** (so the
`-Xdata-literals` variants that the "48" sweep dropped are counted) and
**classifying every crossing by the OUTERMOST type constructor and direction**.
Methodology, reproducible:

```sh
for dir in tests/fixtures/*/; do
  input="$dir/input.tur"; [ -f "$input" ] || input="$dir/$(basename "$dir").tur"
  flags=""; [ -f "$dir/flags" ] && flags=$(cat "$dir/flags")
  TUR_M3_AUDIT=1 ./build/tur $flags emit-c "$input" 2>&1 >/dev/null | grep '\[m3-audit\]'
done
```

**Accurate current baseline: 22 fixtures, 140 crossings.** (Higher than the
"48" line above only because that sweep omitted `-Xdata-literals` and the
`m5-*` constrained-poly fixtures; the underlying tree is the same.)

### The crossings, bucketed by genuine root cause

| Bucket | count | direction | root cause | verdict |
|---|---|---|---|---|
| **A. `:heap` reinterpret casts** (`Vec`/`Map`/`Set`/`MutableMap`/`Cons`) | **124** | carrier->concrete | int64-carrier producers (`vec-of`, `mutmap-new`, ...) feed typed-pointer consumers; post-#377 these are `(Vec__int *)(intptr_t)h` **casts**, not 24-byte deref-copies | benign; clears only when collection PRODUCERS monomorphize (deferred inline-C-body infra) -- **a typed dict slot does NOT touch these** |
| **B. pbp-pointer derefs** (`Tuple3`..`Tuple8`) | **7** | carrier->concrete | structs >16B pass by `const T*`; an ABI-spec callee takes them by value, so the caller derefs the pbp pointer (`tuplen-struct-param-passing`) | legitimate pass-by-pointer convention; **not a carrier crossing** -- the bridge is reached only by an over-match (see (a) below) |
| **C. blessed inline-C construction** (`Result`/`Option`) | **6** | carrier->concrete | a user/instance **inline-C body** builds the value via `tur_ok`/`tur_err`/`tur_some` (returns the int64 carrier) but its declared return is a by-value struct, so the call site derefs once (`inline-c-typed-result-option` 5, `typeclass-method-parameterized-result-decode` 1) | this is the **deliberately blessed** "inline-C may return a real typed Result/Option" boundary (`inline-c-typed-result-option` exists to bless it); inherently carrier -- a typed dict slot does NOT touch it either |
| **D. inline-C carrier-instance dispatch-arg spill** (`Pair int int`) | **1 -> 0 (RESOLVED)** | concrete->carrier | was a by-value `Pair__int__int` passed into the `Serializable [Pair]` `serialize` instance method, whose body was inline-C reading its arg as a carrier pointer (`serial-composite-instances`) | **RESOLVED 2026-06-15** by rewriting the `serialize` body to pure-Turmeric (`(serial-pair-bytes (:: (.fst x) :int) (:: (.snd x) :int))`) so M4c Path A mints `serialize_Pair__spec__(Pair__int__int)` -- direct typed dispatch, no spill. Byte layout unchanged; `deserialize` untouched. See the resolution note below |
| **E. type-erased channel** (`SChan<SRecv ...>`) | **2** | carrier->concrete | `generic-relay-aggregate-result` -- the genuinely type-erased channel path | carrier by design (matrix roadblock 4); never deleted |

### The correction

The "Update 2026-06-15 (post-#377 ...)" verdict above -- *"The 48 crossings
clear when M4 dict-ABI (typed dict slots) lands -- that is the dominant
remaining item"* -- **does not survive the by-root-cause breakdown.** A typed
dict slot rewrites the dict's `bool (*)(int64_t, int64_t)` to
`bool (*)(Pair__int__int, Pair__int__int)` so abstract dispatch stops
spilling. That clears bucket **D only: 1 crossing.** It does **nothing** for
the 124 `:heap` casts (bucket A -- those are producer-side), the 7 pbp derefs
(B -- a different convention), the 6 inline-C constructions (C -- the blessed
carrier boundary), or the 2 SChan (E -- type-erased by design).

Why the earlier sweep over-credited M4: M4c Path A (already merged) mints typed
per-instance **specs** and rewrites every **Turmeric-level direct dispatch** to
call them (`find_matched_abi_spec`, `emit_expr.c:2430`). So the
deref-copy-through-a-carrier-dict hot path the original report worried about
(`emit-abi-trace` Tuple2: 2 -> **0** crossings now) is *already gone*. What
M4c left on the table is exactly bucket D: the case where a by-value struct is
passed into a dict slot that is still consumed as a runtime value (a `(Pair A B)`
into the uniform `int64` slot, not specialized away). That is a single
dispatch-arg spill, not a dominant deref-copy class.

### What this means for deleting / down-scoping `emit_carrier_bridge`

The real remaining carrier **source** is the producer boundary, not the dict:

1. **Collection producers (bucket A, 124).** `vec-of`/`mutmap-set!`/etc. are
   inline-C carrier bases returning int64 (kept that way on purpose -- see the
   Vec slice's "stdlib flip is inline-C, not pure-Turmeric" section: interpreter
   parity + float-element reads). Until these return `Vec__A *` per element type
   (the deferred inline-C-body monomorphization), their results stay int64 and
   the `:heap` cast fires. **These casts are free and correct**, so the pragmatic
   end state is to *accept* them: the matrix's roadblock 4 already says the
   bridge survives for the type-erased boundary; bucket A is the same shape (a
   cast, never a copy).
2. **Inline-C `tur_ok`/`tur_some` (bucket C, 6).** Blessed and intentional.
   Down-scoping the bridge must *keep* this path.
3. **M4 typed dict slots (bucket D, 1).** Worth doing for ABI cleanliness and to
   close the `concrete->carrier` spill, but it is a **1-crossing win**, not the
   dominant item. Scope it accordingly; do not gate the bridge down-scope on it.

**Revised sequencing for the bridge down-scope** (supersedes the "48 crossings
clear when M4 dict-ABI lands" framing): the bridge cannot be *deleted* (buckets
A/C/E are by-design carrier crossings that must keep working), so the realistic
goal stays "down-scope `emit_carrier_bridge` to the casts/blessed-construction/
type-erased boundary." Concretely:

- (a) **CONFIRMED this session:** bucket B (pbp deref) *does* reach
  `emit_carrier_bridge`, but only by an **over-match**, not because it is a
  carrier crossing. `emit_expr.c:2633-2643` is the carrier-bridge `if`; its
  guard fires when `matched_spec && aggregate-arg && (TY_INT || (aggregate &&
  type_uses_carrier_abi && !byvalue))`. A pbp `const Tuple3*` param satisfies
  the aggregate-carrier branch, so the bridge runs and emits
  `(*(Tuple3__int__int__int *)(intptr_t)(t))` -- the deref the dedicated pbp
  `else if` at `emit_expr.c:2665-2678` was written to emit as the cleaner
  `(*(t))`. Because the bridge `if` precedes the pbp `else if`, the pbp branch
  never runs for these args. The two outputs are **semantically identical**
  (both deref the pointer; the `(intptr_t)` round-trip is well-defined and the
  cast is redundant), so this is a correctness-neutral *audit-clarity* + minor
  codegen-cleanliness issue, **not a bug**. Tightening it -- gate the 2633 `if`
  with `!expr_is_pbp_param(ctx, emit_arg)` so a genuine pbp param falls through
  to the dedicated deref -- is a low-risk increment, but it changes the emitted
  C for the `Tuple3..8` pbp call sites and so needs a coordinated snapshot
  regen; left for a regen-window change rather than rushed here. Net: the audit
  over-counts by 7 (these are pbp derefs, not carrier crossings).
  **RESOLVED 2026-06-15 (this session):** landed exactly the suggested gate --
  `!expr_is_pbp_param(ctx, emit_arg)` added to the carrier-bridge `if` at
  `emit_expr.c` (the `(... matched_spec ... aggregate-carrier ...)` guard). A
  genuine pbp param now skips the bridge and falls through to the dedicated pbp
  `else if`, which emits `(*(t))` instead of the redundant
  `(*(Tuple3__int__int__int *)(intptr_t)(t))` cast-round-trip. `expr_is_pbp_param`
  is side-effect-free, so the gate adds no spurious struct-app registration.
  Net effect: **bucket B is gone** -- `tuplen-struct-param-passing` (6) and
  `tuple-type-bracket-sugar` (1) drop to 0 crossings; the audit baseline falls
  from **22 fixtures / 140 crossings to 20 fixtures / 133 crossings**. The
  emitted C for the `Tuple3..8` matched-spec pbp call sites changed
  (`(*(T *)(intptr_t)(t))` -> `(*(t))`), but **no `expected.c` snapshot
  referenced that pattern** (neither changed fixture has a snapshot), so the
  feared "coordinated snapshot regen" turned out to be zero drift. Full suite
  **1649 passed, 0 failed**.
- (b) ~~Land M4 typed dict slots for the bucket-D dispatch-arg spill.~~
  **CORRECTED 2026-06-15 (next-session investigation): bucket D is NOT a
  typed-dict-slot win.** The crossing is `(.serialize p)` where `p :
  Pair__int__int` (by-value) dispatches to the `Serializable [Pair]` instance
  method `__inst_Serializable_serialize_Pair`, emitted as a **direct call** (not
  through the dict singleton -- `fn_binding` is set, M4c Path A's `abi_bindings`
  ARE populated `a -> (Pair int int)`). No `__spec__` is minted because the
  instance body is **inline-C** (`struct {int64_t fst; int64_t snd;} *p =
  (void*)(intptr_t)x; ...`): it reinterprets its arg as a carrier *pointer*, so
  a by-value spec would make `(intptr_t)x` a hard type error. The spec gate at
  `emit_module.c:1455-1461` only mints a by-value spec for a concrete
  `TY_STRUCT` arg, and even the M5 `#{ByVal}` TY_APP extension forces *by value*
  -- which this body cannot accept. Contrast `Eq [Tuple2]` (pure-Turmeric body
  `(and (eq? (.e1 x) ...) ...)`) which DOES re-elaborate under typed params and
  mints `__inst_Eq_eq_qu_Tuple2__spec__...(Tuple2__int__int, ...)`, eliminating
  its crossing. So the discriminator is **inline-C-vs-Turmeric instance body**,
  not dict-slot typing. A typed dict slot changes the indirect-dispatch path
  (dict consumed as a runtime value), which this fixture does not exercise.
  **Net: there are ZERO crossings in the current audit that a typed dict slot
  alone fixes.** The genuine fixes for bucket D are: (i) rewrite
  `Serializable [Pair]`/`[Option]` to **pure-Turmeric recursive bodies** that
  dispatch `serialize` on `.fst`/`.snd` via their declared-but-currently-unused
  `[(Serializable A) (Serializable B)]` constraints -- this both eliminates the
  crossing AND fixes a latent miscompile (the inline-C body writes raw int64
  `fst`/`snd`, so it is "correct for int-carrier elements" only and silently
  serializes a dangling *pointer* for by-value-struct elements like
  `Pair (Pair int int) cstr`; the wire format changes, so the fixture's
  `rd-pair` raw-layout reader must change too); OR (ii) new "by-pointer spec for
  inline-C carrier instance bodies" infra (pass `Pair__int__int *`, leave the
  body's `(intptr_t)x` working) -- larger codegen change + coordinated regen.
- (c) Producer monomorphization (bucket A) and inline-C-body monomorphization
  (bucket C) remain the genuinely large, deferred items -- and per the Vec
  slice's findings they may stay carrier-based for interpreter-parity reasons,
  in which case bucket A's casts are the *permanent* shape, not a defect.

No code changed in this update -- the deliverable is the corrected baseline and
the root-cause categorization that re-scopes "M4 dict-ABI" from "the dominant
remaining item" down to "a 1-crossing cleanup," and re-points the real
remaining carrier weight at the producer boundary.

## Resolution 2026-06-15 (bucket D cleared: pure-Turmeric Pair serialize; baseline 140 -> 139)

Landed the bucket-D fix via the route that avoids the dead ends found above
(no new infra, no wire-format change). The single `concrete->carrier`
`Pair int int` crossing in `serial-composite-instances` is gone; the audit
baseline is now **21 fixtures / 139 crossings** (buckets A/B/C/E unchanged).

- **`Serializable [Pair]` `serialize` rewritten to pure-Turmeric**
  (`stdlib/serial.tur`): `(serial-pair-bytes (:: (.fst x) :int) (:: (.snd x)
  :int))`, where `serial-pair-bytes [fst : int snd : int] : ptr<void>` is a new
  fixed-arity inline-C helper emitting the **identical** `[len=16][fst][snd]`
  raw layout. Because the instance body is now Turmeric (not inline-C), M4c Path
  A mints `__inst_Serializable_serialize_Pair__spec__(Pair__int__int)` and the
  `(.serialize p)` dispatch calls it directly with the by-value Pair -- no
  carrier spill. `deserialize` and the fixture's `rd-pair` reader are untouched
  (byte layout preserved), so this is **not** option (i)'s wire-format-changing
  recursive rewrite, and **not** option (ii)'s new by-pointer-spec infra -- a
  third, smaller route the corrected analysis surfaced.

- **`Serializable [Option]` `serialize` deliberately LEFT inline-C.** A
  symmetric rewrite was attempted and **reverted** because it segfaults: `none`
  is the NULL carrier (`0`), so a pure-Turmeric body mints a by-value
  `serialize_Option__spec__(Option__int)` whose call-site dispatch derefs the
  carrier (`*(Option__int *)(intptr_t)(0)`) *before the body runs*, crashing on
  the none case. The deref is emitted by the bridge at the call site, so no
  body-level NULL guard can save it; the inline-C body NULL-checks `o` and is
  safe. Option therefore stays uniform-carrier (it never crossed the bridge --
  it has no by-value spec). A real fix needs Option to stop representing none as
  NULL (a non-NULL by-value/tagged repr) -- out of scope here. A `;;` NOTE on
  the instance records this so it is not "cleaned up" into a segfault later.

- **deserialize recursion is genuinely blocked (separate finding).** While
  scoping this I confirmed empirically that `(:: (deserialize b) A)` inside a
  `(Serializable A)`-constrained fn **silently mis-dispatches** to
  `__inst_Serializable_deserialize_ptr_void` instead of A's instance (probe:
  `(round 42)` over `(deserialize (serialize x))` returned via the ptr_void
  instance and errored at runtime, not a compile error). That is why the
  format-preserving route (above) is the only correct one: a recursive
  format would need recursive `deserialize`, which the language cannot dispatch
  on a return-type type variable. Filed as
  [docs/archive/return-dispatch-tyvar-silent-misdispatch.md](../archive/return-dispatch-tyvar-silent-misdispatch.md)
  (RESOLVED 2026-06-15 -- both directions landed; `deserialize` now dispatches
  on a constrained type var).

- **Validation:** `serial-composite-instances` -> 0 crossings, output unchanged
  (`pair=7035` / `opt-some=99` / `opt-none=-1`). Compiled suite **1647 passed, 0
  failed**; interpreter gate **1206 passed, 2 failed** (the 2 are the documented
  pre-existing `eq-carrier-capturing-comparator` / `mutmap-eq`). No `expected.c`
  snapshots reference the serialize symbols (the serial fixtures carry none), so
  no regen was needed. No spice uses `Serializable [Pair]` (the only changed
  instance) -- the sibling `spices/{httpd,json}` "serialize" hits are
  spice-local `serialize-response`, not the stdlib instance.

## Update 2026-06-16 (bucket A, Vec producer monomorphization: baseline 133 -> 93)

Executed the producer-side of bucket A for **Vec** (the vec-typed-pointer plan's
deferred "inline-C-body monomorphization"): the inline-C Vec producers/accessors
(`vec-new`/`-push!`/`-get`/`-len`/`-set!`/`-pop!`/`-free`) now get **typed
per-element ABI specs** so their results bind typed-pointer locals
(`Vec__int * a = vec_new__spec__Vec__int()`) and their `:heap` params take
`Vec__int *` directly. Downstream typed consumers (the `Eq[Vec]` spec, nested
`vec-push!`, the `.eq?` dispatch) then receive the typed pointer with **no
`(Vec__int *)(intptr_t)h` reinterpret cast** -- the bucket-A crossing is gone for
every producer->typed-consumer chain.

### Mechanism (return/param-specialize the inline-C base; the carrier base is untouched)

- **Spec-mint gate** (`emit_module.c`): an inline-C producer/accessor whose
  DECLARED signature carries a structural `(Vec _)` slot gets a typed spec at a
  concrete call (keyed on `fd->param_types[i]` / `fd->return_type`, NOT the
  resolved arg -- otherwise the generic `some`/`ok` constructors, whose `A`
  element merely *resolved* to a Vec, were wrongly specialized and lowered
  Option/Result to a by-value struct passed into the int64 dict slot).
- **Float/cstr safety** (`emit_module.c`): in a Vec-producer spec, ONLY the
  `(Vec _)` slots are typed; every element/scalar slot is forced back to the
  int64 carrier (`TYPE_INT`). The inline-C bodies read carrier values with a
  *bit reinterpret* (`return (int64_t)vec->data[i];`), which a monomorphized
  `double`/`const char *` slot would turn into a numeric conversion -- the
  `0.5 -> 0` and `const char *`-from-`int64` miscompiles the first (un-forced)
  cut produced on `tce3-map-cstr-val`.
- **`__TUR_RET__` template** (`emit_core.c`): `vec-new`'s body returns
  `(__TUR_RET__)(intptr_t)v` -- `int64_t` for the carrier base (byte-identical to
  the old `(int64_t)` cast, so the base and all snapshots are unchanged), the
  typed pointer (`Vec__int *`) for the spec. The signature side mirrors it
  (`emit_fns.c` `typed_heap_spec`: an inline-C `:heap` result under an active
  spec lowers to the typed pointer; the forward decl already used
  `type_c_name(spec->result_type)`).
- **Concrete->carrier bridge** (`emit_expr.c`): where a typed `Vec__int *` value
  reaches an int64 carrier-base consumer (the carrier base, or a generic
  `A`-element sink like `some`/`vec-push!`-base, or a forced element slot), the
  arg is wrapped `(int64_t)(intptr_t)(...)` -- the symmetric counterpart to the
  carrier->concrete bridge. Without it, the typed pointer hit the int64 param as
  an implicit `-Wint-conversion` (vec/set) or a hard cc error (Option/Result).

### Scope (Vec only, by design)

The gate is scoped to the `Vec` struct constructor (`type_is_heap_vec`):
Map/Set/MutableMap/Cons are the plan's later steps and have the additional
cstr/float-value monomorphization edges that surfaced here, so they stay on the
carrier base this increment. The carrier base (`vec_hynew`/`vec_hylen`/... with
int64 signatures) is kept for abstract dispatch, the interpreter (the
`native_vec_*` overrides still fire -- the bodies stay inline-C), and the
type-erased boundary.

### What the audit shows now

`TUR_M3_AUDIT=1` per-fixture sweep: **15 fixtures, 93 crossings** (down from
20 / 133 after the bucket-B fix). The 40 removed crossings are the
`carrier->concrete` derefs at typed Vec consumers that the producer specs made
unnecessary. The residual in the typed-Vec-Eq fixtures (`vec-of-tvec-eq` 12,
`option-of-tvec-eq` 12, ...) is **root 2** -- the uniform-int64 `Eq[Vec]` dict
slot / synthesized comparator-thunk carrier (M4 dict-ABI), which the producer
slice does not touch.

### Validation

- Compiled suite **1649 passed, 0 failed**, **zero `expected.c` drift** (the
  base `vec_hynew` body is byte-identical; the new `*__spec__Vec__*` clones only
  appear in typed-Eq-on-Vec fixtures that carry no snapshot).
- Interpreter gate **1206 passed, 2 failed** (the documented pre-existing
  `eq-carrier-capturing-comparator` / `mutmap-eq`).
- Spice roundtrip: `../turmeric-spices/spices/{ecs,json}` -- ecs **22/25** (the
  documented `poly-call-row`/`query-typed`/`sized-dense-rt`), json **5/6** (the
  documented `Decode bool` gap). No new failures; ecs is the heavy Vec user.

### Remaining for bucket A

- Replicate the producer slice for **Map/Set/MutableMap/Cons** (each needs the
  cstr/float-value-slot carrier-forcing this Vec slice proved out).
- **Root 2 (M4 dict-ABI):** typed `Eq[Vec]` dict slots / per-(instance,element)
  dict monomorphization to retire the carrier-base + comparator-thunk casts that
  dominate the residual 93.

## Update 2026-06-16 (root 2: `Eq [Vec]` retired to the carrier-based shape; 98 -> 70)

Re-ran the full per-fixture sweep on the current branch head (suite **1653
passed, 0 failed**). Baseline was **17 fixtures / 98 crossings**; after this
change it is **17 fixtures / 70 crossings**, and the dominant `Vec int`
`carrier->concrete` *deref* crossings (root 2 -- the `Eq [Vec]` carrier base
bridging to the `*-byval` typed specs) are gone, replaced by the clean
carrier-based delegation plus a handful of cheap `concrete->carrier` heap
*casts*.

### What changed and why it is now possible

`Eq [Vec]` was the last **by-value-direction** collection instance: its body
called `vec-len-byval` / `vec-eq-loop-byval` over `(:: x (Vec A))`, so a
concrete dispatch minted a `Vec__int *`-param spec whose carrier base
(`bool __inst_Eq_eq_qu_Vec(int64_t, int64_t)`, referenced by the dict
singleton) had to bridge `(Vec__int *)(intptr_t)(x)` back to the byval specs --
4-6 `carrier->concrete` crossings emitted into *every* program that links
stdlib.

The "Update 2026-06-15 (post-#369 ...)" entry above found the carrier-based
rewrite "breaks the build" -- but that predated #377, when `(Vec A)` was a
**by-value `Vec__int`** and `(:: x :int)` was a struct *widening* (the
`CK_CONCRETE -> CK_CARRIER` EX_ASCRIBE bridge M5 D.4 deleted). Post-#377 `Vec`
is `:heap`, so `(Vec A)` is a typed pointer `Vec__A *` and `(:: x :int)` is a
pure pointer->int *cast*. That is exactly why `Eq [Cons]` (#369) works
carrier-based, and the same is now true for Vec. Rewrote `Eq [Vec]`
(`stdlib/vec.tur`) to mirror `Eq [Cons]`:

```turmeric
(definstance Eq [Vec]
  [(Eq A)]
  (eq? [x y]
    (vec-eq? (:: x :int) (:: y :int)
             (fn [a b] (eq? (:: a A) (:: b A))))))
```

The carrier base now delegates to the carrier-based `vec-eq?` with an
element-comparison closure -- byte-for-byte the `Eq [Cons]` shape -- and mints
no by-value spec carrier base to bridge.

### Enabling compiler support (`src/compiler/emit_expr.c`)

The carrier-based body still mints a typed `__inst_Eq_eq_qu_Vec__spec__(Vec__int
*, ...)` for concrete direct dispatch (M4c Path A), whose body calls the int64
`vec-eq?` via `(:: x :int)`. That ascription needs an explicit pointer->int64
relabel; without it the typed pointer reached the carrier int64 param as a
`-Wint-conversion`. Two additions, both gated to a CONCRETE `:heap` layout so
the carrier base (abstract `(Vec A)` = int64) is untouched (no snapshot drift,
no redundant cast):

- `emit_var_spec_arg_type` helper -- resolves a spec-param var's concrete
  monomorphized type from `current_abi_specialization->arg_types[]` (the
  Path A `.field`-access pattern), because `emit_resolve_type` leaves a
  parametric receiver `(Vec A)` abstract.
- EX_ASCRIBE `(:: x :int)` emit + the call-arg `preserve_ascribe_for_bridge`
  gate now route a concrete-heap-pointer-to-`:int` ascription through
  `emit_carrier_bridge(CK_CONCRETE, CK_CARRIER)` (which already emits the clean
  `(int64_t)(intptr_t)(...)` heap cast).

### Cleanup (plan step 5, now unblocked for Vec)

`vec-len-byval` and `vec-eq-loop-byval` became unreachable (only self-
referential) and were deleted from `stdlib/vec.tur`. `vec-get-byval` /
`vec-data-get__` stay -- they are the shared Option C redirect mechanism still
used by Map/Set-bound fixtures.

### Validation

- Compiled suite **1653 passed, 0 failed**; 77 `expected.c` snapshots
  regenerated in the same change (the `Eq [Vec]` carrier base is emitted into
  every program -- the diff is uniformly "byval-spec calls -> `vec_hyeq_qu` +
  comparator closure," identical in shape to the existing `Eq [Cons]` base).
- Interpreter gate **1209 passed, 2 failed** (the documented pre-existing
  `eq-carrier-capturing-comparator` / `mutmap-eq`).
- Spice roundtrip: json **6/6**; ecs **22/30** -- the 8 ecs failures
  (`poly-call-row`, `query-typed`, `sized-{dense,sparse,tag,world}-rt`,
  `sized-world-spawn`, `sized-zip-cross-shape`) are confirmed **identical on
  the pre-change baseline** (HKT-row gaps + the in-progress `(Static N)`
  sized-world type form); the heavy-Vec users (spawn1k, sparse-rt,
  sparse-stress, for-each-*) all pass.

### Residual 70, by category (unchanged from prior analysis)

The remaining crossings are no longer dominated by root 2; they split into the
by-design carrier boundaries the plan keeps: cheap `:heap` casts
(`concrete->carrier` Vec, the symmetric of bucket A), the blessed inline-C
`tur_ok`/`tur_some` construction (`inline-c-typed-result-option`), the typed
`Result`/`Option`-at-dispatch boundary (genuine M4 dict-ABI), the 4
`MutableMap int int` producer crossings (its producers are still `:int`-typed
-- the No-Lazy-`:int` retype is the prerequisite to its producer slice), and
the type-erased `SChan` path. None is the old deref-copy root-2 shape.

## Update 2026-06-17 (M2-completion: primitive-payload `(ok v)`/`(some v)` at the dispatch boundary; 66 -> 60)

Refreshed the per-fixture audit after #396 (MutableMap retype) merged: the
baseline was **16 fixtures / 66 crossings** (the prior "70" minus MutableMap's
4). Root-caused the `Result`/`Option`-at-dispatch crossings and cleared the
tractable half of them.

### Root cause (the `ok`/primitive-payload asymmetry)

A `#{Construct}` constructor (`ok`/`err`/`some`, body `(make-struct ...)`) is
monomorphized to a direct by-value construction **only when its PAYLOAD is a
by-value struct** -- the arg-side `needs_byvalue_spec` trigger
(`emit_module.c`, `arg_types[i].kind == TY_STRUCT`).  `polymorphic-ok-err-
value-struct-payload` works because `(ok (make-struct User ...))` has a struct
payload, so `ok__spec__Result__User__cstr_User` mints and constructs directly.

But a **primitive**-payload `(ok v)` (`v : int`) inside an instance-method spec
(`(definstance Dec [int] (dec [v] (ok v)))`) had no by-value trigger:

- the payload `int` is a carrier-ABI primitive (no arg trigger), and
- the result `(Result int cstr)` is a **TY_APP**, which `type_uses_carrier_abi`
  (`emit_core.c:277`) reports as carrier, so the result-side trigger
  (`result_type.kind == TY_STRUCT`, `emit_module.c`) misses it (it only fires
  for an already-resolved `TY_STRUCT`, not a `TY_APP`).

Worse, the nested `(ok v)`'s `call->type` is **collapsed to the int64 carrier**
at elaboration (the parametric result type was not preserved), so `result_type`
at the call is a bare int64 -- there is no concrete `(Result int cstr)` to test.
With everything stringifying to `int64_t`, `abi_changes` is false and the call
**early-exits to the carrier path** before any construct gate.  Result: the
spec body emitted `return (Result__int__cstr){... __t->is_ok ...}` over a
deref'd `tur_ok` box -- the `carrier->concrete` crossing the audit flagged in
`typeclass-return-dispatch-result-wrapped` (2) and `m5-instance-spec-
constraint-var` (4).

### Fix (recover the concrete result from the enclosing instance-method spec)

`src/compiler/emit_module.c` (`emit_abi_register_call`): when a `#{Construct}`
call is scanned inside an `__inst_*__spec__*` body whose declared **return** is
a concrete by-value (non-heap) struct of the **same struct family** the
constructor produces (spec returns `Result__int__cstr`, `ok` builds a
`Result`), recover `result_type` from the spec's return type and set
`abi_changes` so it does not early-exit.  The construct then mints
`ok__spec__Result__int__cstr_int64_t` and the spec body becomes
`return ok__spec__Result__int__cstr_int64_t(v)` -- direct by-value, no box, no
bridge.

GATED to instance-method spec bodies (the dispatch-boundary site the audit
flags); a top-level `(ok 5)` in user code keeps the carrier path, so the broad
M2 suite-wide snapshot blast is avoided.

**Call-routing correction (the load-bearing half).** The same `(ok v)` Expr is
emitted in *two* functions: the int64-returning **carrier base**
`__inst_Dec_dec_int` (referenced by the dict singleton for indirect dispatch)
and the by-value **spec** `__inst_Dec_dec_int__spec`.  They must route the call
differently -- carrier base -> `ok` (int64), spec -> `ok__spec` (by-value).  The
existing call->clone lookups (`emit_core.c:emit_call_name`,
`emit_expr.c:find_matched_abi_spec`) fell back to the *first*-recorded entry for
a call Expr regardless of the active outer spec, and the secondary by-args
lookup matched `ok__spec` on arg types alone -- both made the **carrier base**
wrongly call the by-value `ok__spec` (a hard `incompatible types when returning
'Result__int__cstr' but 'int64_t' was expected`).  Fixed both: a return-only-
differentiated spec is reachable **only** via an exact call-Expr+outer match;
when a call was recorded under a spec outer but none matches the active
(`NULL`) outer, the carrier base / top-level emit uses the plain carrier callee
and skips the by-args match (which cannot tell two specs apart when they differ
only by return ABI).

### Validation

- Audit: **16 fixtures / 66 crossings -> 14 / 60.**
  `typeclass-return-dispatch-result-wrapped` (2 -> 0) and
  `m5-instance-spec-constraint-var` (4 -> 0) cleared.
- Compiled suite **1653 passed, 0 failed**, **zero `expected.c` drift** (the
  affected instance-method specs live in fixtures that carry no snapshot, and
  the carrier base / top-level paths are byte-identical).
- Interpreter gate **1209 passed, 2 failed** (the documented pre-existing
  `eq-carrier-capturing-comparator` / `mutmap-eq`; this change is emit-side
  only, so the tree-walker is unaffected).
- Spice: `../turmeric-spices/spices/json` (heavy Result use) -- all 5 `src/`
  modules `tur check` clean (the full roundtrip needs the yyjson cmake-dep,
  not buildable here; the equivalent Result-construct-at-dispatch patterns are
  covered by the in-tree fixtures above).

### Residual 60, unchanged in character

The remaining crossings are the by-design carrier boundaries the plan keeps:
the `Vec` carrier-based `Eq [Vec]` delegation + `:heap` producer-feed casts
(root 2 -- `vec-eq-ascribed*`, `*-of-tvec-eq`: cheap casts, cleared only by
monomorphizing the `vec-eq?` iteration core, which may stay carrier for
interpreter parity); the **blessed inline-C** `tur_ok`/`tur_some` construction
(`decode-bool-carrier-instance-ascription` 3, `inline-c-typed-result-option` 5
-- a user/instance inline-C body literally writing `tur_ok`, inherently
carrier); and the type-erased `SChan` path (`generic-relay-aggregate-result`
2).  None is the by-value-Result-from-a-Turmeric-construct shape this change
targeted.

## Update 2026-06-17 (post-#400 audit floor: `Eq [Vec]` TCO'd by-value loop; 60 -> 34; bridge down-scope COMPLETE for the collection-Eq cascade)

Refreshed the per-fixture sweep on the current branch head (compiled suite
**1653 passed, 0 failed**) after #400 landed -- the TCO-in-ABI-specs `Eq [Vec]`
rewrite (`stdlib/vec.tur`: the pure-Turmeric `vec-eq-loop` self-tail-call that
TCO lowers to a goto loop inside the by-value `Vec__int *` spec). That rewrite
retired the carrier-based `vec-eq?` delegation the "Update 2026-06-16 (root 2)"
entry installed, and the audit fell from **60 to 34 crossings / 10 fixtures**.

### The refreshed 34, bucketed by root cause (methodology unchanged: `TUR_M3_AUDIT=1 tur $flags emit-c <fixture>` per fixture, stderr captured directly)

| Bucket | count | direction | type(s) | nature |
|---|---|---|---|---|
| **A'. fat-closure comparator `:heap` reinterpret casts** | **22** | carrier->concrete | `Vec int` | the `Eq [Vec]` element comparator `(fn [a b] (eq? a b))` lowers to a fat closure with **int64-uniform params**; when the element type is itself a `:heap` `(Vec int)`, the body's `eq?` resolves (M4c Path A) to the typed `__inst_Eq_eq_qu_Vec__spec__(Vec__int *, ...)`, so each int64 closure arg is `(Vec__int *)(intptr_t)(__cmp_a)` -- a **cast, never a deref-copy** (verified: 0 `*(Vec__int *)(intptr_t)` whole-struct derefs across all 5 Vec-crossing fixtures). Fixtures: `vec-of-tvec-eq` 6, `option-of-tvec-eq` 6, `set-of-tvec-eq` 4, `map-of-tvec-eq` 4, `result-of-typed-eq` 2 |
| **C. blessed inline-C `tur_ok`/`tur_some` construction** | **10** | carrier->concrete | `Result Device int` 3, `Option Device` 2, `Result bool cstr` 2, `Result int cstr` 3 | an instance body literally writing `tur_ok`/`tur_some` (returns the int64 heap box); the by-value consumer materialises the struct field-wise from the reinterpreted box pointer (`ok_val__spec__...((Result__Device__int){.is_ok = __t->is_ok, .ok_val = (int64_t)(intptr_t)(__t->ok_val), ...})`). The deliberately blessed boundary (`inline-c-typed-result-option` exists to bless it; `instance-method-return-carrier-bridge` keeps its `Decode [int]` inline-C body **on purpose** as a guard). Fixtures: `inline-c-typed-result-option` 5, `decode-bool-carrier-instance-ascription` 3, `typeclass-method-parameterized-result-decode` 1, `instance-method-return-carrier-bridge` 1 |
| **E. type-erased channel** | **2** | carrier->concrete | `SChan<SRecv int ptr<void>>` | `generic-relay-aggregate-result` -- carrier by design (matrix roadblock 4); never deleted |

### The milestone: ZERO monomorphic deref-copy crossings remain

The original report's deletion target was the `CK_CARRIER -> CK_CONCRETE`
**deref-copy** that materialised a by-value collection/value out of an int64
carrier handle to feed a `*-byval` helper or a typed consumer. **Every such
crossing is now gone:**

- bucket A' is a **reinterpret cast** (Vec is `:heap`, so the int64 *is* the
  `Vec__int *`; no 24-byte copy), forced only by the int64-uniform fat-closure
  ABI -- not a carrier round-trip of the data;
- bucket C is **construction**, not deref-copy: the value genuinely originates
  as an int64 `tur_ok` box from a user/instance inline-C body, and the by-value
  struct is built field-wise at the blessed boundary;
- bucket E is the type-erased channel the plan keeps forever.

This is exactly the "delete the bridge from the **monomorphic** paths;
`emit_carrier_bridge` itself survives for the casts / blessed-construction /
type-erased boundary" end state that the 2026-06-15 sequencing re-scoped the
deletion down to (roadblock 4). **That down-scope is now complete for the
non-HKT collection-Eq cascade** -- there is no remaining monomorphic
deref-copy to remove, and the function fires only at the three by-design
boundaries above. The step-3 "delete `emit_carrier_bridge` wholesale"
checklist is therefore **superseded**: the function must stay (buckets A'/C/E
all require it), and there is no monomorphic-path call left to strip out of it.

### What would reduce the residual 34 (and what would NOT)

- **M4 typed dict slots do NOT touch any of the 34.** The earlier-corrected
  analysis (Update 2026-06-15, point (b)) already established M4 dict-slot
  typing is a near-zero-crossing win on this audit; post-#400 it is literally
  zero -- bucket A' is a *closure* cast (not a dict-slot consumption), bucket C
  is inline-C construction, bucket E is type-erased. Do not gate any bridge
  work on M4 dict slots for this cascade.
- **Bucket A' (22) clears only by typing the fat-closure element ABI** --
  i.e. per-`:heap`-element-type closure shims so the comparator's params are
  `Vec__int *` rather than int64. That is the closure-monomorphization frontier
  (M5/M7-adjacent), a broad ABI change with its own snapshot blast; it is the
  only thing standing between this cascade and zero crossings, and it is a
  large multi-session item, not a stdlib tweak.
- **Bucket C (10) is permanent** unless `Option` stops representing `none` as
  the NULL carrier and inline-C bodies stop being allowed to return real typed
  `Result`/`Option` (the explicitly blessed boundary). Out of scope; keep it.
- **Bucket E (2) is permanent** by design.

### Validation

- Compiled suite **1653 passed, 0 failed**.
- Audit: **10 fixtures / 34 crossings** (A' 22 + C 10 + E 2), zero monomorphic
  deref-copies (verified per fixture). No code changed this session -- the
  deliverable is the corrected post-#400 baseline and the down-scope-complete
  verdict for the collection-Eq cascade.

## Related

- [docs/upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md)
  §M3 / §M4 — the plan this report refines.
- [docs/reported/m2b-stdlib-migration-blocked-on-carrier-fallback.md](m2b-stdlib-migration-blocked-on-carrier-fallback.md)
  — the M2b finding that pointed at M4 as the deeper unblocker.
- `src/compiler/emit_core.c` `emit_carrier_bridge` — the bridge function
  (now carrying a permanent audit hook).
