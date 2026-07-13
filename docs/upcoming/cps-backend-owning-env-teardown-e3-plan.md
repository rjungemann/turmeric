---
title: CPS backend -- refcounted owning-value env teardown (Track B / E3)
category: Planning
status: LARGELY OBSOLETED by P1/P2/E-borrow -- leak-cleanliness for the reachable owning captures is already achieved WITHOUT a teardown (see "Empirical re-scoping" below). The one genuine residual is a CONSUMING multi-field AGGREGATE capture (rare); its separable direct-path hard-fail is now FIXED (collect_handle_captures descends into owning-value ops), so the shape falls back cleanly. Recommend NOT building the general teardown; cover the residual narrowly if a real case appears.
description: The N6 CT-IR backend lifts a continuation body into a heap env struct that is LEAKED with the (also-leaked) DK chain nodes. E-borrow already made the *reachable* owning captures (borrow-only rc handles) leak-clean by riding a bare alias, so E3 is no longer needed for leak-cleanliness of what already emits. E3's remaining job is to ADMIT the shapes that currently evict -- an owning value CONSUMED by a multi-shot continuation, an owning aggregate / carrier handle / ref captured across a control op -- by giving the owning-carrying env a real clone-on-copy / drop-on-teardown discipline. Two phases: E3a attaches per-frame env clone/drop hooks to the DK machine (admits consuming captures memory-safely, still leaking the base ref), and E3b introduces a teardown of the delimited chain at region completion (leak-clean, and it retires the pre-existing DK-node leak). Sound on the fallback today -- missed coverage, not a correctness gap.
---

# CPS backend -- refcounted owning-value env teardown (E3)

## Empirical re-scoping (added after P1/P2/E-borrow landed)

Before building any of the teardown below, the reachable owning-capture shapes
were measured under LeakSanitizer. **Leak-cleanliness is already achieved for
all of them, with no teardown:**

- **borrow-only rc / aggregate capture** (case READS the capture): rides a bare
  shallow alias (E-borrow); the enclosing fn drops it exactly once, now lowered
  into the single-shot continuation by the auto-drop P2 pass
  ([cps-backend-owning-autodrop-lowering-plan.md](cps-backend-owning-autodrop-lowering-plan.md)).
  Verified leak-clean across a case that runs 3x.
- **consuming rc capture** (case DROPS the captured rc, runs N times): E1's
  incref-on-read-out gives each invocation its own +1, which the case's drop
  balances; the base +1 is dropped once by the P2-lowered scope-exit auto-drop.
  **Net zero, freed once -- leak-clean** (verified: a case that drops its rc and
  runs 3x, LeakSanitizer-clean). This is the case E3a was meant to admit "still
  leaking the base"; it does not leak.
- **abortive crossing** (owning value crosses a `shift` whose continuation is
  discarded): P2 correctly refuses to lower it (`expr_has_unsafe_control`), so it
  falls back to the direct emitter, which fires the drop on abort via the
  `tur_frame` defer stack. Correct, no leak.

So the two purposes this plan was written for -- E3a "admit consuming captures
memory-safely" and E3b "leak-clean via teardown" -- are **both already met by
cheaper means** (incref-balanced-by-consume + E-borrow bare alias + P2 auto-drop
lowering). The DK-node leak (`docs/reported/cps-delimited-dk-node-leak.md`)
remains, but it is a fixed, bounded per-region leak unrelated to owning captures.

### The one genuine residual

A **CONSUMING multi-shot AGGREGATE capture** -- a handler case that DROPS a
captured by-value struct's owning field and runs N times -- evicts
(`collect_caps_case` rejects a consuming non-rc capture, since a struct has no
scalar incref) and, on the direct fallback, would double-drop the field. It is
now a **hard error, TUR-E0107** (`is_field_consumed_in_handler` +
`elab_let`), rather than a silent double-drop or a raw `'o' undeclared` C error
(`docs/archive/cps-consuming-aggregate-capture-hardfails.md`). The related
STRAIGHT-LINE field drop is fixed by suppressing the per-field auto-drop
(`is_field_consumed`:
`docs/archive/explicit-field-drop-plus-scope-autodrop-double-drops.md`). Note the
bare-`rc` consuming capture is a DIFFERENT, already-working shape (E1
incref-on-read-out + P1/P2 auto-drop lowering keep it leak-clean; no eviction,
no error -- `docs/archive/cps-handler-case-consumes-owning-capture-evicts.md`).

Why this one is not just "rc with more fields": incref-on-read-out is only
balanced when the case drops *exactly* the cloned owning fields. For a
single-owning-field struct it would balance like rc, but for a multi-field
struct the env-clone would incref fields the case does not drop -> leak. So a
*correct* admission needs the env to OWN the aggregate (clone all fields on
copy, drop all fields once per continuation lifetime) -- the actual Option B
teardown -- OR the case to clone what it consumes. Given the shape is rare and
the separable direct-path hard-fail is now fixed (falls back cleanly), the
recommendation is: **do not build the teardown** unless a real
consuming-aggregate case appears.

The original design (below) is retained for the day that case arrives.

## Why this document exists

E3 is the remaining substrate of Track B in
[cps-backend-multishot-continuations-owning-capture-plan.md](cps-backend-multishot-continuations-owning-capture-plan.md).
That plan's Track B landed **E-borrow** (leak-clean owning captures via a bare
alias) and found **E2** (aggregate / carrier captures) and the consuming-case
shape both **blocked** on the same hole: an owning value whose ownership is not
discharged by an explicit straight-line consume gets a scope-exit auto-drop
(`EX_DEFER`) the CT-IR backend cannot lower, so the function evicts before the
capture machinery runs. This document is the plan for the teardown substrate that
unblocks all of it -- and which O1-b P2/P3
([cps-backend-ref-scope-exit-drop-plan.md](cps-backend-ref-scope-exit-drop-plan.md))
also wait on.

**Nothing here is a correctness gap today.** Every shape E3 would admit is
handled correctly by the general whole-function fallback (the direct/fiber effect
path). This is coverage.

## What E3 is NOT (the E-borrow reframing)

The archived env-capture plan cast E3 ("Option B, refcounted env with
clone/drop") as **the** landing needed for a leak-clean owning-capture story.
E-borrow changed that. The owning captures that actually **reach** a multi-shot
continuation today are borrow-only (a consuming case without a straight-line
consume in the enclosing fn evicts upstream on `EX_DEFER` --
`docs/reported/cps-handler-case-consumes-owning-capture-evicts.md`), and a
borrow-only capture is leak-clean as a bare alias: the enclosing fn owns the
value and drops it exactly once, so the env needs neither clone nor drop. So:

- **Leak-cleanliness of what emits today: already done** (E-borrow;
  `owning_cap_borrow_only` / `cap_owning_ok`, `emit_cps_ir.c`).
- **E3's actual purpose: ADMIT new shapes that currently evict** -- a genuinely
  consuming multi-shot capture, and (once their `EX_DEFER` auto-drop is lowered)
  owning aggregates, carrier handles, and crossing refs.

## The mechanism today (verified)

A colored function's control op lifts its continuation body into a heap env
struct plus DK chain nodes, both **leaked**:

- `emit_cont_env` (`emit_cps_ir.c`) `malloc`s a `<hname>_env`, populates `->__k`
  + `->fI` by shallow value copy, and returns `(intptr_t)__ce_<hname>` for the DK
  frame env. The struct is never freed.
- `emit_handle` builds `DK *__h<id> = dk_handler(tag, case, caseenv,
  dk_frame(kname, hkenv, dk_done()|dk_copy_enclosing_handlers(cur_k)))`, sets
  `ce->cur_k = __h<id>`, and emits the delimited body with `emit_term(delim)`,
  which ends in a **tail** `return dk_run(...)` / `return dk_perform(...)`. There
  is no post-point at which `__h<id>` (or its envs) could be freed -- the chain
  is leaked (`docs/reported/cps-delimited-dk-node-leak.md`). `emit_reset` /
  `emit_perform` are the same shape.
- On a multi-shot resume the DK machine copies the *spine*: `dk_copy_range` ->
  `dk_copy_node` (`emit_dk_runtime.c`) copies each node but **shallow-copies the
  env pointer** (`c->env = n->env`), so every reified sub shares one env with the
  leaked original. `dk_free` (`emit_dk_runtime.c:356`) is called only on those
  reified **sub copies** (`dk_perform`, `dk_shift`, `dk_invoke`) -- never on the
  original chain, and it frees only the frame nodes, not the env payloads.

Two consequences constrain E3:

1. **The env is shared** between the leaked original chain and every freed sub
   copy. A naive `free(env)` in `dk_free` would double-free (the original still
   references it, and multiple sub copies alias it).
2. **The original chain is never freed**, so any owning reference the env holds
   (the "base" +1) leaks unless the original chain gets a teardown.

## The reusable substrate (verified)

- **Refcounted continuation with clone/drop callbacks.** The emitted prelude
  already carries `tur_cloneable_cont_alloc(fn, cap, clone, drop)` with per-clone
  / per-drop callbacks; the cloneable-shift path threads an owning capture through
  `tur_cloneable_cont_alloc(__dk_cont_fn, __cap, __dk_env_clone, __dk_env_drop)`
  (`emit_cps_ir.c:3782`). The callbacks (`emit_dk_runtime.c:45-60`) today clone /
  free the DK **spine** only (`dk_copy_range` / `dk_free`); E3 grows them to also
  clone / drop the owning payload inside a frame env.
- **Owning clone/drop glue.** `rc_strong_increment` / `rc_strong_decrement` for an
  rc handle; the generated struct/ADT clone+drop glue keyed by
  `type_uses_carrier_abi` (carrier ADT) and `adt_is_byvalue_product` (by-value
  product with owning fields) that O2 already emits for owning fields of an
  aggregate. E-borrow's `CapSet.owning[]` already records which capture slots are
  owning; E3 reuses that plus the per-slot Type to pick the right glue.

## Design -- two phases

### E3a -- per-frame env clone/drop hooks (admits consuming captures; still leaks the base)

Give a DK frame node an optional `env_clone` / `env_drop` pair (function
pointers, default NULL = today's leaked, share-on-copy behavior). Fire them in
the copy/free machinery:

- `dk_copy_node`: if `n->env_clone`, set `c->env = n->env_clone((void*)n->env)`
  (deep-copy the env struct and incref/clone each owning field) instead of the
  bare `c->env = n->env`. So each reified sub gets its OWN env with its own +1 on
  each owning field.
- `dk_free`: if `k->env_drop`, call `k->env_drop((void*)k->env)` (decref/drop each
  owning field, then free the env) before freeing the node.

Emit an `<hname>_env_clone` / `<hname>_env_drop` pair per continuation whose caps
contain an owning field (reusing E-borrow's `owning[]` + the O2 glue), and pass
them to a new `dk_frame_owning(fn, env, env_clone, env_drop, next)` constructor at
the `emit_cont_env` site. Copy-only envs keep the existing `dk_frame` fast path
untouched.

With E3a, a **consuming** multi-shot capture is memory-safe: each resume's sub is
a fresh env copy with its own +1 (clone), the case body's drop balances it, and
`dk_free(sub)` drops that copy's +1. The env-share double-free (constraint 1) is
gone -- copies no longer alias. What still leaks is the **base**: the original
chain's env +1, because the original chain is never freed (constraint 2). So E3a
alone is "Option A generalized" -- it admits the shapes, but a fixture exercising
it carries `requires.no-leak-check`, exactly like E1's original incref path.

This is the smaller, self-contained landing: it needs no change to the tail
emission, only the DK-node hook fields + the two callbacks + the per-continuation
glue.

### E3b -- delimited-region teardown (leak-clean; retires the DK-node leak)

Free the original delimited chain (and, via `env_drop`, its base env refs) when
the delimited region completes. This is the real work and the hard part, because
the region is emitted tail-recursively.

Options, to be chosen during E3b:

1. **Non-tail region.** Bind the handle/reset result instead of tail-returning
   it: emit `int64_t __r = dk_run(<chain>, ...);` then `dk_free_deep(__h<id>)`
   (a new teardown that frees the chain AND fires each owning frame's `env_drop`
   exactly once on the base), then deliver `__r` to `cur_k`. Costs the tail call
   at the region boundary (a bounded, one-per-region cost) and needs a
   `dk_free_deep` that walks the ORIGINAL chain (distinct from `dk_free`, which
   the sub-copy path still uses). Must ensure no reified sub outlives the region
   (they do not -- `dk_invoke` copies-and-frees within the handler case, all
   inside the region's dynamic extent).
2. **Region guard / arena.** Allocate the region's chain + envs from a per-region
   arena (or a linked "born this region" list) and free the whole arena at region
   end. Sidesteps the opaque-`DK` single-node-free problem the leak report calls
   out, and naturally frees envs with their nodes. Still needs the region-end
   hook (same non-tail boundary as option 1).

Either way E3b also **retires the pre-existing DK-node leak**
(`docs/reported/cps-delimited-dk-node-leak.md`) as a bonus -- the chain nodes,
not just the owning envs, get freed at region end.

Correctness obligations for E3b:

- **Fire base `env_drop` exactly once.** The base env is dropped by the
  region teardown; the sub copies are dropped by `dk_free`. The clone/drop counts
  must balance: base populate = +1 (or a moved-in +1), each sub copy = +1 (clone)
  and -1 (`dk_free`), region teardown = -1 (base). Net zero, freed once.
- **Abortive control (shift / discontinue) must still fire the teardown.** An
  abortive `shift` discards the continuation; the region-end teardown must run on
  that path too (this is exactly O1-b P2's "fire on abandon", the CPS analogue of
  `tur_frame_fire_chain`). Option 2's arena makes this uniform (free the arena
  whether the region exits normally or abortively).

### E3c -- wire the owning kinds + drop the interim markers

Once E3a/E3b are in:

- Re-land the **E2** `cap_owning_ok` extension (carrier ADT +
  `owning_byvalue_aggregate`) and the consuming-non-rc admission, now with the
  clone/drop glue actually emitted (E3a) instead of evicting -- **contingent on
  the aggregate/carrier/ref scope-exit auto-drop being lowered** so the shapes
  reach CPS at all. That lowering is
  [cps-backend-owning-autodrop-lowering-plan.md](cps-backend-owning-autodrop-lowering-plan.md)
  (NOT part of E3). Note its P2 already unblocks E2's *borrow-only* aggregate
  captures with no teardown; E3 here is only for the genuinely *consuming* /
  abortive crossings (that plan's P3).
- **O1-b P3** (a `ref<T>` captured across a control op): the ref's drop becomes
  the per-capture `env_drop`, fired once per continuation lifetime by E3b.
- **O1-b P2** (a `ref<T>` live across an abortive control op): rides E3b's
  abortive teardown path.
- Drop `requires.no-leak-check` from any E3a-era fixtures once E3b lands.

## Fixtures

- **E3a**: a handler case that *consumes* an rc it captured, resumed twice at
  runtime (Track A / A1 makes the case run twice) -- the capstone the multishot
  plan describes. Memory-safe (each resume clones, drops; balanced), carries
  `requires.no-leak-check` for the base ref until E3b.
- **E3b**: the same fixture with the marker dropped -- runs under normal
  LeakSanitizer. Plus a reset/shift variant (E4) and an abortive-shift variant
  (O1-b P2) confirming the teardown fires on abandon.
- **E3c**: an owning-aggregate capture and a crossing-ref capture, each once the
  auto-drop lowering makes them reach CPS.

## Interaction with other plans

- **Track B E2 / E4** (multishot plan): E2 (aggregate/carrier) re-lands on E3a's
  glue; E4 (reset/shift multi-shot owning capture) rides E3b's teardown.
- **O1-b P2 / P3** (ref scope-exit drop): P2 = abortive teardown (E3b's abandon
  path); P3 = resumable-crossing ref (E3's per-capture drop). E3 must not build a
  parallel teardown -- P2/P3 land on it.
- **The DK-node leak** (`docs/reported/cps-delimited-dk-node-leak.md`): E3b's
  region teardown retires it. Coordinate so the fix is claimed once.
- **N6.5** (fallback deletion,
  [cps-backend-n6-fallback-removal-followups-plan.md](cps-backend-n6-fallback-removal-followups-plan.md)):
  consuming / aggregate / ref owning-capture continuations are residual fallback
  cases; E3 removes them (or they stay a named carve-out until E3 lands).

## Depends on / reuses

- `emit_cont_env`, `emit_handle` / `emit_reset` / `emit_perform`, `CapSet.owning[]`,
  `owning_cap_borrow_only`, `cap_owning_ok` (`emit_cps_ir.c`).
- The emitted DK machine: `dk_copy_node`, `dk_free`, `dk_frame`, the reified-sub
  free sites in `dk_perform` / `dk_shift` / `dk_invoke` (`emit_dk_runtime.c`;
  reference machine `src/runtime/cps_prompt.{c,h}`).
- `tur_cloneable_cont_alloc` + `__dk_env_clone` / `__dk_env_drop`
  (`emit_dk_runtime.c:45-60`, `emit_cps_ir.c:3782`).
- Owning clone/drop glue: `rc_strong_increment` / `rc_strong_decrement`, and the
  O2 struct/ADT clone+drop glue (`type_uses_carrier_abi`, `adt_is_byvalue_product`).

## Out of scope

- **Lowering the owning-aggregate / bare-rc / crossing-ref scope-exit
  auto-drop** so those shapes reach CPS -- that is the `EX_DEFER` lowering, owned
  by [cps-backend-owning-autodrop-lowering-plan.md](cps-backend-owning-autodrop-lowering-plan.md)
  (non-`ref`) and O1-b (`ref`). E3 supplies the teardown the *abortive / multi-shot*
  crossings land on (that plan's P3); it does not lower the auto-drop itself, and
  the *single-shot* crossings (that plan's P2) need no E3 at all. Until the
  lowering lands, E3a/E3b are exercised by the *consuming rc* shape only.
- **The borrow-only fast path** -- E-borrow owns it; a borrow-only capture never
  needs E3's clone/drop (it rides a bare alias, leak-clean already).
- **Recursive / unbounded suspension continuations** -- Track A's bound; separate.
