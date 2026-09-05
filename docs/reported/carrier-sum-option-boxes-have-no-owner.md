# Carrier-riding sum Option/Result boxes have no automatic owner

**Severity: medium (memory growth in long-lived carrier-path programs).**
Filed 2026-08-27 during SR2b.

**Narrowed 2026-08-27 (SR3 slice A):** `(none)` no longer allocates -- the
carrier None is the null pointer (`adt_ctor_is_null_none`, types.c).  The
report now covers `(some x)` / `(ok x)` / `(err e)` boxes only.

**Narrowed again 2026-08-27 (SR2a graduation), and this is most of it.** A
CONCRETE `(Option T)` / `(Result T E)` monomorph now flows by value, so its
constructor is a struct literal and there is no box to own.  What remains is
the ERASED path only: a generic base (`some`, `ok`, an instance method's
carrier base) whose element is still a type variable mallocs the tagged layout
and hands back the pointer.  `arc-weak-upgrade`, the repro below, is by-value
now -- its explicit `option-free` calls had to be REMOVED, because by value
they free a stack slot.  The residue shrinks further with each site that
monomorphizes; end-to-end monomorphization is where it reaches zero.

**Narrowed a third time 2026-08-30 (RM1), and this is most of what was
left.**  The erased residue now HAS an owner for the audited consumer set:
`returns_fresh_sum_box` (a per-callee freshness analysis -- every value path
mints a fresh box or NULL) plus two drop mechanisms (free-after-accessor-call
and free-at-scope-exit) close the `(ok? (ok 1))` / instance-body shapes, which
the corpus sweep showed were the bulk: 8324 -> 7364 bytes across every
erased-base caller in the tree, with the `hkt-stdlib-*` fixtures leaving the
leak list entirely.  What remains open is exactly the unstampable residue: a
box handed to a consumer OUTSIDE the audited read-only allowlist (user-defined
readers, dictionary-dispatched `bind`/`fmap` chains -- a user instance may
retain its argument, so those can never be stamped by name).  That residue
reaches zero where this report always said it would: end-to-end
monomorphization.  Mechanism and measurements:
[reclamation-plan.md](../upcoming/reclamation-plan.md), RM1.

## Summary

SR2b made stdlib Option/Result real sums.  On the default path a `(some x)` /
`(ok x)` / `(none)` construction mallocs a tagged monomorph box and hands back
the pointer as the int64 carrier -- and nothing frees it unless the caller
calls `option-free` / `result-free` by hand.  Before SR2b `(Option A)` /
`(Result A B)` over word-sized elements were BY-VALUE record products: no
allocation, so nothing needed an owner, and almost no caller frees today.

## Repro

`tests/fixtures/arc-weak-upgrade` (links ASan-built libturi, so LeakSanitizer
runs on the fixture binary): before the fixture added explicit
`(option-free (:: u :int))` calls, each `arc-upgrade` leaked its 16-byte
Option box.  Any fixture that links `-lturi` and constructs Options in a loop
shows the same growth.

## Root cause

`ctor_Some__*` / `tur_box_some` / `none()` malloc the tagged layout
(emit_module.c preamble, types.c monomorph ctor emission); the elaborator's
release machinery does not track sum ctor boxes the way it tracked the
(non-allocating) record path, so the box escapes with no release point.

## Fix directions

The second of the two original directions -- move the default path to by-value
flow so the box never exists -- is DONE for concrete monomorphs (the SR2a
graduation, 2026-08-27).  For the erased residue the options are unchanged:
teach the release pass to treat a carrier sum box like other compiler-owned
temporaries (free at the end of the binding scope unless it escapes), or
monomorphize the site so it stops being erased.  The second is where the track
is already heading.

## Workaround

Callers that care (long-lived processes, leak-checked binaries) free the box
explicitly: `(option-free (:: o :int))` / `(result-free (:: r :int))` after
the payload has been read out.

## Narrowed again: bind chains (2026-09-02)

The erased residue's largest rows were `bind` / `fmap` chains over the stdlib
`Result` / `Option` instances. They are owned now, at statically resolved
dispatch sites only: instance methods carry the same inferred non-retaining
masks a defn does, freshness is tracked through a continuation parameter
(`fresh_sum_via_param_mask`), a fresh producer read back by value marks its
carrier owned for the bridge to free, and the closure-argument hoist reaches
dispatch calls. Corpus sweep 7200 -> 5643 B (with the SR4 flip and the comparator shim-box
fix); both `result-monad-*-bind-typed-boundary` fixtures, `result-typed-basic`
and `typed/result-basic` are fully clean. The residual attribution is in
[leak-sweep-decomposition.md](../artifacts/leak-sweep-decomposition.md): what
is left is fixture scaffolding, recursive spines, and dictionary-dispatch
sites. A dynamic dispatch (abstract receiver inside a constrained
generic) is freed only after the emitter re-resolves the instance per
monomorph -- the first round had read a representative instance's flag there,
which was unsound. Details in
[reclamation-plan.md](../upcoming/reclamation-plan.md), RM1.

## Re-measured 2026-09-05, and two rows re-attributed

The sweep reproduces byte for byte (1790 B, same per-fixture split), so the
figures above are stable.  Re-reading it moved two rows, both toward "smaller
than recorded":

- **`zipper-basic`'s 64 B was a test rig**, not the compiler's -- the fixture
  frees the zipper it started with and not the fresh one `zipper-move-right`
  hands back.  Fixed; the fixture now carries `requires.leak-check`.  That makes
  **two** of the sweep's fifteen rows fixtures leaking their own scaffolding,
  1240 B between them.  (`stdlib/zipper.tur`'s `zipper-free-raw` got the null
  guard it was missing in the same change: the API returns the null handle at
  the end of the tape and this fixture's own `unwrap-or ... (:: 0 (Zipper int))`
  idiom hands it straight back to `zipper-free`.)
- **The `__tur_aggrspill_*` rows are this report's erased path**, not a separate
  "poly aggregate-spill" category.  Verified rather than inferred: write the same
  `bind` with its dispatch statically resolved and *no shim is emitted at all* --
  the instance specializes to a by-value return and the closure needs no box.
  The 16-48 B boxes appear only when the dispatch goes through the dictionary,
  which is exactly where this report has always said the residue lives, and they
  go to zero at monomorphization for the same reason everything else here does.

Why the existing freshness machinery does not reach those boxes -- it is one
specific gap, not a general limit -- is written up in
[rm1-leak-sweep-decomposed-2026-09-04.md](../artifacts/rm1-leak-sweep-decomposed-2026-09-04.md#the-__tur_aggrspill_-rows-are-the-erased-dict-path-not-a-category):
freshness is a per-monomorph property recorded once per binding, stamped on the
generic body where the dispatch is not static.  Closing it costs an
all-instances-agree stamp or a per-spec pre-pass, with double-free as the
failure mode, for ~48 B that monomorphization deletes outright.  Deliberately
not taken up.

**The largest remaining category is not this report's.** Of the 1726 B left,
~990 is the per-node recursive spine (`ctor_Cons_Cons__*`, `stdlib/re.tur`'s
`RxCons` cells), which is RM2 -- and RM2's own assessment is that it gets
unblocked by RM3 (regions), not by a better analysis.  RM1's own residue is
~240 B.
