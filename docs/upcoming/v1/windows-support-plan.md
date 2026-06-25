# Windows Support Plan

**Status:** Not started.

**Last updated:** 2026-06-25

---

## North star

**Windows support exists to make `turmeric-godot` usable on Windows.**

The shape of v1 is "drop a `.gdextension` into a stock Godot 4 project on
Windows and write `.tur` scripts that drive nodes." Everything in this plan
is scoped against that outcome. A user on Windows needs to:

1. Install Godot from godotengine.org (no engine recompile).
2. Drop in the `turmeric-godot.gdextension` shipped with a Windows `.dll`.
3. Attach a `.tur` script to a node, hit Play, and have it run.

What that means concretely:

- The **GDExtension shim** (built from `../turmeric-godot/`) must build as a
  Windows `.dll` for `windows-x86_64`.
- The **embedded `libturi`** statically linked into the shim must compile,
  link, and run on Windows. This is the interpreter path the shim uses for
  "edit -> Play" iteration.
- The **`tur` compiler** (`tur build --shared`) must run on Windows so the
  shim's AOT path can shell out and produce a script `.dll`. Cross-compiling
  the *output* (script `.dll`) from Linux CI is not enough -- the editor
  workflow assumes the user can compile a script locally on their Windows
  box.

What Godot already gives us, and therefore is **not** on the critical path:

- **Main loop, event polling, input, rendering, timers.** Godot owns these.
  Scripts that just implement `_ready` / `_process(dt)` / `_input(event)`
  never touch our reactor.
- **File and resource I/O during gameplay.** Scripts read assets through
  Godot's `ResourceLoader`, not our stdlib file APIs.
- **Thread pool for engine subsystems.** Godot threads physics, rendering,
  etc. internally.

So the previous plan's "five POSIX categories all need IOCP, pthreads
shims, ANSI colours, etc." framing is **too broad for v1**. The async
subsystem (`io_epoll.c` / `io_kqueue.c` -> `io_iocp.c`, scheduler, timer
wheel, threading primitives) only matters when a `.tur` script reaches for
turmeric-side concurrency. For the "paddle-pong-tur" demo and any script
that lives inside Godot's frame callbacks, it does not.

That gives a much shorter critical path. The longer "full standalone
Windows port" is preserved here, but explicitly de-prioritised.

---

## Critical path for `turmeric-godot` on Windows

The minimum that has to be true:

1. The Turmeric **compiler driver** (`src/main.c`, `src/pkg/...`) builds
   and runs on `windows-x86_64` under Clang-cl. (Used by the shim's AOT
   path; also lets users run `tur build --shared` from a Windows shell.)
2. The Turmeric **runtime / interpreter** (`libturi`, the `src/turi/`
   tree, plus the elaborator/codegen libs the shim statically links) builds
   on Windows.
3. The **generated C** that `tur emit-c` produces compiles under the same
   Windows toolchain the shim's `tur build --shared` invokes -- i.e. the
   preamble does not depend on `<pthread.h>`, `<unistd.h>`, GCC atomics,
   or POSIX-only types in any code path the GDExtension actually triggers.
4. The **GDExtension shim** in `../turmeric-godot/` builds a Windows `.dll`
   via SCons targeting `platform=windows arch=x86_64`, statically links
   `libturi`, and is installed by the existing `.gdextension` manifest.

That's it for the demo to run. Everything else is "nice to have on Windows
the way it works on Linux/macOS today" and is gated below.

---

## Phase WIN0 -- Compiler + runtime cross-compile to Windows

**Goal:** produce `tur.exe` and a static `libturi.lib` (or equivalent) for
`windows-x86_64` from CI, so the shim can ship them inside the GDExtension.

### Scope

The Turmeric front-end (parse, elaborate, codegen) and the interpreter
(`src/turi/`) are mostly portable C. The non-portable bits are:

- `src/main.c`, `src/pkg/...` -- file and path APIs (`opendir`, `stat`,
  `mkstemp`, `mkdir`, `unlink`, `getcwd`), subprocess (`popen`), and POSIX
  path separator assumptions.
- `src/compiler/diag.c` -- `isatty` / ANSI colour handling.
- Generated code preamble in `src/codegen/emit.c` -- pulls `<pthread.h>`
  and `__atomic_*` builtins unconditionally; needs guards for the
  Windows toolchain even when async features are unused at the script
  level. (If the script uses zero concurrency, the preamble's pthread
  symbols should not be referenced at link time -- audit and trim.)

### Approach

- Target **Clang-cl** as the Windows toolchain. It accepts GCC builtins
  (`__atomic_*`, `__builtin_*`), C11 `_Thread_local`, and the same
  preprocessor surface the existing code already uses. Pure MSVC is **out
  of scope for v1**; revisit only if Clang-cl proves insufficient.
- Add `src/platform_fs.h` with `#ifdef _WIN32` aliases:
  - `mkdir` / `_mkdir`, `unlink` / `_unlink`, `getcwd` / `_getcwd`.
  - `stat` / `_stat`, `S_ISDIR` via `_S_IFDIR`.
  - `opendir`/`readdir`/`closedir` -> `FindFirstFileA` /
    `FindNextFileA` / `FindClose` wrappers with a `DIR*`-shaped struct.
  - `mkstemp` / `mkstemps` -> `GetTempPathA` + `CreateFileA(CREATE_NEW)`.
  - `popen` / `pclose` -> `_popen` / `_pclose` (MSVC CRT has these).
- `pkg.c` constructs paths with `/`. Win32 file APIs accept forward slash,
  so no rewrite is needed; just keep `/` everywhere and avoid emitting paths
  to `cmd.exe`.
- Cross-compile from the existing CI matrix via a `windows-latest` runner
  with Clang-cl. Add `cmake/toolchain-windows-clang.cmake` and a
  `configure-windows` target in the Justfile.
- Link `ws2_32.lib` only when the async subsystem is actually built
  (see WIN3).

### Testing

- Build `tur.exe` in CI under `windows-latest`.
- Run the **codegen-only** subset of `tests/run.sh` -- the fixtures that
  exercise `emit-c` and `build` without invoking async or POSIX file APIs
  beyond what `src/platform_fs.h` already wraps. The full suite is gated
  on WIN3.

### Out of scope (deferred)

- Fiber assembly stub (`fiber_ctx_x64.S` -> `fiber_ctx_x64_win.asm`).
- IOCP backend (`io_iocp.c`).
- pthreads shim across `scheduler.c` / `stm.c` / `timer_wheel.c`.

Those land in WIN3 if/when scripts inside Godot start needing them.

---

## Phase WIN1 -- Generated code preamble portability

**Goal:** the C that `tur emit-c` produces compiles under Clang-cl, for
scripts that do not use async/STM/threads.

### Background

The codegen preamble (`src/codegen/emit.c`) currently emits unconditional
`#include <pthread.h>` and uses `__atomic_*` builtins in the boxed
Result/Option runtime helpers. Clang-cl handles the builtins fine, but
`<pthread.h>` is not present.

### Changes

- Split the preamble into "core" (always emitted -- Result/Option, RC,
  cons cells, format helpers) and "async-runtime" (emitted only when the
  program reaches a concurrency primitive). Most `.tur` scripts in
  `turmeric-godot` will compile with the core preamble only.
- For the core preamble: replace `pthread.h` with `<stdatomic.h>` (C11)
  for the few atomic counters used by RC, and gate the rest behind
  `#ifdef _WIN32` / `#else` blocks where needed.
- Carry the runtime-feature flag through to the AOT subprocess: the shim
  invokes `tur build --shared --no-async <script.tur>` for scripts that
  declare no async usage. (Static analysis of the elaborated AST already
  knows whether `scheduler.c` / fiber primitives are reachable.)

### Testing

- A new fixture group `tests/fixtures/windows-core/` covering the script
  shapes the `turmeric-godot` examples actually use (defstruct, defn,
  inline-C-free arithmetic, cstr formatting). Snapshot the emitted C and
  confirm it compiles under Clang-cl in CI.

---

## Phase WIN2 -- `turmeric-godot` GDExtension Windows build

**Goal:** build the shim from `../turmeric-godot/` as a Windows `.dll` and
load it in a stock Godot 4 binary.

### Scope (lives mostly in `../turmeric-godot/`, tracked here for visibility)

- Add `windows` arch targets to the `SConstruct` in `turmeric-godot`.
- Statically link the WIN0/WIN1 `libturi` artifact.
- Update `turmeric-godot.gdextension` manifest with
  `windows.debug.x86_64` / `windows.release.x86_64` entries.
- Verify the shim's "compile script on demand" path -- the subprocess
  invocation needs to use `tur.exe` (with the `.exe` suffix) and pick up
  the right toolchain on Windows. `tur build --shared` already produces
  `lib<name>.so`; on Windows we want `<name>.dll`. Wire that through the
  shared-library output naming in `src/main.c` (RP0 path: replace `.so`
  with `.dll`, drop the `lib` prefix, on `_WIN32`).
- Cache directory: `%APPDATA%/Godot/turmeric-cache/` or just keep using
  `<project>/.godot/turmeric-cache/` -- the latter works cross-platform
  since Godot already creates `.godot/`. Prefer the in-project path.

### Testing

- Build the `examples/spike` Godot project on a `windows-latest` runner
  (Godot ships a headless Windows binary). Confirm
  `[turmeric-godot] initialize(level=...)` appears in the log.
- Once the paddle-pong demo lands, run it headless in CI as a smoke test.

---

## Phase WIN3 -- Deferred: full async subsystem on Windows

**Goal:** make turmeric-side concurrency (`spawn`, channels, STM, async
I/O) work inside the Windows runtime.

**Status:** Deferred until a `turmeric-godot` user actually reaches for
these features. Tracking the original plan's content here so it does not
get lost.

This is where the previous plan's WIN0/WIN1/WIN2 phases live:

- **Fiber assembly stub** -- add `src/async/fiber_ctx_x64_win.asm` (MASM
  syntax) that saves the Windows x64 callee-saved register set including
  XMM6--XMM15, and uses RCX (not R12) for the first-argument shim.
- **IOCP I/O backend** -- add `src/async/io_iocp.c` implementing `tur_io_*`
  via `CreateIoCompletionPort` / `GetQueuedCompletionStatusEx`, with
  `PostQueuedCompletionStatus` for cross-thread wake-ups (replacing the
  POSIX pipe pair). Sockets use `WSASocket(..., WSA_FLAG_OVERLAPPED)`.
  Add `WSAStartup` to runtime init. Link `ws2_32.lib`.
- **Threading primitives** -- add `src/platform_threads.h` mapping
  `tur_mutex_t` / `tur_cond_t` / `tur_thread_t` to `CRITICAL_SECTION` /
  `CONDITION_VARIABLE` / `HANDLE`+`CreateThread`. `pthread_once` ->
  `InitOnceExecuteOnce`. `clock_gettime(CLOCK_MONOTONIC)` ->
  `QueryPerformanceCounter`. Affects `src/async/scheduler.c`,
  `src/runtime/stm.c`, `src/async/timer_wheel.c`.
- **Atomics** -- Clang-cl supports `__atomic_*` builtins, so no shim is
  needed. (Pure MSVC would require an `Interlocked*` wrapper layer; out
  of scope.)

ARM64 Windows stays out of scope -- GitHub Actions still does not offer
free ARM64 Windows runners, and `armasm64.exe` syntax diverges from the
GNU `as` syntax used in `fiber_ctx_arm64.S`.

When this phase lands, the codegen split from WIN1 collapses: the
async-runtime preamble becomes a real Windows runtime instead of a
linker error.

---

## Phase WIN4 -- Deferred: standalone CLI polish

**Goal:** standalone `tur.exe` is a first-class developer experience on
Windows (not just a build subprocess invoked by the shim).

**Status:** Deferred. The shim drives all editor-time `tur` invocations
in v1; users who want a standalone Windows CLI workflow can wait.

Content:

- **Coloured diagnostics** -- `src/compiler/diag.c` calls
  `_isatty(_fileno(stderr))` on Windows, and enables VT100 via
  `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` on Windows 10+.
  Falls back to plain text if the call fails.
- **REPL** -- `libedit` is not available on Windows; the REPL falls back
  to plain `fgets` (already the path taken when `libedit` is absent).
- **Release packaging** -- add `windows-x86_64` to the release matrix in
  `.github/workflows/release.yml`, producing a `.zip` artifact.
- **Examples / Raylib** -- skip `TUR_EXAMPLES` on Windows builds by
  default. Raylib itself supports Windows, but the demos belong to
  whichever spice repo ships them, not to this plan.

---

## Out of scope

- **ARM64 Windows.** No free GitHub Actions runner; not pursuing.
- **MinGW / MSYS2.** Clang-cl is the chosen toolchain.
- **Windows Subsystem for Linux (WSL).** WSL runs the Linux build today;
  no changes needed.
- **WASM.** Builds separately via Emscripten; unaffected.
- **In-engine REPL on Windows.** Not in `turmeric-godot` v1 on any
  platform; see the Godot binding plan's non-goals.
- **iOS / Android / web export of Godot projects using Turmeric scripts.**
  Desktop only.

---

## Open questions

1. **Codegen split granularity.** WIN1 proposes splitting the preamble
   into "core" and "async-runtime." How fine-grained does this need to
   be? A first pass that gates only `<pthread.h>` and the timer-wheel
   symbols may be enough; the RC atomics already work under Clang-cl.
2. **Shim toolchain detection.** When `tur build --shared` runs inside
   the GDExtension on Windows, which C compiler does it invoke? Clang-cl
   if present, fall back to MSVC `cl.exe`, or bundle a known-good
   toolchain with the GDExtension? Bundling adds size but removes a
   support burden.
3. **Script `.dll` caching across Godot versions.** The cache key today
   is the script path + content hash. On Windows the path is
   case-insensitive; normalise paths before hashing to avoid duplicate
   cache entries.
4. **AOT vs interpreter default on Windows.** macOS/Linux defaults to AOT
   for steady-state speed. Windows users without a system compiler will
   hit a confusing error on first Play. Consider defaulting to
   interpreter on Windows until WIN2's toolchain story is settled.
