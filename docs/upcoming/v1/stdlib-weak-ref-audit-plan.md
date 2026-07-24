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
> **Prerequisites:** none blocking. WR1 (surfacing `weak<T>` + GC as a *safe*
> pairing) should land after the `weak`-in-trial-deletion defect noted in
> `docs/reported/gc-strong-cycles-not-collected.md` is fixed; manual Rust-style
> cycle-breaking with `weak`/`upgrade` already works today independent of that.
>
> **Gate:** none. This is stdlib API + docs + a CI guard, not an experimental
> compiler feature.
>
> **Last updated:** 2026-07-24

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

### WR0 -- Formalize the audit as a guide section

Add a short "Ownership model" section to `docs/guides/gc-guide.md` (or a new
`docs/guides/ownership-guide.md`) recording the finding: stdlib is cycle-free by
construction, via persistence + linearity, and uses `rc<T>` essentially nowhere.
This is the "where we stand vs Rust" reference the audit was asked for.

### WR1 -- Surface a `weak<T>` API in `stdlib/rc.tur`

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

### WR2 -- Regression guard (keep stdlib cycle-free)

A lightweight CI check (grep-based or a small `tur` lint) that flags a new
`rc<T>` field on a `defstruct`/ADT in `stdlib/` -- especially a self-referential
or mutually-referential one -- so any future shared-ownership structure gets a
conscious review for a `weak<T>` break. The property is valuable precisely
because it currently holds trivially; the guard makes regressing it loud.

### WR3 -- Design-guidance doc: when to reach for what

Codify the principle stdlib already embodies, as guidance for stdlib and spice
authors: **persistent-immutable first, linear/affine handles for single-owner
resources, `rc<T>` only for genuine shared ownership, `weak<T>` to break any
resulting cycle.** Include the decision table and the "rc<T> that isn't pulling
its weight" smell (an `rc<T>` that could have been by-value/borrow/persistent).

### WR4 -- (Optional) Test fixtures for the weak API

A couple of fixtures exercising the WR1 surface: a parent/child structure made
leak-free with `weak`, and an `upgrade`-after-drop returning none. These also
seed the cycle-GC test corpus (`gc-cycle-collection-plan.md` CG7).

---

## Risks / notes

- This plan is deliberately small. The temptation is to over-build a weak/GC
  story; resist it -- the audit says stdlib doesn't need one yet. WR1's value is
  *readiness*, not current necessity.
- Do not "adopt `rc<T>`" anywhere in stdlib to justify the API. The library's
  avoidance of shared ownership is the healthy state; WR1 just stocks the
  toolbox.
