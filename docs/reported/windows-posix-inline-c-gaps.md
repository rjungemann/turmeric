# POSIX-only calls in stdlib inline-C break three fixture groups on Windows

**Severity: low (Windows-only, 5 fixtures).** Three unrelated POSIX APIs reached
for from inline-C with no Windows path. One is a near-trivial fix, one needs a
real port, one probably should not be ported at all.

Found 2026-07-31 sweeping `TUR=./build-win/tur.exe bash tests/run.sh` on main
at `f630230e5` (MSYS2/UCRT64, gcc 16.1.0).

## 1. `_mkdir` -- conflicting declaration, not a missing include

Fixtures: `tmpfile-linear`, `tmpfile-linear-borrow`,
`recursive-linear-borrow-branch`.

```
error: implicit declaration of function '_mkdir'
C:/msys64/ucrt64/include/direct.h:47:23: note: previous declaration of
    '_mkdir' with type 'int(const char *)'
   47 |   _CRTIMP int __cdecl _mkdir(const char *_Path);
```

Note the inline-C already uses the **Windows** name, so this was partially
ported at some point -- but it hand-rolls a prototype that conflicts with the
real one in `direct.h`. So the fix is to delete the local prototype, not to add
an include.

Better: route through the shim that already exists. `src/platform_fs.h:82` has
`#define mkdir(path, mode) ((void)(mode), _mkdir(path))` and pulls in
`<direct.h>`; the inline-C is bypassing it. Prefer a second consumer of the
shim over a second ad-hoc declaration.

## 2. `ioctl` + `struct winsize` -- needs a real port

Fixture: `term-string`.

```
error: storage size of 'ws' isn't known
error: implicit declaration of function 'ioctl'
```

Terminal-size query. There is no shim to reach for: the Windows equivalent is
`GetConsoleScreenBufferInfo` on the console handle, and the struct is different
too. Needs a genuine `#ifdef _WIN32` branch.

## 3. `fork` / `getppid` -- probably should not be ported

Fixture: `childhandle-linear`.

```
error: implicit declaration of function 'getppid'; did you mean 'getpid'?
error: implicit declaration of function 'fork'
```

There is no Windows `fork`. Emulating it means a `CreateProcess` re-exec with
explicit state hand-off, which is a real project and not obviously worth it for
one fixture. Recommend the same call as the pipe/reactor family (see
`windows-pipe-reactor-fixtures-do-not-build.md`): a `requires.*` skip marker.

Note `src/platform_fs.h:110` already takes exactly this posture for `symlink`,
stubbing it to `ENOSYS` with a comment explaining that failing cleanly beats a
shim that works only in some configurations. Same reasoning applies.

## Repro

```sh
just build-windows
./build-win/tur.exe build tests/fixtures/tmpfile-linear/input.tur
./build-win/tur.exe build tests/fixtures/term-string/input.tur
./build-win/tur.exe build tests/fixtures/childhandle-linear/input.tur
```

The errors surface in the generated C, so grep `stdlib/*.tur` for the inline-C
blocks to find the actual sites.
