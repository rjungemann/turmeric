# Windows Support Plan

**Status:** Not started.

**Last updated:** 2026-05-16

---

## Summary

Turmeric currently builds and runs on macOS and Linux only. Five categories of
POSIX-specific code block a native Windows (MSVC or Clang-cl) build:

1. **Assembly fiber context stubs** -- use the System V ABI, not the Windows
   x64 calling convention.
2. **I/O multiplexing** -- uses `epoll` (Linux) or `kqueue` (macOS); Windows
   needs an IOCP backend.
3. **Threading primitives** -- use pthreads throughout; Windows needs either
   a thin shim or direct Win32 equivalents.
4. **Compiler driver and package manager** -- use POSIX file, path, and
   process APIs (`opendir`, `popen`, `mkstemp`, `stat`, `unlink`, ...).
5. **Diagnostics** -- `isatty`/`fileno` and ANSI colour codes need minor
   Win32 adjustments.

Code generation (`emit.c`) and the WASM build are not affected.

The plan is broken into five phases that can largely be worked in order, since
each phase unblocks the next layer of the build.

---

## Phase WIN0 -- x64 assembly fiber context (`fiber_ctx_x64.S`)

**Goal:** make the hand-rolled context switch work under the Windows x64
calling convention.

### Background

`fiber_ctx_x64.S` saves and restores callee-saved registers per the
System V AMD64 ABI:

```
RBX, RBP, R12, R13, R14, R15, RSP, RIP
```

The Windows x64 ABI requires saving all of those **plus** the XMM6--XMM15
floating-point registers (which are callee-saved on Windows but caller-saved on
System V). `fiber_entry_shim` passes the fiber pointer in `R12` (System V first
argument scratch register); Windows puts the first argument in `RCX`.

The ARM64 assembly (`fiber_ctx_arm64.S`) uses the architecture-level
ARM64 ABI which is essentially identical between platforms, but the GNU `as`
assembler directive syntax is incompatible with the MSVC ARM assembler
(`armasm64.exe`). Because GitHub Actions does not yet offer free ARM64 Windows
runners, ARM64 Windows is out of scope for this plan.

### Changes

- Add `src/fiber_ctx_x64_win.asm` (MASM syntax) that saves/restores the
  Windows x64 callee-saved register set including XMM6--XMM15.
- Update `src/CMakeLists.txt` to select the correct stub based on
  `CMAKE_SYSTEM_NAME STREQUAL "Windows"` and toolchain.
- Alternatively, add a `#ifdef _WIN32` branch in `fiber_ctx.h` that uses
  `fiber_ctx_x64_win.asm`, keeping the POSIX path unchanged.
- Gate the `FATAL_ERROR` for unsupported architectures to exclude
  `x86_64`+Windows once the Windows stub exists.

### Testing

Add a minimal smoke test: create a fiber, switch to it, switch back, confirm
registers are intact. This can be a C unit test in `tests/turi/`.

---

## Phase WIN1 -- I/O backend (`io_iocp.c`)

**Goal:** implement the `tur_io_*` abstraction using Windows I/O Completion
Ports so the async scheduler works on Windows.

### Background

The I/O layer is already abstracted behind `src/io.h`. `io_epoll.c` and
`io_kqueue.c` each implement the same interface; adding `io_iocp.c` follows
the same pattern.

IOCP is a completion-based API (the kernel notifies you when an operation
*finished*), whereas epoll/kqueue are readiness-based (the kernel notifies you
when an fd is *ready*). This mismatch affects how socket reads and writes are
issued, but it does not change the interface that `scheduler.c` sees.

### Key API mapping

| POSIX (epoll/kqueue)         | Windows IOCP equivalent                           |
|------------------------------|---------------------------------------------------|
| `epoll_create1()` / `kqueue()` | `CreateIoCompletionPort(INVALID_HANDLE_VALUE, ...)` |
| `epoll_ctl(ADD)` / `EV_ADD`  | `CreateIoCompletionPort(fd, iocp, key, 0)`        |
| `epoll_wait()` / `kevent()`  | `GetQueuedCompletionStatusEx()`                   |
| `pipe()` wakeup              | `PostQueuedCompletionStatus()` for self-wake      |
| `fcntl(O_NONBLOCK)`          | `ioctlsocket(FIONBIO)` / overlapped I/O           |
| `read()` / `write()` on pipe | `ReadFile()` / `WriteFile()` (overlapped)         |
| `close(fd)`                  | `CloseHandle()`                                   |

### Changes

- Add `src/io_iocp.c` implementing `tur_io_*` via IOCP and overlapped I/O.
- Sockets on Windows must be created with `WSASocket(..., WSA_FLAG_OVERLAPPED)`
  and associated with the IOCP handle before use.
- Add `WSAStartup` / `WSACleanup` calls to the Windows initialisation path
  (probably in `main.c` or a new `src/platform_win.c`).
- Update `src/CMakeLists.txt` to select `io_iocp.c` on Windows and link
  `ws2_32.lib`.

### Notes

- The `pipe()` wakeup pair used in epoll/kqueue for cross-thread wake-up
  should be replaced with `PostQueuedCompletionStatus()` on Windows, which is
  the idiomatic IOCP self-notification mechanism.
- IOCP completion packets carry a `ULONG_PTR` key; use this to distinguish
  timer-wheel notifications from I/O completions.

---

## Phase WIN2 -- threading primitives

**Goal:** replace all pthreads usage with Win32 equivalents (or a thin
compatibility shim).

### Affected files

| File | pthreads APIs used |
|------|--------------------|
| `scheduler.c` | `pthread_t`, `pthread_create`, `pthread_join`, `pthread_mutex_*`, `pthread_cond_*`, `pthread_once`, `__thread` |
| `stm.c` | `pthread_mutex_*`, `pthread_cond_*`, thread-local `TUR_THREAD_LOCAL` |
| `timer_wheel.c` | `pthread_t`, `pthread_create`, `pthread_join`, `pthread_mutex_*`, `nanosleep`, `clock_gettime(CLOCK_MONOTONIC)` |

### Approach options

**Option A -- thin pthread shim (`src/platform_threads.h`):**
Define `tur_mutex_t`, `tur_cond_t`, `tur_thread_t`, etc. with inline
implementations for each platform. This keeps the call sites unchanged and
is easier to audit. Recommended.

**Option B -- pthreads-win32 / winpthread:**
Use the `pthreads-win32` (LGPL) or MinGW `winpthread` library as a drop-in.
Saves implementation effort but adds a dependency; LGPL may be a concern.

### Win32 mapping (Option A)

| pthread | Win32 |
|---------|-------|
| `pthread_mutex_t` | `CRITICAL_SECTION` |
| `pthread_mutex_init/lock/unlock/destroy` | `InitializeCriticalSection`, `Enter/LeaveCriticalSection`, `DeleteCriticalSection` |
| `pthread_cond_t` | `CONDITION_VARIABLE` (Vista+) |
| `pthread_cond_init/wait/signal/broadcast/destroy` | `InitializeConditionVariable`, `SleepConditionVariableCS`, `WakeConditionVariable`, `WakeAllConditionVariable` |
| `pthread_t` / `pthread_create` / `pthread_join` | `HANDLE` / `CreateThread` / `WaitForSingleObject` + `CloseHandle` |
| `pthread_once` | `InitOnceExecuteOnce` |
| `__thread` / `TUR_THREAD_LOCAL` | `__declspec(thread)` (MSVC) or `_Thread_local` (C11, supported by Clang-cl) |
| `nanosleep()` | `Sleep()` (millisecond granularity is sufficient) |
| `clock_gettime(CLOCK_MONOTONIC)` | `QueryPerformanceCounter()` + `QueryPerformanceFrequency()` |

### Atomic operations

`scheduler.c` and `stm.c` use `__atomic_*` GCC/Clang builtins. Clang-cl
supports these; MSVC does not. For pure MSVC, replace with `Interlocked*`
intrinsics or wrap in `src/platform_atomic.h`. The simplest path is to require
Clang-cl on Windows.

---

## Phase WIN3 -- compiler driver and package manager

**Goal:** make `main.c` and `pkg.c` compile and run correctly on Windows.

### `main.c`

| POSIX API | Windows replacement |
|-----------|---------------------|
| `mkdir()` | `CreateDirectoryA()` or `_mkdir()` (MSVC CRT) |
| `stat()` / `S_ISDIR()` | `GetFileAttributesA()` with `FILE_ATTRIBUTE_DIRECTORY` check |
| `opendir()` / `readdir()` / `closedir()` | `FindFirstFileA()` / `FindNextFileA()` / `FindClose()` |
| `unlink()` | `DeleteFileA()` or `_unlink()` (MSVC CRT) |
| `mkstemp()` / `mkstemps()` | `GetTempPathA()` + `CreateFileA(CREATE_NEW)` |
| `popen()` / `pclose()` | `_popen()` / `_pclose()` (MSVC CRT -- available) |
| `getcwd()` | `_getcwd()` (MSVC CRT) |
| `WIFEXITED()` / `WEXITSTATUS()` | Not needed; `_popen` child exit codes come from `_pclose()` directly |
| Path separator `/` | Accept both `/` and `\`; use forward slash throughout (Windows APIs accept it) |

Most of these have direct MSVC CRT equivalents with a leading `_`. A
`src/platform_fs.h` header with `#ifdef _WIN32` aliases is sufficient.

### `pkg.c`

Same file/path/process APIs as `main.c`. The `run_capture()` helper uses
`popen` to invoke `git`; `_popen` on Windows works identically for this use
case.

Directory scanning and `mkdirp` need the same replacements listed above.

Path separators: `pkg.c` constructs paths with `/`. Forward slash is accepted
by all Win32 file APIs (but not by some shell/cmd constructs), so passing
paths constructed with `/` to Win32 APIs is safe.

---

## Phase WIN4 -- diagnostics and build system

**Goal:** colour diagnostics, CMake configuration, and CI runner.

### `diag.c`

| POSIX API | Windows replacement |
|-----------|---------------------|
| `isatty(fileno(stderr))` | `_isatty(_fileno(stderr))` (MSVC CRT) |
| ANSI colour codes (`\033[31m`) | Enable VT100 mode via `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` on Windows 10+; fall back to plain text on older Windows |

Add a one-time initialisation call in `diag.c` (guarded by `#ifdef _WIN32`)
that enables virtual terminal processing. If the call fails, set a flag to
suppress ANSI codes.

### CMake

- Add `cmake/toolchain-windows-clang.cmake` (or document the recommended
  `clang-cl` invocation) for cross-compile or native Windows builds.
- Update the `TUR_EXAMPLES` guard so Raylib is not fetched on Windows builds
  by default (Raylib itself supports Windows but adds complexity).
- Link `ws2_32` and `ntdll` on Windows (required by IOCP and
  `QueryPerformanceCounter`).

### Justfile

Add a `configure-windows` target:

```
configure-windows:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-clang.cmake \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

### GitHub Actions CI

Add `windows-latest` to the CI matrix once WIN0--WIN3 are complete. Use the
`clang-cl` toolchain (available on the `windows-latest` runner without
additional installation). Add it to the release matrix with target name
`windows-x86_64`.

```yaml
- target: windows-x86_64
  os: windows-latest
```

The release packaging step on Windows should produce a `.zip` instead of
`.tar.gz` (or produce both).

---

## Out of scope

- **ARM64 Windows** -- no free GitHub Actions runner; Microsoft x64 is the
  only practical target for now.
- **MinGW / MSYS2** -- MinGW provides a POSIX shim layer and may work with
  fewer changes, but it produces binaries with a MinGW runtime dependency.
  Not targeted; contributions welcome.
- **Windows Subsystem for Linux (WSL)** -- WSL already works today (it runs
  the Linux build). No changes needed.
- **WASM** -- unaffected; already builds separately via Emscripten.

---

## Open questions

1. **Clang-cl vs. MSVC:** Clang-cl supports `__atomic_*` builtins and C11
   `_Thread_local`, reducing the porting surface significantly. Is pure MSVC
   support a goal, or is Clang-cl sufficient?
2. **Fiber stack guard pages:** The current implementation does not set up
   guard pages. Windows requires `VirtualAlloc` with `PAGE_GUARD` for stack
   overflow detection. Worth adding as part of WIN0?
3. **`libedit` on Windows:** `libedit` is not available on Windows. The REPL
   will fall back to plain `fgets` (already the path taken when `libedit` is
   absent). Acceptable for a first release.
