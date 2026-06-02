---
title: Spice-Level Auxiliary C Sources Plan
category: Planning
description: Extend the spice manifest with :c-sources / :c-includes so a spice can ship hand-written .c files (e.g. KissFFT, miniaudio, stb_image) alongside its .tur modules, compiled and linked automatically by the spice build.
---

# Spice-Level Auxiliary C Sources -- Plan

## Goal

Let a spice ship hand-written `.c` files alongside its `.tur` modules so
that things like KissFFT, miniaudio, stb_image, or a hand-tuned kernel
can be **vendored into the spice** and compiled + linked automatically
by `tur build` / `tur run`, without users having to set up their own
build glue.

Concretely: extend `build.tur`'s `:build-opts` block with two new keys,
`:c-sources` and `:c-includes`, and teach the spice build to compile
the listed sources and add the listed include dirs to the codegen
`-I` set.

Today, every C extension a spice needs has to be inline-C inside a
`.tur` file. That works for ~50 lines of glue but does not scale to
"drop the KissFFT source tree into the spice". This plan unblocks that
case without forcing users into CMake.

## Motivation -- the immediate consumer

[[signal-primitives-expansion-plan]] proposes vendoring KissFFT as the
"Phase A" FFT implementation for `tur-signal`. KissFFT is roughly four
`.c` files + four headers. There is currently no way to drop those into
`spices/signal/c/kissfft/` and have them participate in the spice build.

That plan therefore defers KissFFT to "follow-up only if a consumer
needs it" and ships an in-tree radix-2 FFT in inline-C as its day-one
implementation. This plan removes the dependency the other way around:
once `:c-sources` exists, KissFFT (and any other reasonable C library
the user wants to vendor) is a 5-line manifest change.

Other consumers that are not yet built but will benefit:

- **Image spices**: `stb_image` is the obvious vendor target for a
  `tur-image` spice. Single-file header, but you still need to compile
  one `.c` shim that does `#define STB_IMAGE_IMPLEMENTATION`.
- **Audio I/O**: `miniaudio` is a single ~1MB header with platform
  conditionals -- compiled exactly once via a shim `.c`.
- **Compression**: `miniz` is one .c + one .h.
- **Crypto**: `monocypher`, `BearSSL` cores -- two to ten `.c` files.

All of these are currently impossible to ship cleanly as a spice.

## Background -- what already exists

The spice manifest *already* has C build plumbing. The shape is:

```turmeric
(defpackage tur-mylib
  :name "tur-mylib"
  ...
  :build-opts {
    :c-flags  ["-O2" "-DFOO"]
    :link-libs ["m" "pthread"]
  }
  :exports { ... })
```

Parsed in `src/compiler/pkg.c:485` via `:build-opts -> :c-flags` and
`:link-libs`, and serialized back at `src/compiler/pkg.c:589`. The
`Manifest` struct already carries `c_flags`/`n_c_flags` and
`link_libs`/`n_link_libs` fields with parse + emit + free wired up.

These flags flow through the build pipeline (`src/main.c` consumes
them when invoking the C compiler step). So the question is purely:
"what does it take to add a third slot for source files?"

What does **not** exist:

- A `:c-sources` manifest key.
- Any code path that takes a list of `.c` paths from a manifest and
  hands them to the C compiler as additional compilation units.
- A spice-level `-I` include-path injection mechanism. Vendored
  libraries typically need `-I` for their own headers; today the only
  way is to bake `-I.../include` into `:c-flags` literally, which leaks
  absolute paths into the manifest.
- Cross-spice propagation of `.c` sources -- if spice B vendors KissFFT
  and spice A `:spices`-depends on B, A needs B's compiled object code
  in the final link. The existing `link_libs` propagation logic
  (`src/compiler/pkg.c:1748`, `:1939`) is the model for how this should
  work.

## Design

### Manifest surface

Add two new keys under `:build-opts`:

```turmeric
:build-opts {
  :c-flags    ["-O2"]
  :c-includes ["c/kissfft" "c/local-include"]
  :c-sources  ["c/kissfft/kiss_fft.c"
               "c/kissfft/kiss_fftr.c"
               "c/glue/fft_shim.c"]
  :link-libs  ["m"]
}
```

**Path resolution**: both `:c-sources` and `:c-includes` are
**relative to the manifest's directory** (i.e., the spice root, the
same directory as `build.tur`). Absolute paths are rejected -- a spice
that needs an absolute include path should declare a `:link-libs`
dependency on a system library instead.

**Validation**: at parse time, verify that every `:c-sources` entry
exists on disk (relative to the manifest) and has a `.c` or `.cc` /
`.cpp` extension. Listed but missing -> `diag_emit(DIAG_ERROR, ...)`.
This catches typos at manifest load rather than at compile.

### Storage in `Manifest`

Mirror the existing `c_flags` / `link_libs` pattern:

```c
struct Manifest {
    ...
    char **c_flags;     int n_c_flags;
    char **link_libs;   int n_link_libs;
    char **c_sources;   int n_c_sources;   /* new */
    char **c_includes;  int n_c_includes;  /* new */
    bool no_stdlib;
    ...
};
```

Touch the same five sites in `pkg.c`:

| Site | What changes |
|---|---|
| `parse_build_opts` (`pkg.c:485`) | Add `parse_str_vec` calls for the two new keys; validate paths exist + extension is `.c`/`.cc`/`.cpp` |
| Manifest emitter (`pkg.c:589`) | Emit the two new arrays inside `:build-opts` |
| Manifest free (`pkg.c:683`) | Free the two new arrays |
| Cross-spice dep propagation (`pkg.c:1748`, `:1939`) | When a downstream spice consumes this one, include its `c_sources` (resolved to absolute paths) in the downstream build *if* the dep is built from source -- see "linking model" below |
| Per-file equality / change-detection (wherever Manifest hashing lives) | Include the two new arrays in any "manifest changed" hash |

### Linking model -- how `.c` files reach the final binary

Two reasonable options:

**Option 1: per-spice static archive.**
Each spice with `:c-sources` produces a `lib<spice>.a` in its build
output dir. Downstream spices link against that archive via the
existing `link_libs` machinery. This is the conventional C way and
plays nicely with incremental builds (the archive is rebuilt only when
a source or include changes).

**Option 2: source-level aggregation.**
The final binary's C compile step receives every transitive `.c` file
as an additional translation unit. Simpler -- skipping both the archive
step and link ordering -- but rebuilds get expensive once a vendored library is more
than a handful of files (any change in the consuming spice recompiles
the world).

**Recommendation: Option 1.** Static-archive aggregation is what
existing `link_libs` already does; this slots in alongside without
inventing a new build phase. It also means a future "prebuilt spice
binary" feature (analogous to `:prefer-system`) gets a natural artifact
to ship.

### Where in `tur build` the compile happens

The spice build today walks `src/<package>/` for `.tur` files,
codegens them to a single C translation unit, and invokes the system
C compiler with `c_flags` + `link_libs`. With `:c-sources`:

1. Resolve each `:c-sources` path to an absolute path under the spice
   root.
2. Compile each `.c` separately with: the spice's `c_flags`, the
   spice's `c_includes` (passed as `-I`), and the global codegen
   header path (so vendored sources that want to call into Turmeric
   runtime headers can).
3. `ar rcs lib<spice>-aux.a *.o` into the spice's build output dir.
4. Add `lib<spice>-aux.a` to the final binary's link line, *before*
   `link_libs` (so vendored `.c` can resolve symbols against
   `link_libs` like `m`).
5. Propagate `lib<spice>-aux.a` to downstream consumers via the same
   path that propagates `link_libs` today.

The codegen for `.tur` files is unchanged. Inline-C blocks in `.tur`
files still flow through the existing translation-unit path; they do
not move into the auxiliary archive.

### Include-path injection

`:c-includes` becomes:

- Added as `-I<spice-root>/<entry>` to the spice's own `.c` compile
  step (so vendored sources see their own headers).
- Added as `-I<spice-root>/<entry>` to the **inline-C compile** of the
  same spice (so an inline-C block in `.tur` can `#include
  "kissfft/kiss_fftr.h"`).
- **NOT propagated to downstream consumers.** Vendored headers are an
  implementation detail of the spice. A downstream consumer should see
  the spice through its `.tur` exports, not through C-level includes.
  If a spice intends to expose C headers to consumers, that is a
  separate (and bigger) "C-level export" feature -- explicitly out of
  scope here.

### `:no-stdlib` interaction

`:c-sources` honours `:no-stdlib` -- if the spice opts out of the
Turmeric C runtime, vendored sources do too. In practice this means
the auxiliary compile uses the same compiler invocation flags as the
main spice compile, minus the prelude link.

### Workspace / `:path` deps

For `:path`-based local deps the auxiliary archive is built in the
dep's own build output dir and reused. For `:url`-fetched deps, same
thing -- the fetched cache directory holds both the source and its
build artifacts. No new caching semantics; just the existing
spice-build cache extended with the aux-archive output.

## What this plan deliberately does *not* do

- **No CMake integration.** Spices stay buildable with just `tur` on
  the path. (If a vendor library needs CMake or autotools, that
  library is not a candidate for vendoring as a spice; suggest a
  `:link-libs` system dep instead.)
- **No transitive header export.** A spice's `:c-includes` is private.
  Cross-spice C-level coupling is explicitly out of scope.
- **No `.cpp` / `.cc` first-class support.** The parser accepts the
  extensions (because some vendor libraries are C++ even when they
  pretend to be C), but the compile flags do not switch to a C++ mode
  automatically. If a spice needs C++, it lists `-x c++` in
  `:c-flags`. Revisit if a real consumer wants ergonomic C++.
- **No platform conditionals in the manifest.** All listed sources are
  compiled on every platform. Vendor libraries with platform-gated
  files (miniaudio, BearSSL) handle this via `#ifdef` inside the
  source, the same way they do in any C project.
- **No assembly sources (`.S`/`.s`).** Not needed by any plausible
  near-term consumer; revisit when one shows up.
- **No precompiled object distribution.** Spices ship source; building
  is the consumer's job. A "prebuilt spice archive" feature would be a
  separate plan layered on top.

## Deliverables

1. **Manifest parser**: `:c-sources` and `:c-includes` keys parsed,
   validated, stored in `Manifest`, freed cleanly.
2. **Manifest emitter**: round-trips the two new keys.
3. **Spice build**: compiles `:c-sources` into a per-spice auxiliary
   static archive; links it into the consuming binary in correct
   order; propagates across `:spices` deps.
4. **Include-path injection**: `:c-includes` `-I` flags reach both the
   spice's own `.c` compile step and any inline-C blocks in the
   spice's `.tur` files.
5. **Tests**:
   - A fixture spice with a single hand-written `.c` exporting one
     function, consumed via inline-C in a `.tur` module, that compiles
     and runs.
   - A fixture spice with `:c-includes` pointing at a private header
     dir, where the header is used by both an aux `.c` and an inline-C
     block.
   - A cross-spice fixture: spice B vendors a `.c`, spice A depends on
     B via `:path`; building A pulls B's archive into A's link line.
   - Error cases: missing source file, non-`.c` extension, absolute
     path, manifest cycle through a `.c` consumer.
6. **Docs**: extend `docs/guides/developing-spices-guide.md` with a
   "Vendoring C sources" section: when to reach for it (heuristic:
   library is small, single-purpose, and has no native package
   manager presence), how to lay out `c/<libname>/`, and the manifest
   keys.
7. **Style note** in the same guide: vendored sources go under `c/`,
   not under `src/` (which is reserved for `.tur` -- the manifest-driven
   build walks `src/` looking for `.tur` files). Don't mix.

## Validation

```sh
# new manifest fixture
tur build tests/fixtures/spices/spice-with-aux-c/build.tur
# binary runs, vendored function returns the right value:
./build/spice-with-aux-c-runner   # exits 0

# cross-spice
tur build tests/fixtures/spices/consumer-of-aux-spice/build.tur

# error cases (each must DIAG_ERROR, not segfault):
tur build tests/fixtures/spices/aux-c-missing-source/build.tur
tur build tests/fixtures/spices/aux-c-bad-extension/build.tur
tur build tests/fixtures/spices/aux-c-absolute-path/build.tur
```

Plus: existing `bash tests/run.sh` stays green (this is additive --
spices without `:c-sources` get exactly the same build pipeline as
today).

## Phasing

1. **Manifest parse + emit + free + round-trip test.** No build-side
   changes; just shape the data through. Cheapest possible first PR.
2. **Per-spice auxiliary archive**, single-spice (no propagation).
   First fixture (`spice-with-aux-c`) passes.
3. **`:c-includes` injection** into spice's own `.c` compile and into
   inline-C compilation of the same spice's `.tur` files. Second
   fixture passes.
4. **Cross-spice propagation** through `:spices` deps. Third fixture
   passes. Reuse the propagation path that `link_libs` already uses
   (`pkg.c:1748` / `:1939`).
5. **Error-case fixtures + diagnostics.** Validate the parse-time
   checks (missing source, bad extension, absolute path) all produce
   clean errors with the right span.
6. **Docs**: developing-spices guide section + a worked example.
7. **(Optional follow-up)** `tur-signal` switches its FFT to KissFFT
   per [[signal-primitives-expansion-plan]] Phase 14 -- the first real
   consumer, validates the feature end-to-end.

Each phase is independently mergeable.

## Open questions

- **`.cpp` ergonomics.** Should the parser auto-set `-x c++` for
  `.cpp`/`.cc` entries, or stay agnostic? Default: stay agnostic; the
  spice author lists `-x c++` in `:c-flags`. Revisit if any real
  consumer wants it.
- **Per-source flags.** Some vendor libraries want `-Wno-foo` on one
  specific file. Do we need per-entry `:c-flags`? Default: no -- if
  one warning is annoying, silence it spice-wide and document why.
  Add per-source flags only when a vendor library forces it.
- **Caching granularity.** Today the spice build hashes the manifest
  + sources to decide rebuild. Aux `.c` files trivially extend that.
  Do `:c-includes` *header file contents* contribute to the hash?
  Default: yes -- a header change must trigger an aux recompile.
  Implementation: walk `:c-includes` dirs at hash time, hash every
  `.h`/`.hpp`/`.hh` found. Cheap and correct.
- **Naming**: `:c-sources` vs `:native-sources` vs `:c-files`. The
  existing key is `:c-flags`, so `:c-sources` is the obvious
  consistent name. Lock it.
- **Single-archive vs multi-archive.** A spice with 200 vendored `.c`
  files (e.g., the full BearSSL source) generates 200 `.o` files into
  one archive. Is that fine? Default: yes -- `ar` handles thousands of
  members in real-world archives without issue. Revisit only if a
  concrete spice blows past a measured threshold.

## Acceptance checklist

- [ ] `:c-sources` and `:c-includes` parse, store, emit, round-trip
      through `build.tur` -> `Manifest` -> `build.tur`.
- [ ] Parse-time validation: missing file, non-`.c`/`.cc`/`.cpp`
      extension, and absolute path each produce a `DIAG_ERROR` with
      the right span.
- [ ] A single-spice fixture compiles a vendored `.c`, links it into
      the binary, and the binary runs.
- [ ] `:c-includes` `-I` reaches both the aux `.c` compile and the
      inline-C compile of the same spice's `.tur` files.
- [ ] A cross-spice fixture (consumer `:spices`-deps on a vendor spice
      with `:c-sources`) builds and links cleanly.
- [ ] Header changes under a `:c-includes` dir trigger a rebuild on
      the next `tur build`.
- [ ] `docs/guides/developing-spices-guide.md` has a "Vendoring C
      sources" section with a worked example.
- [ ] (Optional, not required for this plan to land) `tur-signal`
      switches its FFT primitives to KissFFT and the existing FFT
      tests still pass.
