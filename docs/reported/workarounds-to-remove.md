# Workarounds to remove once their blockers land

**Severity:** none on its own. This is a checklist, not a defect -- but each row
is a place the tree is deliberately doing the second-best thing, and second-best
things outlive their reasons unless someone writes down what the reason was.

**Status:** OPEN by construction. Close it when the last row is struck.

Each row: what is in the tree, why, what to do when the blocker clears, and how
to prove the workaround is no longer needed.

## 1. `StThunk` exists only because a `:fn` field crashes

**Where:** `stdlib/logic.tur` -- `(defopaque StThunk :ptr<void>)` and
`(StInc :StThunk)`.

**Why:** the honest declaration is `(StInc :fn)`. That accepts a capturing
closure, type-checks, and **segfaults** when forced, because the ascription
that calls it back emits a bare function-pointer call while a capturing lambda
is a fat closure. The `:ptr<void>` carrier gets the fat dispatch instead.

**Blocker:** [closure-in-defdata-field](closure-in-defdata-field.md), case 1.

**When it lands:** replace `StThunk` with a plain `:fn` field, delete the
opaque and the paragraph in `logic.tur` explaining why it exists, and drop the
`(:: ... :StThunk)` wrappers at the four construction sites.

**Proof it is no longer needed:** the probe in that report --
a `defdata` with a `:fn` field holding a *capturing* lambda -- runs instead of
segfaulting. Note the *non*-capturing version already works, so a probe that
does not capture proves nothing.

## 2. `weak-upgrade-after-drop` carries a `known-leak` marker

**Where:** `tests/fixtures/weak-upgrade-after-drop/known-leak`.

**Why:** `stdlib/weak.tur`'s `weak/upgrade` builds its `Option` inside inline C
with `tur_some_ptr`, which allocates a carrier box no elaborated expression
owns, so nothing frees it. The marker keeps `tests/run-leak-check.sh` honest
(it reports KNOWN rather than PASS) without failing the gate.

**Blocker:** [inline-c-option-carrier-box-leaks](inline-c-option-carrier-box-leaks.md).

**When it lands:** delete the marker file. The fixture should then report PASS
and the gate's tally line should read `54 passed, 0 failed, 0 known-open`.

**Proof:** `bash tests/run-leak-check.sh` -- the known-open count goes to zero.

## 3. The sanitizer gate is not armed

**Where:** `tests/run.sh` -- findings are reported after the summary but only
fatal under `TUR_SANITIZER_GATE=1`, which nothing sets.

**Why:** arming it on discovery would have turned 60 silent findings into 60 red
fixtures at once, which is how a gate gets switched off instead of fixed.

**Blocker:** [sanitizer-gate-not-armed-in-ci](sanitizer-gate-not-armed-in-ci.md)
-- specifically, the zero-finding count is measured on Linux only and UBSan
findings vary by toolchain.

**When it clears:** set `TUR_SANITIZER_GATE: 1` in the `ci.yml` jobs that run
`tur_tests`. Consider then flipping the default to armed and giving the
*opt-out* an env var instead, so a new finding is loud by default.

**Proof:** CI green on every platform with the variable set.

## Not on this list, and why

**`disjoined-dfs` is not a workaround.** It is a second search strategy with
its own semantics -- depth-first, incomplete, faster on goals whose left branch
terminates -- and it stays whatever happens to anything above. `disjoined`
(interleaving, complete) remains the default.

**The `TUR_SR1_SUM_BYVALUE` and `TUR_ADT_SLAB` seams are not workarounds
either.** They are measurement instruments that are deliberately off; the slab
is [shelved](multi-variant-adts-always-heap-allocate.md) rather than pending.
