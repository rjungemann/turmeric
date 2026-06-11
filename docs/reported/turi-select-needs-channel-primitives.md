# `select` (EX_SELECT) is unimplementable in turi without channel primitives

**Summary:** The tree-walking interpreter cannot evaluate `(select ...)`
(`EX_SELECT`) because Turmeric channels have no native representation in
`turi`. Channels are built entirely from inline-C (`pthread_mutex_t` /
`pthread_cond_t` ring buffers); every existing `select-*` fixture defines its
channel operations as user inline-C, which is a permanent interpreter
carve-out (TI7). So even a full `turi_select` case arm would have nothing to
select *over* under the interpreter.

**Severity:** Low / ergonomics. `select` simply does not run under
`tur --interpret` / `tur repl` / the WASM REPL. This is now a *clean* error
(`eval: select is not supported in interpreter mode ...`) rather than the
generic `eval: unhandled expression kind N` default, so the failure mode is at
least legible.

## Root cause

- `src/turi/eval.c` has no channel runtime. A grep of `src/turi/` for `chan`
  returns only unrelated "side channel" comments -- there is no `chan-new`,
  `chan-send`, `chan-recv`, or waiter-queue native.
- The compiled path lowers channels to inline-C structs. All six select
  fixtures (`select-n-way`, `select-fair-block`, `async-select`,
  `cancel-select`, `select-send-block`, `select-fairness`) define
  `chan-new` / `chan-fill` / `chan-recv` / ... as ` ```c ... ``` ` blocks, so
  they already require the compiled path regardless of `EX_SELECT`.

## Observed vs. expected

- **Observed (before this change):** `(select ...)` hit the default arm and
  reported `eval: unhandled expression kind 84 (not yet implemented in
  interpreter)`.
- **Observed (now):** a dedicated `case EX_SELECT` returns
  `eval: select is not supported in interpreter mode (channels require native
  primitives; use the compiled path)`.
- **Expected (long-term):** either (a) a native channel layer in `turi`
  (mutex/condvar-backed ring buffer registered as interpreter natives, plus a
  non-blocking `turi_select` that polls clauses in declaration order and parks
  the fiber on the scheduler from `src/turi/fiber.c` when nothing is ready and
  no `:default` arm exists), or (b) leave it as a documented carve-out.

## Proposed fix directions

1. **Carve-out (recommended for now).** Document `select` / channels as
   interpreter-unsupported in the TI9 parity matrix and `eval-api.md`,
   alongside inline-C and WASM async. The clean error arm landed with TI6.
2. **Native channels (large).** Add an opaque `TURI_CHANNEL` value plus
   `chan-new`/`chan-send`/`chan-recv` natives and a `turi_select` that reuses
   the fiber scheduler. This is a self-contained sub-project, not a single case
   arm; it also requires native (non-inline-C) channel fixtures to test
   against, since the current fixtures are inline-C-bound.

## Validation

- `./build/tur -Xeffect-types --interpret <file-with-select>` should emit the
  clean "not supported in interpreter mode" error and exit non-zero, not the
  generic unhandled-kind default.

## Status

Filed while executing TI6 of
`docs/upcoming/v1/turi-parity-post-v1-plan.md`. The handler-value half of TI6
(`EX_HANDLER_LIT` / `EX_WITH_HANDLER` / `EX_COMPOSE_HANDLERS`) landed in the
same change; `EX_SELECT` is split out here because it is blocked on a channel
runtime, not on a missing case arm.
