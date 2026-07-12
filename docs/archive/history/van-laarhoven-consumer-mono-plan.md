---
title: Path B endgame -- consumer monomorphization for ambiguous lens params,
  then retire --enable=vl-wide-mono and delete the Path A wide branch
category: Planning
description: Successor to van-laarhoven-monomorphization-plan (VBM1-VBM4, all
  landed). VBM3's redirect makes a wide-functor lens site zero-overhead ONLY when
  the consuming fn's lens param resolves to a single concrete lens; a consumer
  called with two distinct wide lenses is marked ambiguous and falls back to the
  Path A carrier box. This plan specializes the CONSUMER per concrete lens
  (consumer monomorphization) so every statically-resolvable wide lens use goes
  Path B, then graduates --enable=vl-wide-mono to default-on and deletes the Path
  A box/unbox on the wide branch. MEDIUM. Sliced (CM1-CM4). The one shape that
  cannot be reached -- a lens value chosen at runtime -- gets an explicit
  decision (diagnostic vs. a residual Path A keep) in CM4.
---

# Path B endgame -- consumer monomorphization and flag retirement

## Context

[`van-laarhoven-monomorphization-plan.md`](../archive/van-laarhoven-monomorphization-plan.md)
(slices VBM1-VBM4, all landed 2026-07-04) took the wide-by-value van Laarhoven
lens from "boxed at every crossing" (Path A) to "zero-overhead by value" (Path
B) on the common case, and graduated `--enable=vl-wide-functor` to default-on so
wide functors are always accepted. What remains is exactly the residual VBM4
named when it chose to KEEP Path A rather than delete it:

> Path B only redirects lens sites that resolve *uniquely and unambiguously*; a
> lens used with two distinct wide lenses (ambiguous) still needs the Path A
> carrier bridge. Deleting Path A now would reintroduce a miscompile for that
> unsupported-but-legal shape. Path A retirement stays tied to the deferred
> `vl-wide-mono` default-on step, where consumer monomorphization would cover
> the ambiguous case first.

This plan is that step. It has two deliverables the user asked for together:

1. **Consumer monomorphization** -- close the ambiguous-consumer gap so every
   *statically-resolvable* wide lens use compiles to Path B, not Path A.
2. **Retire `--enable=vl-wide-mono`** -- make Path B the default on the wide
   branch and delete the now-dead Path A box/unbox, per the standard
   experiment-lifecycle cadence.

## Where VBM3 left the wide path

The shipped machinery lives in
[`src/compiler/mono_specs.c`](../../src/compiler/mono_specs.c) and the poly-call
emit:

- **VBM2a resolve pass** (`mono_specs_resolve_program`, `resolve_walk`
  `mono_specs.c:216`). For each abstract lens spec -- one per `(l g s)` pin
  inside a consumer such as `set-px` -- it scans the program for calls of the
  consumer and reads the concrete lens in the abstract lens param's arg slot.
  The FIRST concrete lens sets `redirect_lens` + `redirect_resolved`
  (`:234-237`); a SECOND, distinct one sets `redirect_ambiguous = true`
  (`:233`).
- **VBM3 redirect** (`mono_spec_redirect_for_binding`, `mono_specs.c:143`, read
  from the `is_poly_call` emit in `emit_expr.c`). On a uniquely-resolved,
  non-ambiguous binding it hands back `<lens>__mono_<hash>`, and the `(l g s)`
  emit becomes a direct by-value call to the mono body. On
  `redirect_ambiguous` it returns false (`:150`) and the site falls through to
  the Path A carrier dispatch.
- **g-closure de-box** (`clear_g_box_walk`, `arg_closure_fndef`,
  `mono_specs.c:334/356`). For a resolved consumer it clears
  `box_aggregate_result` on the inner `g` closure FnDef so `g` returns `(f A)`
  by value into the by-value mono body.

So the gap is narrow and precisely located: **the `redirect_ambiguous` bail at
`mono_specs.c:150/233`.** Everything downstream of a *resolved* binding (the
redirect, the g de-box, the by-value mono body) already works; consumer
monomorphization is about turning "ambiguous -> give up" into "specialize the
consumer per concrete lens."

## The ambiguous-consumer gap (worked example)

```turmeric
;; point-x and point-y are BOTH wide (f := Identity) lenses on Point.
(defn set-px [l <lens-forall> b : int s : Point] : Point
  (run-id (l (fn [a : int] : (Identity int) (mk-id b)) s)))

(defn main [] : int
  (let [p  (make-struct Point :x 3 :y 4)
        p1 (set-px point-x 99 p)     ;; l := point-x
        p2 (set-px point-y 88 p)]    ;; l := point-y  <- second distinct lens
    ...))
```

Today `set-px`'s lens param `l` is marked `redirect_ambiguous` (two distinct
lenses), so BOTH call sites fall to Path A: `g` is boxed, `(f S)` is boxed, and
`point_x__mono` / `point_y__mono` are emitted but unused on these sites. The
consumer body is one function that must accept any lens through the carrier
dict.

Consumer monomorphization emits two specialized consumers instead:

```c
static Point set_px__point_x(int64_t b, tur_adt_Point *s) { ... run_id__spec(point_x__mono(g_x, s)) ... }
static Point set_px__point_y(int64_t b, tur_adt_Point *s) { ... run_id__spec(point_y__mono(g_y, s)) ... }
```

and rewrites `(set-px point-x 99 p)` -> `set_px__point_x(99, p)` and
`(set-px point-y 88 p)` -> `set_px__point_y(88, p)`. The lens argument is baked
into the clone and dropped from the call; each clone's inner `g` is de-boxed
(both build `Identity`, since `set-px` fixes the functor), and each `(l g s)`
is redirected to the matching `<lens>__mono`. No box anywhere.

**Note the functor is shared.** `set-px` fixes `g` to build `Identity`
(`(mk-id b)`), so every lens passed to it is used with `f := Identity`. The
clones differ ONLY in which `<lens>__mono` the `(l g s)` binds to; the g closure
and its by-value functor are identical across clones. Consumer clones are
therefore cheap -- the same body with one call statically bound.

## Slices

Each slice stays behind `--enable=vl-wide-mono` until CM4 graduates it. CM1 is
registry-only (no new emit); CM2/CM3 add emit + call rewrite; CM4 flips the
default and deletes Path A on the wide branch. Fixtures land with each slice.

### Slice CM1 -- resolve each lens param to its full concrete-lens SET (fixpoint over the call graph)

**Goal.** Replace the binary `redirect_resolved` / `redirect_ambiguous` with a
**set** of concrete lenses per (consumer FnDef, lens param), each element
carrying the concrete lens FnDef, its `<lens>__mono_<hash>`, and the list of
call-site `Expr *` that pass it. Resolve transitively: a consumer that receives
its lens from ANOTHER consumer's lens param inherits that param's resolved set.

**Work.**

- `src/compiler/mono_specs.c` -- widen `MonoSpecKey` (`:38-42`): replace the
  single `redirect_lens` + `redirect_ambiguous` bool with a small
  `LensResolution { const FnDef *lens; char mono_hash[..]; const Expr **sites;
  size_t n_sites; }` vector on the abstract key. In `resolve_walk` (`:216`),
  on encountering a call of the enclosing fn, ADD the concrete lens (with the
  call `Expr *`) to the set instead of flipping `redirect_ambiguous` on the
  second distinct lens. Dedup by lens FnDef identity; append the site either way.
- **Transitive resolution (fixpoint).** A consumer's lens-param arg may itself
  be another consumer's lens param binding (a lens threaded two levels), not a
  concrete lens FnDef. Iterate `resolve_walk` to a fixpoint: on each pass, when
  a call passes a lens param that is itself already resolved to a set `S`, add
  all of `S` (and cross-product each with the outer call site) to the callee's
  set. Terminates because the lens universe is the finite set of concrete lens
  FnDefs in the program, and each pass only grows sets monotonically toward that
  bound.
- **Keep `mono_spec_redirect_for_binding` for the |set| == 1 fast path.** A
  binding whose set has exactly one element is the current unique case; the
  in-place VBM3 redirect (no clone) still fires for it. New API
  `mono_spec_lens_set_for_binding(binding) -> const LensResolution *, size_t`
  exposes the multi-element sets to CM2/CM3.
- **Registry-only.** No emit change. Extend `--dump-mono-specs`
  (`mono_specs_dump`, `:83`) to print, per consumer lens param, the resolved
  SET (`consumer=set-px lens-param=l -> {point-x, point-y}`) so the fixpoint is
  reviewable by eye. Fixture `van-laarhoven-lens-wide-consumer-resolve`.

**Risk.** Medium. The fixpoint is the new machinery. Bound it explicitly (a
per-key site/lens cap with a `log`-style note when hit, never a silent
truncation) and assert monotonic growth so a bug surfaces as a cap hit, not an
infinite loop. Every read stays behind `g_opt_vl_wide_mono`.

### Slice CM2 -- emit a consumer clone per (consumer FnDef, concrete lens)

**Goal.** For each consumer lens param whose resolved set has >= 2 elements,
emit one specialized consumer body per concrete lens, with the lens param bound
to that lens (so `(l g s)` redirects to `<lens>__mono`), the inner `g` closure
de-boxed, and a mangled name `<consumer>__lens_<lenshash>`.

**Work.**

- Reuse the ABI-spec clone path VBM2b already drives from `emit_program`
  (`emit_module.c`, the block after the main ABI-spec emit loop). A consumer
  clone is structurally the SAME transform as the lens mono body: intern an
  `EmitAbiSpecialization` for the consumer FnDef, but instead of substituting a
  functor tyvar, bind the lens PARAM (a `Binding *`) to a concrete lens FnDef so
  every `(l g s)` in the body resolves to that lens.
- The `(l g s)` inside the clone must redirect to `<lens>__mono` unconditionally
  (the clone's `l` is now a known lens). Route this through the existing
  `is_poly_call` redirect (`emit_expr.c`) by making the clone's lens binding
  report a singleton set -- i.e., the clone reuses the VBM3 redirect verbatim,
  it just guarantees the singleton the redirect needs.
- **g de-box is per-clone.** Cloning the consumer body creates fresh FnDefs for
  the inner `g` closure, so `clear_g_box_walk` runs on the clone's own `g` --
  each clone gets an independently de-boxed g with no cross-clone aliasing.
- Forward-declare each `<consumer>__lens_<hash>` into the same `fwd_decls`
  buffer VBM2b uses (assembled before `file`) so CM3's rewritten call sites
  resolve.
- Fixture: `tur emit-c` on the two-lens example shows `set_px__point_x` +
  `set_px__point_y`, each box-free, each calling the matching `<lens>__mono`.

**Risk.** Medium. The genuinely new bit is binding a *value* param (the lens) at
clone time rather than a type param. Mitigation: the lens is always a top-level
`defn` (a global FnDef), so binding it is a name substitution, not a closure
capture -- the clone references the global directly. Assert the bound lens is a
global FnDef and bail to Path A (with a `log` note) if a resolved "lens" is ever
a local/captured value (it should not be, given CM1's fixpoint only admits
concrete lens FnDefs).

### Slice CM3 -- rewrite call sites to the matching clone; keep the |set|==1 in-place path

**Goal.** Each call `(consumer concrete-lens args...)` whose consumer lens param
has a >= 2 resolved set emits a direct call to `<consumer>__lens_<hash>(args...)`
with the lens argument dropped. The `|set| == 1` case keeps the shipped in-place
redirect (no clone).

**Work.**

- `src/compiler/emit_expr.c` -- in the call emit for a consumer whose callee has
  consumer-mono clones, look up the concrete lens actually passed at THIS site
  (recorded by CM1 as the call-site `Expr *`), pick the matching clone symbol,
  emit the call with the lens arg elided. On a site whose lens is not statically
  a concrete lens (the runtime-selected residual, below), fall through to Path A
  unchanged.
- **Transitive rewrite.** When a consumer forwards its (now-bound) lens to
  another consumer, the inner call inside the clone is itself rewritten to the
  inner consumer's matching clone. This is automatic if CM2 emits the clone body
  through the same call-emit path (the clone's forwarded call sees a singleton
  lens set and picks the inner clone). Confirm with a two-level fixture
  (`over-of point-x` inside a `tweak` consumer called with `point-x`/`point-y`).
- **Cartesian blow-up bound.** A consumer with L lens params each reached with K
  lenses yields up to K^L clones. Real code has L == 1 (lenses passed one at a
  time) and small K, but cap the product; above the cap, leave the over-cap
  sites on Path A and `log` the fallback (never silently drop coverage).
- Fixture: byte-compare the two-lens and two-level `emit-c` dumps to confirm
  zero `emit_agg_box` / `emit_agg_unbox` / `dict_clone` on any wide site, and
  runtime fixtures returning the expected values.

**Risk.** Medium. Same Path-A/Path-B seam hazard VBM3 flagged, now at the
consumer call site. Mitigation: reuse VBM3's invariant -- assert that a wide
consumer call emits EITHER a clone call OR the Path A carrier path, never both,
and that a clone call never carries a residual lens argument.

### Slice CM4 -- graduate `--enable=vl-wide-mono` to default-on; delete the Path A wide branch

**Goal.** With CM1-CM3, every statically-resolvable wide lens use compiles to
Path B. Make Path B the default on the wide branch, delete the Path A box/unbox
there, and retire the flag.

**Work.**

- **Default-on.** Make the VBM registration + resolve + redirect + consumer-mono
  path fire unconditionally on the wide branch (drop the `g_opt_vl_wide_mono`
  guards), exactly as VBM4 made the Path A allow unconditional. Run
  `mono_specs_resolve_program` (currently `main.c:387`, behind the flag) always.
- **Delete Path A on the wide branch.** Remove the WF1 `g`-box
  (`elab_call.c`, the `box_aggregate_result` set for wide functors), the WF3
  double-unbox guard (`emit_expr.c`), and the wide-branch
  `emit_agg_box`/`emit_agg_unbox` + carrier dict clone. The
  carrier-COMPATIBLE-functor path (opaque `Const`/`Identity`, `:heap`) is
  untouched -- it never used these. Verify by removing the branch and confirming
  every `van-laarhoven-lens-wide-*` fixture still passes on Path B.
- **Retire the flag.** Remove the `vl-wide-mono` `EXPERIMENTS[]` row and the
  `g_opt_vl_wide_mono` global per
  [../../guides/experimental-flags-guide.md](../../guides/experimental-flags-guide.md),
  leaving a graduation comment (mirroring the `vl-wide-functor` graduation
  comment). Drop `vl-wide-mono` from every fixture's flags; the
  `-mono`/`-mono-resolve` fixtures become flagless (Path B is the default).
- **Resolve the runtime-selected-lens residual** (see the dedicated section
  next) -- this is the one decision CM4 must make before Path A can be *fully*
  deleted.
- **Docs.** Update [../../guides/lens-guide.md](../../guides/lens-guide.md): the
  functor-width section drops the `--enable=vl-wide-mono` opt-in and states that
  wide functors dispatch by value with no box under the default settings; note
  the single residual (runtime-selected lenses). Archive this plan and
  [`van-laarhoven-monomorphization-plan.md`](../archive/van-laarhoven-monomorphization-plan.md)
  per the CLAUDE.md STRICT archiving rule.

**Risk.** Low-medium. Mostly deletion once CM1-CM3 land, but the deletion is the
irreversible step -- gate it behind a full green suite AND the residual decision.

## The runtime-selected-lens residual (the one shape consumer mono cannot reach)

Consumer monomorphization specializes over *concrete lens FnDefs* threaded
through the call graph. One legal shape has no concrete lens to specialize on:

```turmeric
(set-px (if cond point-x point-y) 99 p)   ;; the lens itself is chosen at runtime
```

Both `point-x` and `point-y` have the same forall type, so `(if cond ...)` is a
well-typed lens VALUE, but there is no single concrete lens at the call site --
consumer mono cannot pick a clone. This is the ONLY wide lens use CM1-CM3 leave
unresolved. CM4 must decide between two end states:

- **Option R1 -- reject with a narrow diagnostic (enables full Path A deletion).**
  A wide-functor lens argument that is not a statically-known concrete lens
  (i.e., resolves to a computed/branched value, not a global `defn`) is an error
  with a clear message ("a wide by-value functor lens must be a named lens, not a
  runtime-selected value; wrap the selection at the focus level, or use a
  carrier-compatible functor"). This is a MUCH narrower restriction than the
  retired TUR-E0309 (which rejected all wide functors) -- it only bites the
  runtime-lens-selection shape -- and it lets Path A be deleted outright.
- **Option R2 -- keep a minimal Path A fallback (no capability regression).**
  Leave the Path A carrier bridge alive on the wide branch *only* as the
  fallback for a non-concrete lens argument; delete nothing, gate the flag off.
  Path B is default; Path A is unreachable except for this shape. No new error,
  no deletion, but the carrier code stays.

**Recommendation: R1.** The runtime-selected-lens-over-a-wide-functor shape is
vanishingly rare (it requires selecting between two lenses at runtime AND a
wide-by-value functor AND caring about the box), it is adjacent to the
already-excluded "store a constrained forall in a container" non-goal, and R1 is
what actually retires Path A rather than parking it. The plan proceeds on R1
unless a concrete user need for R2 surfaces during CM1-CM3; the choice is
isolated to CM4 so it can flip late.

## Non-goals

- **Retiring the mode-B carrier for carrier-compatible functors.** Unchanged from
  the parent plan: opaque / `:heap` functors are already one word, zero-copy, and
  stay on the carrier. This plan only deletes the *wide-branch* box/unbox.
- **Non-lens optics (Prism/Traversal), impredicative use, storing a constrained
  `forall` in a container.** Still out, matching the parent plans.
- **Specializing over the FOCUS/WHOLE types beyond what VBM2 already keys.**
  Consumer mono specializes over the concrete LENS; the functor is fixed by the
  consumer. No new focus/whole specialization is introduced.
- **Widening the mode-B carrier (Path C from the report).** Still rejected.

## Cost estimate (rough)

| Slice | Surface | Type system | Codegen | Tests/docs | Risk |
| --- | --- | --- | --- | --- | --- |
| CM1 -- lens-set resolution + fixpoint | small | medium (set + fixpoint on MonoSpecKey) | none | small (dump fixture) | medium |
| CM2 -- consumer clone emit | small | small (bind lens param) | medium (reuse VBM2b clone path) | small | medium |
| CM3 -- call-site rewrite + transitive | small | none | medium (call emit branch) | medium (byte-compare) | medium |
| CM4 -- default-on + delete Path A wide + residual | small | small (residual diagnostic) | small (deletions) | small | low-medium |

Consumer mono is smaller than VBM2 because it reuses the VBM2b clone/emit path
and the VBM3 redirect wholesale -- the new machinery is the CM1 fixpoint and the
CM3 call-site rewrite. Everything else is a specialization of code that already
ships.

## Open questions

1. **Fixpoint ordering vs. emit.** CM1's fixpoint must complete before CM2 emits
   (a clone's forwarded lens set must be final). Confirm
   `mono_specs_resolve_program` runs to fixpoint at `main.c:387` before
   `emit_program`, and that no emit-time resolution can observe a half-grown set.
2. **Clone identity across call sites.** Two call sites passing the same concrete
   lens to the same consumer must share one `<consumer>__lens_<hash>` clone (as
   VBM2's `__mono` bodies already share by content hash). Confirm the clone key
   is `(consumer FnDef, lens FnDef)` and nothing distinguishes sites lexically.
3. **Recursion through a consumer's own lens param.** A self-recursive consumer
   that forwards `l` to itself must call its OWN clone for the same lens (a fixed
   point of the rewrite), not spin up a fresh clone per recursion depth. The
   content-hash keying should make this automatic; add a self-recursive fixture
   to prove termination of the emit, not just the resolve.
4. **Interaction with drop glue.** A consumer clone that builds and discards
   intermediate `(f a)` values by value must not double-drop -- same discipline
   VBM2 OQ #3 raised for the mono body, now at consumer scope. Confirm before CM2
   fixtures land.
5. **R1 vs R2 timing.** If R1 (the residual diagnostic) is chosen, land it in CM4
   with a dedicated error fixture; if a user need for R2 appears earlier, CM4
   keeps Path A and the plan stops short of deleting it. Decide no later than the
   start of CM4.

## Related

- [`van-laarhoven-monomorphization-plan.md`](../archive/van-laarhoven-monomorphization-plan.md)
  -- the parent plan (VBM1-VBM4); this plan is its deferred "consumer mono +
  vl-wide-mono retirement" tail. Archive both together at CM4.
- [../../guides/lens-guide.md](../../guides/lens-guide.md) -- the shipped lens
  guide; CM4 drops the `vl-wide-mono` opt-in from its functor-width section.
- [../../guides/experimental-flags-guide.md](../../guides/experimental-flags-guide.md)
  -- the experiment-lifecycle procedure CM4 graduates `vl-wide-mono` under.
- `src/compiler/mono_specs.c:216` -- `resolve_walk`, extended by CM1 from a
  unique/ambiguous flag to a concrete-lens set + fixpoint.
- `src/compiler/mono_specs.c:143/150` -- `mono_spec_redirect_for_binding` and the
  `redirect_ambiguous` bail CM1 replaces with the set + clone dispatch.
- `src/compiler/mono_specs.c:334/356` -- `arg_closure_fndef` / `clear_g_box_walk`,
  the per-consumer g de-box CM2 runs per clone.
- `src/compiler/emit_module.c` (VBM2b emit block) -- the ABI-spec clone path CM2
  reuses to emit consumer clones (binding a lens param instead of a functor
  tyvar).
- `src/compiler/emit_expr.c` (the `is_poly_call` redirect) -- the VBM3 redirect
  CM2/CM3 reuse; CM3 adds the consumer call-site rewrite alongside it.
- `src/main.c:387` -- `mono_specs_resolve_program`, run behind the flag today;
  CM4 makes it unconditional.
