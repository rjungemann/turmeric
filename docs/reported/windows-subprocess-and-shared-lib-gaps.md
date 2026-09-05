# Windows: the subprocess and shared-library layers are unported

> **PARTIALLY RESOLVED 2026-09-04, and this report's read-only findings needed
> correcting once they were actually run.** `tur new` and REPL spice loading
> now work end to end on Windows. A third failure mode -- cmd.exe eating the
> outer quote pair of a `/c` string -- was not in this report and is not
> findable by reading the call sites. See "Resolution" at the end for what is
> fixed, what is still POSIX-only, and what turned out to be misdiagnosed.

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


## Resolution (2026-09-04) -- partial

### What this report got wrong

It was explicit that its claims were "read-verified, not empirically
exercised". Exercising them:

**`tur new` did not fail.** It SUCCEEDED, with exit 0 and the project on disk --
and no git repository, which is the worse outcome because nothing signals it.
Its stderr carried three cmd.exe errors and this line:

```
tur new: git config user.name/user.email not set; using placeholder author
```

That is false on a machine where `git config user.name` is plainly set. The
popen that read it had failed, so the tool blamed the user's git configuration
for its own broken subprocess. A user following that message would go and set a
value that was already set.

### The third cause, which reading could not have found

Fixing the two causes this report names -- `/dev/null` and single quotes -- was
not enough. The spice build then failed with:

```
The filename, directory name, or volume label syntax is incorrect.
```

`system()` runs `cmd.exe /c <string>`, and cmd.exe strips the FIRST and LAST
quote characters of that string. With one quoted program path that is harmless;
with a quoted program AND quoted arguments it tears the command in half. The
remedy is an extra enclosing pair, so the pair cmd.exe eats is one we added.

This is invisible in a source audit: every individual argument is quoted
correctly and the command still does not run.

### What is fixed

`src/platform_proc.h` (new) carries the three portable pieces, all no-ops off
Windows:

| helper | what it settles |
| --- | --- |
| `TUR_DEVNULL` | `NUL` vs `/dev/null` |
| `tur_shell_quote` | double quotes on Windows, with the CRT's backslash-doubling rule so a trailing `C:\dir\` cannot escape its own closing quote |
| `tur_shell_command` | the outer wrap cmd.exe consumes |

Converted and **verified end to end on Windows**:

- `tur new` -- stderr now empty; `git log` shows `Initial scaffold from tur new`
- REPL spice loading -- `Loaded spice from ... (1 export)`, and calling the
  export from the prompt returns `=> 42`
- `tests/turi/repl-spice-load.sh` -- 9/9 pass

### What is still POSIX-only

Not converted, because they cannot be driven end to end here and a change that
cannot be run is a change that cannot be trusted:

```
install.c:1177   git ls-remote '%s' '%s' 2>/dev/null      (needs a remote)
pkg.c:1587/1589  tar -c '%s' 2>/dev/null | shasum -a 256  (needs a spice tree)
pkg.c:1858       git -C '%s' rev-parse HEAD 2>/dev/null
```

The `tar`/`shasum` pair has a second problem the helpers do not touch: neither
tool exists on a stock Windows box, so correct quoting only changes the error.
That wants a different remedy -- an in-process hash, or a documented dependency.

### Also still open, and unrelated to subprocesses

The REPL cannot resolve `stdlib/typeclass-show.tur` when loading its eval
prelude (`load: cannot open ...`). It is visible in every REPL session above and
is a stdlib path-resolution bug, not a subprocess one.
