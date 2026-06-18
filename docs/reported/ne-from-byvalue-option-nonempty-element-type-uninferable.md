---
title: Close the `ne-from?` inference gap by retyping its list parameter (NonEmpty by-value retype, plan)
category: Stdlib / Type inference -- Option none-as-NULL retirement (Track A, step 4 NonEmpty half)
severity: Medium ergonomics / expressiveness hole. Retyping `ne-from?` from the
  carrier `:int` Option to a by-value `(Option (NonEmpty A))` is unblocked on
  the codegen side (see Related), but the retype currently has no witness for
  the element type `A`: `ne-from?`'s list arg is `xs : int`, the untyped
  carrier, so `A` stays a free tyvar at every call site and downstream
  consumers (`ne-head`, `println`) fail to resolve an overload for a bare
  tyvar. The principled fix is to give `ne-from?` a typed list parameter so
  `A` is recovered from the argument; pushing the type onto callers via
  `(:: ... (Option int))` ascriptions is **not** a path we will take.
status: OPEN. `ne-from?` / `ne-unwrap` remain on the carrier ABI in
  `stdlib/refined.tur` with a pointer to this plan. **No caller-ascription
  workaround is in the tree**, and none will be added: the workaround would
  spread carrier-`:int` reasoning across every NonEmpty call site, defeats
  the No-Lazy-`:int` rule, and is the kind of "tighten the types later"
  patch the CLAUDE.md guidance explicitly forbids. The BoundedIdx half of
  step 4 (`bidx-of?` / `bidx-unwrap` -> by-value `(Option BoundedIdx)`) IS
  landed (PR #431) -- BoundedIdx is ground, so it has no element-type
  ambiguity and did not need this work.
---

# Closing the `ne-from?` inference gap with a typed list parameter

## Context

`docs/reported/option-consumer-retype-byvalue.md` step 4 retypes the refinement
smart constructors to by-value `(Option X)`. The two compiler bugs that gated
the codegen side are resolved:

- `docs/archive/zero-arg-construct-ground-byvalue-return.md` (0-arg `(none)`
  in a ground `(Option T)` return) -- fixed in PR #430.
- `docs/archive/parametric-option-return-clone-struct-app-leak.md` (compiler
  leak on a doubly-nested parametric return) -- no longer reproduces on the
  current tree (verified with LSan on the exact repro).

The **BoundedIdx half** of the retype landed in PR #431:
`bidx-of? : (Option BoundedIdx)` and `bidx-unwrap [o : (Option BoundedIdx)]`
are now pure-Turmeric by-value, and `tests/fixtures/refined-bounded-idx/`
ascribes nothing at its `(some? ...)` sites. BoundedIdx is *ground*, so the
Option's element is fully determined and no inference work was needed.

## The NonEmpty blocker

`NonEmpty` carries a *phantom* element type `A` (`(defopaque NonEmpty [A] :int)`).
The current smart constructor recovers a `NonEmpty` from a raw carrier list:

```turmeric
(defn ne-from? [xs : int] : int ...)   ; carrier signature, in-tree today
```

A by-value retype would want:

```turmeric
(defn ne-from? [A] [xs : ???] : (Option (NonEmpty A)) ...)
```

If we leave `xs : int`, `A` appears only in the return position with no
argument witness, and at a call site it stays a free type variable:

```turmeric
(let [o (ne-from? (tcons 7 (tnil)))]      ; A unconstrained -- (tcons ...) is :int
  (if (some? o) (println (ne-head (ne-unwrap o))) (println -1)))
```

`(ne-head (ne-unwrap o))` has type `A` (a bare tyvar), and:

```
error [TUR-E0006]: operator lookup failed for 'println': got 1 arg(s),
                   first arg type tyvar
```

The carrier version sidesteps this entirely: it returns a bare `:int`, so
there is no `A` to infer, and `ne-unwrap` returns the concrete `(NonEmpty int)`.

## Why caller-ascription is not the plan

A `(:: o (Option int))` (or `(:: o (Option A))` with an `A` ascription on
the let-binding) at every NonEmpty call site would make the example compile,
but it is the wrong tool:

- It propagates the carrier-`:int` convention into every consumer of
  `ne-from?`, exactly the No-Lazy-`:int` defect this retype exists to
  remove.
- It freezes the element type at the call site rather than recovering it
  from the value, so a `(NonEmpty float)` produced from a `:float` list
  silently truncates through an `int` ascription -- the same class of
  "works by luck because the carrier widths match" defect CLAUDE.md
  forbids.
- It is exactly the "we'll tighten the types in a follow-up" pattern the
  project rule names as a never-happens follow-up.

The principled fix is to give `ne-from?` a witness in its argument type.

## The plan: typed list parameter

Retype `ne-from?` so its list argument carries the element type:

```turmeric
(defn ne-from? [A] [xs : (List A)] : (Option (NonEmpty A))
  (if (list-empty? xs) (none) (some (:: (list->carrier xs) (NonEmpty A)))))
```

where `(List A)` is the typed list type whose element witness is `A`.
`A` is then recovered from the argument at every call site, the return
type is fully determined, and downstream `ne-head` / `ne-tail` /
`println` resolve normally.

### Choosing the typed-list spelling

Three options for `(List A)`; we pick (1).

1. **`(defopaque List [A] :int)` over the existing carrier.** Adds a
   phantom-typed wrapper around the int64 cons-list pointer the rest of
   stdlib already uses. Zero runtime cost. Construction goes through a
   `list-of` smart constructor (or an explicit `(:: xs (List A))`
   ascription on a `(tcons ...)` chain whose head type pins `A`).
   `(Cons A)` (`stdlib/list.tur`) stays the cons-cell struct for typed
   head/tail access; `(List A)` is the "either empty or a chain of Cons
   cells of A" view that `ne-from?` consumes.

2. **Take `(Cons A)` directly.** A `(Cons A)` is already non-empty by
   construction, so `ne-from?` would degenerate into the identity-up-to-
   ascription wrapper of `ne-of` -- the whole point of `ne-from?` is to
   accept a possibly-empty list and emit `(none)`. Rejected.

3. **`(defopaque List [A] (Option (Cons A)))`.** Cleanest semantically
   (encodes "empty-or-cons" in the type), but cascades immediately into
   the by-value Option ABI for every list operation -- a much larger
   change than this plan should land in one step. Defer until a later
   pass that retires the int64 cons-list carrier wholesale.

Option 1 is additive (`(List A)` sits alongside the existing `:int`
carrier; existing list helpers keep working), pins the element type at
the boundary, and matches the BoundedIdx pattern (a `defopaque` over
`:int` carrying the proof).

### Cascade

Adding `(List A)` does not require retyping the whole NonEmpty surface
in one shot. The minimum is:

1. `stdlib/list.tur`: introduce `(defopaque List [A] :int)`,
   `(defn list-of [A] [& xs : A] : (List A) ...)` (variadic, recovers
   `A` from the rest type), `(defn list-empty? [A] [xs : (List A)] : bool
   ...)`, `(defn list->carrier [A] [xs : (List A)] : int ...)`. These
   are thin wrappers over the existing carrier helpers; no codegen
   changes.
2. `stdlib/refined.tur`: retype `ne-from?` to take `(List A)` and return
   `(Option (NonEmpty A))`; retype `ne-unwrap` to
   `[A] [o : (Option (NonEmpty A))] : (NonEmpty A)` with a pure-Turmeric
   `(.value o)` body. Drop the inline-C carrier bodies and the carrier
   ABI comment block.
3. `tests/fixtures/refined-nonempty/input.tur`: switch the call sites
   from `(ne-from? (tcons 7 (tnil)))` to `(ne-from? (list-of 7))` (or
   `(ne-from? (:: (tcons 7 (tnil)) (List int)))` for the explicit form).
   Drop the `(:: o (Option int))` ascription on the `some?` check and
   the `(ne-head (ne-unwrap o))` chain reads the element type from
   `(List int)`.
4. **No caller-ascription regression.** A grep for
   `(:: .* (Option int))` and `(:: .* (NonEmpty int))` across `stdlib/`
   and `tests/fixtures/refined-nonempty/` after the change must come up
   clean.

`ne-of` / `ne-head` / `ne-tail` / `ne->list` / `ne-len` keep their
current `[A]` typed signatures; they already take `(NonEmpty A)`, not a
raw carrier list, so they do not appear in this cascade. `ne->list`'s
return type stays `:int` (the underlying carrier) for now; a follow-up
can retype it to `(List A)` once the rest of stdlib threads typed lists.

### Optional follow-up (not in this plan)

Retyping the broader list surface (`list-head`, `list-tail`,
`list-length`, etc.) to take `(List A)` is a separate, larger pass.
This plan is deliberately scoped to the `ne-from?` boundary: the typed
wrapper is the witness, downstream operations stay on the carrier until
a future pass addresses them. That keeps the change small enough to
land cleanly and avoids a stdlib-wide regen.

## Validation

- `tests/fixtures/refined-nonempty/input.tur` calls
  `(some? o)` / `(ne-unwrap o)` directly with no `(Option int)`
  ascription; `(ne-head ...)` resolves with the element type concrete
  (not a bare tyvar). `expected.stdout` is unchanged (`7\n-1\n`).
- A new fixture `tests/fixtures/refined-nonempty-typed-list/`
  exercises `(ne-from? (list-of 1.5 2.5))` to prove the `A = float`
  case threads through `ne-head` without truncation -- the kind of
  silent miscompile a caller-ascription workaround would have hidden.
- Full suite green (~1442 fixtures; expect to add 1).
- No new `(:: ... (Option int))` or `(:: ... (NonEmpty int))` strings
  introduced in `stdlib/` or `tests/fixtures/`. Grep before opening
  the PR.

## Out of scope (explicitly)

- Retyping `unwrap-or`, `kleisli.tur` `comp`/`k-apply-raw`, or the
  ~10 stdlib modules that produce carrier-int Options -- tracked
  separately under `option-consumer-retype-byvalue.md` step 5.
- Retyping the rest of the list API to `(List A)` -- a separate pass,
  see "Optional follow-up" above.
- Any `(:: ... (Option int))` style bridge at a NonEmpty boundary.
  If a call site cannot be expressed without one, that is a finding to
  report, not a workaround to ship.

## Related

- `docs/reported/option-consumer-retype-byvalue.md` (step 4, NonEmpty half).
- `docs/archive/zero-arg-construct-ground-byvalue-return.md` (the ground
  fix that landed the BoundedIdx half).
- `docs/archive/parametric-option-return-clone-struct-app-leak.md` (the
  leak that no longer reproduces).
- `stdlib/list.tur` (`(Cons A)` cons-cell struct; the `(List A)` opaque
  introduced by this plan sits alongside it).
- `stdlib/refined.tur` (`ne-from?` / `ne-unwrap` -- the retype target).
