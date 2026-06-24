# turi value-pool: residual un-pooled escaping allocations

**Severity:** low (follow-up to a landed change; not a correctness bug).

After Phase 1 of `turi-env-owned-value-arena-pool-plan` (env-owned
`value_arena` + `turi_val_*`), `turi_env_free` reclaims every escaping value
payload exercised by ordinary scripts, and the `tur_env_teardown` leak gate is
clean. A few escaping allocations remain on raw libc and still leak across
`turi_env_free`:

## 1. inline-C emulation buffers (`ic_exec_*`)

`ic_exec_constructor` (string fat-pointer `s`, ctor backing `mem`),
`ic_exec_snprintf_fmt` (result buffer), and the small string buffers in the
inline-C body walkers (`src/turi/eval.c`, the `ic_*` helpers) `malloc` results
that escape as program values. These helpers take no `env`, so threading the
pool through them is wider than Phase 1.

- Impact: only reachable via the TI7 inline-C carve-out, which
  `tests/run-turi.sh` skips for all but a tiny allowlist -- so these barely run
  under the interpreter today.
- Fix direction: thread `env` through `ic_exec_constructor` /
  `ic_exec_snprintf_fmt` / the simple-inline-C dispatcher and swap their
  `malloc`/`strdup` for `turi_val_alloc`/`turi_val_strdup`.

## 2. coroutine stacks for fibers and generators

The `TuriFiber` / `TuriGen` *structs* are now pool-owned, but their execution
stacks are not: native uses `mmap` (`eval.c` async-fiber path, ~line 6977) and
the WASM path `malloc`s `GEN_STACK_SIZE` / `TURI_ASYNC_STACK_SIZE`. These are
never `munmap`/`free`d (process-lifetime, like the historical model).

- Impact: `mmap` regions are not tracked by LeakSanitizer, so they do not fail
  the leak gate; the `malloc` (WASM) path is not built on native. A long-lived
  native host that creates many fibers still grows.
- Fix direction: track per-env coroutine stacks (a small intrusive list on
  `TuriEnv`) and `munmap`/`free` them in `turi_env_free`, or fold them into the
  scheduler teardown (`turi_sched_free`).

Both are deferred: Phase 1 delivers leak-clean teardown for the common payload
kinds and the per-unit-env embedding pattern; these tails do not block it.
