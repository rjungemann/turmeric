---
title: (eq? p1 p2) on a Pair whose first component is a niche option segfaults
category: Reported
description: The Eq[Pair] dispatch mints an Eq[Option] specialization with MISMATCHED parameter types -- one void*, one int64_t -- so the inner Eq[String] receives an integer where a pointer is expected. cc warns, the program segfaults. Pre-existing, verified against a compiler built before the comparator-convention series.
---

# `(eq? p1 p2)` on a Pair whose first component is a niche option segfaults

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
