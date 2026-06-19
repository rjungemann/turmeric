---
title: Retype `kleisli.tur` `comp` / `k-apply-raw` to thread a by-value `(Option B)` through the Kleisli arrow
category: Stdlib / ABI -- Option none-as-NULL retirement (Track A, step 5)
severity: Medium ergonomics + audit hygiene. The Kleisli arrow over Option
  threads its element through `k-apply-raw : int -> int -> int` and the
  `Category [Kleisli]` `comp` instance uses `(some? (:: r (Option int)))` +
  `(unwrap-or r 0)` to bridge the carrier int into the by-value `some?`
  consumer. Both of these are No-Lazy-`:int` violations: the Arrow's
  element type `B` is erased at the carrier boundary, the carrier-int
  Option is fed to a by-value-typed `some?` via a `(Option int)`
  ascription that silently mis-casts for non-int payloads, and the
  closure interior is dominated by `:int` plumbing. Retyping the surface
  to a by-value `(Option B)` removes the ascription, removes the
  type-erasure, and lets `Category [Kleisli]` be written in honest
  monadic style (`bind` on the by-value Option) without an inline-C
  carrier hop.
status: RESOLVED 2026-06-19 -- end-to-end monomorphization is complete and the
  small residual ABI bridge is intentional and necessary. The Kleisli arrow
  staying on the carrier ABI behind that bridge is no longer a defect to retire,
  so this step-5 retype is closed with no further work. Archived to
  docs/archive/. Prior status follows.
  OPEN, NOT YET STARTED. `k-apply-raw` / `k-apply` / `kleisli` /
  `Category [Kleisli]` / `ArrowZero [Kleisli]` remain on the carrier
  ABI in `stdlib/kleisli.tur`. The `(:: r (Option int))` ascription
  inside `comp`'s closure is the one in-tree `(Option int)` bridge that
  this plan retires; it landed in PR #426 alongside the `some?` retype
  as the minimum patch to keep `kleisli.tur` compiling against the new
  `some?` signature. **No further caller-ascription bridges will be
  added** -- the proper fix is the retype below, not more `(:: ...
  (Option int))` patches.
---

# Retyping the Kleisli arrow surface to by-value `(Option B)`

## Resolution (2026-06-19)

End-to-end monomorphization landed. The small ABI bridge that remains is
intentional and necessary, so the Kleisli arrow continuing to thread its element
through the carrier behind that bridge is accepted rather than a defect to chase.
This step-5 retype is closed with no further work. Report archived.

## Context

The Kleisli arrow over Option is the second worked example of the arrow
hierarchy (the first is `(->)` in `stdlib/arrow.tur`). It exists to
give `ArrowZero` an honest inhabitant -- `zero-arrow == \_ -> none` --
which `(->)` cannot provide. Today it threads its element type through
the int64 carrier:

```turmeric
(defopaque Kleisli [A B] :int)
(defn k-apply-raw [k : int x : int] : int ...)
(defn k-apply [k : Kleisli x : int] : int (k-apply-raw (:: k :int) x))
```

and the `Category [Kleisli]` `comp` method bridges through
`(:: r (Option int))`:

```turmeric
(definstance Category [Kleisli]
  (comp [f g]
    (let [fc (:: f :int) gc (:: g :int)]
      (kleisli (fn [x]
                 (let [r (k-apply-raw fc x)]
                   (if (some? (:: r (Option int)))
                     (k-apply-raw gc (unwrap-or r 0))
                     0)))))))
```

The ascription works at compile time only because the Arrow erases
`B` at the carrier boundary and the layout of `(Option int)` happens
to match the layout of any `(Option T)` whose `T` is also carrier-int
(`:int` element, `:cstr` element via pointer, opaque-handle element).
For a `(Option float)` element it would silently mis-read 8 bytes of
double as int64 and corrupt the value. The fact that no fixture
currently constructs a `(Kleisli A float)` is luck, not a guarantee.

## Why this is a retype, not a bridge

Two options were considered:

1. **Bridge at the Arrow boundary**: keep `k-apply-raw : int -> int -> int`
   and have callers ascribe `(Option int)` at every use. This is the
   current state and it is the No-Lazy-`:int` defect this plan retires.
2. **Retype the Arrow surface to thread `(Option B)` by value**: change
   `k-apply-raw` / `k-apply` to return `(Option B)`, change the
   `Category [Kleisli]` `comp` body to use by-value `(Option B)` directly,
   and drop the `(:: r (Option int))` ascription.

Option 2 is the plan.

## Target shape

```turmeric
(defopaque Kleisli [A B] :int)   ;; same carrier; the change is at the call-site type

(defn k-apply-raw [A B] [k : int x : A] : (Option B)
  ```c return TUR_APPLY1((int64_t)k, (int64_t)x); ```)

(defn k-apply [A B] [k : (Kleisli A B) x : A] : (Option B)
  (k-apply-raw (:: k :int) x))

(definstance Category [Kleisli]
  (ident [] (kleisli (fn [x] (some x))))
  (comp [f g]
    (let [fc (:: f :int) gc (:: g :int)]
      (kleisli (fn [x]
                 (let [r (k-apply-raw fc x)]
                   (if (.is-some r) (k-apply-raw gc (.value r)) (none))))))))

(definstance ArrowZero [Kleisli]
  (zero-arrow [] (kleisli (fn [_] (none)))))
```

Key changes from the current code:

- `k-apply-raw` declares `(Option B)` as its return so the inline-C cast
  produces a by-value `(Option B)` at the call boundary. The inline-C
  one-liner relies on a `TUR_APPLY1` that already returns the underlying
  int64 carrier; the change is purely the declared type spelling.
- The closure inside `comp` reads `.is-some` / `.value` on the by-value
  result. No `(:: r (Option int))` ascription, no `some?` carrier hop,
  no `(unwrap-or r 0)` short-circuit.
- `ident` constructs `(some x)` directly (no `(:: (some x) :int)`
  ascription).
- `ArrowZero` constructs `(none)` directly (no `0` carrier nil).

## Cascade

The Arrow change has two upstream prerequisites and two downstream
consumers:

### Prerequisite 1: by-value `(Option A)` -> by-value `(Option A)` works through `TUR_APPLY1`

`TUR_APPLY1` invokes a fat closure through a function-pointer table.
The closure body returns its `(Option B)` -- which, post-PR #421 and
PR #425, is a by-value aggregate when the payload is a by-value
primitive / by-value struct, and a carrier int when the payload is
unresolved (the spec-return-ABI consult in `emit_core.c` /
`emit_expr.c` handles this).

A closure whose declared return is `(Option B)` with `B` left as a
bare tyvar at the closure-cast boundary needs the carrier base, because
the function-pointer slot is the same width for every `B`. This is the
same constraint that landed in PR #421's spec-return-ABI consult.
Validate by writing a minimal repro before starting:

```turmeric
(let [f (fn [x : int] : (Option int) (some (* x 2)))]
  (println (.value (k-apply-raw (:: f :int) 7))))   ;; expect 14
```

If this fails, file a fresh report and pause this plan.

### Prerequisite 2: `unwrap-or` cascade landed (optional)

The `comp` body above no longer calls `unwrap-or`, so this plan can land
independently of [unwrap-or-byvalue-cascade](unwrap-or-byvalue-cascade.md).
If `unwrap-or` is still on the carrier when this lands, that does not
block this plan; the `comp` body just uses `.is-some` / `.value`
directly.

### Downstream consumer 1: `tests/fixtures/kleisli-arrow-instance/`

The fixture currently constructs `(kleisli (fn [x] (some x)))` and
ascribes its result through carrier int (`(:: ... :int)`) to read
`some?` against the carrier convention. After the retype, the fixture
drops the ascription and reads `some?` / `.value` against the by-value
result directly.

### Downstream consumer 2: any spice or stdlib module using `Kleisli`

`grep -rln "Kleisli\|k-apply\|kleisli " stdlib/ tests/fixtures/
../turmeric-spices/` before starting. Today the answer is "only the
arrow-instance fixture and stdlib/kleisli.tur itself"; verify nothing
new has appeared.

## Strategy

Single PR, because the surface is small (one module, ~90 lines) and
self-contained:

1. Apply the target-shape rewrite above to `stdlib/kleisli.tur`.
2. Update `tests/fixtures/kleisli-arrow-instance/input.tur` to drop
   the carrier ascription on the Arrow result and the `(:: r (Option
   int))` style bridge.
3. Add a regression fixture `tests/fixtures/kleisli-non-int-element/`
   constructing a `(Kleisli int float)` (or similar non-int element)
   to prove the retype catches what the carrier convention would have
   silently mis-cast. This is the value-add that justifies the retype:
   the `(Option float)` payload now round-trips through `k-apply-raw`
   and `comp` without a width mismatch.
4. Regenerate `expected.c` for any affected snapshot.
5. Green `bash tests/run.sh`. No new `(:: .* (Option int))` strings
   introduced anywhere in the diff.

If prerequisite 1 fails the minimal repro, stop and file the gap; do
not introduce caller-ascription patches to make the retype "land
anyway."

## Validation

- `stdlib/kleisli.tur`: pure-Turmeric `comp` body; no
  `(:: r (Option int))` ascription; no `(unwrap-or r 0)`. The inline-C
  `k-apply-raw` body is unchanged; only its declared return type moves.
- `tests/fixtures/kleisli-arrow-instance/input.tur`: no carrier
  ascriptions at Arrow boundaries.
- `tests/fixtures/kleisli-non-int-element/`: a `(Kleisli int float)` (or
  `cstr`) round-trips a non-int Option payload correctly. New fixture
  added in the same PR.
- Full suite green (~1442 fixtures; expect to add 1).
- No new `(:: .* (Option int))` or `(:: .* :int)` strings introduced in
  `stdlib/kleisli.tur` or the affected fixtures.

## Out of scope (explicitly)

- The `unwrap-or` retype -- tracked under
  [unwrap-or-byvalue-cascade](unwrap-or-byvalue-cascade.md). The `comp`
  body in this plan does not call `unwrap-or`, so the two plans are
  independent.
- The NonEmpty / `ne-from?` retype -- tracked under
  [ne-from-byvalue-option-nonempty-element-type-uninferable](
  ne-from-byvalue-option-nonempty-element-type-uninferable.md).
- A `Monad [Option]` typeclass surface for `>>=` (the natural successor
  to this retype). The hand-rolled `(if (.is-some r) ... (none))` in
  `comp` is fine for now and matches the existing arrow-instance style.
- Any caller-ascription bridge as a permanent solution.

## Related

- `docs/reported/option-consumer-retype-byvalue.md` -- the umbrella;
  this file is its step 5 broken out for independent tracking.
- `docs/reported/unwrap-or-byvalue-cascade.md` -- sibling cascade
  (independent of this one).
- `docs/reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md`
  -- sibling NonEmpty cascade.
- `stdlib/kleisli.tur` (the retype target).
- `stdlib/arrow.tur` (the typeclass hierarchy whose `ArrowZero`
  inhabitant `Kleisli` provides).
- `tests/fixtures/kleisli-arrow-instance/` (the existing regression
  coverage to update).
