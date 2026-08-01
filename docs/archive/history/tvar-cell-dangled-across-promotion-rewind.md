# TVar cells dangled across a scratch-promotion rewind (REPL use-after-reset)

**RESOLVED 2026-07-27** -- found by inspection while building the TR3
collection sweep (`docs/archive/turi-interp-incremental-reclamation-plan.md`),
fixed in the same change.

**Severity was:** high for the REPL (silent wrong reads / ASan use-after-poison
on an ordinary STM session), latent since TR2.4 enabled scratch promotion in
the REPL by default (2026-07-25).

## The bug

`EX_TVAR_NEW` allocated its `TuriTVar` cell from the env's **`value_scratch`**
pool (`turi_val_calloc`, `eval.c`) and returned the cell's address as an
opaque `TURI_INT` carrier. The scratch-promotion walk relocates only *tagged*
values -- an int carrier passes through `promo_copy` untouched -- so promotion
could neither move the cell nor know the eval boundary was not quiescent.
The first successful rewind after

```turmeric
(def t (tvar/new 0))
```

reset the scratch arena with the cell still inside it. The global `t` kept the
stale address; a later `(atomically (stm (tvar/read t)))` read poisoned/reused
arena memory. Nothing in-tree hit it because the STM fixtures create and use
their tvars inside a single eval; it needed a tvar living *across* top-level
evals with promotion on -- exactly the REPL's default configuration.

This is the carrier-opacity problem TR1 names, showing up as a soundness hole
rather than a decline: `promo_check` conservatively refuses shapes it can see
and cannot relocate, but a bare int carrier is invisible, so there was nothing
to refuse.

## The fix

TVar cells are malloc'd and registered as tracked collection boxes
(`turi_env_track_collection`, the same list Vec/Set/Map wrappers live on),
with `free` as the destroy hook and a one-value scan hook:

- the cell now survives every rewind (it was never scratch's to reclaim);
- an *unreachable* tvar is reclaimed by the TR3 eval-boundary sweep, and any
  tvar is reclaimed at teardown -- no new leak class;
- the scan hook feeds the tvar's stored value into the sweep's conservative
  mark, so a collection handle held only inside a TVar keeps its buffer alive.

Pinned by `test_collection_sweep` in `tests/turi/env-longlived.c`: a global
tvar reads back `33` after 50 promotion rewinds (pre-fix: use-after-reset,
loud under the harness's ASan build), and a vec reachable only through a
tvar's value survives the sweep.

## Related

- `docs/archive/turi-interp-incremental-reclamation-plan.md` -- TR3, where
  the sweep and this fix landed together.
- `docs/archive/history/turi-value-pool-carrier-relocation-plan.md` (TR1) --
  the general carrier-opacity tail this bug is one instance of.
