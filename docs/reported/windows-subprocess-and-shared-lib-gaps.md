# Windows: the subprocess and shared-library layers are unported

**Severity: high for anyone actually using `tur` on Windows.** `tur.exe` now
builds and compiles-and-runs programs, but the commands that shell out or
produce/load a shared library fail. `tur install`, `tur fetch`, `tur new`,
`tur build --shared`, and REPL spice loading are all affected.

Found 2026-07-31 by source audit while fixing the Windows build on main at
`f630230e5`. **These are read-verified, not empirically exercised** -- the build
breakage was the blocker, and the runtime layer was not driven end-to-end
afterwards. Treat line numbers as confirmed and behavioural claims as
high-confidence inference.

## 1. Shell-string subprocesses assume `/bin/sh`

Every subprocess in the package/install path goes through `system()`/`popen()`
with a POSIX shell command string. On Windows the CRT hands these to `cmd.exe`,
which parses none of it the same way:

```
src/compiler/install.c:166    rm -rf -- '%s'
src/compiler/install.c:1177   git ls-remote '%s' '%s' 2>/dev/null      (popen)
src/compiler/pkg.c:1383/1385  tar -c '%s' 2>/dev/null | shasum -a 256
src/compiler/pkg.c:1654       git -C '%s' rev-parse HEAD 2>/dev/null
src/compiler/pkg.c:3439       git config user.name 2>/dev/null         (popen)
src/compiler/pkg.c:3709-3711  git init && git add && git commit
src/turi/spice_loader.c:309   %s build --shared %s -o %s --manifest %s >/dev/null 2>&1
```

Two independent failures in each: `>/dev/null` is a redirect to a nonexistent
path under `cmd.exe`, and the quoting is wrong. `shell_quote`
(`src/turi/spice_loader.c:271-286`) wraps arguments in **single quotes** with
`'\''` escaping; `cmd.exe` does not treat `'` as a quote character at all, so
the quoted program path is passed through as a literal token and nothing runs.
`install.c:161` additionally guards on `strchr(path, '/')`, which a `C:\...`
path fails.

Some also depend on tools absent from a stock Windows box (`tar`, `shasum`,
`sha256sum`).

None of these sites are new since the bring-up -- this is pre-existing debt the
bring-up did not reach, not a regression.

## 2. `tur build --shared` emits `lib<name>.so`

```
src/main.c:4791   snprintf(chosen_out, ..., "%s/lib/lib%s.so", build_dir, base);
src/main.c:5193   buf_puts(&cmd, " -fPIC -shared");     (only an __APPLE__ sibling)
src/main.c:5967   same, in the `tur link --shared` path
src/main.c:7938   help text still says "build a shared library (.so)"
```

Windows wants `<name>.dll` with no `lib` prefix, and `-fPIC` is a no-op there;
what is missing instead is import-library/export handling. This is already
called out as the WIN2 prerequisite in
`docs/upcoming/v1/windows-remaining-plan.md` and is still not done.

The loader half agrees with neither: `src/turi/spice_loader.c:644` builds
`"%s/lib-%u.so"` and dlopens that. The `platform_dl.h` shim makes `dlopen`
resolve to `LoadLibrary`, so the mechanism is present -- but the filename
contract is `.so` on both ends.

## 3. `tur repl` JIT module graph hits the `symlink` stub

`src/main.c:3747` calls `symlink()` when building the shadow-module tree.
`src/platform_fs.h:110` deliberately stubs that to `ENOSYS` (a documented
choice -- see its comment), so the graph construction aborts with
`tur repl: jit: symlink ...: Function not implemented`.

This one **is** new since the bring-up.

## 4. The JIT engine is structurally unported

Not urgent -- `TUR_JIT` defaults `OFF` (`CMakeLists.txt:104`) and is
additionally behind an experiment, so a default Windows build simply lacks the
feature rather than breaking. But the port is not a matter of shims:

- `src/jit_engine.c:354` resolves every external through
  `dlsym(RTLD_DEFAULT, name)`. Win32 has no process-wide global symbol
  namespace; `GetProcAddress` needs a specific `HMODULE`. `platform_dl.h:40-43`
  defines `RTLD_NOW`/`LAZY`/`LOCAL`/`GLOBAL` but **not** `RTLD_DEFAULT`, because
  there is nothing to define it as. Replacing it means an `EnumProcessModules`
  loop.
- The design relies on `ENABLE_EXPORTS` (`src/CMakeLists.txt:462`, `:674`), i.e.
  ELF `-rdynamic` semantics. The PE equivalent is dllexport/import libraries.
- `src/jit_engine.c:403` autolinks via `snprintf(soname, ..., "lib%s.so", name)`.
- Executable-memory allocation could not be checked: MIR is FetchContent'd
  (`cmake/mir.cmake:93`), not vendored, so whether c2mir uses `mmap(PROT_EXEC)`
  or `VirtualAlloc` and whether it has a Win64 target header is unknown from
  this tree.

## Suggested order

1 and 2 gate real use; 3 is a small fix on a new regression; 4 can wait for a
deliberate decision about whether the JIT targets Windows at all.
