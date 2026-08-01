# Stdlib Ownership Audit vs Rust's Ideal (WR0--WR4)

> **Status:** The audit itself is **essentially complete** (done in the
> 2026-07-24 GC study session) and the result is strongly positive: **stdlib is
> already at or beyond Rust's "weak-refs-where-necessary" ideal.** It does not
> build a single `rc<T>` cycle, and it needs `weak<T>` nowhere -- because it
> almost never reaches for shared ownership at all. The remaining work is
> therefore not "fix leaks" but "formalize the finding, keep the property from
> regressing, and surface the weak-reference escape hatch before anyone needs
> it." That is a small, low-prerequisite plan -- so yes, it is very possible to
> write and execute now.
>
> **Prerequisites:** none remaining. The trial-deletion weak-dangling defect
> that WR1's "`weak<T>` + GC is a safe pairing" story waited on was fixed by CG4
> on 2026-07-25 (`docs/archive/gc-strong-cycles-not-collected.md`).
>
> **Gate:** none. This is stdlib API + docs + a CI guard, not an experimental
> compiler feature.
>
> **Progress: COMPLETE.** WR0 (guide section) and WR2 (regression guard) landed
> 2026-07-24. WR1 (weak API surface), WR3 (guidance doc), and WR4 (fixtures)
> landed 2026-07-26 -- see "Execution notes" below for the three defects WR1
> surfaced on the way, two of which had to be fixed for the API to work at all.
>
> **Last updated:** 2026-07-26

---

## Motivation

Rust's memory ideal for shared data: use `Rc`/`Arc` only when ownership is
genuinely shared, and break every resulting cycle with `Weak` so nothing leaks.
The question was how close Turmeric's stdlib is to that ideal. The answer turned
out to be that stdlib clears the bar by *sidestepping* it: it leans on
persistent-immutable structures (structural sharing implemented at the C layer)
and linear/affine single-owner handles, and surfaces `rc<T>` as a
provided-but-unused primitive. There are no shared-ownership graphs to leak.

That is a genuinely good state, and worth (a) recording so it is not
re-litigated, (b) protecting with a guard so a future PR does not quietly
introduce a cyclic `rc<T>` structure with no weak break, and (c) completing the
one missing piece: an ergonomic `weak<T>` API in `stdlib/rc.tur` so the escape
hatch exists *in the library* the day someone does adopt shared ownership.

---

## Findings (the audit)

Grounded across all 138 stdlib `.tur` modules:

- **`weak<T>` appears nowhere in stdlib** -- zero occurrences of `weak<`,
  `downgrade`, or a weak `upgrade` API.
- **`rc<T>` appears only in `stdlib/rc.tur`** (the defining module) and the
  generated `stdlib/docstrings.tur`. No other module constructs, clones, stores,
  or imports an `rc<T>`. `rc.tur` provides `rc/of`, `rc/clone`, `rc/drop`,
  `rc->ptr` and `Functor`/`Foldable`/`Clone [rc]` instances (`stdlib/rc.tur:9-141`)
  -- and nothing consumes them.
- **No cycle-forming shapes exist.** `list.tur` is forward-only immutable cons
  (`stdlib/list.tur:15`, no set-tail); `hamt`/`map`/`set` are immutable DAGs with
  C-level refcounted structural sharing (`stdlib/hamt.tur:14`, `map.tur:39`,
  `set.tur:36`); `free.tur`/`fix.tur` are finite inductive trees; `zipper.tur`
  has neighbor arrays but no parent pointer (`stdlib/zipper.tur:20-27`). Every
  `set!` in stdlib (`ref`, `vec`, `mutmap`, `grid`, `gen`, `range`, `sized-*`)
  mutates storage the structure *solely owns* -- none writes a back-edge into a
  shared node.
- **Concurrency/IO handles are single-owner by type.** `chan`, `future`,
  `taskgroup`, `mutex`, `reactor`, `net`, ... are `defopaque ... :linear` /
  `:affine` handles (e.g. `stdlib/chan.tur:45`, `future.tur:71-73`,
  `reactor.tur:68`) -- the deliberate alternative to refcounting, single teardown,
  no cycles.

**Bottom line:** stdlib builds no `rc<T>` graphs at all, cyclic or otherwise, and
needs `weak<T>` nowhere. It is already past Rust's ideal in the sense that it
avoids shared mutable ownership almost entirely (Clojure/Haskell-style
persistence + linear types), rather than managing it with weak refs.

The single forward-looking caveat: **`stdlib/rc.tur` offers `rc/clone` with no
`weak`/`downgrade`/`upgrade` counterpart.** `weak` and `upgrade` exist as
*language* intrinsics (`elab_core.c:40` `TY_WEAK`, `elab_core.c:1702-1703`), but
the library gives no ergonomic surface for them. So the moment stdlib or user
code adopts `rc<T>` for a parent/child or observer structure, there is no
in-library tool to break the cycle -- they'd drop to raw intrinsics.

---

## Phases

### WR0 -- Formalize the audit as a guide section [DONE 2026-07-24]

Add a short "Ownership model" section to `docs/guides/gc-guide.md` (or a new
`docs/guides/ownership-guide.md`) recording the finding: stdlib is cycle-free by
construction, via persistence + linearity, and uses `rc<T>` essentially nowhere.
This is the "where we stand vs Rust" reference the audit was asked for.

Landed as the "Ownership across the stdlib" section of `docs/guides/gc-guide.md`
(the three strategies -- persistent-immutable, single-owner-mutable,
linear/affine -- plus the vs-Rust framing and a pointer to the WR2 guard).

### WR1 -- Surface a `weak<T>` API [DONE 2026-07-26]

Landed as **`stdlib/weak.tur`** (opt-in: `(load "stdlib/weak.tur")`), not inside
`rc.tur` as originally written -- `rc.tur` does not compile, see the execution
notes. Surface: `rc/downgrade`, `weak/upgrade`, `weak/unwrap`, `weak/alive?`,
`weak/drop`. No `Weak` type alias: `weak<A>` is already a builtin type spelling,
so an alias would add a name without adding anything.

The original phase text follows.

Add the escape hatch to the library so it exists before it's needed:

- `weak` / `downgrade` (rc -> weak), `upgrade` (weak -> `option<rc<T>>`),
  wrapping the existing intrinsics with docstrings and the stdlib naming
  convention. Consider a `Weak` type alias for symmetry with `rc`.
- Document the Rust-parallel usage pattern: parent holds `rc<child>`, child holds
  `weak<parent>`, `upgrade` at the point of use. This is the manual,
  GC-independent cycle-breaking that works *today*.
- **Sequencing:** advertising "`weak<T>` + turn on the GC" as a combined story
  must wait for the trial-deletion weak-dangling fix
  (`docs/reported/gc-strong-cycles-not-collected.md`); the manual `weak`/`upgrade`
  pattern has no such dependency.

### WR2 -- Regression guard (keep stdlib cycle-free) [DONE 2026-07-24]

A lightweight CI check (grep-based or a small `tur` lint) that flags a new
`rc<T>` field on a `defstruct`/ADT in `stdlib/` -- especially a self-referential
or mutually-referential one -- so any future shared-ownership structure gets a
conscious review for a `weak<T>` break. The property is valuable precisely
because it currently holds trivially; the guard makes regressing it loud.

Landed as `tests/check-stdlib-no-rc-cycles.sh`, registered as the
`tur_stdlib_no_rc_cycles` ctest (so CI's non-suite `ctest` run exercises it). It
is a tripwire, not a prohibition: any `: rc<...>` type annotation in stdlib
(outside `rc.tur` / generated `docstrings.tur`) fails the guard unless the line
carries an explicit `rc-cycle-ok` review marker, forcing a conscious "did I break
the cycle with `weak<T>`?" review before a shared-ownership field can land.

### WR3 -- Design-guidance doc: when to reach for what [DONE 2026-07-26]

Landed as `docs/guides/ownership-guide.md` (indexed in `docs/guides/README.md`,
cross-linked from the gc-guide ownership section): the one-line rule, the
five-row decision table, where stdlib actually sits, the "rc<T> that is not
pulling its weight" smell as five concrete questions, and the cycle-break
section contrasting `weak<T>` (prompt, deterministic, GC-off) with the
collector (opt-in, manually driven, walker-limited).

The original phase text follows.

Codify the principle stdlib already embodies, as guidance for stdlib and spice
authors: **persistent-immutable first, linear/affine handles for single-owner
resources, `rc<T>` only for genuine shared ownership, `weak<T>` to break any
resulting cycle.** Include the decision table and the "rc<T> that isn't pulling
its weight" smell (an `rc<T>` that could have been by-value/borrow/persistent).

### WR4 -- Test fixtures for the weak API [DONE 2026-07-26]

- `tests/fixtures/weak-upgrade-after-drop` -- the full WR1 surface: upgrade
  while alive mints a new strong handle (count 1 -> 2 -> 1 across the caller's
  `rc/drop`), and upgrade after the referent dies reports none.
- `tests/fixtures/weak-breaks-parent-child-cycle` -- the Rust parent/child
  break, 5000 pairs, no collector, zero heap growth. The deliberate mirror of
  `gc-collects-strong-cycle`, which builds the same shape with a strong
  back-edge and needs `(gc-enable!)` plus a `(gc!)` per iteration.

The original phase text follows.

A couple of fixtures exercising the WR1 surface: a parent/child structure made
leak-free with `weak`, and an `upgrade`-after-drop returning none. These also
seed the cycle-GC test corpus (`gc-cycle-collection-followup-plan.md` CG7).

---

## Risks / notes

- This plan is deliberately small. The temptation is to over-build a weak/GC
  story; resist it -- the audit says stdlib doesn't need one yet. WR1's value is
  *readiness*, not current necessity.
- Do not "adopt `rc<T>`" anywhere in stdlib to justify the API. The library's
  avoidance of shared ownership is the healthy state; WR1 just stocks the
  toolbox.

---

## Execution notes (2026-07-26)

WR1 was supposed to be five wrappers over intrinsics that already worked. It
was -- but standing them up turned over three defects, two of which had to be
fixed for the API to do anything at all. Recording them here because each is a
case of "the feature was shipped, but no code path in the tree exercised it."

### 1. `rc.tur` does not compile -- so the API moved

The plan says "an ergonomic `weak<T>` API in `stdlib/rc.tur`." That module
cannot be loaded into a compiled program: its `__functor_rc_fmap`,
`Functor [rc]`, and `Foldable [rc]` bodies call `tur_rc_of` / `tur_rc_clone` /
`tur_rc_ptr` / `tur_rc_drop`, none of which exist anywhere in the tree, and
`Foldable [rc]` additionally casts a `tur_poly_fn_t` struct to a raw function
pointer. `tur check stdlib/rc.tur` passes; only the C stage fails, which is why
it went unnoticed -- no fixture loads `rc.tur`. Verified pre-existing on a clean
tree at `f4493704`.

So WR1 landed as `stdlib/weak.tur`. An API in a module nobody can load is not an
API.

**Followed up 2026-07-26:** `rc.tur` is fixed and now compiles
(`docs/archive/rc-tur-legacy-instances-do-not-compile.md`), pinned by
`tests/fixtures/rc-tur-typeclass-instances`. `weak.tur` stays a separate module
by choice rather than necessity: reaching for a weak reference should not drag in
rc.tur's Functor/Foldable/Clone instances and the typeclass surface behind them.
Same shape as `rcchain.tur` -- one opt-in module, one job.

### 2. A `weak<Name>` struct field did not resolve its inner type

`rc<Name>` over a user aggregate resolves to `type_rc_adt(def)`; `weak<Name>`
did not, and stayed `weak<?>`. Since `(weak r)` over an `rc<Node>` produces
`weak<ADT>`, the *only* shape `weak<T>` exists for failed to type-check:

    (set! (.parent child) (weak parent))
    ; error: value type weak<<adt>> does not match field type weak<?>

Fixed by generalizing `adt_rc_inner_full_type` (`src/compiler/elab_structs.c`)
to the `weak<` prefix and adding `type_weak_adt` (`src/compiler/types.h`).
`TY_WEAK` is the same `RcControlBlock *` carrier as `TY_RC`, so it is
layout-neutral.

A second, related gap: `rc<A>`/`weak<A>` store the inner as a bare `TypeKind`,
so `rc<A>` over a type parameter lowers to `type_rc(TY_TYVAR)` with the
variable's *name* already erased. Every polymorphic rc/weak function therefore
returned an un-instantiable `rc<tyvar>` that no concrete field would accept --
so a generic `rc/downgrade` could not install a back-edge, and callers had to
use the raw intrinsic. `type_eq` now treats a tyvar inner as unifying with any
concrete one (two *concrete* inners are still compared strictly; two tyvars were
already equal, the name being gone). This is general, not weak-specific -- it
unblocks any `(defn f [A] [r : rc<A>] : rc<A> ...)`.

### 3. The zombie transition never dropped the value -- the pattern leaked

The real one. `rc_strong_decrement` on a block whose strong count reaches 0
*with a weak still observing* took the "zombie" branch and returned without
running `drop_fn` at all, deferring the whole value teardown to the last
`rc_weak_decrement`.

That deadlocks precisely on the parent/child break. The surviving weak lives
**inside the parent's own value** (the child holds `weak<Parent>`, and the
parent strongly owns the child), so the only thing that can release it is the
value's drop glue -- which is what we are waiting to run. Neither ever happens:
parent's value is never dropped, so its `rc<Child>` is never released, so the
child's `weak<Parent>` is never dropped. Measured at **217 bytes leaked per
parent/child pair**, with the collector off and nothing for it to collect.

Fixed by releasing the value at strong 0 and keeping only the control block
alive for the observer -- exactly what Rust's `Rc` does, and what the CG4 note in
`docs/archive/gc-strong-cycles-not-collected.md` already described as the
intended behavior. Factored as `rc_release_value` in `src/runtime/rc.c` with the
mirrored copy in the emitted preamble (`src/compiler/emit_module.c`); both
null `value` after dropping, the same discipline `gc.c` already used. Pinned by
`tests/fixtures/weak-breaks-parent-child-cycle` (0 bytes over 5000 pairs).

This is the one that justifies WR4 not being optional. The API reads correctly
without it and leaks in its headline use case.
