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
