# Stdlib Inline-C De-Workaround Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** stdlib hygiene -- replace inline-C workarounds with idiomatic Turmeric
> **Sibling plans:**
> - [stdlib-opaque-handle-types-plan.md](stdlib-opaque-handle-types-plan.md) -- handle nominal typing
> - [stdlib-advanced-typing-plan.md](stdlib-advanced-typing-plan.md) -- linearity / sessions / effects / refinement / typeclasses

---

## Overview

An audit of `stdlib/` turned up a cluster of inline-C blocks that exist not
because the work is genuinely C-level (libc bindings, syscalls, ABI glue) but
because the stdlib author at the time worked around a Turmeric language or
codegen gap that has since closed. The smell is consistent: a `:int`-typed
parameter masquerading as a cons-list pointer, an inline-C `struct
__tur_cons_t { int64_t head; int64_t tail; }` declared on the spot, and a
hand-rolled walk over the abstraction that the *same module* defines.

These workarounds are now actively harmful: they bypass `Cons`/`List[A]`
typing, hard-code buffer caps that should be unbounded, and reimplement the
fat-closure ABI per call site. They are also the canonical bad examples that
new contributors copy.

This plan is **distinct from but coordinates with** the two sibling plans.
The opaque-handle plan hardens handle *signatures*; the advanced-typing plan
adds linearity / sessions / effects on top. This plan removes inline-C from
bodies that should be pure Turmeric in the first place -- a prerequisite for
the typeclass consolidation work in the advanced-typing plan's Phase T, since
several of the targets here are the same modules whose `Monad`/`Functor`
instances are still open-coded.

---

## Relationship to the sibling plans

| Concern | This plan | Opaque-handle plan | Advanced-typing plan |
|---|---|---|---|
| Handle `:int`/`:ptr<void>` -> `defopaque` | defers to | **owns** | layers linearity |
| `Map :int` -> `defopaque Map` | defers to | **add to Tier 1** | -- |
| Raw `__tur_cons_t` walks in `list.tur` | **owns** | -- | unblocks T's `Functor [Cons]` |
| `httpd-mw-fold` raw walk + 64 cap | **owns** | -- | unblocks E (effects on httpd) |
| Domain inline-C in `json`/`re`/`parsec`/`future`/`threadpool`/`chan`/`httpd` | **owns** | -- | -- |
| `tuple7`/`tuple8` mechanical expansion | **owns** (documents only) | -- | -- |
| No tuple type syntax (KB-029) | **owns** (documents only) | -- | -- |

Two coordination points:

- **Phase 1 here lands before Phase T of the advanced-typing plan.** T1's
  `Bifunctor` instance for `Result` is fine independently, but T's
  `Functor [Cons]` / `Monad [Cons]` instances should sit on top of an
  idiomatic `list.tur`, not on top of `__cons-fmap`.
- **Phase 2 here ships in tandem with the opaque-handle plan's Tier 1.**
  Replacing `httpd-mw-fold`'s raw cons walk is cleaner once `Middleware` is
  a `defopaque` newtype, so the typed `foldr` has a real element type to
  unify against.

The `Map :int` finding from the audit is intentionally folded into the
opaque-handle plan (recommend adding `Map` to that plan's Tier 1) rather
than duplicated here.

---

## Inventory

### Tier 1 -- `list.tur` self-violations (highest priority)

These are the worst offenders because they live in the module that *defines*
the `Cons` abstraction. Every other stdlib module that walks a list
correctly is implicitly endorsing these as the pattern to copy.

| Location | Function | Smell |
|---|---|---|
| `stdlib/list.tur:151-163` | `list-eq?` | raw `__tur_cons_t` walk; should be a fold |
| `stdlib/list.tur:185-205` | `__cons-fmap` | raw walk + hand-rolled fat-closure cast + malloc loop |

`list-eq?` should be expressible as a tail-recursive Turmeric function over
`tcons`/`tnil`; the fat-closure call now has a polymorphic-fn calling
convention so no inline-C is required. `__cons-fmap` is the `Functor [Cons]`
implementation in disguise; once it is pure Turmeric, the advanced-typing
plan's `definstance Functor [Cons]` collapses to a one-liner.

### Tier 2 -- domain logic that escaped into inline-C

| Location | Function | Replacement |
|---|---|---|
| `stdlib/httpd.tur:3181-3199` | `httpd-mw-fold` | typed `foldr` over `List[Middleware]`; removes the 64-element cap (latent bug for >64 middlewares) |
| `stdlib/re.tur:253-288` | `re/union-patterns` | `string-join "\|" (map wrap-paren patterns)` |
| `stdlib/re.tur:309-340` | follow-on inline-C | same pattern, fold into the above |

These three are pure data shuffling expressed as C because the original
author needed cons-walking primitives that did not yet exist in stdlib. They
do today.

### Tier 3 -- inline-C hotspots to triage

These files have disproportionately heavy inline-C for what is largely
domain logic on top of small primitives. Each deserves a focused audit
ticket; this plan does not commit to rewriting them all, only to triaging:

| Module | inline-C blocks | Triage question |
|---|---|---|
| `stdlib/httpd.tur` | 70 | which blocks are "libc bridge" vs. "request/response domain"? |
| `stdlib/json.tur` | 30 | how much is tag-bit manipulation that needs a `defopaque Value`? |
| `stdlib/future.tur` | 24 | how much overlaps with the opaque-handle plan's `Promise`/`Future`? |
| `stdlib/parsec.tur` | 24 | how much disappears with advanced-typing T2 (`Monad [Parser]`)? |
| `stdlib/threadpool.tur` | 16 | overlap with opaque-handle Tier 1 |
| `stdlib/chan.tur` | 11 | overlap with opaque-handle Tier 1 + advanced-typing S |

**Justified, do not touch:**

- `stdlib/fs.tur` (~22 blocks) -- libc/syscall bindings.
- `stdlib/io.tur`, `stdlib/net.tur`, `stdlib/process.tur`, `stdlib/serial.tur`
  -- platform glue.

### Tier 4 -- documentation-only follow-ups

These are honest acknowledgements of language gaps rather than fixable
workarounds, but they belong in this plan so they are tracked somewhere:

- **`stdlib/tuple.tur:284-400+`** -- `tuple7`/`tuple8` plus per-N accessors
  are mechanical expansions. Real fix requires variadic generics; document
  the gap and link to a future language plan.
- **`stdlib/session.tur:68-72` (KB-029)** -- `(recv ch)` cannot annotate
  its return because the surface has no tuple/pair type syntax. Track as a
  language-surface issue; do not paper over with another inline-C cast.

---

## Design principles

1. **Never re-declare `__tur_cons_t` inside stdlib.** A new inline-C block
   that does so should fail review. The only place that knows the cons
   layout is the runtime; everything else uses typed `Cons`/`tcons`/`tnil`.
2. **No bespoke fat-closure casts in stdlib.** Use the polymorphic-fn
   calling convention (`tur_poly_fn_t`) or, preferably, call the closure
   from Turmeric and let the codegen emit the cast.
3. **No fixed-size buffers in fold/reduce helpers.** If a Tier 2/3 rewrite
   surfaces a "but we had a 64-element cap before" question, the cap was
   the bug, not the feature.
4. **Inline-C is acceptable for**: libc/syscall bridges, ABI glue, atomics,
   memory-ordering primitives, and codegen-emitted intrinsics. Everything
   else should be Turmeric.

---

## Phasing

### Phase 1 -- `list.tur` cleanup

1. Rewrite `list-eq?` as a tail-recursive Turmeric fn over `tcons`/`tnil`.
2. Rewrite `__cons-fmap` similarly; rename to drop the `__` prefix once it
   is no longer the unsafe escape hatch.
3. Regenerate `tests/fixtures/*/expected.c` per the snapshot rule.
4. Confirm `bash tests/run.sh` clean.
5. Single PR. Block advanced-typing T's `Functor [Cons]` on this landing.

### Phase 2 -- `httpd-mw-fold` and `re/union-patterns`

1. Coordinate with opaque-handle Tier 1: if `Middleware` becomes a
   `defopaque`, ship that first; otherwise proceed against the raw `:int`.
2. Replace `httpd-mw-fold` with a typed `foldr`. Add a fixture that
   exercises >64 middlewares to lock in the cap removal.
3. Replace both `re/union-patterns` inline-C blocks with
   `string-join`/`map`.
4. Regenerate snapshots; one PR per module.

### Phase 3 -- Tier 3 triage

For each of `json.tur`, `future.tur`, `parsec.tur`, `threadpool.tur`,
`chan.tur`, `httpd.tur` (the non-mw-fold portion):

1. Classify each inline-C block as "libc/ABI glue" (keep) or "domain logic"
   (rewrite candidate).
2. Open a tracking issue per module summarising the count and the
   rewrite plan; do not bundle into this plan's PR.
3. Sequence rewrites after the relevant opaque-handle / advanced-typing
   phases land so the rewrites can use the new types instead of `:int`.

### Phase 4 -- Documentation-only follow-ups

1. Add a short note to `stdlib/tuple.tur`'s module docstring linking to a
   "variadic generics" language plan stub (create the stub under
   `docs/upcoming/` if absent).
2. Promote KB-029 from a comment in `session.tur` to a tracked entry in the
   language plan covering tuple/pair type syntax.

---

## Risks

- **Snapshot churn.** Phase 1 will move list internals in ways that touch
  any fixture using `list-eq?` or fmap-over-list. Standard regeneration
  recipe from CLAUDE.md applies.
- **Performance.** The inline-C versions of `list-eq?` and `__cons-fmap`
  may be marginally faster than the Turmeric equivalents until the codegen
  inlines them. Benchmark in Phase 1; if a regression appears, fix the
  codegen rather than re-introducing the inline-C.
- **Hidden callers.** `__cons-fmap`'s `__`-prefixed name suggests internal
  use; grep for cross-module callers before renaming. The polymorphic-fn
  calling convention used by the current inline-C may be relied on by
  downstream macros.
- **Coordination drift.** If the opaque-handle plan ships Tier 1 before
  this plan's Phase 2 is ready, Phase 2 inherits the new `Middleware`
  newtype "for free"; no rework needed. The reverse ordering also works
  (rewrite first against `:int`, re-type later). The only ordering that
  must hold is Phase 1 before advanced-typing T's `Functor [Cons]`.

---

## Acceptance criteria

- Tier 1 (`list-eq?`, `__cons-fmap`) contains zero inline-C; no
  `__tur_cons_t` redeclaration anywhere in `stdlib/` outside `list.tur`'s
  unsafe escape hatch (if one is retained at all).
- Tier 2 (`httpd-mw-fold`, `re/union-patterns`) is pure Turmeric; a fixture
  with >64 middlewares passes.
- Tier 3 has per-module tracking issues with classified inline-C counts.
- `bash tests/run.sh` passes with zero `FAIL` lines, ASan/LSan on.
- `tur run docs` regenerated.
