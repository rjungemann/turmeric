---
title: Make data structures Eq- and Show-able (element-type dispatch off the carrier)
category: Type system / Codegen / stdlib
description: Vec/Set/Map are not correctly Eq- or Show-able for any element type whose display/equality value differs from its int64 carrier (cstr, and eventually multi-word types). The container ABI already moved off the carrier (end-to-end monomorphization, 2026-06-19), but element-level typeclass dispatch inside constrained-generic container instances still grounds the element to the int64 carrier instead of the concrete element type. This plan fixes that, retypes the Set element API off :int, and settles the cstr/String question.
status: proposed
---

# Containers must be Eq- and Show-able for every element type

## Why this matters

Per the requirement: our data structures being `Eq` and `Show`-able is a hard
line -- "anything less than that is a complete no-go." Today they are not,
for any element whose semantic value differs from its raw int64 carrier.

## Ground truth (empirically verified, not inferred)

All of the following were reproduced against `./build/tur` at HEAD. Read them
as the spec of what is broken -- prior investigation (and two exploratory
sub-agents) read the code and *concluded this already works*; it does not. The
audits that declared "end-to-end monomorphization landed" measured **ABI
crossings**, not **dispatch correctness**, and no fixture exercises a
content-typed element, so the gap has been invisible.

1. **`Eq [Vec]` is wrong for `cstr`.**
   `(eq? (vec-of "a" "b") (vec-of "a" "c"))` returns **`true`**. Elements are
   compared by carrier (pointer) identity, not by `Eq[cstr]` (`strcmp`).

2. **`Show [Vec]` / `Show [Set]` / `Show [Map]` print pointers for `cstr`.**
   `(show (vec-of "a" "b"))` -> `[94005738389612 94005738389610]`. `cstr`-keyed
   maps show pointer keys.

3. **The failure locus is precise.** In a constrained generic:
   - `(defn show-a [^Show A] [x : A] : cstr (show x))` called with a concrete
     `cstr` -> **correct** (`"a"`). A value that flows in as a real `:A`
     parameter grounds `A` and dispatches `Show[cstr]`.
   - `(defn show-vget [^Show A] [v : (Vec A) i : int] : cstr
        (show (:: (vec-get v i) A)))` -> **wrong** (prints the pointer). Reading
     via the carrier-returning accessor `vec-get` and ascribing the result to
     the *constraint type variable* `A` does **not** recover the concrete
     element type.
   - `(:: (vec-get v i) cstr)` (ascribe to the *concrete* type) -> **correct**.

   So: `(:: <carrier-returning-call> A)` where `A` is a constraint tyvar fails
   to re-drive dispatch; `(:: <carrier-returning-call> cstr)` succeeds; a direct
   `:A` param succeeds. The container instances are built on the first (broken)
   form.

4. **`int`/`bool` elements work by coincidence** -- their carrier word *is*
   their value, so the int-carrier default dispatch (`Eq[int]`/`Show[int]`)
   happens to be correct. This is why every existing `vec-eq` fixture (all 9
   use int elements) passes and the bug never surfaced.

5. **The `Set` element API is typed `:int`, not `:A`.**
   `set-add`/`set-remove`/`set-member?` declare the element parameter as `:int`
   (`stdlib/set.tur`), so `(set-add (:: (set-new) (Set cstr)) (hash "x") "x")`
   is a hard `TUR-E0001: expected int, got cstr`. A `Set` of anything but `int`
   is not even expressible today -- a direct violation of the "no lazy `:int`
   stand-ins" rule, and it means `Set` also never threads a content comparator
   into the HAMT (unlike `Map`, which does).

## What already exists -- do NOT rebuild it

The heavy lifting is done; this is a targeted correctness fix on top, not a
re-architecture.

- **End-to-end by-value monomorphization concluded 2026-06-19.** Polymorphic
  values thread by value at their natural C layout; the int64 carrier is no
  longer the default ABI. See `docs/guides/monomorphization-abi-guide.md`.
- **Path A / per-instantiation specs** exist: a non-HKT typeclass instance
  dispatches a concrete receiver via a by-value spec (`emit_module.c` /
  `emit_fns.c`, `EmitAbiSpecialization`). `Eq[Vec]`/`Eq[Map]`/`Eq[Set]`/
  `Eq[MutableMap]`/`Eq[Cons]` all dispatch the **receiver** by value
  (`Vec__T *`, `Map__K__V *`) -- audit floor is 0 carrier deref-copies
  (`docs/archive/typed-collection-eq-consumers-plan.md`, COMPLETE).
- **Constrained-generic loop helpers** already have the right *shape*:
  `vec-show-loop [^Show A]`, `vec-eq-loop`, `set-show-loop`, `map-show-loop`,
  etc. carry the element dictionary and call `(show ...)` / `(eq? ...)` on the
  element. They just mis-ground the element type (ground truth #3).
- **The residual carrier bridge** (`emit_carrier_bridge`,
  `ensure_aggregate_spill_shim`) is intentional and load-bearing for HKT
  continuation returns (`Monad bind` / `Applicative ap`). Out of scope; leave
  it.

The gap is exactly the seam between "receiver dispatches by value" (done) and
"element dispatches on its concrete type" (broken).

## The fix

### Phase 1 -- element-type recovery for carrier reads (the keystone)

**Goal:** inside a container typeclass instance specialized at `A = T`, an
element read (`vec-get`, HAMT key/val) dispatches the element's method on `T`,
not on the int64 carrier.

**How dispatch actually works (traced, current build).** There is **no runtime
dictionary passing** -- dispatch is statically monomorphized in two phases:

1. **Elaboration** (`elab_method_call`, `src/compiler/elab_typeclasses.c:4707`)
   selects an instance from the receiver expression's static `type.kind` and
   tags the `EX_CALL` with an `EX_DICT` node. For a `KIND_STAR` receiver the
   match is exact on `type.kind` (`elab_typeclasses.c:5468`): a `TY_CSTR`
   receiver picks `Show[cstr]`; a **carrier (`TY_INT`) receiver picks
   `Show[int]`** -- and a carrier-collapsed element read is indistinguishable
   from a genuine `int`. An *abstract-tyvar* receiver takes a shortcut
   (`elab_typeclasses.c:5369`) that picks the int representative **but tags the
   call** for emit-side re-resolution.
2. **Emit** (`emit_reresolve_disp_type`, `emit_core.c:1486`) re-resolves the
   tag per ABI specialization -- but **only if `emit_dispatch_tyvar`
   (`emit_core.c:1455`) can recover a dispatch tyvar** from the call, and it
   looks in exactly three syntactic places: an `EX_ASCRIBE` receiver whose
   ascribed type is `TY_TYVAR`, a bare `TY_TYVAR` receiver, or a `TY_TYVAR`
   call result (plus an `EX_GET_FIELD` special case).

**Why the collection instances mis-dispatch (three concrete erasure points):**

- **The instance receiver loses `A`.** In `Show [Vec] [(Show A)]`, definstance
  pass 1 substitutes the head `[Vec]` for the class var and lowers a `TY_APP`
  receiver to the carrier (`elab_typeclasses.c:3505-3552`,
  `if (elab_param_type.kind == TY_APP) param_type = TYPE_INT;`). So the
  receiver `x` is typed **bare `Vec` / carrier, not `(Vec A)`** -- the
  constraint var `A` from `[(Show A)]` is a *separate* tyvar never attached to
  `x`. This is exactly why every stdlib collection instance re-ascribes
  `(:: x (Vec A))` (`typeclass-show.tur:234`, `vec.tur:385-390`) to
  reconstruct the type the receiver lost.
- **`vec-get`'s result collapses to the carrier.** `vec-get [A] ... : A` has
  `result_full_type = TY_TYVAR(A)` but `result_kind = TY_INT`; the call's type
  is taken from `result_kind` (`elab_call.c:741`), and the G3/LT4 branches
  deliberately decline to restore a bare-tyvar return (`elab_call.c:2556-2600`)
  because doing so would erase the per-call-site instantiation. With no
  argument carrying `A`, `(vec-get x i)` is `TY_INT`.
- **The `Eq` comparator lambda collapses.** `Eq [Vec]` passes
  `(fn [a b] (eq? a b))` (`vec.tur:390`); the **untyped lambda params `a`,`b`
  default to the int64 carrier**, so `(eq? a b)` bakes `__inst_Eq_eq_qu_int` at
  elaboration -- the emitted `Vec__cstr` spec correctly specializes the *loop*
  but passes a comparator box that hard-codes `Eq[int]` (pointer compare).

Net: for `int` elements the carrier word *is* the value so `Eq[int]`/`Show[int]`
are accidentally correct; for `cstr` they are wrong. Note the stdlib
`(:: x (Vec A))` idiom re-types the *receiver of the loop helper* but does not
rescue the *element* read/compare inside the helper -- which is why the bug
survives it (verified: `(show (vec-of "hi" "yo"))` -> `[<ptr> <ptr>]`,
`Eq[Vec]` on cstr compares by pointer).

**Fix candidates (from the trace; spike on `Show/Eq [Vec]` at `(Vec cstr)`):**

- **(1) Attach the constraint var to the receiver's applied type** -- narrowest.
  In definstance lowering (`elab_typeclasses.c:3505-3552`) give the receiver of
  `Show [Vec] [(Show A)]` type `(Vec A_tyvar)` instead of bare `Vec`/carrier.
  Then `emit_dispatch_tyvar` recovers `A` from the element read without a
  hand-written ascription, and the existing carrier-representative +
  re-resolution machinery grounds it per spec.
- **(2) Broaden dispatch-tyvar recovery** in `emit_dispatch_tyvar`
  (`emit_core.c:1455`) and its elab twin `obj_is_unascribed_carrier_elem`
  (`elab_typeclasses.c:4683`) to also cover (a) an inline-C generic accessor
  read whose *declared* `result_full_type` is a tyvar even when `result_kind`
  erased it, and (b) **untyped lambda params bound in a constrained instance
  body** whose declared comparator type is `(fn [A A] bool)` -- propagate `A`
  into the lambda params instead of defaulting them to the carrier. (b) is
  required for the `Eq` comparator case specifically.
- **(3) Thread a real per-constraint dictionary** (an element-method pointer per
  `[(C A)]`) as the general fallback for cases where `A` is recoverable from no
  syntactic position. Larger; only if (1)+(2) leave gaps.

Recommendation: pursue **(1) + (2)** together (they are the narrowest and cover
both `Show` reads and `Eq` comparator lambdas). Watch for the **clone-name /
signature consistency issue** the `Eq Cons` probe hit
(`docs/archive/m4-final-state-bridge-still-essential-for-collection-eq.md`
§"Probed alternative": `emit_abi_clone_name` -> `type_c_name` on a `TY_APP`
returned `int64_t` on one path and `Cons__int` on another, minting two
disagreeing specs) -- if it resurfaces once the receiver carries `(Vec A)`, it
gates the cascade and must be fixed alongside. Prove `cstr` first, then `bool`,
`float`, and an opaque `defopaque` element, then cascade through `Eq` and the
`Set`/`Map` HAMT key/val reads.

**Phase 1 exit criteria:**
- `(eq? (vec-of "a" "b") (vec-of "a" "c"))` -> `false`; `(= ... same)` -> `true`.
- `(show (vec-of "a" "b"))` -> `[a b]`; `#map{...}` with cstr keys prints the
  strings.
- The same holds for `bool`, `float`, and an opaque handle element type.
- `int` elements still correct (regression).
- New fixtures (below) green; full `run.sh` / `run-turi.sh` unregressed.

### Phase 2 -- retype the `Set` element API off `:int`

Independent of Phase 1 and lower risk. `set-add`/`set-remove`/`set-member?`
must take the element as `:A`, not `:int`, and thread `Hash[A]` + `MapKey[A]` +
the content comparator into the HAMT exactly as `Map` already does
(`map-assoc` boxes via `mk-box`, hashes via `hash`, compares via `mk-cmp`,
stamps the comparator on the root). This makes `Set[cstr]` / `Set[struct]`
expressible *and* content-keyed (fixing the "Set never threads a comparator ->
pointer-identity membership" bug), and removes a flagship `:int` stand-in the
codebase's own rules forbid. Once done, `Set` composes with Phase 1's element
dispatch for free.

### Phase 3 -- the `cstr` / `String` decision

With Phase 1 landed, **`cstr` works in containers with no new type**: the
element read grounds to `cstr` and dispatches `Eq[cstr]` (`strcmp`, content) and
`Show[cstr]` (identity). Strings become Eq/Show-able as Vec elements and Map
keys through the machinery everything else uses. So a higher-level string type
is **not required** to satisfy the Eq/Show requirement.

There are, however, two genuine `cstr` gaps a higher-level owned `String` would
close -- worth a *separate, optional* track, not a blocker:

- **Ownership / lifetime.** `Map`/`Set` insert `cstr` keys with `mk-owned? = 0`
  -- they borrow the caller's `char*`. A computed or later-freed string dangles.
  An owned `String` (heap-allocated, copied on insert, freed with the map) is
  the clean fix; the HAMT already has the boxed-owned-key path
  (`hamt.h` boxed-key ops) used today for multi-word struct keys.
- **`str` is inert.** `stdlib/str.tur`'s `str` (a borrowed `ptr<void>`
  pointer+len view) has only `Eq[str]`; no `Show`/`Hash`/`MapKey`, and it is a
  raw `ptr<void>` so it collides with `Show [ptr<void>]`. It is not a usable
  first-class string today.

**Recommendation:** for v1, **keep `cstr` as the string type** and let Phase 1
make it container-worthy; document the borrow semantics of `cstr` map/set keys.
Defer an owned `String` (either by promoting `str` to a real `defopaque` with
the full instance set + `cstr <-> String` conversions, or a new nominal type)
to a follow-up once Phase 1 lands and real ownership footguns are observed. Flag
this as a decision point for the maintainer -- if owned strings are wanted now,
Phase 3b (below) is the shape.

*Phase 3b (optional, deferred):* introduce `String` as an owned heap value with
`Eq`/`Show`/`Hash`/`MapKey`/`Clone` instances, `mk-owned? = 1` (copy-on-insert
via the boxed-key path), and `str->String` / `String->cstr` conversions. This is
additive stdlib + runtime, gated behind Phase 1, and does not touch the carrier.

## Explicitly out of scope

- **Multi-word (by-value struct / ADT) elements.** `Vec.data` is an
  `int64_t[]` and HAMT slots are one `void*`; a multi-word struct element does
  not fit a single slot. Making `Vec[SomeStruct]` / `Map[K SomeStruct]` store
  elements by value is a separate, larger effort (typed element buffers /
  boxed-element path) and is **not** needed for Eq/Show-ability of the
  single-word element types (all primitives, `cstr`, opaque handles, heap
  struct pointers). Call it out as future work; do not let it expand this plan.
- **The HKT continuation carrier bridge** (`bind`/`ap` return closures). Unrelated
  and intentional.

## Sequencing, risk, and validation

1. **Phase 1 is the keystone and the highest-risk** (compiler codegen). Order:
   (i) fix the clone-name/signature consistency bug; (ii) spike lever (b) on
   `Vec[cstr]` `Show`; (iii) cascade to `Eq`, then `Set`/`Map` reads.
2. **Phase 2** (Set `:int` retype) is stdlib-level, can proceed in parallel, and
   lands cleanly once Phase 1's element dispatch works.
3. **Phase 3** is a decision plus optional additive work.

**The missing test coverage is itself a deliverable** -- add before/with the
fix so the gap can never silently reopen:
- `tests/fixtures/` : `Eq`/`Show` over `Vec`/`Set`/`Map` with **`cstr`**,
  **`bool`**, **`float`**, and an **opaque** element/key type -- assert content
  equality (`(vec-of "a") != (vec-of "b")`) and readable `Show` output, not just
  "compiles."
- Keep an explicit `int`-element regression so the carrier==value coincidence
  stays covered.
- A `Set[cstr]` construction + content-dedup fixture (Phase 2).

## Open questions for the maintainer

1. **Lever (a) vs (b)** for Phase 1 -- return-specialized accessors vs widened
   ascription relabel. The spike decides; flagged so the direction is a
   conscious choice, not an accident of whichever compiles first.
2. **cstr ownership** (Phase 3) -- ship v1 on borrowed `cstr` keys with
   documented semantics, or invest in an owned `String` now? Recommendation:
   borrowed for v1, `String` as a fast-follow.

## Dispatch code map (for the implementer)

- `src/compiler/elab_typeclasses.c:4707` -- `elab_method_call` (instance
  selection); `:5455-5469` (match on `obj->type.kind`); `:5369` (abstract-tyvar
  shortcut that tags for re-resolution); `:4683` (`obj_is_unascribed_carrier_elem`);
  `:3505-3552` (definstance receiver class-var substitution + `TY_APP`->carrier
  lowering -- **fix (1) site**); `:4154` (`make_dict_expr`, the static tag).
- `src/compiler/elab_call.c:741` (call type = `result_kind`); `:2556-2600`
  (bare-tyvar return deliberately not patched -- the carrier-collapse point).
- `src/compiler/elab_types.c:2476-2561` -- `elab_ascribe` (how `(:: e A)` vs
  `(:: e cstr)` resolve).
- `src/compiler/emit_core.c:1455-1468` -- `emit_dispatch_tyvar` (**fix (2) site**);
  `:1486-1575` -- `emit_reresolve_disp_type` (per-spec re-resolution).
- `stdlib/vec.tur:101` (`vec-get`), `:362-390` (`vec-eq-loop` + the
  `(fn [a b] (eq? a b))` comparator), `stdlib/typeclass-show.tur:208-235`
  (`vec-show-loop` + the `(:: x (Vec A))` idiom).

*Investigation note:* a sub-agent trace of the above initially concluded the
current build "already works" for the ascribed-receiver form; direct
reproduction disproves that (`Show[Vec]`/`Eq[Vec]` over `cstr` remain broken as
in Ground Truth). Trust the repros -- the `(:: x (Vec A))` idiom re-types the
loop *receiver* but not the element read/compare *inside* the loop.

## Related

- `docs/guides/monomorphization-abi-guide.md` -- the by-value ABI, authoritative.
- `docs/archive/m4-final-state-bridge-still-essential-for-collection-eq.md` --
  the `(:: t (Cons A))` probe, the ascription-bridge gate, and the clone-name
  consistency blocker (the exact obstacle Phase 1 removes).
- `docs/archive/typed-collection-eq-consumers-plan.md` -- receiver-level
  collection Eq off-carrier (the "done" half this builds on).
- `docs/archive/phase4-carrier-helper-inventory.md` -- which helpers are
  carrier-essential vs retypeable.
- `docs/archive/repl-no-show-instances-for-collections-and-structs.md` -- the
  resolved REPL-display report whose "cstr shows carrier" limitation this plan
  removes at the root.
