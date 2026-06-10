---
title: `tur build <file>` does not auto-discover the enclosing spice; sibling per-file commands do
severity: ergonomics gap -- the only per-file subcommand that lacks auto-spice discovery, causing "module not found" for intra-spice imports that work everywhere else
status: fixed
discovered: 2026-06-06
fixed: 2026-06-10
discovered-in: turmeric (src/main.c command dispatch)
---

# `tur build <file>` does not auto-discover the enclosing spice

## Summary

Inside a spice tree (`build.tur` plus `src/`), the per-file subcommands
`tur check`, `tur emit-c`, `tur emit-h`, and `tur run <file>` all walk
up from the input file looking for an enclosing `build.tur`, then add
that spice's `src/` and every declared `:spices` dep's `src/` to the
module-resolution search path. `tur build <file>` is the **only**
per-file subcommand that skips this walk-up.

As a result, the same source file that compiles cleanly under
`tur check`, `tur emit-c`, and `tur run` fails with a module-not-found
error under `tur build`. Users have to either:

- switch to `tur build <dir>` (which reads the manifest), or
- pass every `-I` flag manually (`tur build -I src -I spices/foo/src ...`),
- use `tur run <file>` and discard the produced binary.

None of those workarounds matches the natural ergonomic intuition --
"`tur emit-c file.tur` works, so `tur build file.tur` should too."
This is called out as a known wart in
[docs/guides/developing-spices-guide.md, "When auto-discovery does
not apply"](../guides/developing-spices-guide.md#when-auto-discovery-does-not-apply)
but no fix has been scheduled.

## Repro

```sh
cd ../turmeric-spices/spices/frame
tur check    src/frame/quickstart.tur   # OK -- intra-spice imports resolve
tur emit-c   src/frame/quickstart.tur   # OK
tur emit-h   src/frame/quickstart.tur   # OK
tur run      src/frame/quickstart.tur   # OK -- builds + executes
tur build    src/frame/quickstart.tur   # FAIL: module 'frame/schema' not found
```

Pre-condition: the spice imports at least one sibling module from its
own `src/` (or from a declared `:spices` dep). A standalone file with
only stdlib imports won't trigger the gap.

## Root cause

`src/main.c` -- the `"build"` command dispatch (line 8003-8100) walks
the argv, parses `-I`, `-o`, `--shared`, `--target`, then in the
single-file branch (line 8077-8097) calls only:

```c
char *b_root = find_spice_root(input);
// ... read manifest for :reader-macros ONLY ...
rc = cmd_build(input, out, build_inc, n_build_inc,
               build_target, b_rm, b_n);
```

It deliberately fishes the `:reader-macros` block out of the enclosing
manifest -- but it never calls `auto_append_spice_includes`, which is
the function every other per-file dispatch uses. Compare:

- `emit-c` dispatch (line 7883):
  `auto_append_spice_includes(input, &emit_inc, &n_emit_inc, ...);`
- `emit-h` dispatch (line 7914):
  `auto_append_spice_includes(input, &eh_inc, &n_eh_inc, ...);`
- `check` (per-file path inside `cmd_check`, line 1149):
  `auto_append_spice_includes(path, &inc, &n_inc, &owned, &n_owned, &ls2);`
- `run` (single-file branch, line 2651):
  `auto_append_spice_includes(explicit_file, &user_inc, &n_user_inc, ...);`

Only `build`'s single-file branch is missing the call. Reader-macro
discovery was bolted on (see `RM4` comment at 8078), but include-path
discovery was not.

The `--no-auto-spice` opt-out (line 7137-7151 / 2081) is already
plumbed and would compose correctly with a fix -- `auto_append_spice_includes`
short-circuits when `g_no_auto_spice` is set, so adding the call to
`build` does not regress users who pass `--no-auto-spice`.

## Observed vs. expected

| Subcommand           | Auto-discovers spice src/? | Auto-discovers dep src/? |
| -------------------- | -------------------------- | ------------------------ |
| `tur check <file>`   | yes                        | yes                      |
| `tur emit-c <file>`  | yes                        | yes                      |
| `tur emit-h <file>`  | yes                        | yes                      |
| `tur run <file>`     | yes                        | yes                      |
| **`tur build <file>`** | **no**                   | **no**                   |
| `tur build <dir>`    | yes (manifest-driven)      | yes (manifest-driven)    |

Expected: all per-file commands behave the same way w.r.t. include-path
discovery. The whole point of auto-discovery is that editors, format
hooks, and quick "compile this one file" loops Just Work without
per-spice configuration -- and `tur build <file>` is squarely a
"compile this one file" loop.

## Proposed fix

In `src/main.c`, the single-file `build` branch (around line 8077),
**before** the `cmd_build(...)` call, invoke `auto_append_spice_includes`
with the same shape used by `emit-c` / `emit-h`:

```c
} else {
    /* RM4: auto-discover the spice manifest containing `input` so
     * `:reader-macros [...]` entries apply when building a single
     * file from inside a project. No-op when there is no manifest. */
    char *b_root = find_spice_root(input);
    char **b_rm  = NULL;
    int    b_n   = 0;
    if (b_root) { /* unchanged manifest read */ }

    /* NEW: extend include path with enclosing spice src/ + dep src/,
     * mirroring emit-c / emit-h / run. */
    char **b_owned = NULL; int n_b_owned = 0;
    Ls2ResolverCtx b_ls2 = {0};
    auto_append_spice_includes(input, &build_inc, &n_build_inc,
                               &b_owned, &n_b_owned, &b_ls2);
    ls2_resolver_ctx_set(&b_ls2);

    rc = cmd_build(input, out, (const char **)build_inc, n_build_inc,
                   build_target, (const char **)b_rm, b_n);

    ls2_resolver_ctx_set(NULL);
    ls2_resolver_ctx_dispose(&b_ls2);
    for (int i = 0; i < n_b_owned; i++) free(b_owned[i]);
    free(b_owned);
    free(b_root);
    free_reader_macro_paths(b_rm, b_n);
}
```

This piggybacks on the same `--no-auto-spice` flag the other commands
already honor, so users who deliberately want the pre-discovery
behavior still have an escape hatch.

## Validation

1. The fixture under `tests/fixtures/spice-autoload-*` (or whichever
   exercises spice-tree resolution from per-file commands) should
   gain a `tur build <file>` invocation alongside its existing
   `tur check` / `tur emit-c` ones, and pass.
2. A new fixture should `tur build src/<spice>/<file>.tur` from
   inside `../turmeric-spices/spices/frame` (gated on
   `requires.spices`) and assert success + a runnable artifact.
3. The `--no-auto-spice` regression case: invoking
   `tur --no-auto-spice build src/frame/quickstart.tur` should still
   fail with the unresolved-import error, confirming the opt-out works.
4. The single-file build remains a single-file build -- it must not
   start descending into sibling modules implicitly. Auto-discovery
   only widens the **include path** for resolving imports; it should
   not change which file is the compilation entry point.

## Notes and related work

- `docs/guides/developing-spices-guide.md` already lists this as a
  documented limitation (section "When auto-discovery does not
  apply"); the fix lets us delete that bullet and keep the guide
  honest.
- This is a **per-file** asymmetry only. `tur build <dir>` already
  configures itself from `build.tur` and is unaffected.
- Side-stepping by users via `tur run <file>` works but discards the
  binary; it's a workaround, not a substitute, for tooling that wants
  to keep the artifact (e.g. editor "compile current file" actions
  that diff the resulting object).
- An adjacent gap worth tracking but **not** addressed here:
  `tur format <file>` also skips include-path resolution. That is by
  design -- the formatter does not need to resolve imports -- and the
  guide already documents it, so no fix is implied.

## Resolution

Fixed in `src/main.c`'s single-file `build` branch (the `else` arm of the
`is_directory(input)` check). Before the `cmd_build(...)` call it now invokes
`auto_append_spice_includes(input, &build_inc, &n_build_inc, ...)` with the
same shape `emit-c` / `emit-h` / `check` / `run` use, sets/disposes the LS2
resolver context, and frees the owned include paths -- exactly the
proposed-fix sketch above. The pre-existing `:reader-macros` discovery
(`RM4`) is preserved; the new include-path widening sits alongside it.

Because `auto_append_spice_includes` short-circuits when `g_no_auto_spice`
is set, `--no-auto-spice` still opts out, and the single-file build stays a
single-file build -- auto-discovery only widens the include path for
resolving imports, it does not change the compilation entry point.

Validated by extending the dedicated-runner suite
`tests/spice-resolver-tests.sh` (ctest target `tur_spice_resolver_tests`):

- **SC4-build**: `tur build <file>` on `spice-resolver-ok/src/foo/b.tur`
  (which `(import foo/a)`) now compiles with no `-I` and the produced binary
  returns 42.
- **SC4-build regression**: `tur --no-auto-spice build <file>` still fails
  with the SC0 "intra-spice import" diagnostic, confirming the opt-out.
- **SC5-build**: `tur build spice-resolver-deps/src/app/main.tur` resolves the
  `:spices :path` producer and the binary returns the dep's value (100).

Suite passes `64 passed, 0 failed`, leak-clean (detect_leaks=1). The guide's
"When auto-discovery does not apply" bullet for `tur build <file>` was
removed and the per-file family list updated to include it.
