---
title: RM1 leak sweep, decomposed by allocation site (2026-09-04)
category: Artifact
description: The erased-base leak sweep re-run and broken down by WHO allocated, not just how many bytes. The headline total was dominated by one fixture's hand-written test scaffold; RM1's own sum-box residue is ~240 B, and the largest real category is the recursive spine RM0 closed for want of a constituency.
---

# RM1 leak sweep, decomposed by allocation site (2026-09-04)

The sweep behind [reclamation-plan.md](../upcoming/reclamation-plan.md)'s RM1
reports a byte total per fixture.  That total is what the plan's "aside worth
its own report" priced at 8.3 KB and what makes the phase look bigger or
smaller than it is.  Re-run here and broken down by **which function
allocated**, which changes the reading substantially.

Method: build each fixture with `-fsanitize=address`, run under
`detect_leaks=1`, attribute each leak block to its first non-sanitizer frame.

## Totals

| | bytes | note |
|---|---:|---|
| plan's recorded sweep (2026-09-03) | 5643 | `rm1-leak-sweep-after.txt` |
| re-run before this commit | 2966 | later commits had already halved it |
| **after freeing one fixture's scaffold** | **1790** | this commit |

## Where the bytes actually are

| category | bytes | whose |
|---|---:|---|
| **fixture test scaffold** | ~~1176~~ 0 | **not the compiler's** -- fixed here |
| recursive sum spine (`ctor_Cons_*`, regex cells) | ~990 | **RM2** |
| `tur_string_from_bytes` payloads | ~250 | String ownership, neither phase |
| RM1 sum boxes (`ctor_Option_Some` / `ctor_Result_Ok` / `_Err`, `tur_box_some` / `_err`) | **~240** | **RM1** |
| poly aggregate-spill shim (`__tur_aggrspill___poly_*`) | ~48 | RM1-adjacent |
| program-owned containers (Vec buffers, zipper cells) | ~100 | the program's |

### 1. The largest single entry was a test rig

`httpd-req-string-opt` reported 1285 B, the biggest in the sweep by 2.3x, and
1176 of that was one `calloc(1, sizeof(HttpdConn))` in the fixture's OWN
inline C -- a struct that is mostly its `char path[1024]`.  The fixture builds
three fake conns by hand and freed none of them.

Fixed by giving the fixture the `free-conn` it never had.  1285 -> 109 B, and
the 109 that remain are real.  **A leak-check marker on a fixture that leaks
its own test rig measures the rig**, which is the reason this one carries
`requires.no-leak-check` and the reason the marker could not simply be widened
as the plan's aside proposed.

### 2. RM1's own residue is small

~240 B across eight fixtures, in units of 16-48 B.  The plan's account of what
is left -- "consumers outside the audited allowlist (user readers like
`opt-val`, monadic `bind` chains -- unstampable by design, a user instance may
retain)" -- matches what the traces show.  Nothing here contradicts it; the
phase has done most of what it can without monomorphization.

### 3. The largest real category is RM2's, which was closed for want of one

~990 B of `ctor_Cons_Cons__int` / `__cstr` / `__float` and `re-string`'s
regex cells: per-node spine boxes of self-recursive sums.  RM2 is gated on
RM0(b) and `leak-sweep-decomposition` records it as "RM2, which RM0 closed: no
constituency".

Two measurements now argue the constituency exists:

- **This sweep.** Once the scaffold is out, the spine is the biggest category
  in it -- 55% of what is left.
- **Growth, not just count.** A 64-link `Subst` chain leaks 64 boxes, and 100
  rounds of an 8-link chain leak 800, so a program that builds and discards
  recursive values in a loop grows without bound.  RM0 priced the spine as an
  allocation count in a corpus sweep, which cannot see that: every fixture
  builds its structure once.  A backtracking solver does not.

That is not a decision to reopen RM2 -- its own blocker ("a tree's nodes escape
their constructor by construction", so RM1's scope-exit rule cannot reach them)
is unchanged and real.  It is the evidence its gate asked for and did not have.

## Fixture-by-fixture

```
constrained-defn-cons-return-monomorphize  432B  ctor_Cons_Cons__{cstr,float,int}    SPINE
re-string                                  548B  re-find-from/step-class cells, strings, cons  SPINE+STR
option-niche-crossings                     183B  vec_new, tur_string_from_bytes, tur_box_some x2
httpd-req-string-opt                       109B  tur_string_from_bytes x5            (was 1285)
hkt-stdlib-result-ok-biased                 96B  ctor_Result_Ok x3, _Err x2, tur_box_err   RM1
refined-nonempty                            64B  ctor_Cons_Cons__int x4              SPINE
zipper-basic                                64B  zipper-move-right-raw               program
hkt-constrained-byvalue-bind-pure           80B  ctor_Option_Some x2, spec temp      RM1
conv-defstruct-option-hkt-instance-bodies   40B  main, ctor_Option_Some              RM1
hkt-stdlib-option-result-instances          40B  main, ctor_Option_Some              RM1
hkt-constrained-byvalue-carrier             32B  aggrspill poly, at-opt
hkt-constrained-hole-headed-instance-head   32B  ctor_Result_Err, aggrspill poly     RM1
option-niche-string                          38B  tur_string_from_bytes, tur_box_some
hkt-constrained-spec-reresolves-instance     16B  aggrspill poly
option-map-capturing-closure                 16B  ctor_Option_Some                   RM1
```

## What this says about the plan's aside

> None of these 24 fixtures carries `requires.leak-check`, so `bash tests/run.sh`
> has never seen any of it ... Widening the marker set is cheap and is not gated
> on RM1.

Cheap, but not as simple as adding markers: a fixture must first stop leaking
its own scaffolding, or the marker pins the rig rather than the compiler.  One
fixture is fixed here; the rest need their category resolved (spine -> RM2,
strings -> String ownership) before a marker on them means anything.

## Re-run 2026-09-05

The sweep reproduces **byte for byte** -- 1790 B, same per-fixture split -- on a
tree several commits further along, so the numbers above are stable and not an
artifact of the day they were taken.  Two things changed on re-reading it.

### `zipper-basic` was a second test rig, not "the program's"

The table above files its 64 B under "program-owned containers ... the
program's".  That reading is wrong in the way that matters: it is the FIXTURE's
program, and the fixture had a bug.  `test-move-right-focus` frees `z` and never
frees `z2` -- but `zipper-move-right-raw` mallocs a fresh struct AND fresh
left/right arrays rather than mutating, so the second zipper and both its arrays
(40 + 16 + 8) go unowned.  `test-focus`, right above it in the same file, gets
the discipline right.

Fixed here: 64 -> 0 B, and `zipper-basic` now carries `requires.leak-check`
(82 opted-in fixtures, all green).  Same shape as `httpd-req-string-opt` in the
section above, and the same lesson -- **two of the sweep's fifteen rows were
fixtures leaking their own rig**: 1176 + 64 = 1240 B, which is 42% of the 2966 B
the sweep totalled before either was fixed.  The remainder is 1726 B.

`stdlib/zipper.tur`'s `zipper-free-raw` got a null guard in the same change.
The API produces the null handle (`zipper-move-left/right-raw` return 0 at the
end of the tape) and the documented way to consume the Option is
`(unwrap-or opt (:: 0 (Zipper int)))`, so a caller following this very fixture's
pattern reaches `zipper-free` with 0 on the exhausted path.  Every other entry
point guards; that one dereferenced.

### The `__tur_aggrspill_*` rows are the erased dict path, not a category

The table lists "poly aggregate-spill shim" as its own ~48 B line, RM1-adjacent.
Traced through, it is not adjacent to RM1 -- it is the same erased path the
report already names, and the shim is where it becomes visible.

`hkt-constrained-spec-reresolves-instance` is the clean specimen.  Its
`poly-bind` calls `bind` through the dictionary, so the continuation crosses the
`tur_poly_fn_t` int64 ABI; the closure returns `tur_adt_Option__int` by value, so
`__tur_aggrspill___poly_1441` mallocs a 16-byte box purely to make the return fit
one word.  Write the same call with the dispatch statically resolved --

```turmeric
(defn main [] : int
  (println (unwrap-or (bind (:: (some 7) (Option int)) (fn [v] (some (* v 2)))) -1))
  0)
```

-- and **no shim is emitted at all**: the instance specializes to
`__inst_Monad_bind_Option__spec__...` returning the aggregate by value, and the
closure is passed as `__poly_1437` with no box.  So the row is not "a shim that
forgot to free"; it is the cost of erasure, and it goes to zero at
monomorphization, which is what the report has said through four narrowings.

Why the freshness machinery does not reach it, recorded so nobody re-derives it:
the box IS provably fresh, and the analysis can even see it -- inside
`poly_bind__spec`, `emit_call_returns_fresh_sum_box` re-resolves the dispatch to
`__inst_Monad_bind_Option`, whose `fresh_sum_via_param_mask` names the
continuation, whose body is a ctor.  The temp there is marked owned.  But the
spec **returns** that carrier rather than reading it back, and the owned mark
does not cross the function boundary: at the call site in `main`,
`call_returns_fresh_sum_box_as` asks `poly-bind`'s own binding, which
`elab_stamp_sum_freshness` stamped once, on the GENERIC body, where
`call_dispatch_is_static` is false because the receiver's type head is the
class variable.  Freshness is a per-monomorph property recorded as a
per-binding one.

Closing it means either an all-instances-agree stamp at elab time or a per-spec
"returns a fresh box" pass that runs before callers are emitted -- both real
work, both with double-free as the failure mode, for ~48 bytes across three
fixtures that monomorphization deletes outright.  Not taken up.

### One row WAS a compiler leak: the niche readback arm

The table's two `option-niche-*` rows (183 + 38 B) are filed under
`tur_string_from_bytes` and `tur_box_some` without a category.  48 B of that is
neither String ownership nor RM1 residue: `emit_carrier_bridge`'s **niche** arm
copied an inline-C producer's payload word out with `tur_opt_value_checked` and
never freed the box, while the by-value readback below it and the CE_WORD
Vec-slot store above it both did.  Fixed, pinned by
`tests/fixtures/option-niche-inline-c-box-freed` (leak-checked, verified 32 -> 0 B
against a pre-fix binary), written up in
[inline-c-option-carrier-box-leaks.md](../archive/inline-c-option-carrier-box-leaks.md).

That is the third of fifteen rows to change category on a second reading, and
the pattern is worth naming: **this sweep's per-fixture byte totals are a
starting point, not an attribution.**  Two rows were fixtures leaking their own
rig, one was a compiler bug filed under a runtime allocator's name.  Read the
frames, not the totals.

**Sweep total: 1790 -> 1678 B** (zipper scaffold 64, niche boxes 48).
