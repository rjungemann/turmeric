# Windows Support Plan (ARCHIVED)

**This is the original end-to-end plan, kept for its history:** the toolchain
rationale, the WIN0/WIN1 blockers, the (macOS-only) Wine loop, and the full WIN3
investigation. WIN0, WIN1, and the hard core of WIN3 (async I/O + both fiber
context switches) are DONE on the `windows-bringup` branch. Remaining work --
WIN2 (the turmeric-godot GDExtension), the WIN3 async tail, WIN4, and the codegen
defects the port surfaced -- has moved to
[docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md).

---

**Status (final, before archiving):** WIN0 and WIN1 landed. `tur.exe` builds, and
`tur build` compiles and runs real programs on Windows. WIN3's select() reactor
backend and register-snapshot fiber switches landed; the async subset passes ~65
fixtures. WIN2 (Godot shim) is not started.

**Last updated:** 2026-07-14

---

## Toolchain: MinGW/UCRT64, not Clang-cl

The plan below names **Clang-cl** as canonical and puts MinGW/MSYS2 under *Out
of scope*. **That was reversed during WIN0**, deliberately. The tree is far
closer to MinGW than to MSVC:

- The codegen preamble emits `<pthread.h>` and `<unistd.h>`, and the runtime
  uses `select()` and `clock_gettime()`. MinGW (UCRT64) has all of them; MSVC
  has none.
- `tur build` shells out to `cc` with `-O2 -fPIC -shared -lm`. MSYS2 ships a
  real `cc.exe` that takes those flags verbatim.
- MSYS2 even supplies libedit (via `wineditline`), so the REPL keeps line
  editing -- contradicting WIN4's claim that it must fall back to `fgets`.

MinGW also needs no Visual Studio install: the whole toolchain lives in
`C:\msys64` and is managed with `pacman`. Setup is `just setup-windows`; see
the "Windows setup" block at the bottom of the Justfile.

Clang-cl may still matter for WIN2 if the Godot `.dll` must match an MSVC-built
Godot -- GDExtension is a C ABI and godot-cpp supports `use_mingw=yes`, so this
is expected to be fine, but it is unverified.

## What actually blocked WIN0 (none of it was in this plan)

The real blockers were not the POSIX-API list below. They were:

1. **`src/async/io.h` shadowed the CRT's `<io.h>`.** `-Isrc/async` is on the
   include path, and GCC searches `-I` dirs before system dirs *even for
   angle-bracket includes*. MinGW's `<io.h>` was therefore never included, so
   `_finddata_t` was undefined (which breaks `<dirent.h>` itself) and
   `mkdir`/`getcwd` were never declared. Renamed to `async_io.h`. Invisible on
   Linux/macOS, which have no system `<io.h>`.
2. **`_POSIX_C_SOURCE` was defined on Windows** by an `#else` branch meant for
   glibc. MinGW reads that macro as "hide the Win32 CRT names" -- the exact
   opposite of its intent.
3. **`-std=c11` sets `__STRICT_ANSI__`**, under which MinGW's own headers are
   not self-consistent. Windows builds use `-std=gnu11`.
4. **Text-mode stdout.** Windows rewrites every `\n` to `\r\n`, which corrupted
   generated C, `tur fmt --stdout`, the Content-Length framing of the LSP/DAP
   servers, and the output of every compiled Turmeric program. Both `tur` and
   generated `main()` now set binary mode.
5. **No `.gitattributes`**, so Git for Windows checked sources out as CRLF --
   and the reader copies inline-C bodies verbatim, leaking `\r` into codegen.

## Verified

- `tur.exe` builds under MSYS2/UCRT64 (`just build-windows`).
- `tur emit-c` is **byte-identical** to the Linux-generated snapshots on all
  140 fixtures carrying an `expected.c` (checked before regenerating them).
- `tur build` compiles and runs real fixtures end to end.
- `--version`, `check`, `emit-c`, `interpret` all work.

### Suite result

`TUR=./build-win/tur.exe bash tests/run.sh` -- **2128 passed, 51 failed, ~20 min**
(vs ~4-5 min on Linux; the gap is process-spawn cost plus Defender scanning
every freshly-linked .exe -- worth an exclusion on the temp dir before treating
20 min as the real number).

Every one of the 51 is the deferred async/fiber runtime:

- **48** are `httpd-*` / `reactor-*` / `async-*` / `scheduler-io-park` /
  `taskgroup-async` -- all routed through the `io_iocp.c` stub. Expected; WIN3.
- ~~2 multishot + 1 multithread~~ **FIXED (WIN3-C).** The generated-program
  ucontext moved from Win32 Fibers to a register-snapshot switch (thread-agnostic
  + re-entrant), so `fh-multishot-value`, `multishot-effect-cont-kv-sugar`, and
  `scheduler-multithread` all pass. The report is archived at
  [docs/archive/win32-fiber-multishot-abort.md](../../archive/win32-fiber-multishot-abort.md).

The 13 non-async failures from the first pass are all fixed (image self-exe,
fnmatch->PathMatchSpecA, process spawn->_spawnvp, the `__has_include` hoister
wrap, a missing-hamt.h implicit decl, two dead `printf_s` externs, and `C:\tmp`).

### Two bugs the port surfaced that are NOT Windows bugs

- Generated C has type errors that **GCC >= 14 rejects** (permerrors). MSYS2
  ships GCC 16, so it failed 137 fixtures there; a Linux box on GCC 14+ fails
  identically. Worked around with `-Wno-error=`; see
  [docs/reported/codegen-gcc14-permerrors.md](../../reported/codegen-gcc14-permerrors.md).
- At `-O0` the generated C fails to link: `tur_get_contract_handler` /
  `tur_set_contract_handler` are declared but never defined. At `-O1`+ the calls
  are optimised away, which is why nobody has noticed.

## Deferred, and failing loudly rather than silently

- `src/async/io_iocp.c` -- async I/O backend is a stub (WIN3).
- `src/async/fiber_ctx_win.c` -- the SysV context switch takes its first
  argument in `%rdi` (Windows uses `%rcx`) and `tur_ctx_t` has no slots for the
  XMM6-15 the Windows x64 ABI requires preserved. Reusing it would corrupt
  state, so it aborts (WIN3).
- POSIX `<regex.h>` has no MinGW equivalent, so a program using `stdlib/re.tur`
  gets a `#error` naming the cause.
- Signals, DAP stdout capture, and the fork-based fixture worker compile out.

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

## Local Wine + llvm-mingw loop (macOS host)

Wine + `llvm-mingw` lets you iterate on **WIN0** (compiler/runtime cross-build)
and **WIN1** (generated-code preamble portability) entirely on a Mac, without
round-tripping through GitHub Actions for every prod-cycle. This is **not** the
gate for WIN2 (Godot shim) or WIN3 (async/IOCP) -- CI on a real
`windows-latest` runner stays authoritative for those. But for the "does the
compiler build, does the emitted C compile" loop, it cuts a 5-10 minute CI
round-trip down to seconds.

### Toolchain choice: `llvm-mingw` for local, Clang-cl for CI

The plan targets **Clang-cl** as the canonical Windows toolchain. Clang-cl
under Wine is a hassle (needs the MSVC headers + CRT, which are licensed and
fiddly to stage), so for local iteration use **`llvm-mingw`** instead -- a
self-contained tarball of `clang` + `lld` + MinGW-w64 headers/import libs that
runs natively on macOS and produces real Windows PE binaries. The preprocessor
surface (`_WIN32`, `__atomic_*`, `_Thread_local`, `<stdatomic.h>`) is the same
as Clang-cl's, so portability bugs caught here will also be caught in CI. The
small delta -- MinGW's CRT vs. MSVCRT, `__declspec(dllexport)` vs.
`__attribute__((dllexport))`, name mangling for `extern "C"` -- only matters
once you're shipping the actual `.dll`/`.exe` artifact, which is what CI is
for.

### macOS setup (Homebrew + mise)

```sh
# 1. Wine (CrossOver-flavoured fork, runs Win64 binaries on Apple Silicon)
brew install --cask wine-stable
# Apple Silicon: also install Rosetta if not already present
softwareupdate --install-rosetta --agree-to-license

# 2. llvm-mingw -- self-contained Clang + lld + MinGW-w64 toolchain
#    Pinned via mise so the version is reproducible across machines.
mise use -g llvm-mingw@20250528  # or whichever release is current
# (If no mise plugin exists, fall back to a manual install:
#   curl -L -o /tmp/llvm-mingw.tar.xz \
#     https://github.com/mstorsjo/llvm-mingw/releases/download/20250528/llvm-mingw-20250528-ucrt-macos-universal.tar.xz
#   sudo tar -C /opt -xf /tmp/llvm-mingw.tar.xz
#   sudo ln -s /opt/llvm-mingw-20250528-ucrt-macos-universal /opt/llvm-mingw
#   then add /opt/llvm-mingw/bin to PATH)

# 3. Verify
x86_64-w64-mingw32-clang --version  # should print clang version + target
wine64 --version                    # should print wine-9.x or later
```

Wine first-run will populate `~/.wine/` -- let it finish before invoking any
`.exe`. On Apple Silicon, Wine runs x86_64 binaries via Rosetta automatically;
for ARM64 Windows binaries (out of scope for v1 anyway) you'd need
`wine-crossover` or a separate ARM64 prefix.

### CMake toolchain file

Add `cmake/toolchain-windows-mingw.cmake` for local cross-builds (alongside
the `toolchain-windows-clang.cmake` WIN0 already plans for CI):

```cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-clang)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-clang++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_AR           ${TOOLCHAIN_PREFIX}-ar)
set(CMAKE_RANLIB       ${TOOLCHAIN_PREFIX}-ranlib)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Run the produced .exe through Wine so CTest / tests/run.sh work transparently
set(CMAKE_CROSSCOMPILING_EMULATOR wine64)
```

Configure + build:

```sh
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-mingw.cmake \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-win -j
wine64 build-win/tur.exe --version  # smoke test
```

`CMAKE_CROSSCOMPILING_EMULATOR=wine64` means `ctest` and any harness that
invokes the built binary directly will Just Work -- the test runner does not
need to know it's running a Windows binary.

### Running the codegen-only fixture subset locally

WIN0's testing plan calls for running the `emit-c` / `build` fixtures that
don't reach POSIX async. Once `tur.exe` is built and Wine is on PATH, the same
`tests/run.sh` invocation works:

```sh
TUR=wine64\ $(pwd)/build-win/tur.exe \
  timeout 600 bash tests/run.sh tests/fixtures/windows-core/
```

(Use the dedicated `windows-core/` fixture group WIN1 introduces; the full
suite stays gated on WIN3.)

### Caveats -- don't trust Wine for everything

- **IOCP / overlapped I/O** under Wine is approximate. WIN3 async work needs
  real Windows.
- **GUI / Godot integration** under Wine adds a second variable. WIN2's
  `turmeric-godot.dll` smoke test stays on `windows-latest` CI.
- **CRT divergence.** `llvm-mingw` defaults to UCRT, Clang-cl defaults to
  MSVCRT. They agree on the C11 surface this plan cares about, but if you
  see a divergence in `printf` formatting, `errno` codes, or wide-char
  behaviour, suspect the CRT before suspecting the code.
- **First Wine run is slow** (~30s to build the prefix). Subsequent runs are
  near-native.

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

## Phase WIN3 -- async on Windows (in progress; the plan below was wrong)

**The original WIN3 framing -- "add io_iocp.c, add a MASM fiber stub" -- was
written for Clang-cl and does not match how compiled programs actually work.**
Investigation on MinGW/UCRT64 found:

- A compiled async program does NOT use the `io_backend` abstraction
  (epoll/kqueue/iocp). It emits a **`select()`-based reactor** and LINKS the
  `tur_reactor_*` implementation from `libturi.a`. `select()` works on Windows
  (Winsock), so **no IOCP backend is needed for the compiled path at all**.
- `async-sleep`, `reactor-timer`, `reactor-linear`, `reactor-chan-bridge` and
  friends already run correctly on Windows once linked.

So WIN3 for the compiled fixtures is three tiers, none of them IOCP:

**A. libturi.a resolution (DONE).** The `-lturi` autolink resolver only finds an
*installed* SDK (`share/turmeric`); dev builds rely on `-Lbuild/src` from
`TUR_CC_FLAGS`. That was hardcoded to `build/src`, but the Windows build lives in
`build-win/`, so every reactor/async fixture failed to *link*. `tests/run.sh`
now derives the `-L` from `$(dirname $TUR)/src`. Unblocked ~32 fixtures.

**B. Winsock socket port (IN PROGRESS).** `async_socket.tur` is ported and
verified (WSAStartup / ioctlsocket / closesocket / WSAGetLastError). But that
alone fixes no fixture, because the failing socket fixtures do NOT use the stdlib
module -- **~31 of them carry their own `socket(AF_INET...)` inline-C** and
reimplement the same POSIX idioms (`fcntl(F_GETFL/F_SETFL, O_NONBLOCK)`,
`close(fd)`, `errno == EWOULDBLOCK`). So the surface is distributed across the
fixtures, not centralized in stdlib.

Two incompatibility classes, and the second is the harder one:

- *Compile-time*: `F_GETFL`/`F_SETFL`/`O_NONBLOCK`/`fcntl` don't exist for
  Windows sockets.
- *Runtime*: the would-block check reads `errno`, but Winsock reports through
  `WSAGetLastError()` -- so even if it compiled, the fiber would never park and
  the async loop would spin or break.

Editing 31 fixtures is the wrong fix. The right one is a **Winsock compat shim
emitted into the preamble** (gated on a socket-reachability flag so it doesn't
churn the 140 non-socket snapshots): define `F_GETFL/F_SETFL/O_NONBLOCK`, a
`fcntl` that maps `F_SETFL|O_NONBLOCK` to `ioctlsocket(FIONBIO)`, and route the
would-block check. `close(fd)` on a socket is the one that resists a clean
macro (it collides with file `close`); options are a socket-tracking set or
accepting the leak (the process exits anyway).

Still TODO after that: `async_pipe.tur` (stdin/stdout non-blocking -- genuinely
hard, Windows consoles have no `O_NONBLOCK`; needs overlapped I/O or a reader
thread) and `httpd.tur` (its own accept/close/fcntl loop plus pthreads, which
winpthreads already provides).

**Bind test listeners to 127.0.0.1, not INADDR_ANY.** When the socket fixtures
are ported to run on Windows, their server side must bind `INADDR_LOOPBACK`
(127.0.0.1), not `INADDR_ANY`. A loopback-only listener does not trigger the
Windows Defender Firewall "allow this app" dialog, whereas `INADDR_ANY` does --
and the suite builds each fixture to a fresh temp path, so `INADDR_ANY` would pop
that dialog dozens of times per run. The fixtures are same-process server+client
tests that already connect to "127.0.0.1", so this is functionally a no-op for
them. This applies to the *fixtures* only; `async_socket.tur` itself stays
`INADDR_ANY` (a real server wants all interfaces).

**C. Fiber context switch (TODO).** The Win32-Fiber shim's multishot abort and
thread-affinity hang (`fh-multishot-value`, `multishot-effect-cont-kv-sugar`,
`scheduler-multithread`). These need the real register-snapshot x64 context
switch below.

### WIN3 results so far

On the `reactor-/async-/scheduler-/taskgroup/httpd-` subset: **64 passed, 12
failed** (was 32 with tier A alone, 55 after the select backend). The 12
remaining are: 8 `pipe()` build failures (the socket-only-select limit below, a
platform boundary) and 3 concurrency stdout mismatches (httpd-h4-keepalive,
httpd-h6-routing, taskgroup-async).

Landed:

- Winsock compat shim (`g_needs_winsock`, emitted for socket programs) --
  fcntl->ioctlsocket, WSAGetLastError->errno, socket-aware close, WSAStartup.
- `async_socket.tur` ported; `TUR_BIND_LOOPBACK` so the suite binds 127.0.0.1
  and never trips the firewall.
- **A real select()-based reactor I/O backend** (`io_iocp.c`, replacing the
  stub). Verified: async-echo-server and the core httpd fixtures (h1/h2/h3/h5/h7,
  the mw-* middleware set) build and run byte-identical on Windows.
- **WIN3-C: a real Windows x64 fiber context switch** (`fiber_ctx_x64_win.S`,
  replacing the aborting stub). libturi's internal fibers (the reactor's
  `tur_ctx_swap` path) now run instead of aborting -- fixing the
  `reactor-fibers-*` cluster and the httpd-async fixtures that share it.
  RCX-first-arg ABI, preserves RSI/RDI/XMM6-15; TEB stack fields omitted because
  GCC's `___chkstk_ms` does not consult them. NOTE: this is the libturi fiber
  path; the generated-program ucontext (Win32 Fibers) is separate and still has
  the multishot abort / thread-affinity issues.

The 21 that remain are the genuinely-hard tail, and two clusters are Windows
platform limits rather than missing work:

- **`reactor-fd-*` (pipe polling): a hard `select()` limit.** These create a
  `pipe()` and register the pipe fds with the reactor. Windows `select()` is
  **socket-only** -- it cannot poll pipe/file fds at all. Making them work needs
  pipe->loopback-socketpair AND read/write->recv/send routing (which collides
  with file I/O). Intricate, fragile, and outside the turmeric-godot north star
  (Godot scripts don't poll pipes). Left as honest build failures.
- **`httpd-async-*` / `reactor-fibers-*` / `taskgroup-async` (stdout mismatch):**
  multi-fiber concurrency under the select backend -- timing/scheduling
  divergence, not yet diagnosed.
- **`scheduler-multithread`:** the tier-C Win32-Fiber thread-affinity hang.

The original deferred content follows, kept for the fiber-context-switch design
(tier C) which is still accurate:

## Phase WIN3 (original) -- full async subsystem on Windows

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
