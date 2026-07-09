---
title: Generalize the Set element API off :int (Set must hold any hashable type)
category: stdlib / typed collections
description: Set's public API types its element as :int and hashes each element as its own identity, so Set[cstr] / Set[Sym] / Set[struct] are not expressible and content-equal elements at distinct addresses are treated as distinct members. Map already solved this (Hash[K] + MapKey[K] dispatch over the _eq_o HAMT path); Set must mirror it. Vec and Map are already correctly typed -- this plan is Set-only, with a verification pass on the other two.
status: implemented
---

# Set must hold any hashable type, not just int

## Implemented (set-element-api-generalization)

Landed. `Set[cstr]`, `Set[Sym]`, and any `Set[A]` with `Hash[A]` + `MapKey[A]`
are now content-keyed (verified: two equal-text strings at distinct addresses
are one member; membership / union / intersect / diff / `Eq[Set]` all compare
content). `Set[int]` is byte-for-byte unchanged. Green across `run.sh` (1995),
`run-turi.sh` (1469), `run-flags.sh` (78), `run-stdlib-checks.sh` (32).

What shipped, and where it deviated from the sketch below:

- **Raw layer** `set-add-eq-o` / `set-has-eq-o?` / `set-del-eq-o` in
  `stdlib/set.tur` over `tur_hamt_{set,has,del}_eq_o`, plus interpreter natives
  in `src/turi/collections_native.c`.
- **Public surface as MACROS, not defns** (this is the key correction).
  `set-add` / `set-remove` / `set-member?` are macros that dispatch
  `mk-box`/`mk-cmp`/`mk-owned?` on the concrete element EXPRESSION at the call
  site (a let-bound `__tur_se__`), exactly like `map-assoc`. A generic *defn*
  dispatching on an abstract `:A` param compiles fine but the tree-walking
  interpreter grounds `mk-cmp` to the int carrier and silently drops content
  equality -- so the macro form is required for compiled/interpreter parity.
- **Signature kept 3-arg** `(s h x)` (explicit hash), NOT the 2-arg form
  sketched below, so all ~57 existing call sites keep working unchanged (only
  `x`'s type widened `:int -> :A`). The auto-hash convenience stays in
  `set-add1` / `set-of` / `#set{...}`.
- **`set-empty-for`** (new) pins `A` from the first element so `set-of` /
  `#set{...}` seed a typed empty set (mirrors `map-empty-for`); a bare
  `(set-new)` seed left `A` unpinned and defaulted the element dispatch to the
  carrier.
- **`set.tur` now `(load "stdlib/map.tur")`** for the `MapKey` class (no cycle;
  map does not load set); the interpreter preload orders set before map, so the
  explicit load is required there.
- **union / intersect / diff** thread the source's stamped comparator
  (`tur_hamt_keyeq` + `tur_hamt_has_dynamic` / `tur_hamt_set_eq_o`), compiled
  and interpreter both.
- **Non-`MapKey` elements** (e.g. `Set[Vec[int]]`, structural-`Eq` via the
  compiler-synthesized `set-eq-cmp?` dispatch) now use the raw `set-add-eq-o`
  directly -- the public `set-add` requires `MapKey[A]`. `set-of-tvec-eq` was
  updated accordingly.
- **Owned-key elements** (a future owned `String`, `mk-owned? = 1`): the scalar
  `owned=0` path is wired; union/intersect/diff pass `owned=0` and would need to
  retain boxes for an owned element type (noted inline; out of scope until the
  owned-String plan).

The remainder of this document is the original plan; the sketch's 2-arg macro
was superseded by the non-breaking 3-arg macro above.

# (original plan follows)

## Scope check: Set is the outlier; Vec and Map are fine

Verified empirically at HEAD:

- **Vec is already `:A`-typed.** `vec-push! [A] [v : (Vec A) val : A]`,
  `vec-get [A] ... : A` (`stdlib/vec.tur`). `(vec-get (vec-of "a" "b") 1)`
  returns `"b"`. Vec *holds* any single-word element type today. (Its `Show`/`Eq`
  element-**dispatch** bug is a separate matter -- see
  `containers-eq-show-element-dispatch-plan.md`; not this plan.)
- **Map is already key-typed.** The public `map-assoc` / `map-get` macros
  dispatch `Hash[K]` + `MapKey[K]` and thread a content comparator into the
  `tur_hamt_*_eq_o` HAMT path (`stdlib/map.tur:475+`). `#map{"k" "v"}` is
  content-keyed; aggregate struct keys work when the user supplies a `MapKey`
  instance (data-literals guide). Values are `:V`. Map is fine.
- **Set is broken.** `set-add` / `set-remove` / `set-member?` declare the
  element as **`:int`** (`stdlib/set.tur:63,89,115`: `[s (Set A) h :int x :int]`),
  `set-add1` is `[s (Set A) x :int]` and hashes the element as **its own
  identity** (`(set-add s x x)`, `set.tur:435`), and the ops call the **plain**
  `tur_hamt_set` / `tur_hamt_has` / `tur_hamt_del` (pointer/value identity), not
  the `_eq_o` family. Consequences:
  - `(set-of "p" "q")` / `(set-add (:: (set-new) (Set cstr)) ...)` -> hard
    `TUR-E0001: expected int, got cstr`. A `Set` of anything but `int` is not
    expressible.
  - Even where a caller passes a content hash, membership collision resolution
    is pointer identity -- two equal-text strings at distinct addresses would be
    distinct members.
  - It is a flagship violation of the "No Lazy `:int` Stand-Ins" rule.

## The fix: give Set the Map treatment

Map's two-layer shape is the template. Bring Set to parity.

### 1. Raw carrier layer (mirror `map-assoc-eq-o` et al.)

Add Set counterparts that take the key already boxed to the int64 carrier plus
an explicit comparator and ownership flag, delegating to the ownership-aware
HAMT ops:

- `set-add-eq-o   [A] [s (Set A) h :int x :int keyeq owned :int] : (Set A)`
  -> `tur_hamt_set_eq_o(hamt, h, x, (void*)1, keyeq, owned)`
- `set-has-eq-o?  [A] [s (Set A) h :int x :int keyeq owned :int] : bool`
  -> `tur_hamt_has_eq_o(...)`
- `set-del-eq-o   [A] [s (Set A) h :int x :int keyeq owned :int] : (Set A)`
  -> `tur_hamt_del_eq_o(...)`

The `x :int` here is the **carrier** slot (as in Map's raw layer), not a public
type -- it is fed a boxed `:A`. `tur_hamt_*_eq_o` and the `owned` flag already
exist (`src/runtime/hamt.h`, WKC3) and are content-comparator-threaded and
comparator-stamped on the root, so structural `Eq[Set]` recovers the comparator
via `hamt/keyeq` exactly as `Eq[Map]` does.

### 2. Typed public surface (mirror the `map-assoc` macro)

Retype the public ops to take `:A` and dispatch the element typeclasses:

```turmeric
(defmacro set-add [s x]
  `(let [__e__ ~x]
     (set-add-eq-o ~s (hash __e__) (mk-box __e__) (mk-cmp __e__) (mk-owned? __e__))))
```

...and the same shape for `set-member?` / `set-remove`. `hash` (`Hash[A]`),
`mk-box` / `mk-cmp` / `mk-owned?` (`MapKey[A]`) are the exact instances Map
already relies on -- no new typeclass surface. This makes `Set[cstr]` content-
keyed (via `Hash[cstr]` + `tur-cstr-key-eq?`), `Set[Sym]` identity-keyed on the
interned pointer, and `Set[struct]` work when the user supplies `MapKey`.

Retire the identity-hash `set-add1` / rewrite `set-of` and `#set{...}` lowering
to go through the typed `set-add` (each element mentioned once -- keep the
single-evaluation property the current `set-add1` macro guards).

### 3. Set-operation ops (union / intersect / diff)

`set-union` / `set-intersect` / `set-diff` (`set.tur:157-242`) iterate the HAMT
and re-insert via plain `tur_hamt_set`. Route their re-insertions through the
`_eq_o` path with the source set's stamped comparator (read via `hamt/keyeq`) so
the result stays content-keyed. `set-eq?` already has a comparator-aware variant
(`set-eq-cmp?`).

## Backward compatibility & the interpreter

- `Set[int]` must stay identical (identity hash == value for ints; the `_eq_o`
  path with a NULL/int comparator behaves as the plain path).
- The interpreter (`--interpret` / REPL) registers Set natives in
  `src/turi/collections_native.c`; the current `set-add` etc. map to
  `native_set_*`. The retype needs matching native overrides for the new
  `set-add-eq-o` family (the `native_map_*_eq_o` analogs already exist for Map;
  add the Set twins), so `Set[cstr]` works under the interpreter too. Keep the
  `MapKey[cstr]` / `Hash[cstr]` natives (already registered) in play.

## Tests (deliverable, currently absent)

- `Set[cstr]` construction + **content** dedup: two equal-text strings at
  distinct addresses collapse to one member; `set-count` == 1.
- `Set[Sym]` (keyword) membership.
- `Set[int]` regression (unchanged behavior + `expected.c` if snapshotted).
- Structural `Eq[Set]` over `Set[cstr]` stays content-correct.
- Once `containers-eq-show-element-dispatch-plan` lands: `Show[Set]` over
  `Set[cstr]` prints the strings.

## Sequencing

Independent of the element-dispatch compiler plan and lower-risk (pure
stdlib + interpreter-native work). It can land first; it makes `Set` *hold*
non-int types. `Show[Set]`/`Eq[Set]` *rendering/comparison* of those elements
then rides the element-dispatch plan. Do them in either order; note the
dependency so `Show[Set]` over cstr is only claimed done once both land.

## Related

- `stdlib/map.tur` -- the `map-assoc` macro + `map-*-eq-o` raw layer to mirror.
- `src/runtime/hamt.h` -- `tur_hamt_*_eq_o`, `tur_hamt_box_key_ops` (owned keys).
- `docs/upcoming/v1/containers-eq-show-element-dispatch-plan.md` -- the
  element Show/Eq dispatch fix (composes with this).
- `docs/upcoming/v2/collection-multiword-element-boxing-plan.md` -- owning
  multi-word struct elements/keys (the boxed-`owned=1` path this plan wires up
  for scalars extends to there).
