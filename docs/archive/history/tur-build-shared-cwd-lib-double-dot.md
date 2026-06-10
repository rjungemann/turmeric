# `tur build --shared .` produces `lib..so` / `lib..so.manifest`

## Summary

Running `tur build --shared` with a cwd-relative target (`.`) produces a
shared library named `lib..so` (two dots) and a sidecar
`lib..so.manifest`. The empty-basename case (`./`) instead produces a
nameless `lib.so`. **Severity: ergonomics / output-name bug.** No
miscompile -- the binary itself is correct -- but the artifact name is
wrong and the file pollutes the spice tree in `../turmeric-spices/`
under every spice that's been built from inside its own dir.

## Repro

From any spice directory, e.g. `../turmeric-spices/spices/ansi/`:

```sh
tur build --shared .
ls lib*.so*
# lib..so  lib..so.manifest        <-- observed
```

Expected: `libansi.so` and `libansi.so.manifest` (basename derived
from the spice's manifest `:name`, or from the absolute cwd basename
when no manifest is in play).

## Root cause

`default_output_name` at `src/main.c:1389`:

```c
static void default_output_name(const char *input, char *out, size_t cap) {
    const char *base = basename_of(input);     // basename_of(".") -> "."
    size_t n = strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    /* Only strip extension if this looks like a file (has a dot that's not at the start) */
    if (n > 0 && out[n-1] != '/') {
        char *dot = strrchr(out, '.');
        if (dot && dot != out) { *dot = '\0'; }   // dot == out for "."
    }
}
```

For `input == "."`:

1. `basename_of(".")` returns `"."` (no `/`, so `strrchr` returns NULL and
   the original pointer is handed back -- `src/main.c:96`).
2. The dot-strip guard `dot != out` skips the strip because the dot is
   at index 0.
3. `base` ends up as `"."`.

The shared-build path then composes the artifact name at `src/main.c:3176`:

```c
snprintf(chosen_out, sizeof(chosen_out), "lib%s.so", base);   // -> "lib..so"
```

The manifest sidecar reuses the same `chosen_out` with a `.manifest`
suffix, producing `lib..so.manifest`.

## Adjacent case

`tur build --shared ./` -> `basename_of("./") == ""` -> `chosen_out = "lib.so"`.
Different symptom (empty name instead of double dot), same root cause:
`default_output_name` is not safe for paths that resolve to "the current
directory."

## Proposed fix

Two layers, smallest first:

1. **Sanitize `default_output_name` for "current dir" inputs.** When
   `basename_of(input)` returns `"."` or `""`, resolve `realpath(input,
   ...)` and use `basename_of` on the resolved absolute path instead.
   This is a localized patch and fixes the symptom for callers that
   don't know about manifests.

2. **Prefer manifest `:name` when available.** The
   `cmd_build_multi_files` caller at `src/main.c:3172` already has the
   parsed `PkgManifest` in hand (a few frames up); thread its `:name`
   in as a preferred basename and only fall back to
   `default_output_name(dir, ...)` when no manifest is present. This
   also future-proofs the [build-output-directory plan](../upcoming/build-output-directory-plan.md),
   which assumes manifest `:name` is the default artifact basename.

Either fix alone closes this bug; doing both gives a sensible result
whether or not a `build.tur` is in scope.

## Validation

Add a fixture or smoke test that runs `tur build --shared .` inside a
minimal spice tree and asserts:

- `lib<name>.so` exists (with the manifest's `:name`),
- `lib..so` does **not** exist,
- `lib..so.manifest` does **not** exist.

Also re-run the existing shared-build fixtures (`tests/fixtures/build-shared-*`)
to confirm absolute-path and named-dir invocations still produce the
expected `lib<name>.so`.

## Cleanup of existing pollution

`find ../turmeric-spices -maxdepth 3 -name 'lib..so*' -delete` removes
the historical droppings. The fix prevents them from regenerating.
