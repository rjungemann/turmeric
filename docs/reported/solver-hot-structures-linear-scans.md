# The refinement solver's hot structures are linear scans

**Severity:** low today, structural. Every one of these is inside a cap, so none
of them can hang a compile -- they make the caps bite sooner than they need to,
which costs completeness rather than time. Worth fixing when the surrounding
phase touches the code anyway; not worth a standalone project.

**Status:** OPEN. Not fixed -- filed while working through the solver-extension
plan (SX0-SX2), which is where the natural fixes live.

## 1. `euf_index` interns terms by linear scan -- O(n^2) in term count

`src/compiler/refine_solver_euf.c:63`

```c
static uint32_t euf_index(EufState *st, VCTerm *t) {
    if (!t) return UINT32_MAX;
    for (uint32_t i = 0; i < st->n; i++) if (st->terms[i] == t) return i;   /* <-- */
    ...
```

Every subterm registration scans the whole term array. Registering `n` terms is
O(n^2) pointer compares, and `REFINE_MAX_EUF_TERMS` is 512, so the worst case is
~131k compares per EUF state -- rebuilt per cube today, up to 64 cubes.

The lookup is by POINTER identity on a hash-consed `VCTerm`, so a pointer-keyed
hash table drops it to O(1) amortized with no change in semantics. `euf_equal`
(line 96) has the same scan.

**Where the fix belongs:** SX3 (incremental EUF) rewrites this state to carry
mark/undo anyway. Doing the index there is nearly free; doing it now is a
separate change to code that is about to be rewritten.

## 2. The congruence-closure fixpoint is a naive O(n^2) sweep

`src/compiler/refine_solver_euf.c:117,131` -- re-scan all pairs until nothing
changes. The file header says so explicitly and calls it the right tradeoff at
this size, which it is. Recorded here only so the two scans are not confused:
this one is the ALGORITHM (and fixing it means Nieuwenhuis-Oliveras, i.e. SX3
proper), while #1 is an implementation detail with a free fix.

## 3. `collect_shared` scans every EUF term per S3 cube

`src/compiler/refine_solver_no.c:29`. It always did; SX0(b) made it scan past
the cap too, to count what it discards for the telemetry. Bounded by
`REFINE_MAX_EUF_TERMS` (512) per cube, so ~32k iterations of a cheap predicate
in the worst case, and only on obligations that reach S3. Called out for
completeness, not because it is worth changing on its own.

## Related, and being addressed

`stdlib/logic.tur`'s `Subst` is a persistent association list
(`SBind :int :Term :Subst`) walked linearly by `subst-lookup`, so unification
over `n` bindings is O(n^2) and `logic-walk` is O(n) per variable. This is
exactly what SX2 of the solver-extension plan exists to measure a replacement
for -- see that phase rather than treating it as a separate finding.

## 4. The persistent-structure per-operation constant (~200 ns)

SX2's head-to-head (`benchmarks/logic-subst-results.md`) measured
`stdlib/logic.tur`'s real `Subst` at **~190-230 ns per bind+walk**, and that
figure is roughly FLAT from n=1 to n=16 -- it only starts climbing past n=128.
A flat cost is not the O(n) scan of finding #0 below; it is a large constant
paid per operation regardless of size, and at every size a real query reaches it
is the constant, not the asymptotics, that dominates.

Each such operation is one `SBind` construction, one `TInt` construction, a
`subst-lookup` match and a `term-int-val` match. 200 ns is a lot for that, and
where it goes is NOT established here. An attempt to isolate it with a
microbenchmark was optimized away entirely -- a tail-recursive loop whose result
went unused folded to nothing, the same trap SX0(a)'s closure baseline hit -- and
a number from a folded loop is worse than no number.

**What would answer it:** a microbenchmark whose result is genuinely consumed
(SX0(a)'s `sink` pattern, plus a callee the optimizer cannot devirtualize),
comparing bare loop / one ADT construction / construction plus match. If the
cost is in allocation, it is shared by every persistent structure in the stdlib
and is the single highest-leverage perf item in the tree; if it is in the match
or the tail-call lowering, it is a different fix entirely. Worth doing before
anyone optimizes on a guess.

## Context

Turmeric has not had a deliberate performance pass. These are not regressions;
they are first-implementation choices that were correct when the structures were
small and are worth revisiting as the surrounding phases touch them. Filing them
so they are not rediscovered from scratch each time.
