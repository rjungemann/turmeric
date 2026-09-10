# Move the macOS and Linux release archives to the prefix layout

**Severity: low (consistency, not correctness).** Nothing is broken by the
current split -- `release-archive-cannot-compile` fixed the functional half by
teaching `locate_runtime_lib` to probe `<exe_dir>`, so a flat archive compiles.
This is about the three platforms disagreeing with each other, with `tur`'s own
resolution logic, and with what every consumer immediately rearranges them into.

Filed 2026-09-06, after v0.44.1 shipped Windows in one shape and the other three
in another.

## Where things stand

| target | archive | shape |
| --- | --- | --- |
| windows-x86_64 | `.zip` | `bin/` `lib/` `include/turi/` `share/turmeric/stdlib/` |
| linux-x86_64 | `.tar.gz` | `tur` `libturi.a` `include/turi/` `stdlib/` |
| linux-aarch64 | `.tar.gz` | flat, as above |
| macos-arm64 | `.tar.gz` | flat, as above |

Windows got the prefix layout because at the time it was the only shape that
worked: the flat one matched none of `locate_runtime_lib`'s probe paths. That
reason is gone -- `<exe_dir>` is probed now -- so the split is a leftover, not a
design.

## Why unify on the prefix layout

**1. It is the shape `tur` resolves natively.** `resolve_stdlib_root` step 3 is
`<exe_dir>/../share/turmeric/stdlib`, and `locate_runtime_lib` probes
`<exe_dir>/../lib`. Both exist because that is the layout an installed toolchain
has. The flat archive works via a probe added specifically to accommodate it.

**2. Every consumer already rearranges into something close to it.**

- `tvm` extracts the tarball into `versions/<v>/{bin/tur, lib/libturi.a,
  include/turi/, stdlib/}` -- the guide documents exactly that. It is doing the
  restage by hand because the archive does not ship it.
- The Homebrew formula (`Formula/turmeric.rb`) installs `bin/tur` and
  `share/turmeric/stdlib`. It builds from source rather than consuming the
  tarball, so it is not blocked by this -- but it is a second independent vote
  for the same shape.

**3. It removes a per-platform difference from the install instructions,** which
currently have to describe two layouts.

## What it would break

The archive is consumed in three places, and this is the whole cost of the
change:

- **`tvm`** (external repo). Its extraction and its `TUR_STDLIB_DIR` export both
  assume the flat shape. It would need to land its change before, or at the same
  time as, the first release in the new layout -- otherwise `tvm install` of that
  version produces a broken toolchain.
- **Trowel** (external repo). `CMakeLists.txt` fetches the tarball and copies the
  extracted directory next to the binary; `bundledTurPath()` in
  `src/repl/repl_session.cpp` probes `<appdir>/turmeric/tur` (macOS:
  `Contents/Resources/turmeric/tur`). Both need the extra `bin/`. Note Trowel
  already needs a change here for Windows -- it has no Windows entry and its path
  has no `.exe` -- so this is one edit rather than two.
- **`docs/guides/releases-and-installation-guide.md`**, whose extract-and-symlink
  instructions name `~/.local/turmeric/tur`.

Plus anyone's unpublished scripts, which is the usual unknowable.

## Suggested approach

1. Land the `tvm` and Trowel changes first, tolerant of BOTH shapes (probe
   `bin/tur` and fall back to `tur`). That is a small edit in each and it makes
   the order of everything after it irrelevant.
2. Then flip the three `build` matrix legs in `release.yml` to the same packaging
   the `build-windows` job already uses, and update the guide.
3. Keep the `<exe_dir>` probe in `locate_runtime_lib` afterwards. It costs one
   `stat` and it is what lets an OLD archive keep working with a NEW `tur`, which
   someone will do.

Step 1 is what makes this safe, and it is worth doing even if step 2 never
happens.

## What this is not

Not urgent, and not a correctness fix. If it is never done, the only cost is
that the four archives look different from each other. The
compile-from-archive check now on every release leg means either shape is
verified at publish time, so a regression in whichever layout is chosen fails
the release rather than reaching a user.

---

## Resolved 2026-09-09

Done in the order the plan above prescribed: consumers first, tolerant of both
shapes, then the packaging.

### Step 1 -- consumers (landed before the flip, in the same PR)

**`tvm`** turned out to need considerably more than the described
`TUR_STDLIB_DIR` edit, and to have a live defect of its own that had nothing to
do with the layout split.

The binary half was already tolerant -- `tvm install` accepted `tur` or
`bin/tur` and normalized to `bin/tur` -- so that part of this report was out of
date when it was filed. But normalizing *only the binary* is what broke it:
moving `tur` down into `bin/` while leaving `libturt_runtime.a` at the version
root matches none of `locate_runtime_lib`'s probes (`<exe_dir>/src`,
`<exe_dir>`, `<exe_dir>/../lib`). `TUR_RT_AUTO` fell back to source mode, which
wants `src/runtime/*.c` that no archive ships. Reproduced against a real
v0.46.0 build:

```
$ tvm install --from file://...-macos-arm64.tar.gz 0.46.0
$ tur run hello.tur
clang: error: no such file or directory: '.../0.46.0/src/runtime/hamt.c'
tur: cc invocation failed (status 256)
```

That is `release-archive-cannot-compile` reintroduced by tvm's own restage --
the extracted archive compiles, the tvm install of it does not. Worth recording
because it inverts this report's risk story: the prefix layout **fixes** the
tvm path rather than endangering it. A tvm that predates this change handles a
prefix tarball fine (verified: `tvm use` plus the binary compiles, `lib/` and
the stdlib walk-up both land), so users who never update tvm are not broken by
the flip either.

`tvm install` now normalizes the whole tree and accepts either input shape.
Three smaller defects fell out of the same read:

- `use` exported `TUR_STDLIB_DIR` only on a hit. It is an exported variable, so
  a miss left the *previous* version's value in place, and `tur` honors any
  directory with a readable `macros.tur`. Demonstrated: `tvm use 0.45.0; tvm
  use 0.46.0` left it on 0.45.0. It now unsets on a miss.
- `run` and `exec` exported it unconditionally, so against a prefix layout
  every invocation printed `ignoring TUR_STDLIB_DIR=... (no readable
  macros.tur there)`.
- `--build` never copied `libturt_runtime.a`, so a source-built version had the
  same cannot-compile defect from a different direction.

The stdlib probe accepts both `share/turmeric/stdlib` and `stdlib` because
versions installed by an older tvm are already on disk in the un-normalized
shape.

**Trowel** (rjungemann/trowel, PR: `repl: accept either Turmeric archive layout
for the bundled tur`) was further along than this report knew -- it already had
a `windows-x86_64` entry and a `bin/tur.exe` probe, behind a stale comment
saying it did not. The macOS and Linux branches still assumed flat, and a third
site the report did not mention needed the same fix: the stdlib pin looks for
`stdlib/` beside the resolved binary, which in a prefix tree is
`turmeric/bin/stdlib` -- absent, so the pin quietly dropped and the REPL
inherited exactly the ambient `TUR_STDLIB_DIR` the pin exists to override.

### Step 2 -- the packaging flip

The three `build` matrix legs in `release.yml` now use the same Package step as
`build-windows`, and the archive-verify step runs `./bin/tur`. The guide's
extract-and-symlink instructions gained a `bin/`.

### Step 3 -- the compatibility probe, kept

`locate_runtime_lib` is untouched: it still probes `<exe_dir>`, so an archive
published before this change still compiles with a `tur` built after it.
`find_stdlib_beside_exe` likewise checks both `stdlib/` and
`share/turmeric/stdlib/` at every level of its walk-up. Neither was weakened.

### What was actually verified, and what was not

Run on macOS/arm64 against a real Release build of v0.46.0:

- Both archive shapes built exactly as the two Package steps build them, then
  extracted and compiled a program: both work standalone.
- The **flat** tarball through the unpatched tvm: **fails**, as above. Through
  the patched tvm: both shapes install into an identical tree and both compile.
- `tvm/tests/run.sh` extended to build one release of each shape and assert
  they produce the same tree: 36 passed, up from 21. Two failures on that box
  (a `mise` `tur` shim on `PATH`; macOS `TMPDIR`'s trailing slash) are present
  on the baseline unchanged and are host artifacts.
- Trowel built and driven through its smoke suite with a real bundled `tur`:
  `tests/smoke/test_repl.py` is 10/10 against both a flat and a rearranged
  prefix staging, plus a probe evaluating `(when (= 1 1) 4242)` -- a
  stdlib-only macro, so it fails if the stdlib pin did not resolve. The
  negative control is what makes this meaningful: with the change reverted and
  a prefix staging in place, the REPL banner reads the `PATH` toolchain's
  v0.42.2 instead of the bundled v0.44.2, i.e. the bundle is silently ignored.
- The new Package and verify steps were run verbatim as shell against the local
  Release build, and `release.yml` parses.

**Not verified:** the workflow itself. A release cannot be published from here,
so the flip is verified as far as the shell logic and the resulting archive go,
and no further. The compile-from-archive check on every leg is what verifies it
at publish time -- as this report notes, that check means a regression in
whichever layout is chosen fails the release rather than reaching a user.
