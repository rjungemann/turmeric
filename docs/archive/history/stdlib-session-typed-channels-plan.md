---
title: Stdlib Session-Typed Channel Wrappers
category: Planning
description: A thin generic wrapper `SChan<p>` over the (opaque, linear) `Chan` newtype that carries a protocol phantom advanced by each `schan-send` / `schan-recv` / `schan-close` operation. Makes worker-pool and RPC-pipe protocols compile-time checks.
---

> **Superseded by [session-types-guide](../../guides/session-types-guide.md).**
> This plan has shipped; the user-facing reference for `stdlib/schan.tur` and
> the broader session-types story is the guide. This document is retained
> here for historical context.

# Stdlib Session-Typed Channel Wrappers -- Plan

> **Status:** COMPLETE (landed in "Add stdlib/schan.tur: session-typed
> channel wrappers", #247). All three phases shipped:
> - **S1** -- `stdlib/schan.tur` with `schan-new` / `schan-send` /
>   `schan-recv` / `schan-close` (plus the `schan-cell-*` out-parameter
>   helpers). Round-trip fixture `tests/fixtures/schan-roundtrip` exercises
>   the `SSend int (SRecv int SClose)` protocol; the negative fixture
>   `tests/fixtures/errors/schan-skip-step` confirms a skipped step is a
>   compile-time `TUR-E0001` phantom mismatch.
> - **S2** -- `tests/fixtures/schan-worker-pool` drives a real
>   three-worker request/response loop over wrapped channels, leak-clean
>   under ASan.
> - **S3** -- documented in `docs/guides/session-types-guide.md`
>   (Session-Typed Channel Wrappers section); `stdlib/docstrings.tur`
>   regenerated to carry the schan entries.
>
> One deviation from the original design is recorded inline: `schan-recv`
> returns the typed continuation directly and delivers the received value
> through a caller-provided cell, rather than the planned
> `Pair<T SChan<R>>`. That ideal is blocked by a monomorphizer limitation
> around parametric aggregates whose element type is a phantom carried
> inside an opaque -- see `docs/reported/generic-struct-opaque-element-miscompile.md`.
> The continuation stays fully typed, so the protocol-ordering guarantee
> is intact on every step.
>
> **Type:** stdlib API hardening -- session-typed channels
> **Prerequisites:** `Chan` / `AsyncChan` are opaque newtypes
> ([[stdlib-opaque-handle-types-plan]] Tier 1) **and** linear
> ([[stdlib-linearity-affinity-plan]] L1).

## Motivation

`stdlib/session.tur` ships with `-Xsessions` and demonstrates protocol
typing, but the channels used in practice (`tur/chan`, `tur/taskgroup`
worker pools, `tur/reactor` event sources) carry untyped `int` /
`:ptr<void>` payloads. A session-typed wrapper makes the request /
response shape of a worker channel or RPC pipe a compile-time check.

Combined with linearity (the prerequisite plan), this enforces that
every protocol step happens exactly once and in order.

## Design

A thin generic wrapper over the already-opaque `Chan` newtype:

```turmeric
(defopaque SChan p ptr<void> :linear)   ;; p :: protocol phantom

(defn schan-new   [p]                                  : SChan<p>)
(defn schan-send  [c : SChan<Send T rest> v : T]       : SChan<rest>)
(defn schan-recv  [c : SChan<Recv T rest>]             : Pair<T SChan<rest>>)
(defn schan-close [c : SChan<Close>]                   : nil)
```

The implementation delegates to `chan-send` / `chan-recv`; the protocol
is a phantom parameter advanced by each operation. The phantom is
threaded through the result type, so a caller cannot skip a step or
reorder send / recv.

## Scope

- New wrapper module `stdlib/schan.tur`. Original `tur/chan` keeps its
  untyped surface for low-level use.
- Convert a `taskgroup` worker-pool example fixture to demonstrate the
  wrapper end-to-end.
- Defer wrapping `reactor` event sources to a follow-up -- the variant
  shape there is more complex (multiple source kinds in one queue).

## Phasing

1. **S1** -- `stdlib/schan.tur` with `schan-new` / `schan-send` /
   `schan-recv` / `schan-close`. Round-trip test fixture: a two-step
   `Send Int (Recv Bool Close)` protocol.
2. **S2** -- Worker-pool example fixture, exercising a real
   request / response protocol over a `SChan` against a real worker
   loop reading from the wrapped channel.
3. **S3** -- Document in `docs/guides/session-types-guide.md`; cross-link
   from the channel guide.

## Risks

- The phantom parameter advances only at the type level; the underlying
  `Chan` is the same untyped queue, so a malicious cast can still mis-
  spell the payload type. The wrapper is checked-by-discipline, not
  by-runtime.
- Linearity is required to prevent a "use the same protocol step twice"
  shape from compiling. If [[stdlib-linearity-affinity-plan]] L1 has not
  landed, this plan should not land.

## Acceptance

- `stdlib/schan.tur` exists with the four operations and their declared
  types.
- A round-trip protocol fixture passes; a deliberately-skipped-step
  fixture fails to compile with a phantom-mismatch diagnostic.
- Worker-pool example exists under `tests/fixtures/` and runs leak-clean
  under ASan.
- `tur run docs` regenerated.

## Cross-references

- Layered on [[stdlib-opaque-handle-types-plan]] (Tier 1) and
  [[stdlib-linearity-affinity-plan]] (L1).
- Split out from the original umbrella `stdlib-advanced-typing-plan`.
