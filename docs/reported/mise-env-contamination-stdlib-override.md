---
title: Mise global environment contamination overrides repository development settings
severity: MEDIUM. Causes widespread local build and test failures for active developers.
status: OPEN. Found 2026-06-29 during interpret subcommand implementation.
---

# Mise global environment contamination overrides repository development settings

## Summary

The repository root `mise.toml` specifies `turmeric = "v0.25.6"`. This ensures that the global `turmeric` tool is active inside the workspace. However, `mise`'s activation of global `turmeric` exports `TUR_STDLIB_DIR` into the shell pointing to the global installation path (e.g., `/Users/rjungemann/.local/share/mise/installs/turmeric/0.25.6/stdlib`).

Because the environment variable `TUR_STDLIB_DIR` is exported globally and inherited by subprocesses, any execution of the local `./build/tur` compiler or the test runner (`tests/run.sh`) loads the standard library from the global installation directory rather than the workspace's local `./stdlib` directory.

Since the global standard library (such as `stdlib/hamt.tur`) contains `__tur_autolink__` paths referring to source files like `src/runtime/hamt.c`, the locally built `tur` compiler tries to locate these files inside the global installation folder—where they do not exist. This triggers spurious compilation failures across almost the entire test suite.

## Repro

On a machine with `mise` managing `turmeric` globally:

1. Enter the repository directory:
   ```sh
   cd /Users/rjungemann/Projects/turmeric
   ```
2. Verify that `TUR_STDLIB_DIR` is set to the global `mise` installation path:
   ```sh
   env | grep TUR_STDLIB_DIR
   # TUR_STDLIB_DIR=/Users/rjungemann/.local/share/mise/installs/turmeric/0.25.6/stdlib
   ```
3. Attempt to compile a simple local test fixture using the local `./build/tur` executable:
   ```sh
   ./build/tur build tests/fixtures/adt-basic/input.tur -o /tmp/adt-basic-exe
   # clang: error: no such file or directory: '/Users/rjungemann/.local/share/mise/installs/turmeric/0.25.6/src/runtime/hamt.c'
   # tur: cc invocation failed (status 256)
   ```

## Why it happens

In `src/main.c`, `resolve_stdlib_root` respects the environment variable `TUR_STDLIB_DIR` over all other lookup strategies (such as walking up from the executing binary path or falling back to the legacy relative path `./stdlib`):

```c
static const char *resolve_stdlib_root(void) {
    ...
    const char *env = getenv("TUR_STDLIB_DIR");
    if (env && *env) {
        ...
        return g_stdlib_root; // Uses $TUR_STDLIB_DIR verbatim
    }
    ...
}
```

Since the environment variable is globally set by `mise`, it pollutes local development, causing the local `./build/tur` to look at the global standard library.

## Fix Directions

To fix this contamination once and for all:

1. **Test Runner Immunity (Recommended/Immediate)**:
   Explicitly `unset TUR_STDLIB_DIR` near the beginning of `tests/run.sh` to isolate all testing from the surrounding shell environment:
   ```bash
   # tests/run.sh
   unset TUR_STDLIB_DIR
   ```
2. **Mise Environment Exclusion**:
   Configure the repository root `mise.toml` to disable the global `turmeric` tool specifically within the workspace, or use a profile that prevents the export of `TUR_STDLIB_DIR`.
