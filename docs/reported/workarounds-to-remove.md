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

## 3. The sanitizer gate is not armed -- REMOVED 2026-08-26

The blocker ([sanitizer-gate-not-armed-in-ci](../archive/sanitizer-gate-not-armed-in-ci.md))
is cleared and this entry's own instruction was carried out: `ci.yml`'s `test`
job -- the only job that runs `tur_tests` -- sets `TUR_SANITIZER_GATE: "1"` at
job level. The macOS half of the missing measurement came back zero (2703
fixtures, Apple clang 21, arm64), matching Linux, and was confirmed with a
positive control rather than inferred from silence.

The follow-on this entry floated -- flipping the *default* to armed and giving
the opt-out an env var -- was deliberately **not** taken. The default governs
every local run and every downstream harness that shells into `run.sh`, not
just CI, and the two toolchains measured here are not the whole population: the
first contributor on an unmeasured compiler would meet a red suite they did not
cause. CI is where the invariant is worth enforcing, and CI now enforces it.
Revisit if the local default ever starts costing more than it saves.

**Proof:** ran as specified -- `TUR_SANITIZER_GATE=1 bash tests/run.sh` exits 0
on macOS/arm64 (`2703 passed, 0 failed`, no `SANITIZER:` block), and a planted
overflow makes it exit 1 with the finding attributed to its fixture and phase.

## 4. `with-untrailed` callers bind a result they do not want -- REMOVED 2026-08-26

The blocker
([poly-call-in-statement-position-dropped](../archive/poly-call-in-statement-position-dropped.md))
is fixed, and both sites were reverted per this entry's own instructions: the
HAZARD block is deleted, `with-untrailed`'s docstring example is back in the
natural discarded form, and `sx2-trail-combinators` discards the result --
pinning the spelling users actually write, and going red if the discard ever
regresses. Proof ran as specified: the discarded-form write happens (the
fixture prints 55, not 0), and `poly-statement-position-effect` is green.

## 5. `ws-server` casts its hub mutex through `:int` -- UNBLOCKED 2026-08-29

**Where:** `turmeric-spices`, not this tree -- `spices/ws-server`
(`broadcast_test.tur` and `fixtures/broadcast/server.tur`):

```turmeric
(def hub-mutex (:: (mutex-new) :int))
(defn hub-lock!   [] : nil (mutex-lock   (:: hub-mutex Mutex)))
(defn hub-unlock! [] : nil (mutex-unlock (:: hub-mutex Mutex)))
```

**Why:** `(def hub-mutex (mutex-new))` emitted no global at all while the use
sites still referenced it -- `'hub_hymutex_1866' undeclared`. Holding the
carrier and casting back at each borrow sidestepped it.

**What it costs:** every borrow is an unchecked `:int`-to-`Mutex` cast, so the
type checker stops helping at exactly the point where a mutex most needs it.

**Blocker:** [module-level-def-with-linear-init-emits-no-global](../archive/module-level-def-with-linear-init-emits-no-global.md)
-- **RESOLVED 2026-08-29**. The cause was not linearity (that framing was a red
herring; a non-linear `defopaque` global failed the same way) but
`def_is_opaque_type_decl` matching any `def` whose value merely had an opaque
type.

**When it lands:** it has. Restore the direct spelling -- `(def hub-mutex
(mutex-new))`, and `(mutex-lock hub-mutex)` at each borrow -- and delete the six
`::` casts.

**Proof it is no longer needed:** `tests/fixtures/module-level-def-of-opaque-value`
in this tree pins both flavours; against a `tur` carrying that fix, the direct
spelling builds and runs.

## Not on this list, and why

**`disjoined-dfs` is not a workaround.** It is a second search strategy with
its own semantics -- depth-first, incomplete, faster on goals whose left branch
terminates -- and it stays whatever happens to anything above. `disjoined`
(interleaving, complete) remains the default.

**The `TUR_SR1_SUM_BYVALUE` and `TUR_ADT_SLAB` seams are not workarounds
either.** They are measurement instruments that are deliberately off; the slab
is [shelved](multi-variant-adts-always-heap-allocate.md) rather than pending.
