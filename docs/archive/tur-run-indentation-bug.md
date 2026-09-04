# `tur run` Indentation Bug Report

## Summary

The `tur run` command silently fails to execute recipes when the Justfile uses 2-space indentation for recipe bodies. This differs from the upstream `just` tool, which accepts 2 or more spaces as valid indentation.

## Root Cause

The bug is in `src/compiler/justrun.c` in the `is_body_line()` function (originally lines 507-512). The function only recognized recipe body lines starting with:
- A tab (`\t`), OR
- Exactly 4 spaces

This caused recipe body lines with 2-space indentation to be ignored, resulting in recipes with empty bodies that silently do nothing.

```c
/* Original buggy implementation */
static int is_body_line(const char *line) {
    if (line[0] == '\t') return 1;
    if (line[0] == ' ' && line[1] == ' ' && line[2] == ' ' && line[3] == ' ')
        return 1;
    return 0;
}
```

## Verification

1. `just run` works correctly — prints the command and executes it
2. `tur run run` prints nothing and exits with code 0
3. `tur run --dry-run run` also prints nothing (should print the command)
4. The terminal-est project's Justfile uses 2-space indentation throughout:
   ```justfile
   clean:
     rm -r build

   build:
     cmake -S . -B build
     cmake --build build

   run:
     build/terminal-est
   ```

## Fix

Updated `is_body_line()` to accept lines starting with either:
- A tab, OR
- Two or more spaces

```c
/* Fixed implementation */
static int is_body_line(const char *line) {
    if (line[0] == '\t') return 1;
    if (line[0] == ' ' && line[1] == ' ') return 1;
    return 0;
}
```

This matches the behavior of the upstream `just` tool, which accepts any recipe body line with 2 or more leading spaces or a tab.

## Impact

- Affects any project using Justfiles with 2 or 3 space indentation
- Symptoms: `tur run <recipe>` silently does nothing, exits with code 0
- Workaround: Use 4-space indentation or tabs in Justfile
