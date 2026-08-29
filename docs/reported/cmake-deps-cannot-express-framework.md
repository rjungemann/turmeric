# `:cmake-deps` cannot express `-framework`, so no Cocoa-backed dep links on macOS

**Severity: high (blocker, macOS only)** -- not a miscompile; a hole in what the
dependency manifest can express, with no workaround short of bypassing `tur`'s
link line entirely. Found 2026-08-28 getting `turmeric-spices` CI green.

**Blocks `raygui` and `opengl` on macOS today**, and will block anything else
whose native dependency has an Objective-C backend.

## Summary

raylib's and glfw's macOS backends are Objective-C. They need
`-framework Cocoa -framework IOKit` (plus libobjc) on the link line. A framework
cannot be spelled as `-l`, and `tur` has no way to emit anything else.

The failure is `ld: symbol(s) not found for architecture arm64`, with
`_objc_retain` / `_objc_release` among the missing symbols -- which reads as a
toolchain or architecture problem rather than a missing link flag.

## The gap is structural, across three layers

Verified 2026-08-28 by reading `main` at `5c9d533`. There is **no occurrence of
the string `framework` anywhere under `src/`** -- this is not a case of one
emitter forgetting it, there is no representation for it at any layer.

**1. The manifest JSON schema has no key for it.** `pkg.c:3484-3494` parses
exactly five: `resolved_via`, `system_version`, `include_dirs`, `link_dirs`,
`link_libs`.

**2. `emit_link_lines` (`pkg.c:2514`) never emits one.** It writes `link_dirs`
and `link_libs` and nothing else.

**3. The consumer hardcodes the three prefixes.**
`pkg_cmake_manifest_append_cc_flags` (`pkg.c:3535`) is the single place the
manifest becomes cc flags:

```c
for (...) buf_printf(buf, " -I%s", e->include_dirs[j]);
for (...) buf_printf(buf, " -L%s", e->link_dirs[j]);
for (...) buf_printf(buf, " -l%s", e->link_libs[j]);
```

Every token is `-I`, `-L`, or `-l`. There is no path for a token of any other
shape.

**The manifest-level escape hatch does not escape either.** `:build-opts
:link-libs` is appended at `main.c:1905-1907` with the same hardcoded `-l%s`, so
writing `:link-libs ["framework Cocoa"]` produces `-lframework Cocoa`, not
`-framework Cocoa`.

## The information is already there, and already being read for the other half

The frameworks are present in the CMake target's `INTERFACE_LINK_LIBRARIES`.
`tur` does not read that property -- but it **already reads the sibling
property** for include dirs, via a generator expression, twenty lines away:

```c
"$<TARGET_PROPERTY:%s,INTERFACE_INCLUDE_DIRECTORIES>"     /* pkg.c:2496, :3125 */
```

So the mechanism exists, is proven, and is applied to exactly one of the two
properties that matter. `INTERFACE_LINK_LIBRARIES` is never referenced.

## Fix direction

Three options, in increasing order of how much they solve:

1. **A `:link-flags` passthrough** on the dep and/or the manifest -- verbatim
   tokens appended to the link line, no prefix added. Smallest possible change,
   unblocks macOS immediately, and is the honest escape hatch for anything the
   structured keys cannot describe. The cost is that it is unstructured and
   platform-conditional in user code.

2. **Read `INTERFACE_LINK_LIBRARIES`** the way include dirs already read
   `INTERFACE_INCLUDE_DIRECTORIES`, and translate its entries: a bare name to
   `-l`, an absolute path verbatim, and a `-framework X` / `"-framework X"`
   entry passed through. This is the one that needs no user action at all --
   raygui and opengl would just work, because CMake already knows.

3. **Link by target file path.** For a dep declared with `:targets`, emit
   `$<TARGET_FILE:tgt>` (the full path to the built artifact) instead of
   `-L<dir> -l<name>`. This sidesteps `OUTPUT_NAME`, namespace aliasing, and
   static-vs-shared preference in one move. It does not solve frameworks on its
   own -- transitive interface libraries still need (2) -- but it is the right
   shape for the `-l`-name problems filed in
   [cmake-deps-link-name-not-overridable](cmake-deps-link-name-not-overridable.md),
   and the two changes belong in the same pass.

(2) is the real fix and (1) is the pragmatic unblock; they compose, and doing
(1) first costs nothing that (2) later invalidates.

## Related

[cmake-deps-link-name-not-overridable](cmake-deps-link-name-not-overridable.md)
is the same function (`emit_link_lines`) and the same underlying decision --
`tur` reconstructs a link line from a target *name* by string manipulation
rather than asking CMake what the target actually is. That report covers the
`-l` name; this one covers tokens that are not `-l` at all. They should be
fixed together.

## Guides to update when fixed

- docs/guides/consuming-spices-guide.md -- the `:cmake-deps` / `:targets`
  section.
