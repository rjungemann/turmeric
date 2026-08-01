---
title: JIT Execution Engine Guide (`tur jit`)
category: Contributor
description: What the in-process MIR JIT does differently from the cc path -- the fallback contract, the constraints that are permanent, and the inline-C rules that only bite under the JIT
---

# JIT Execution Engine Guide (`tur jit`)

`tur jit <file>` compiles and runs a program **in process**, through MIR's
c2mir C front end, with no subprocess `cc` and no disk artifacts. It is the
fastest way through the run-edit-run loop and it backs the spice REPL's
reload path.

This guide is about **what differs from the `cc` path**. For how fast it is
and when to prefer it, see the execution-engine triangle in
[performance-guide.md](performance-guide.md).

Two gates, both off by default:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON   # build time (fetches MIR)
tur --enable=jit jit hello.tur                              # run time (experiment gate)
```

The run-time gate is the `jit` row in `EXPERIMENTS[]`
(`src/runtime/experiments.c`), so the usual experiment lifecycle warnings
apply -- see [experimental-flags-guide.md](experimental-flags-guide.md).

---

## The fallback contract

c2mir accepts a smaller language than gcc or clang. When a translation unit
fails to compile or link under MIR, `tur jit` **falls back to `cc`** and runs
the program that way. The fallback is the design, not a failure mode: a
program that takes it is slower to start but produces identical output.

The corollary is what matters for correctness work: **a fallback is a loud,
safe outcome. A silent layout or ABI divergence is not.** Every constraint
below exists because it is in the second category -- the code compiles under
both engines and means different things.

A few families always take the fallback, deliberately:

- `__atomic_*` / `__sync_*` builtins. MIR binds one name to one signature, so
  any shim would write the wrong width for some caller.
- `_Thread_local` / `__thread`. Collapsing a thread-local to a global would
  trade a clean compile error for silent cross-thread corruption under
  `spawn`.

The JIT prelude (`JIT_PRELUDE` in `src/jit_engine.c`) deliberately does *not*
fake either family. Do not add them.

---

## Constraints that are permanent

### `__attribute__((packed))` is silently ignored

c2mir lays packed structs out at natural alignment and emits no diagnostic
worth catching. The struct compiles, the program runs, and its offsets
disagree with the host's.

`#pragma pack` **is** implemented (tracked through the preprocessor, so a
`pack(push)` spanning an `#include` stays correct -- which is how the Apple
and Windows SDKs actually use it). The attribute spelling goes through a
different path and is still ignored.

Two rules follow:

- Do not use `__attribute__((packed))` in inline C that may run under the JIT.
  Use `#pragma pack` if you need packing at all.
- **Codegen must never start emitting packed structs** without fixing c2mir
  first. This one is load-bearing rather than advisory: there is no
  diagnostic and no fallback to catch it.

### Host and c2mir must agree on every shared struct layout

Any runtime struct whose layout the host `cc` and c2mir compute differently
is a silent ABI seam. Program-side inline C reading such a struct gets wrong
offsets with nothing to tell it so.

The instance that found this: c2mir modeled `__uint128_t` as a two-word
struct with alignment 8 where clang gives 16, which skewed `ucontext_t` --
and with it `FiberBlock` -- by 32 bytes. `async-await-channel` and
`fiber-scheduler` hung under the JIT and only under the JIT. It is fixed in
the MIR fork, but the shape recurs.

The durable mitigations:

- Keep `ucontext_t`-bearing (and other layout-fragile) structs behind
  host-resident accessors rather than re-declaring them program-side.
- Assert `sizeof` / `offsetof` agreement at JIT startup for the structs that
  cross the boundary.
- Never hand-roll a `typedef` for a struct the stdlib already defines. Three
  mutually inconsistent local `TaskGroupBlock` typedefs in `emit_module.c`
  once put a `pthread_mutex_lock` at the wrong offset; the symptom was silent
  empty stdout on arm64 macOS.

### An unprototyped call is broken differently under each engine

c2mir treats a call with no prototype in scope as **all-anonymous-variadic**.
On Apple arm64 that shifts arguments by a register: shimming Darwin's
`_OSSwapInt16` by address alone made `htons(8080)` return `0xb8f6` instead of
`0x901f` -- corrupted ports and header lengths, not a crash.

On the `cc` path the same omission is merely bad (an implicit `int` return
truncates 64-bit values). Declare prototypes for everything; see
[c-integration-guide.md](c-integration-guide.md).

### `bool` truthiness diverges

Reading a byte that is neither 0 nor 1 through a `bool` lvalue is undefined,
and the two back ends resolve it differently: clang masks to bit 0, c2mir
tests the whole byte. A latent layout bug can therefore be invisible on
Linux/`cc` (glibc happens to zero the byte) and deterministic under the JIT
(macOS leaves `0x5A` there).

### Weak-symbol overrides do not cross the JIT boundary

The runtime declares `__attribute__((weak)) int tur_closure_headers_enabled`
and the emitted program overrides it with a strong definition. Under `cc` the
linker resolves that. Under the JIT, host code in this process was linked
long ago and reads its own weak copy, so `jit_sync_config_globals` copies the
program's value onto the host global after `MIR_link`.

That works for weak **data**. Weak **functions** cannot be fixed by copying a
value -- `scheduler_common.c` still carries six weak no-op
`tur_scheduler_*_st` functions with the same hazard. Any new weak default in
the runtime is a latent JIT bug; prefer an explicit runtime call from the
program's static init over a link-time handshake.

### `atexit` is intercepted

Registering the real `atexit` is worse than failing: the handler is JIT'd code
and `MIR_gen_finish` unmaps it before libc drains its list, so the process
dies in freed code at exit. The JIT owns the list and drains it on the entry
thread while the generated code is still mapped.

### `-I` does not reach c2mir

Include directories for the JIT come from `jit_sdk_include_dirs`
(`src/main.c`), rooted at `TUR_SDK_ROOT` or discovered by walking up from the
executable. A `-I` on the `tur jit` command line does not affect what c2mir
can find.

### MIR is a pinned fork, not upstream

`cmake/mir.cmake` pins `rjungemann/mir`, not `vnmakarov/mir`, for fixes that
have not landed upstream (`#pragma pack` support; a `make_one_ret` miscompile
that aliased both slots of a two-word struct return across a `goto`
backedge -- exactly the emitter's self-tail-call loop shape). Repoint with
`-DTUR_MIR_GIT_REPOSITORY=... -DTUR_MIR_GIT_TAG=...` when equivalents land
upstream.

**Repinning requires a fresh build directory.** An existing one silently
keeps fetching the old pin.

---

## Running the fixture corpus under the JIT

`tests/run-jit.sh` runs the corpus through `tur jit`. Two things about it are
easy to get wrong:

- **Environment parity with `tests/run.sh` is mandatory.** `run.sh` exports
  `TUR_BIND_LOOPBACK=1`, which `stdlib/httpd.tur` and `stdlib/async_socket.tur`
  read at run time to bind `INADDR_LOOPBACK` instead of `INADDR_ANY`. When
  `run-jit.sh` did not export it, an httpd fixture failed on macOS only -- BSD
  permits a wildcard bind while a specific address holds the port where Linux
  refuses -- and it read as a JIT defect for a while. Any env var `run.sh`
  exports, this harness must export.
- **A Release-built `tur` strips contracts under NDEBUG.** A fixture that
  asserts contract or refinement runtime behavior must carry `--keep-contracts`
  in its own `flags` rather than inheriting it from how `tur` was built. See
  [test-runner-contract.md](test-runner-contract.md).

On macOS, `CC` must name the same compiler that built `tur`, or fallback
links die on `___asan_version_mismatch_check_v8` and the numbers are garbage
in a direction that looks like a product bug. See the macOS build notes in
the top-level `CLAUDE.md`.

---

## Tuning knobs

| variable | default | effect |
|---|---|---|
| `TUR_JIT_GEN` | lazy | `eager` restores whole-program generation (slower start, compiles every function up front) |
| `TUR_JIT_STACK_MB` | 64 | entry stack size; MIR does not do gcc's sibling-call optimization, so a deep recursion the `cc` path survives can overflow here |
| `TUR_SDK_ROOT` | discovered | root for the runtime headers c2mir needs |

---

## See also

- [performance-guide.md](performance-guide.md) -- the interpreter / JIT / `cc`
  triangle, measured, and the self-tail-call rules that decide which of your
  loops are actually loops.
- [c-integration-guide.md](c-integration-guide.md) -- inline-C rules, several
  of which only bite under the JIT.
- [experimental-flags-guide.md](experimental-flags-guide.md) -- the
  `--enable=jit` gate and its lifecycle.
- `docs/upcoming/jit-engine-plan.md` and `docs/upcoming/jit-engine-j0-findings.md`
  -- in-flight work and the numbered findings log. This guide carries what is
  settled; those carry what is moving.
