# Fix: `:cmake-deps` link lines are asked of CMake, not derived from target names

Resolves, in one pass as both asked:

- [cmake-deps-cannot-express-framework](../cmake-deps-cannot-express-framework.md) (high, macOS blocker)
- [cmake-deps-link-name-not-overridable](../cmake-deps-link-name-not-overridable.md) (medium)

Landed 2026-08-28 against `main` at `423f6546`.

## The one sentence

`tur` reconstructed a link line from a CMake target's *name* by string
manipulation; it now asks CMake what the target actually is.

## What changed

**A `link_flags` array in `cmake/spice-deps-manifest.json`** -- verbatim
link-line tokens, no prefix added. This is the representation the framework
report found missing at all three layers (schema, emitter, consumer).

**`emit_link_lines` (`src/compiler/pkg.c`) sources it from two generator
expressions**, per `:targets` entry:

- `$<TARGET_FILE:tgt>` -- the exact artifact, replacing the name-derived
  `-L<dir> -l<basename>`. Wrapped in a `TYPE != INTERFACE_LIBRARY` guard,
  because `$<TARGET_FILE:...>` is an error on a header-only target. CMake does
  not evaluate the false arm of a `$<cond:...>`, so the guard yields an empty
  element rather than a configure failure -- verified directly, not assumed.
- `$<TARGET_PROPERTY:tgt,INTERFACE_LINK_LIBRARIES>` -- the transitive
  requirements, joined with the same `$<JOIN:...>` idiom
  `emit_include_dirs_line` already used for the sibling
  `INTERFACE_INCLUDE_DIRECTORIES` property.

**`pkg_cmake_manifest_append_cc_flags` classifies each token** instead of
hardcoding `-I`/`-L`/`-l`: a flag (`-framework Cocoa`) or absolute path passes
through verbatim, a bare name becomes `-l`, and two shapes are skipped -- see
"What is still dropped" below.

**Two manifest overrides**, `:link-libs` and `:link-flags`, on a `:cmake-deps`
entry; plus `:build-opts :link-flags` for a project's own link. `:link-libs`
records key presence separately from count, because `:link-libs []` is a value
("link nothing") distinct from an absent key, and is the only way to express a
header-only dep (raygui) or a generator that builds no library (glad).

## Evidence

One fixture demonstrates both reports, because glfw hits both defects. A spice
declaring `:cmake-deps {"glfw" ... :targets ["glfw"]}` with an inline-C call to
`glfwGetVersionString`, built on arm64-apple-darwin27 with Apple clang:

| | manifest | result |
| --- | --- | --- |
| before (`423f6546`) | `"link_libs": ["glfw"]` | `ld: library 'glfw' not found` |
| after | `"link_libs": []`, `link_flags` = `libglfw3.a`, `-framework Cocoa`, `-framework IOKit`, `-framework CoreFoundation` | links, runs, prints `3.4.0 Cocoa NSGL Null EGL OSMesa monotonic` |

Note which defect bites first: the archive is `libglfw3.a`, not `libglfw.a`, so
the `OUTPUT_NAME` half fails at `-lglfw` before the link ever gets far enough to
miss the frameworks. That is why the two reports had to be fixed together --
fixing either alone leaves the fixture red, and fixing the name alone would have
made the framework gap look like a *new* failure rather than the next one.

The frameworks were confirmed load-bearing independently, by linking the same
program against the resolved `libglfw3.a` with and without them: without, the
exact failure the report describes (`Undefined symbols for architecture arm64`,
`_OBJC_CLASS_$_NSApplication` and friends); with, it links.

Override behavior, each verified from a clean project:

- `:link-libs ["glfw3"]` -- `link_libs` becomes `["glfw3"]`, the
  `$<TARGET_FILE:...>` entry is suppressed (the user has taken over artifact
  selection), transitive `link_flags` are still emitted, and it links.
- `:link-libs []` -- no `-l` and no artifact path, only the transitive flags.
  The link then fails on `_glfwGetVersionString`, which is the *correct*
  meaning of "link nothing"; a genuinely header-only dep has nothing to
  resolve. This is the check that `[]` is distinguished from absent.
- `:link-flags ["-framework Cocoa" "-framework IOKit"]` -- appended verbatim
  after the derived tokens.

## What is still dropped, and why that is not a regression

Two `INTERFACE_LINK_LIBRARIES` shapes are skipped by the consumer:

- **`$<LINK_ONLY:Threads::Threads>`** -- an *unevaluated* nested genex.
  `file(GENERATE)` expands `$<TARGET_PROPERTY:...>` one level and does not
  re-evaluate what comes back, so nested genexes arrive in the JSON as literal
  text. (This was measured, not inferred; it is also why the emitted JSON is
  safe to read as data.)
- **`Foo::Bar`** -- a namespaced target name. Only CMake can say what artifact
  that is, and by consumption time CMake is out of the loop.

Neither is a loss relative to the old behavior: before `link_flags` existed,
`INTERFACE_LINK_LIBRARIES` was not read at all, so *every* entry was dropped.
Resolving them properly would mean a second CMake round-trip; `:link-flags` is
the escape hatch until something needs it.

## Suites

`run-transitive-cmake-deps` 5/0, `test-emit-cmake` 15/0, `spice-resolver-tests`
80/0, `run-build-project` 38/0, `run-manifest-entry` 11/0, `run-sweet-manifest`
5/0, `repl-spice-linklibs` 1/0, `run-tur-new` green, plus `tests/run.sh`.

## Follow-on

The five spices the link-name report names (`raygui`, `glfw`, `glad`, `libpq`,
`zlib`) each carry a hand-written `cmake-deps/` shim that exists only to work
around this. Those shims are now removable in
[`turmeric-spices`](https://github.com/rjungemann/turmeric-spices), and the
`raygui`/`opengl` macOS CI jobs the framework report blocked should be retried.
That is spice-repo work and was not done here.
