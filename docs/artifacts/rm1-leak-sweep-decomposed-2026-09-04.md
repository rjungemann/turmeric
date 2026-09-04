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
