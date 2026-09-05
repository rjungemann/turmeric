# The region escape walk refuses every ADT result, however simple

**Severity: low (a lost saving, never a wrong answer) -- but it is the single
blocker on RM2's largest reclaimable slice.** Filed 2026-09-05, split out of
[region-bracket-lost-when-bt-scope-specializes](../archive/region-bracket-lost-when-bt-scope-specializes.md)
when fixing that one moved the blocker here rather than closing it.

Under `--enable=regions`, a `bt-scope` whose result is a by-value record ADT now
opens a region (that was the other report) and then **retires** it --
`tur_region_pop`, which frees nothing -- instead of rewinding it with
`tur_region_pop_checked`. Correct, and worth nothing.

## Repro

```turmeric
(load "stdlib/trail.tur")
(defdata Link :heap (Link [v : int nxt : int]))
(defdata RPair (RIP :int :int))

(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (:: (Link n acc) :int))))
(defn chain-sum [c : int] : int ```c /* walk the spine, sum ->v */ ```)

(defn one-round [n : int] : RPair
  (bt-scope (fn [] (RIP n (chain-sum (build n 0))))))
```

```
$ ./build/tur --enable=regions emit-c repro.tur | grep tur_region_pop
    tur_region_pop(__tur_rgn_189);        <-- retires
```

Change `one-round`'s result to `: int` and it is `tur_region_pop_checked` --
that is `tests/fixtures/region-scope-value-survives`, which rewinds.

## Root cause

`region_type_reaches_node` (emit_expr.c) walks every ctor field and refuses on a
NULL `full_type`, whose comment describes only the self-recursive spine. But
`full_type` is populated for a type-VARIABLE field (TP1) and left **NULL for an
ordinary `:int`**, so `(RIP :int :int)` -- two machine integers, nothing to
reach -- is refused at field 0.

## Why the obvious widening is wrong, measured

Deciding a NULL-`full_type` field by its `kind` does not work, and the failure
mode is a use-after-free rather than a lost saving:

| field | declared | `kind` | `full_type` |
|---|---|---:|---|
| `RIP` field 0 | `:int` | `TY_INT` (3) | NULL |
| `MAcons` field 1 | `:MB` (an ADT) | `TY_INT` (3) | NULL |

A carrier-erased ADT field is indistinguishable from a machine integer by kind.
Accepting on kind made a mutually-recursive result reclaim its own spine:

```turmeric
(defdata MB :copy (MBnil) (MBcons :int :MA))
(defdata MA :copy (MAnil) (MAcons :int :MB))
(defn esc [n : int] : MA (bt-scope (fn [] (mkA n))))
(println (sumA (esc 6) 0))     ;; 42 with the flag off, 0 with it on
```

`def->is_self_recursive` does not catch that -- neither def is self-recursive on
its own. (The walk's cycle handling was hardened in the same change so the path
check catches it, but the kind ambiguity remains and is the real blocker.)

## Fix direction

**The declared type is reachable; it just is not where the walk looks.**
`c->field_forms[fi]` IS populated for a plain `defdata`, contrary to its own
comment in types.h ("NULL for plain defdata constructors") -- verified by
inspection at both fields of `MAcons`. So the widening needs form-to-type
resolution at emit time, which is a layering question rather than a missing
fact. Either:

1. Resolve `field_forms[fi]` to a Type in the walk (a type-namespace lookup the
   emitter can already do for a named ADT), or
2. Populate `full_type` for ctor fields at elaboration. The comments say a
   carrier-ADT `full_type` was deliberately avoided because it "misclassifies
   field READS", so this needs a separate slot rather than filling the existing
   one.

Whichever: **admit one shape at a time with a fixture per shape**, which is
RM3 R5's graduation item 2, and keep `region-scope-adt-result`'s mutual-recursion
case as the negative -- it is the one that actually produced a wrong answer.

## What it costs

312 B of [rm2-spine-residue-categorized](../artifacts/rm2-spine-residue-categorized.md)'s
category 2: `stdlib/re.tur`'s `re-find-from` builds and discards its `RxPos`
spine inside one call and returns `RxPair = (RxIP :int :int)`. It is the
textbook region, it now gets a bracket, and the bracket retires.
