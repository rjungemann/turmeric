# Research spike: should turmeric-godot use the JIT instead of shelling out to `cc`?

**Summary:** The Godot GDExtension compiles every `.tur` script by staging a
transient project and running `tur build --shared` as a **subprocess**, which
means every machine that runs a Turmeric-scripted Godot game needs a working C
toolchain. `libturi` can now carry the MIR JIT and already exposes an embedding
API. Spike whether the shim should compile in-process instead.

**Severity:** Enhancement / research. The AOT path works; this is about whether
the product is shippable to non-developers.

**Type:** Timeboxed research spike. Platform-independent motivation, but see the
Windows note -- that is where it goes from "nice" to "load-bearing."

---

## The problem with the current design

`aot_cache.cpp` in `../turmeric-godot` does this per script:

1. Stage `<project>/.godot/turmeric-cache/<hash>/` with a `build.tur` and a copy
   of the source.
2. `std::system()` a `tur build --shared ...` command line.
3. `dlopen` the resulting shared library and `dlsym` each export from the
   manifest.

Step 2 is the problem. It requires, on the end user's machine:

- the `tur` binary,
- a C compiler that `tur` can invoke,
- and on Windows, a whole MSYS2/UCRT64 installation.

That is a reasonable ask of a *developer* and an unreasonable one of anyone who
just wants to run a game. It also puts a process spawn and a full C compile on
the script-load path, which is why the cache exists at all.

The JIT removes the external compiler: c2mir is vendored into `libturi`, which
the GDExtension **already statically links**.

## What already exists in our favour

- **`libturi` propagates the JIT to embedders.** Under `-DTUR_JIT=ON` the
  library gets `tur_mir` on its link line and `TUR_HAVE_JIT=1` in its
  *public* preprocessor interface
  ([src/CMakeLists.txt:535](../../src/CMakeLists.txt)) -- explicitly so "an
  embedding host gets MIR on its link line ... so a host can write one `#ifdef`
  and fall back to the tree-walking interpreter when the library was built
  without a JIT." The GDExtension is exactly that host.
- **There is a supported embedding API.** `tur_jit_compile_image` /
  `tur_jit_image_sym` / `tur_jit_image_free`, with `tests/turi/jit-embed.c` as
  a worked example that cross-checks `turi_eval` against a JIT'd image
  ([docs/guides/jit-guide.md](../guides/jit-guide.md), "Embedding").
- **The shapes line up.** `tur_jit_image_sym` is the natural replacement for
  `dlsym` in `aot_image.cpp`, which already models an image as "a handle plus a
  table of resolved symbol pointers" -- see `AotImage` in
  `../turmeric-godot/src/aot/aot_image.h`. The dispatch layer above it
  (`aot_dispatch`) should not need to care which produced the pointer.

## Open questions the spike must answer

1. **Does the GDExtension still link and load with a JIT-enabled `libturi`?**
   Cheapest possible first step: rebuild `libturi` with `-DTUR_JIT=ON`, relink
   the shim, load it in Godot. Answers "does MIR survive being inside a Godot
   plugin" before any code is written.
2. **W^X inside a host process.** A JIT allocating RWX pages inside an
   application the JIT does not control is a different proposition from doing it
   in `tur`. On macOS this is the MAP_JIT/hardened-runtime question (the plan
   notes the arm64 macOS MAP_JIT gate is closed for `tur` --
   [jit-engine-plan.md:70](../upcoming/jit-engine-plan.md) -- but Godot ships
   with its own entitlements and codesigning). On Windows it is the EDR /
   antivirus question. Neither is answered by `tur`'s own JIT working.
3. **`constructor` attribute.** c2mir discards it, so the embedding path must
   call `__tur_static_init` explicitly (the guide says so outright). Confirm the
   shim's init ordering has somewhere sensible to do that.
4. **What happens to the cache?** If compiles are in-process and fast, the
   on-disk cache may become unnecessary -- the REPL's JIT path already takes
   this position: "Every load compiles fresh, which is the point -- there is no
   cached artifact to go stale" ([jit-guide.md:310](../guides/jit-guide.md)).
   Deleting `aot_cache.cpp`'s staging/hashing/subprocess machinery would remove
   a great deal of surface area, including all of its platform-specific parts.
   Measure compile time for a representative script before assuming this.
5. **Is this a replacement or a second path?** A shipped game wants JIT (no
   toolchain); an editor session may still want AOT (debuggable artifacts, and
   the JIT is opt-in at build time so a `libturi` without it must still work).
   Two paths mean two things to keep correct -- decide deliberately, and note
   that `TUR_HAVE_JIT` was designed for exactly this fallback shape.
6. **Does the JIT reach the same fixture-level correctness?** `tests/run-jit.sh`
   is the whole-corpus JIT run; the archived findings list real JIT-only
   divergences (reactor fixtures aborting under MIR, GC/RC/weak fixtures on
   macOS). Scripts in Godot are not fixtures, but the failure classes carry.

## The Windows intersection

On Linux and macOS this spike buys convenience. On Windows it is close to
load-bearing: requiring MSYS2 to run a Godot game is not a shippable story, and
the AOT path there depends on the most fragile platform code in the shim
(`cmd.exe` quoting, `std::system` exit-code decoding, `.dll` cache naming) --
all of which the JIT path would delete rather than fix.

But it compounds risk rather than avoiding it: it needs a Windows JIT to exist
at all. Sequence [jit-windows-support-spike.md](jit-windows-support-spike.md)
first, or at minimum read its question 1 before planning anything Windows-shaped
here. That spike's questions 3 and 4 (emitted `__asm__`, executable memory) hit
this path unchanged.

## Method

- Do question 1 on Linux or macOS first, where the JIT is known to work. It is a
  rebuild and a relink, and it either loads or it does not.
- Only then attempt a single script end-to-end through
  `tur_jit_compile_image` + `tur_jit_image_sym`, bypassing `aot_cache` entirely
  -- a scratch branch in `../turmeric-godot`, not a refactor of the real path.
- Record findings; do not land a dual-path design out of the spike.

## Exit criteria

A verdict on whether in-process JIT compilation is (a) viable in a Godot host,
(b) fast enough to drop the on-disk cache, and (c) worth maintaining alongside
or instead of the AOT path. Plus a measured script compile time, since question
4 turns entirely on it.

## Related

- [jit-windows-support-spike.md](jit-windows-support-spike.md) -- sequence that
  one first for anything Windows-shaped.
- [docs/guides/jit-guide.md](../guides/jit-guide.md) -- "Embedding" section.
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md) -- WIN2.
