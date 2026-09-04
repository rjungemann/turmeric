---
title: (eq? p1 p2) on a Pair whose first component is a niche option segfaults
category: Archive
description: The nested-dispatch spec path forced only parameter 0 to the resolved receiver, so a binary class method's second parameter kept the erased carrier and one Eq[Option] spec came out (void *, int64_t). RESOLVED 2026-09-04 -- a parameter declared with the receiver's own erased type IS the class variable and resolves with it.
---

# `(eq? p1 p2)` on a Pair whose first component is a niche option segfaults

**RESOLVED 2026-09-04.**  Root cause and resolution at the bottom.

**Severity: medium** -- a SEGV on the DEFAULT path for a shape `tur check`
accepts, in the default equality operator.  Found 2026-09-04 while probing
`pair-eq?`; **pre-existing**, not fallout from the comparator-convention work
(see Attribution).

## Repro

```turmeric
(load "stdlib/string.tur")
(load "stdlib/pair.tur")
(defn main [] : int
  (let [x  (:: (some (string/from-cstr "aa")) (Option String))
        y  (:: (some (string/from-cstr "bb")) (Option String))
        p1 (:: (pair x 1) (Pair (Option String) int))
        p2 (:: (pair y 1) (Pair (Option String) int))]
    (println (if (eq? p1 p2) "s-eq" "s-ne"))
    0))
```

```
warning: passing argument 2 of '__inst_Eq_eq_qu_String' makes pointer from
         integer without a cast [-Wint-conversion]
   bool __ps_306 = (__inst_Eq_eq_qu_String(vx_940, vy_941));
note: expected 'void *' but argument is of type 'int64_t'
Segmentation fault
```

The specialization the diagnostic names is the tell:

```
__inst_Eq_eq_qu_Option__spec__bool_void___int64_t
```

One `void *` and one `int64_t` in the SAME `Eq[Option]` spec -- the two sides
of a binary equality minted with different representations for the same type.
The `(Some vx)` / `(Some vy)` binders then disagree, and `Eq[String]` gets an
integer where it wants a pointer.

`cc` warns rather than errors, so this reaches a running binary and dies there.
The warning is the only signal, and only if anyone reads it.

## Scope

- Only the SYNTHESIZED path, `(eq? p1 p2)`.  A hand-written comparator through
  the `pair-eq?` macro is fine in both spellings (pinned by
  `tests/fixtures/niche-elem-comparator-conventions`, group 5).
- `(eq? ...)` over `Vec`, `Option`, `Result` and `Cons` of the same niche
  element is fine (same fixture, group 1).  Pair is the outlier.
- Not yet probed: whether a Pair of some OTHER two-representation pair of
  components (a niche option beside a by-value struct, say) hits the same
  mismatch, and whether `Map`/`Set` of a Pair does.

## Attribution -- verified, not assumed

The obvious suspicion is the 2026-09-04 comparator-convention series, which
added `Pair` to the containers whose synthesized comparator is marked as
receiving slot words.  It is not that:

- Removing `Pair` from `container_helper_passes_slot_words` does not change the
  segfault.
- Nor does reverting the direct-application marking added the same day.
- A compiler built from `581902f6` -- before the first commit of that series --
  reproduces it identically, from a `git worktree` so the working tree was
  untouched.

Worth stating because the failure surfaced *while* that work was in flight,
which is exactly when a pre-existing defect is most likely to be misfiled as a
regression and "fixed" by reverting something unrelated.

## Root cause

Not established.  The mismatch is minted somewhere in the `Eq[Pair]` dispatch's
choice of `Eq[Option]` specialization: the two argument representations should
be the same and are not.  `elab_typeclasses.c`'s dispatcher synthesis and the
ABI-spec minting in `emit_module.c` are where to look; the spec NAME
(`bool_void___int64_t`) is the artifact to trace back, since it records both
representations at the point they were already wrong.

## Fix direction

Find where the two sides of a binary method's specialization can be given
different representations for one type, and make the second follow the first.
A `cc` `-Wint-conversion` on an `__inst_*` symbol is a good ratchet candidate
independently: the emitter should never produce one, and the existing
`tests/check-cc-warn-ratchet.sh` is the place it would live.

## Guides to update when fixed

- None known; this is a codegen defect with no documented behaviour attached.


## Root cause (established 2026-09-04)

`emit_module.c`'s NESTED-dispatch spec path -- the one that mints a spec for a
class method called from inside another instance's specialization:

```c
for (uint8_t i = 0; i < n_spec_args; i++) {
    if (i == 0 && !return_dispatch) {
        arg_types[0] = *resolved;          /* the concrete receiver */
    } else {
        arg_types[i] = emit_abi_instantiate_type(&fd->params[i]->type, eb, enb, ...);
    }
}
```

Only parameter 0 was forced to the resolved receiver.  Its own comment spells
out the assumption -- "Other params instantiate their erased declared type
through the element bindings" -- which is right for a method whose remaining
parameters are elements, and wrong for a BINARY method like `(eq? [x y] ...)`
whose second parameter is also the class variable.  The class variable is not
an element tyvar, so `emit_abi_instantiate_type` had nothing to substitute and
left it the erased carrier: one `Eq[Option]` spec minted `(void *, int64_t)`,
matching `x` as a niche option and `y` as a tagged box.

A parameter declared with the receiver's own erased type IS the class variable
-- the tyvar was erased at instance elaboration, the same fact the sibling
substitution on the owned path already relies on -- so it now resolves to the
same concrete receiver.

The direct `(eq? o1 o2)` spelling was always fine because it goes through the
OWNED path, whose loop substitutes every parameter through the class-var
branch.  Only the nested dispatch took this path, which is why the defect
needed a container's instance body to surface at all.

## The defect was never niche-specific

Worth recording, because it changes what this was: the same mismatch was minted
for every binary class method reached this way.  A `(Pair (Vec int) int)` built
`__inst_Eq_eq_qu_Vec__spec__bool_tur_adt_Vec__int___int64_t` -- the identical
`(concrete, carrier)` shape -- and merely WARNED, answering correctly, because
both representations are pointers to the same layout.  It became a SEGV only
where the two representations genuinely diverge, as a niche pointer and a
tagged box do.  So the crash was the visible tail of a general defect, and the
fix removes the warning from the quiet cases too (that spec is now
`..._tur_adt_Vec__int___tur_adt_Vec__int__`).

## What this does NOT fix

Two `Pair` monomorphs in one program that SHARE a component type still
cross-bind their per-component dispatch, filed as
[eq-two-pair-monomorphs-sharing-a-component-cross-bind](../reported/eq-two-pair-monomorphs-sharing-a-component-cross-bind.md).
This fix measurably improves that repro -- 5 `-Wint-conversion` warnings before,
2 after -- but does not close it, and "better" is not "fixed".  It is a
different site: the damage there is in the spec BODY, not the key.

That is exactly what this report's own "Scope" section listed as not yet
probed, which is the argument for writing such a list down.

## Validation

- `bash tests/run.sh` **2787 passed / 0 failed, zero snapshot drift** -- notable
  for a spec-minting change on the default path, and the reason to run the full
  snapshot suite for one: the widening fires only where a mismatched spec was
  already being minted.
- `tests/fixtures/eq-nested-binary-class-method-spec` pins four shapes: the
  crashing `(Pair (Option String) int)`, a Pair nested inside a Pair, a Pair
  reached through `Eq[Vec]`, and the warning-only `(Pair (Vec int) int)` twin.
  The C-level guard is the primary assertion -- `run.sh` fails any fixture
  whose build stderr carries `-Wint-conversion`, so this shape simply never had
  a fixture, which is the whole reason it survived.
- option-niche seam 10/0, sr2 55/0, sr4 24/0.
