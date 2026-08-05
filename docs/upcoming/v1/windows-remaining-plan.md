# Windows Support -- Remaining Work

**Status:** WIN0 (compiler/runtime), WIN1 (generated-code portability), and the
hard core of WIN3 (async I/O + fiber context switches) are **done and on the
`windows-bringup` branch**. `tur.exe` builds under MSYS2/UCRT64, `tur build`
compiles and runs real programs, and the async/reactor/httpd/fiber subset passes
~65 fixtures. This plan tracks only what is left.

The original end-to-end plan (toolchain rationale, the WIN0/WIN1 blockers, the
Wine loop, the full WIN3 investigation) is archived at
[docs/archive/windows-support-plan.md](../../archive/windows-support-plan.md).

**Toolchain:** MinGW-w64 / UCRT64 via MSYS2 (`C:\msys64`), not Clang-cl. Build
with `just build-windows`; run the suite with
`TUR=./build-win/tur.exe bash tests/run.sh` (it derives the libturi `-L` from
`$TUR` and exports `TUR_BIND_LOOPBACK=1` so socket fixtures don't trip the
firewall).

---

## WIN2 -- `turmeric-godot` GDExtension Windows build (THE north star, not started)

**Goal:** build the shim from `../turmeric-godot/` as a Windows `.dll` and load
it in a stock Godot 4 binary. This is the actual point of Windows support -- the
compiler/runtime work exists to serve it.

### Prerequisite in this repo: shared-library output naming -- DONE

`tur build --shared` now emits `<name>.dll` (no `lib` prefix) on Windows.
`TUR_SHLIB_PREFIX` / `TUR_SHLIB_EXT` in `src/platform_fs.h` are the single
source of truth; `src/main.c` (default output path) and
`src/turi/spice_loader.c` (the `.tur-repl-cache/` image) both build their names
from them. macOS deliberately keeps `lib<name>.so` -- see the header comment.

`tests/run-build-shared.sh` passes 11/11 against `build-win/tur.exe`: the .dll
links, exports `smokelib__add42`, dlopens through `src/platform_dl.h`, calls,
and writes `exports.manifest`. Getting there needed three real fixes:

- **Multi-TU collision in the emitted ucontext shim.** The WIN3-C register-
  snapshot shim is emitted into *every* generated TU with `.globl`
  `__tur_uctx_swap` / `__tur_uctx_tramp` and an external `__tur_uctx_run`, so
  any build with more than one TU died with "multiple definition". A `--shared`
  build always has two (the generated `tur_runtime.c`), and so does every
  multi-module `tur build <dir>`. The definitions now sit in COMDAT
  (`.text$<name>` + `.linkonce discard`), the same mechanism a C++ inline
  function uses. Plain `.scl 3` is not enough for `__tur_uctx_swap` -- the C
  code calls it, so GCC re-externalises the symbol underneath you.
- **`basename_of()` split on `/` only.** Windows `realpath()` returns the
  backslash spelling, so `tur build --shared .` used the whole absolute path as
  the artifact name and ld rejected it. Now splits on `\` too, under `_WIN32`.
- **`dlfcn.h` in the test harness.** MinGW has none; the harness includes the
  repo's existing `src/platform_dl.h` instead.

Nine harnesses also hardcoded `TUR="./build/tur"` rather than honouring a `TUR`
override, so they could not be pointed at `build-win/tur.exe` at all. They now
read `${TUR:-./build/tur}` like the other 35.

Still open for WIN2: confirm the shim's "compile script on demand" subprocess
invokes `tur.exe` (with the suffix). `tur_settle_exe_output` already handles the
executable case.

### Scope (mostly in `../turmeric-godot/`)

- Add `windows` arch targets to the `SConstruct`.
- Statically link the WIN0/WIN1 `libturi` artifact (`build-win/src/libturi.a`),
  and its Windows link deps: `ws2_32`, `shlwapi`, `pthread` (winpthreads).
- Update `turmeric-godot.gdextension` with `windows.debug.x86_64` /
  `windows.release.x86_64` entries.
- Cache directory: prefer the in-project `<project>/.godot/turmeric-cache/`
  (cross-platform, Godot already creates `.godot/`).

### Testing

- Build `examples/spike` on a `windows-latest` runner (Godot ships a headless
  Windows binary). Confirm `[turmeric-godot] initialize(level=...)` in the log.
- Run the paddle-pong demo headless in CI as a smoke test once it lands.

### Open question -- shim toolchain on the user's box

When `tur build --shared` runs inside the GDExtension, which C compiler does it
invoke? Godot on Windows is MSVC-built, but GDExtension is a C ABI and godot-cpp
supports `use_mingw=yes`, so a MinGW-built shim + MinGW `tur` is expected to
work. Bundling a known-good toolchain vs. requiring MSYS2 on the user's machine
is the main UX decision. (This is why the port stayed on MinGW rather than
Clang-cl -- see the archived plan.)

---

## WIN3 tail -- the ~11 async fixtures still failing

None of these block WIN2; they are the long tail of the async subset.

### Pipe-fd polling: a hard `select()` limit (~8 fixtures)

`reactor-fd-*`, `reactor-fibers-cancel-on-free`, `reactor-fibers-park-fd`,
`reactor-stop-from-callback`, `reactor-wake-cross-thread`, `scheduler-io-park`
create a `pipe()` and register the pipe fds with the reactor. **Windows
`select()` is socket-only** -- it cannot poll pipe or file fds at all, so the
select-based backend (`src/async/io_iocp.c`) cannot service them.

Options, in rough order of effort:
1. Accept as a platform limit (Godot scripts don't poll pipes). Mark the fixtures
   `requires.*`-skip on Windows.
2. Route `pipe()` through a loopback socket pair AND `read`/`write` through
   `recv`/`send` for those fds -- intricate, collides with file I/O, fragile.
   Only worth it if a real turmeric-godot use case needs pipe-style async.

Recommendation: option 1 unless a use case forces option 2.

### Concurrency stdout mismatches (~3 fixtures)

`httpd-h4-keepalive`, `httpd-h6-routing`, `taskgroup-async` build and run but
produce diverging output -- scheduler/timing under the select backend, not yet
diagnosed. `scheduler-multithread` sometimes prints the same worker-thread id
twice (distribution nuance, but it passes). Worth a focused diagnosis pass;
start by comparing fiber/worker scheduling order against Linux.

---

## Codegen defects the port surfaced (NOT Windows-specific)

Both are latent on Linux too and will bite when CI's toolchain advances.

- **GCC >= 14 permerrors.** Generated C trips `-Wincompatible-pointer-types`,
  `-Wint-conversion`, and `-Wimplicit-function-declaration` (a missing `hamt.h`
  include), all promoted to hard errors in GCC 14. Worked around with
  `-Wno-error=` in `src/main.c`; the real fix is well-typed codegen. See
  [docs/reported/codegen-gcc14-permerrors.md](../../reported/codegen-gcc14-permerrors.md).
- **`-O0` link failure.** Generated C references `tur_get_contract_handler` /
  `tur_set_contract_handler`, which are declared but never defined; at `-O1`+ the
  calls are optimised away so it links. Surfaces only in a debug (`-O0`) build.

---

## WIN4 -- standalone CLI polish (deferred)

Not required for turmeric-godot; do only if a standalone Windows CLI workflow is
wanted.

- **Coloured diagnostics:** `src/compiler/diag.c` -> `_isatty(_fileno(stderr))`
  plus `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` on Win10+.
- **REPL:** MSYS2 ships wineditline, so line editing already works; nothing to do
  unless targeting a non-MSYS2 build.
- **Release packaging:** add `windows-x86_64` to `.github/workflows/release.yml`,
  producing a `.zip`.

---

## Out of scope

ARM64 Windows (no free CI runner), WSL (runs the Linux build already), WASM
(separate Emscripten build), in-engine REPL on Windows.
