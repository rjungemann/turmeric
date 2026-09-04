# turmeric-godot AOT: the staged build has no `godot-*` natives, so it cannot compile any real script

**Summary:** The AOT path stages a script into a transient project and compiles
it with standalone `tur`. But every `godot-*` name is a C++ native the
GDExtension registers into the *interpreter* env at run time, and standalone
`tur` has never heard of any of them -- so the staged build fails at the first
one with `unknown function or operator 'godot-export'`. Any script that touches
the Godot API, which is every useful script, cannot be AOT-compiled.

**Severity:** High for AOT. The interpreter path is unaffected and works.

**Platform:** Not platform-specific. Found on Windows only because that is where
the AOT path was first driven end to end; standalone `tur` has no `godot-*`
natives on any host, so this fails identically on Linux and macOS.

## Repro

```sh
cd ../turmeric-godot/examples/paddle-pong-tur
# force AOT for one script
sed -i '1i #mode aot' scripts/ball.tur
TUR_BIN=/path/to/tur Godot --headless --path . --quit
cat .godot/turmeric-cache/*/build.log
```

```
.../src/ball.tur:10:2: error: unknown function or operator 'godot-export'
10 | (godot-export "vel-x"   "float" 240.0)
   |  ^^^^^^^^^^^^
```

## What works, and why that matters

Everything around the compile is fine -- this is not a plumbing bug. The cache
tree is created and populated correctly:

```
.godot/turmeric-cache/<hash>/
  build.tur   src/ball.tur   build/{bin,lib,obj}   build.log
```

`tur build --shared` is invoked, and its non-zero exit is captured and surfaced
through the diag sink. So staging, quoting, subprocess spawn and exit handling
all behave. The failure is purely that the staged project is compiled in a world
where the Godot bridge does not exist.

## Root cause

Two different name-resolution worlds:

- **Interpreter path.** `TurmericLanguage` registers ~90 natives
  (`godot-export`, `godot-call`, `godot-vec2`, ...) with
  `turi_register_default_native_typed`, so every per-script `TuriEnv` has them
  and `turi_eval` resolves them. This is why the interpreter path works.
- **AOT path.** `aot_cache.cpp` writes the source to disk and shells out to
  `tur build --shared`. That is an ordinary compile of an ordinary project. The
  natives are C++ functions living inside the GDExtension `.dll`/`.so` -- there
  is no declaration for `tur` to resolve against, and nothing to link.

The prelude and generated facade do not help: they are *Turmeric* wrappers whose
bodies bottom out in the same `godot-*` natives, and they are evaluated into the
interpreter env, not staged.

## Fix directions

None of these is small; pick deliberately.

1. **Emit an `extern-c` declaration header into the staged project.** Generate a
   `.tur` file declaring every `godot-*` native's C signature, stage it
   alongside the source, and link the built `.so`/`.dll` against the
   GDExtension's exported symbols. Requires the extension to export them
   (it currently builds with `-fvisibility=hidden`) and requires the staged link
   line to reference the extension binary.
2. **Resolve at load instead of link.** Compile the staged library with the
   natives left undefined and bind them at `dlopen` time, the way the export
   table is already handled in `aot_image.cpp`. Trades a link-time dependency
   for a startup fixup pass.
3. **Drop AOT for the JIT.** The JIT compiles in-process, so the natives are
   already resolvable in the host's address space -- the whole problem
   disappears rather than being solved. See
   [jit-godot-embedding-spike.md](jit-godot-embedding-spike.md); this report is
   a concrete argument in that spike's favour, and worth weighing before
   investing in options 1 or 2.

   **Spike result (2026-08-05), and it strengthens this option.** A
   `-DTUR_JIT=ON` Release configure + build was attempted on Windows
   (MSYS2/UCRT64, gcc 16.1):

   - **MIR fetches and compiles cleanly under MinGW.** The feared answer from
     [jit-windows-support-spike.md](jit-windows-support-spike.md) question 1 --
     "MIR-gen only implements the SysV ABI, so Windows is a back-end port" --
     did **not** materialise at build time. Nothing in `_deps/mir-src` failed.
   - **The only thing that fails to compile is our own
     [src/jit_engine.c](../../src/jit_engine.c)** (711 lines), in three small
     and specific ways:
     - `#include <dlfcn.h>` -- MinGW has none. Fixed here by including the
       tree's existing `platform_dl.h`, the same remedy already used in
       `spice_loader.c` and the Godot shim's AOT image loader.
     - `#include <sys/resource.h>` plus `getrusage` / `RUSAGE_SELF`, inside the
       block the file itself labels "Temporary: measures the tier so I0's
       go/no-go gate has numbers. Remove wholesale if I0 comes back negative."
       Guarding or deleting that block costs no shipped functionality.
     - `RTLD_DEFAULT` is undeclared -- `platform_dl.h` defines `RTLD_NOW`,
       `RTLD_LAZY`, `RTLD_LOCAL` and `RTLD_GLOBAL` but not this one. It means
       "search the main program's symbols", whose Win32 spelling is
       `GetModuleHandle(NULL)`. This is a genuine gap in the shim, not a
       jit_engine quirk, and fixing it there benefits every caller.

   So the Windows JIT looks like **a bounded port of one file**, not a MIR
   project. Two of the three are already-solved problems applied to one more
   site; the third is a one-symbol addition to `platform_dl.h`.

   **Update, later the same session:** the build now completes and the engine
   *runs* -- it engages c2mir, and on failure falls back to the cc path
   cleanly. The new wall is that c2mir cannot digest the MinGW system headers
   (`vadefs.h` `#error`, `x86intrin.h`, `#pragma pack(push, MACRO)`), which
   the emitted C drags in via `#include <winsock2.h>` under `_WIN32`. The
   recommended route is the S2 split-runtime path with a windows-header-free
   TU -- see the findings block at the top of
   [jit-windows-support-spike.md](jit-windows-support-spike.md). Still true:
   no JIT-generated code has executed on Windows, so the MS x64 ABI and
   executable-memory questions remain open, and "MIR compiles" is still not
   "the JIT works".

## Note on `#mode`

Forcing AOT per-script requires the `#mode aot` directive, which until recently
made the reader fail with a parse error (the shim parsed the directive but never
stripped it before eval). Fixed in turmeric-godot as
"Strip the `#mode` directive before the source reaches a reader". Without that
fix this report is not reproducible, because AOT never engages at all.

## Related

- [godot-baked-in-prelude-fails-to-eval.md](godot-baked-in-prelude-fails-to-eval.md)
  -- the three bugs that had to be cleared before the AOT path could be reached.
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md) -- WIN2.
