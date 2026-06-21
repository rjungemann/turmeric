---
title: Generic int-carrier list helpers (list-length, ...) over a :heap Cons with a by-value aggregate head read tail at the wrong offset and segfault
category: Carrier <-> Concrete ABI -- :heap cons cell layout vs the int64-carrier list API
severity: Medium. Runtime segfault (not a compile error). A `(Cons (Option int))`
  -- a list whose element is a by-value aggregate -- now *builds* and is usable
  through the TYPED accessors (`.head`, ascribed `.tail`), but feeding it to a
  generic int-carrier list helper such as `(list-length (:: xs :int))` crashes.
  Blocks treating a by-value-aggregate-element list through the `:int`-carrier
  list API (list-length, list-eq?, etc.).
status: PARTIAL -- found 2026-06-21 while closing gap G1
  (docs/carrier-concrete-abi-crossing-audit-plan.md). Exposed once the
  homogeneity-check fix let such a list construct. The concrete `(Cons A)`
  consumer path is FIXED (2026-06-21, branch
  claude/carrier-concrete-abi-audit-94dbi2) via the element-aware `tlength`
  walker; the two strictly carrier-erased halves below remain OPEN.
---

# `:heap` Cons with a by-value aggregate head breaks the int-carrier list API

## Resolution status (2026-06-21)

**Concrete `(Cons A)` consumer path: FIXED.** The maintainer chose audit Option 2
(monomorphize, not box the head). `tlength [A] [xs : (Cons A)] : int`
(`stdlib/list.tur`) is the element-aware sibling of the carrier `list-length`,
written in pure Turmeric over the typed `(.tail xs)` read (no inline-C). The
compiler lowers `(.tail xs)` at the concrete `(Cons A)` stride and monomorphizes
`tlength` per element type, so a by-value aggregate head (`Cons__Option__int`)
no longer misplaces the tail link. The carrier `list-length` inline-C is left
untouched for the scalar/pointer bulk (the dual "by-value-body + carrier-shim"
shape landmine #7 endorses), so all existing callers stay green. Fixture
`tests/fixtures/list-length-byvalue-aggregate-element`; suite green (1745/0).

**Still OPEN (the two carrier-erased halves this report now tracks):** both lose
`A` at a point with no runtime type tag, so neither is safely recoverable
without further compiler work.

1. **Bare `(:: xs :int)` escape-hatch into the carrier helpers.**
   `(list-length (:: xs :int))` (and `list-head`/`list-tail`/...) still walks the
   fixed `{int64 head; int64 tail}` layout and segfaults on an aggregate-element
   list. This is inherent to explicitly erasing a layout-load-bearing element
   type -- there is no recovery short of a runtime tag or refusing the coercion.
   The supported route for aggregate elements is the typed `(Cons A)` walker
   (`tlength`, `thead`, ascribed `.tail`).
2. **Phantom `(List A)` view collapses (the "typed wrapper collapses too" case
   below).** `(defopaque List [A] :int)` lowers to `:int`, so a `(List A)` helper
   is emitted once at the carrier ABI with `A` erased; a layout-dependent op (a
   typed `list-count`) collapses its inner `(:: ... (Cons A))` back to the generic
   `Cons *` and segfaults. Confirmed empirically: a `list-count [A] [xs : (List A)]`
   routing through `tlength` emits a single carrier `list_hycount(int64_t)` whose
   body calls `tlength(int64_t)` at the generic `Cons *` layout. Fixing it needs
   the monomorphizer to **specialize phantom-param functions per phantom type
   arg** -- a distinct, larger feature than the `(Cons A)` walker, tracked here as
   the remaining work.

The original analysis and the (unchanged) carrier-layout root cause follow.

## One-line summary

`Cons` is `(defstruct Cons :heap [A] (head A) (tail :int))` -- the generic
int-carrier list helpers walk a cons chain as a fixed
`struct { int64_t head; int64_t tail; }`. When the element `A` is a by-value
aggregate (`Option__int`, a multi-word struct), the real cell is
`struct { Option__int head; int64_t tail; }`, so `tail` no longer sits at
offset 8. The carrier walk reads `tail` from inside the `head` aggregate,
follows a bogus pointer, and segfaults.

## Minimal repro

```turmeric
(defn main [] : int
  (let [xs (:: (list (some 42) (some 7)) (Cons (Option int)))]
    (println (list-length (:: xs :int))))   ;; segfault
  0)
```

The list builds fine (gap G1 fixed); the crash is in `list-length` walking the
chain at the carrier layout. The typed path is unaffected:

```turmeric
(let [h0 (.head xs)
      t0 (:: (.tail xs) (Cons (Option int)))
      h1 (.head t0)]
  ...)                                       ;; works: reads 42 then 7
```

(see `tests/fixtures/list-homog-byvalue-aggregate-element`).

## Root cause (direction)

The `:int`-carrier list helpers (`list-length`, `list-eq?`, the inline-C cons
walkers) assume the `__tur_cons`/`Cons` cell has an int64 `head` at offset 0
and an int64 `tail` at offset 8. That holds for scalar / `:heap`-pointer
elements (head fits the carrier) but not for a by-value aggregate head, where
the C struct embeds the aggregate inline and pushes `tail` past offset 8.

This is the consumer-side companion of the carrier<->concrete family: the
*producer* (`list-build__` via `tcons-of`) correctly specializes the cell to
`Cons__Option__int { Option__int head; int64_t tail; }`, but the *generic*
consumer still reads it at the carrier layout.

### Not just the raw escape hatch -- the typed `(List A)` API collapses too

The typed `(List A)` wrapper (`stdlib/list-typed.tur`) is itself carrier-backed:
`list-empty?`, `list->carrier`, and any typed traversal bottom out in
`(:: xs :int)` and then the same `{int64 head; int64 tail}` carrier walk
(`tnil?`, `list-length`, ...). So a by-value-aggregate-element list is broken for
*every* carrier-level traversal, not only the explicit `(:: xs :int)` escape
hatch. Only the direct typed field accessors (`.head`, ascribed `.tail`) read the
real concrete layout and stay correct. This widens the impact: the fix has to
make the *carrier representation itself* correct for aggregate elements, not just
patch one helper.

## Fix directions

1. Box the by-value aggregate head into the carrier inside `tcons-of` for
   aggregate elements (so the cell stays `{ int64 head; int64 tail; }` and the
   head is a heap pointer) -- keeps the generic int-carrier API working, at the
   cost of an allocation per element.
2. Or monomorphize the int-carrier list helpers per element type (a `list-length`
   that walks `Cons__Option__int`), the same end-to-end-monomorphization
   direction as the rest of the audit. The `(:: xs :int)` coercion that erases
   the element type to the carrier is the point to intercept.

Option 2 keeps the by-value thread end-to-end and is consistent with the
crossing-audit's P2; option 1 is the smaller local change.

### Why this is architecturally significant (not a one-line patch)

Both options touch the cons-cell **layout invariant** established by #482 / gap
G1 (a by-value aggregate element is stored *inline* in the cell, which is what
makes the typed `.head` a direct read with no allocation):

- **Option 1 (box the head)** reverts that decision for aggregate elements: the
  cell becomes uniformly `{int64 head; int64 tail}` with a heap pointer head, so
  every carrier helper works -- but `.head` and `(:: head A)` must now *deref*
  (a carrier->concrete bridge), and every aggregate element costs an allocation.
  This changes the field-read codegen for aggregate-element cons and regenerates
  fixtures.
- **Option 2 (monomorphize the helpers)** keeps the inline layout but requires a
  per-element-type `list-length`/`list-eq?`/`tnil?`/... family minted at the
  `(:: xs :int)` coercion -- a substantial codegen feature, the end-to-end
  direction of the audit but not a small change.

Either way this is a design decision about the cons layout on the one track to
v1, with real regression risk to the currently-working typed path -- it is the
kind of architecturally significant change to confirm direction on before
undertaking, not a rough edge to patch in passing.

## Monomorphization route (option 2): scope, refined shape, and estimate

### The one shape that actually works

You cannot make the tail recursive (`tail : (Cons A)`): `elab_structs.c`
(`struct_field_user_type_storage`, ~line 190) **explicitly excludes
self-referential by-value struct fields** from inline layout, and `:heap` forces
carrier storage regardless. So the tail link stays `:int`.

The route that works keeps `(tail :int)` but makes the helpers element-aware.
Crucially, **typed field access already reads the correct offset** -- `(.head xs)`
and `(.tail xs)` on a value typed `(Cons A)` lower against the concrete
`Cons__Option__int` layout (this is exactly why the typed path in the
`list-homog-byvalue-aggregate-element` fixture works while the `:int` carrier walk
segfaults). So the refined shape is:

1. Re-type the leaf helpers from `[l : int]` to `[A] [xs : (Cons A)]` and
   **rewrite their bodies in pure Turmeric using `(.head xs)` / `(.tail xs)`,
   retiring the hand-written inline-C** that casts to
   `struct { int64_t head; int64_t tail; }`. The compiler already lays out the
   typed read at the right stride, so `list-length` becomes a plain recursive
   count over `(.tail xs)` -- no `__TUR_TY_` template needed for these.
2. `(.tail xs)` yields the field's declared `:int`; re-ascribe it `(:: _ (Cons A))`
   to keep A threaded through the recursion **without changing the struct**.
3. Thread A through the remaining pure-Turmeric helpers (`list-eq?`,
   `__cons-fmap`, `list-concat`, `list-reverse`) and the `Eq [Cons]` instance.
4. **Stop erasing**: drop `(:: xs :int)` where it feeds a list helper, so the spec
   keys on the concrete element at the call site.

This is the **same move M4c-pre used** for `Eq Tuple2`/`Eq Pair`/`Eq Result`
(retire the inline-C carrier helper, consult by-value fields via dispatch) -- see
the landmines below.

### Estimate

**Medium--large; ~1--2 weeks of careful work.** Not greenfield: the inline-C
`__TUR_TY_<NAME>__` template mechanism (`emit_core.c:2104`) and the ABI
specializer exist and are exercised by ~9 fixtures (`inline-c-template-spec`,
...). The refined shape above mostly *avoids* templating by going pure-Turmeric
on the leaf helpers (cheaper, per M4c-pre), and the template is the fallback only
if some helper genuinely can't be expressed without inline-C. The cost is in
correctness threading and the blast radius across the ~69 `(:: ... :int)`
coercion sites in stdlib (only ~3 list-specific; the rest are monad/typeclass
glue in `parsec`/`logic`/`backtrack`/`zipper`/`sized-buf`, relevant only where
they funnel a *list* through an `:int` state slot).

## Lessons from prior monomorphization false starts (read before starting)

Monomorphization has stalled here repeatedly. The concrete landmines, each from a
resolved archive doc:

1. **Retire the inline-C carrier helper; don't try to monomorphize it in place.**
   `docs/archive/m4c-stdlib-carrier-helpers-block-dispatch-rewrite.md`: per-method
   ABI was blocked because instance bodies delegated to inline-C helpers
   (`tuple2-eq-carrier?`) that hard-cast `Tuple2 *p = (Tuple2 *)(intptr_t)x`.
   Re-elaborating the body by-value doesn't help while the helper still declares
   `int64_t`. The fix (RESOLVED 2026-06-13) rewrote the 3 instance bodies
   (`stdlib/tuple.tur:473`, `stdlib/pair.tur:111`, `stdlib/result.tur:239`) to
   read `(.fst x)`/`(.snd x)` directly and **dropped** the carrier helpers. Apply
   the same to the list helpers: rewrite to typed field access, retire the cast.
2. **Inline-C bodies are a hard contract to the int64 carrier; forcing a spec on
   them miscompiles.** `docs/archive/generic-inline-c-struct-arg-monomorphises-to-int64.md`:
   always-force per-instantiation broke 24 fixtures whose inline-C hand-coded
   `int64_t *buf = malloc(...)`. The gate was narrowed to only specialize inline-C
   when a slot is a real `TY_STRUCT` by-value type or the body carries a
   `__TUR_TY_` template (`emit_module.c:2398`). Don't widen that gate to force the
   list helpers; rewrite them instead (landmine 1).
3. **Sibling specs drop silently when AST passes field-copy call nodes.**
   `docs/archive/m5-eq-vec-byval-rewrite-drops-sibling-specs.md`: rewriting one
   instance body perturbed the spec worklist and an *unrelated* `thead__spec__int`
   was never minted because a CPS pass field-copied an `EX_CALL` and dropped
   `call_.abi_bindings`. Specs key on node identity, not just `(binding,
   arg_types)`. After rewriting list helpers, verify transitively that other
   monomorphizations still intern -- diff emitted C / `emit-abi-trace`, don't trust
   "it compiled."
4. **Partial monomorphization makes two coexisting ABI views that miscompile.**
   `docs/archive/sc7-carrier-duality-plan.md`,
   `docs/archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`: a helper
   whose *body/result* went by-value while its *dispatch dict slot* stayed int64
   produced a fn declared `int64_t`, a body returning `Schema__int`, and a call
   site binding `int64_t` -- three mismatches. Don't monomorphize a list helper's
   return without the call boundary that consumes it.
5. **The carrier bridge stays load-bearing for abstract-element dispatch; don't
   plan to delete it.** `m3-...`: deleting `emit_carrier_bridge` regressed
   1186 fixtures / 2429 crossings, dominated by `Eq [Cons]`'s carrier base
   recursing via `(:: t1 (Cons A))` for abstract `A`. The re-ascription in step 2
   above is exactly that pattern -- it works *with* the bridge present (correct,
   one bridge crossing per step), so keep the bridge; G4 is not a bridge-deletion
   task.
6. **The defect is invisible without a direct concrete-element test.** `m3-...`:
   the `Eq [Cons]` carrier-base miscompile shipped latent because no fixture
   exercised `(eq? cons cons)` with concrete elements. Add the
   `(Cons (Option int))` aggregate-element fixtures **first (red)**, ASan on, and
   keep the scalar/pointer-element path (`Cons int`, `Cons cstr` -- the bulk of
   existing usage) green at every step.
7. **Don't assume "the carrier disappears after we ship this."**
   `docs/archive/end-to-end-monomorphization-plan.md` (+ `-2.md`, why a second plan
   was needed) and `m2b-stdlib-migration-blocked-on-carrier-fallback.md`: every
   attempt to rewrite `ok`/`some`/etc. off the carrier required a whole-call-graph
   prerequisite (M4 per-method ABI, blocked on HKT elaborator threading) and never
   landed in isolation -- the generic carrier-fallback emit kept producing
   `(int64_t){...designated init...}`, which C rejects. Design the list rewrite to
   work **both** by-value (concrete calls) **and** carrier (abstract dispatch),
   like `unwrap-or`'s by-value-body-plus-carrier-shim pattern.

### Why list is more tractable than the general case

`docs/archive/phase4-carrier-helper-inventory.md`: after the by-value migration,
the *genuinely* carrier-essential helpers are the ones walking heterogeneous /
runtime-erased memory (HAMT `set-eq?`/`map-eq-raw?`). A list is **homogeneous** --
all elements are one type A -- so once A is threaded (steps 1-2) the carrier cast
can be retired entirely, exactly as M4c-pre did for the fixed-arity structs. The
list's only extra difficulty over Tuple2/Pair/Result is the *recursive* `:int`
tail, handled by the re-ascription in step 2.

### De-risking order

Red aggregate-element fixtures first; convert one leaf helper at a time
(`list-head`/`list-tail`/`list-length`), then the recursive pure-Turmeric helpers,
then un-erase call sites module by module (`list-typed` first, the monads last).
Keep the scalar/pointer-element path green at every step -- it is the existing
bulk of usage and the regression tripwire. Verify with `emit-abi-trace` (not just
"it compiled") that sibling specs still intern (landmine 3).

## Cross-reference

`docs/carrier-concrete-abi-crossing-audit-plan.md` -- downstream of gap G1;
distinct from gap G2 (nested instance-method dispatch). Prior monomorphization
history: `docs/archive/m4c-stdlib-carrier-helpers-block-dispatch-rewrite.md`,
`docs/archive/generic-inline-c-struct-arg-monomorphises-to-int64.md`,
`docs/archive/m5-eq-vec-byval-rewrite-drops-sibling-specs.md`,
`docs/archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`,
`docs/archive/sc7-carrier-duality-plan.md`,
`docs/archive/phase4-carrier-helper-inventory.md`,
`docs/archive/end-to-end-monomorphization-plan.md` (+ `-2.md`).
