# The Option niche's 16 -> 8 does not reach container elements: both representations box at parity

**Resolved 2026-09-03 by CE1/CE2** -- the container row reads 17.8 MB against
79.7 MB now; see the Resolution at the end.

**Severity: low** as a defect -- no wrong answer, no diagnostic, just an
optimisation that does not fire where the census said the value was. It is
load-bearing anyway: it is condition 3 of the option-niche graduation hold and
the exit gate of the container-element-form plan. Filed 2026-08-30 from the
2026-08-28 graduation measurement.

## Summary

`--enable=option-niche` carries an `(Option P)` over a non-nullable pointer AS
that pointer -- 16 bytes to 8, no tag word, no `tur_adt_Option__P` typedef.
**That headline does not apply to container elements as implemented.** A
`(Vec (Option String))` costs identical memory with the experiment on and off,
because both representations materialize the same heap carrier box at the
erased `vec-push!` boundary.

Measured (SR-family method: 2e6-iteration loops, wall + `ru_maxrss`, 3 runs,
-O2, shared `String` payload so only the Option representation is in the loop):

| workload | default (16B by-value) | niche (8B pointer) | delta |
|---|---|---|---|
| direct positions (construct + `some?` + branch) | 11-14 ms | 2-3 ms | ~5x faster |
| 2e6 `(Option String)` vec elements | 0.080 s / 79.8 MB | 0.071 s / 79.8 MB | **parity** |

So the niche's win is confined to direct positions -- locals, params, returns,
match, struct fields. That is where the census population (`env`,
`httpd-string`, `args`, `re`, `docstrings`) actually lives, but those are
request-scoped flows rather than hot loops, and the direct-position number
carries its own caveat: at -O2 a one-word value inlines and registers where a
16-byte aggregate does not, so a synthetic loop amplifies the gap.

## Root cause -- the box is not in Vec

`vec-push!` (`stdlib/vec.tur:120`) is element-agnostic inline-C over
`int64_t *data`; it moves an opaque word and never interprets it. The box is
minted at the **erased call boundary**: the element enters through
`vec-push!`'s `val : A` carrier parameter, and the concrete -> carrier crossing
(`emit_carrier_bridge`, emit_core.c) materializes the one form every erased
consumer agrees on -- the tagged carrier box. The read side undoes it at the
`(:: (vec-get v i) T)` ascription.

Both representations cross the same boundary, so both pay the same box. That
is exactly why the measurement shows parity rather than a regression, and it
is why this is a missing optimisation rather than a bug: store and read agree,
by both using the universal convention.

## Why "just store the word" is unsound as a local fix

The slot convention must be decidable at **every** site that touches the slot,
and two classes cannot decide it:

1. **Erased stores.** A generic body -- `(defn push-it [A] [v : (Vec A) x : A]
   (vec-push! v x))` -- receives `x` already boxed, because the CALLER boxed it
   at the erased call boundary before any container was in sight. Bare words
   from concrete stores and boxes from generic-body stores in one vec is two
   conventions in one container, indistinguishable to every reader.
2. **Inline-C higher-order bases.** `vec-eq?` (`stdlib/vec.tur:503`) hands raw
   slot words to its comparator from inside its own C loop (`:513`,
   `cmp(a->data[i], b->data[i])`) -- there is no per-element site where a
   compiler bridge could normalize. The callback's convention IS the slot's,
   decided at closure-creation time, possibly in another function.

A per-vec runtime "element form" header flag was priced and **declined**: new
libturi ABI, a branch in every push/get/free/eq native, and a second source of
truth for a fact the type system already holds.

## Fix direction

[docs/upcoming/container-element-form-plan.md](../upcoming/container-element-form-plan.md)
(CE) is the plan, and it is a corollary of end-to-end monomorphization rather
than a niche feature: the invariant is satisfied exactly when every base the
element crosses is a per-monomorph spec.

- **CE0 is done** -- census in
  [docs/artifacts/ce0-container-element-census.md](../artifacts/ce0-container-element-census.md).
  4699 element stores and 9021 reads across 2111 fixtures; **zero class-3
  (undecidable) sites reachable with a niche element on either half**, which is
  the favorable branch of its gate. The 6 `niche=yes` stores in the corpus are
  all class 1.
- **CE1-CE5 are unbuilt.** CE2 (Vec store + read, in one commit -- they are two
  halves of one convention) is the phase that breaks the parity row.

The exit gate is literally this report's table: niche allocations per element
drop to zero and the container row stops reading parity. Nothing extra needs
building inside the niche experiment itself -- the niche monomorph's slot form
is already the 8-byte word in repr, ctor sigs, and specs.

## Not an open representation cell

Deliberately not filed in the open-cells table of
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md),
despite living in the value-representation family. That table tracks crossings
with **no working bridge**; this crossing has one and it works. It materializes
a box the niche would rather not pay for, which is a different kind of finding,
and filing it as a cell would misreport a working bridge as a broken one.

## Related

- `docs/archive/sr3-option-niche-plan.md` -- "The graduation call", hold
  reason 3, and the container-boxing story sketched below it.
- `option-niche-graduation-breaks-carrier-some-null` -- hold reason 2.
- `docs/artifacts/ce0-container-element-census.md` -- and read its "What this
  census does NOT cover" section before relying on the zero: it counts emission
  sites, not call reachability.

## Resolution (2026-09-03)

CE1 and CE2 of the container-element-form plan are built. `container_elem_form`
(types.c) is the one chokepoint; the argument loop flags a Vec element-store
sink and the bridge's niche row hands the slot the payload word; the hoist
marks raw slot reads and the same row casts them back. Both halves through the
one crossing they already shared, so a double-bridge is unrepresentable.

| representation | wall | peak RSS |
|---|---:|---:|
| default (boxed per element) | 0.081-0.096 s | 79.7 MB |
| niche, word in slot | 0.018-0.019 s | 17.8 MB |

The exit gate named in this report -- niche allocations per element drop to
zero and the container row stops reading parity -- is met. On the way CE2
found the class-2 (spec) path had been double-wrapping the element and
reading it back unwrapped, under the experiment AND (as a build failure) on
the default path; both fixed. Pinned by `tests/fixtures/option-niche-vec-word`.
