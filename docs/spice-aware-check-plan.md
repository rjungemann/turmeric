# Plan: Spice-aware module resolution for per-file `tur` subcommands

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Compiler / CLI / Tooling

---

## Overview

`tur check <file.tur>` reports `module 'X' not found (looked for '...')` for
every intra-spice `(import ...)` call when run on a file that sits inside a
spice's `src/` tree. The errors are bogus: the same modules resolve fine
when the spice is built end-to-end (via `tur build <dir>` or the cmake
pipeline that consumes `build.tur`). They appear because per-file commands
strip the include-path plumbing that `tur build <file>` already wires up.

The fix is two small additions to the compiler driver:

1. **Accept `-I` flags on every per-file subcommand**, not just
   `tur build <file>`. The compiler already supports include paths
   end-to-end -- we just need to thread them through `check`, `emit-c`,
   `emit-h`, `format`, and `run` in `src/main.c`.
2. **Auto-discover the enclosing spice's source root** by walking up from
   the input file to find `build.tur`, and adding `<spice_root>/src/` to
   the implicit include path. Users get the right behavior without
   passing flags; the explicit `-I` remains as an escape hatch and lets
   CI / fixtures override.

A third smaller piece -- improving the error message so it points at
`-I` and `tur build` when it fails -- is cheap and lands first.

This is a UX / tooling fix, not a correctness fix. The whole-spice build
already works; this plan just makes the per-file commands match its
behavior so editors, LSP clients, format-on-save hooks, and quick
"compile this one file" loops stop emitting spurious errors.

---

## Reproducer

Inside any spice that imports its own modules (e.g. `turmeric-spices/spices/frame`,
`spices/opengl`, `spices/c-dsl`):

```sh
cd turmeric-spices/spices/frame
tur check src/frame/frame.tur
```

Current output:

```
src/frame/frame.tur:29:3: error: module 'frame/schema' not found
  (looked for 'src/frame/frame/schema.tur')
```

The compiler walks the import as
`<dir-of-input>/<module-name>.tur` = `src/frame/` + `frame/schema.tur` =
`src/frame/frame/schema.tur`, which doesn't exist. The actual file is
`src/frame/schema.tur` -- one level up.

The same command on a `tur build`-style invocation succeeds:

```sh
tur build src/frame/frame.tur -I src
# (no spurious error)
```

`tur build <whole-dir>` and the cmake pipeline both compute include paths
correctly from the spice layout. Only the per-file driver path is missing
that step.

---

## Root cause (with file references)

### Where the resolver looks

`src/compiler/elab_module.c:80-130` is the import resolver. It tries
three locations, in order:

1. `module_base_dir + "/" + name + ".tur"` -- the directory of the
   importing file.
2. `module_stdlib_dir + "/" + name + ".tur"` -- the stdlib path
   (`stdlib/` next to the `tur` binary).
3. Each `module_include_dirs[i] + "/" + name + ".tur"` -- the `-I` paths.

If none hit, it emits `module '%s' not found (looked for '%s')` at
`elab_module.c:125-126`. The path printed is whichever the last attempt
was -- usually `module_base_dir`'s path, which is why the error looks like
it doesn't know about `-I` even when `-I` was passed (the error string
shows the first miss, not the full search list).

### Where the per-file commands strip include paths

`src/main.c:4724-4743` is the `check` handler. It:

- Requires `argc == 3`, so any flag fails the arg-count check.
- Calls `compile_to_c(argv[2], &out, NULL, 0)` -- passing `NULL, 0` for
  `include_dirs, n_include_dirs`.

By contrast, `src/main.c:4750-4799` (the `build` handler for the
single-file case) collects `-I` flags into a `build_inc` array and
passes them through to `cmd_build`.

`run` (`src/main.c:4801-4807` → `cmd_run`), `emit-c`, `emit-h`, and
`format` follow the `check` pattern: no flag parsing, no include paths.

### Why auto-discovery is missing

Nothing in `src/main.c` walks up from the input file to find a
`build.tur`. The resolver's `module_base_dir` is set to the importing
file's directory only (and re-set per-import to the directory of the
*currently-being-loaded* module, which is why deeply nested imports
fan out into wrong locations as in the reproducer above).

---

## Recommended fix

### Part 1: thread `-I` through every per-file subcommand

Lift the `-I` parsing loop from the `build` handler
(`src/main.c:4754-4789`) into a small helper, then call it from `check`,
`run`, `emit-c`, `emit-h`, `format`, and any future per-file subcommand.

```c
/* Parse -I/--include flags from argv into an array of strings.
 * Returns the number of include dirs collected; writes them into *out_dirs
 * (heap-allocated; caller frees).  Returns -1 on bad usage. */
int parse_include_flags(int argc, char **argv, int start, char ***out_dirs);
```

Each handler then calls the helper before its core compile call:

```c
char **inc = NULL;
int n_inc = parse_include_flags(argc, argv, /*start=*/2, &inc);
if (n_inc < 0) { return usage_check(); }
rc = compile_to_c(input, &out, (const char **)inc, n_inc);
free(inc);
```

### Part 2: auto-discover the spice root

Add a `find_spice_root(const char *file_path)` helper in `src/main.c`:

```c
/* Walks up from file_path's directory looking for `build.tur`.
 * Returns a heap-allocated absolute path to the directory containing
 * the build.tur (the spice root), or NULL if none is found within
 * MAX_WALK_UP steps (16). */
char *find_spice_root(const char *file_path);
```

Wire it into every per-file subcommand: after parsing explicit `-I`
flags, if `<spice_root>` is non-NULL and `<spice_root>/src/` exists,
prepend it to the include list. Explicit `-I` flags still win on a tie
because they were added first; spice-root resolution is the fallback.

Add a `--no-auto-spice` flag for the rare cases where auto-discovery
would interfere (CI fixtures testing the resolver itself, anyone
explicitly working "below" a spice root for an unrelated file).

### Part 3 (no-code, lands now): improve the error message

Even before parts 1 and 2 land, change the diagnostic at
`src/compiler/elab_module.c:125-126` to list *every* path tried and
suggest the workaround:

```
src/frame/frame.tur:29:3: error: module 'frame/schema' not found
  searched:
    src/frame/frame/schema.tur     (importing file's directory)
    stdlib/frame/schema.tur        (stdlib)
  hint: this looks like an intra-spice import; try:
    tur check -I src src/frame/frame.tur
  or build the whole spice with:
    tur build src/
```

When `-I` paths *were* provided, list them too so users see what
actually got searched.

---

## CLI changes (after parts 1 + 2)

All per-file subcommands accept the same include-path flags as
`tur build`:

```sh
tur check src/frame/frame.tur                            # auto-discovers spice root
tur check -I some/extra/path src/frame/frame.tur         # also adds -I path
tur check --no-auto-spice src/frame/frame.tur            # skip walk-up
tur emit-c src/frame/frame.tur                           # same auto-discovery
tur run src/frame/quickstart.tur                         # same
tur format --check src/frame/frame.tur                   # same
```

Behavior is identical between
`tur check <file>` and `tur build <file>` for any file whose imports can
be resolved against the spice's own `src/` tree. No flag needed for the
common case.

---

## Implementation phases

- [ ] **SC0** -- Improve the diagnostic at
  `src/compiler/elab_module.c:125-126` to list every search path tried
  (importing-dir, stdlib, every `-I`), and to suggest `-I src` / `tur
  build src/` when no path resolved. Lands as a one-commit improvement
  ahead of the structural fix. Tests: a single fixture that asserts
  the new diagnostic text on a missing import.

- [ ] **SC1** -- Extract the `-I` parsing loop out of the `build`
  handler in `src/main.c` (lines 4754-4789) into a small reusable
  helper `parse_include_flags`. Wire it into the `check` handler
  (lines 4724-4743). Update `usage_check()` to mention `-I`. Test:
  `tur check -I src src/frame/frame.tur` succeeds inside the FR0
  fixture spice.

- [ ] **SC2** -- Wire `parse_include_flags` into the `emit-c`, `emit-h`,
  `format`, and `run` handlers. Test: same fixture, run each subcommand
  with explicit `-I src` and assert success.

- [ ] **SC3** -- Add `find_spice_root` helper in `src/main.c`:
  walk up from the input file's directory until a `build.tur` is found
  or `MAX_WALK_UP` (16) steps are taken. Return the directory containing
  `build.tur` or NULL.

- [ ] **SC4** -- Wire `find_spice_root` into every per-file subcommand
  (the same ones touched in SC1+SC2). If a spice root is found and
  `<spice_root>/src/` exists, append it to the include-path list
  computed by `parse_include_flags`. Add `--no-auto-spice` to opt out.
  Tests: `tur check src/frame/frame.tur` (no flags) succeeds against
  the FR0 fixture spice.

- [ ] **SC5** -- Spice-cache-aware fallback for cross-spice deps. When
  `find_spice_root` returns a directory whose `build.tur` declares
  `:spices` entries (e.g. `tur-test`), also add the cache locations
  for fetched deps to the include path. The cache layout is whatever
  `tur fetch` produces today -- read `src/compiler/pkg.c` to confirm
  before coding. This is the only phase that touches the dep
  resolver; punt to a follow-up plan if the cache layout is in flux.

- [ ] **SC6** -- LSP server (`src/main.c:4745-4748`) calls the same
  helpers so editor diagnostics no longer report bogus
  "module not found" errors in spice files. The LSP entry point goes
  through `compile_to_c` already; the change is purely in how the
  include list is built up before the call.

- [ ] **SC7** -- Documentation: a paragraph in `CLAUDE.md` and in
  `docs/guides/spice-authoring-guide.md` (or wherever the canonical
  spice-author docs live) explaining the auto-discovery rule, the
  `--no-auto-spice` escape hatch, and how `-I` interacts. Update the
  notebook plan's `tur fmt` references too -- the plan currently
  worries that per-file `tur fmt` will misbehave inside a spice; with
  SC4 it won't.

- [ ] **SC8** -- **Per-spice CI regression matrix.** Add a CI job
  (turmeric-spices repo) that runs `tur check src/**/*.tur` for every
  spice in `turmeric-spices/spices/*` and fails on any error. Today
  the output of these runs is all bogus "module not found" warnings;
  after SC4 it becomes a genuine signal for intra-spice import
  breakage and missing-export regressions. Wire into the existing CI
  workflow alongside the per-spice test runs. Blocked by SC4.

---

### Stdlib loading noise (sub-track)

Independent of the per-file resolver fix, `tur check` (and likely
every other subcommand that loads stdlib) emits a handful of
`cannot open 'stdlib/X.tur': No such file or directory` lines to
stderr on every invocation, even on success. The visible names today
are `macros.tur`, `safe.tur`, `contract.tur`, `hamt.tur`, `map.tur`,
but there may be more attempted silently. Cause: the launcher likely
hardcodes a relative path that only resolves when `tur` is invoked
from the turmeric repo root.

These phases land in parallel with SC0-SC8; they touch the launcher,
not the resolver, so there is no ordering constraint with the SC
chain. They do, however, gate SC0's improved diagnostic from looking
clean -- without them, the new multi-line "searched: ..." block
prints below five lines of stdlib noise.

- [ ] **SN0** -- Investigate where the `tur` binary computes
  `module_stdlib_dir`. Likely in `src/main.c` near the elaborator
  context setup or in a launcher helper. Document: how the path is
  currently derived, why it fails when cwd is not the turmeric repo
  root, the full list of stdlib files attempted at startup, and which
  of them are required vs. optional. Output: a one-paragraph
  root-cause note that SN1 builds on.

- [ ] **SN1** -- Fix stdlib path resolution to be cwd-independent.
  Options to weigh during implementation: (a) walk up from the `tur`
  binary's location (resolve `argv[0]` via `readlink /proc/self/exe`
  on Linux, `_NSGetExecutablePath` on macOS, `GetModuleFileName` on
  Windows) to find a sibling `stdlib/`; (b) bake the install prefix
  in at cmake build time via a `-D` macro; (c) honor a
  `TUR_STDLIB_DIR` env var as override. Recommendation: (a) with
  (c) as escape hatch, and (b) for installed builds where the
  source-tree walk-up would not apply.

- [ ] **SN2** -- Stop emitting `cannot open 'stdlib/X.tur'` lines on
  success. Once stdlib is found correctly (SN1), the spurious
  warnings should disappear in the happy path. Audit the launcher's
  stdlib-load loop: every attempted file that legitimately may not
  exist should fail silently (no stderr output); only genuine
  failures should print. If any of the named files are required,
  fail hard with one clear message instead of five separate lines.

- [ ] **SN3** -- Add a regression test that runs `tur check` on a
  known-good fixture file from a non-repo-root cwd (e.g. `/tmp`) and
  asserts stderr is empty on success. Today this would catch the
  stdlib noise; after SN1+SN2 it is a regression guard preventing
  reintroduction. Wire into CI so any future launcher-side
  path-resolution regression fails the build.

---

## Tests

Fixture: a 2-file spice in `tests/fixtures/spice-resolver/`:

```
tests/fixtures/spice-resolver/
  build.tur                      -- minimal manifest naming the two modules
  src/foo/a.tur                  -- (defmodule foo/a ...) exporting one fn
  src/foo/b.tur                  -- (import foo/a :refer [...]) and uses it
```

Test cases (one per SC phase):

| Test | Asserts |
|------|---------|
| `tur check fixture/src/foo/b.tur` (pre-SC4) | exit 2, error message lists paths tried (SC0) |
| `tur check -I fixture/src fixture/src/foo/b.tur` | exit 0 (SC1) |
| `tur emit-c -I fixture/src fixture/src/foo/b.tur` | exit 0, valid C printed (SC2) |
| `tur check fixture/src/foo/b.tur` (post-SC4) | exit 0 via auto-discovery (SC4) |
| `tur check --no-auto-spice fixture/src/foo/b.tur` | exit 2 (SC4 escape hatch) |
| `tur check fixture/src/foo/b.tur` from a deeply-nested cwd | exit 0 (walk-up tolerates path) |
| `find_spice_root` returns NULL above 16 levels | unit test |

CI: add `tur check` of one source file from each spice in
`turmeric-spices/spices/*/src/` to the regression matrix. Today this is
all bogus errors; after SC4 it should be a real signal.

---

## Design notes

### Why auto-discovery and not "just always pass `-I`"

Editors do not run user-edited shell pipelines. VS Code's "format on save"
hook, vim's `:make`, neovim LSP, helix LSP, and emacs `flycheck` all
shell out to `tur check <file>` with no project context. If they have to
be configured per-spice to add `-I src`, every spice gets a `.vscode/`
folder, every contributor has to remember to set it up, and the failure
mode is silent (red squiggles on lines that are actually correct).

Walking up to `build.tur` is the universal fix: it works in every editor
without configuration, it matches how every other Lisp / package
system finds a project root (look for the manifest), and it is
overrideable via `--no-auto-spice` when needed.

### Why 16 as the walk-up limit

Arbitrary, but generous. The deepest source file in any current spice
is `spices/<name>/src/<mod>/file.tur` -- 3 dirs above the spice root.
16 lets us tolerate users running from inside deeply-nested temp
checkouts, worktrees, or `node_modules`-style nesting without ever
walking up to `/` on a filesystem that has no `build.tur` anywhere.

### Why not stop the walk at the first `build.tur` *or* `.git/`

Considered. The complication: a developer can be working in a checked-out
copy of `turmeric-spices` where the top-level `.git/` is at the monorepo
root, not at any spice root. We want to keep walking past `.git/` to
find the per-spice `build.tur`. Conversely, a `build.tur` is the
signal we actually care about; stopping there is unambiguous.

### Interaction with `tur build <dir>` and the cmake pipeline

`tur build <dir>` already does the right thing (it knows the dir is the
spice root). The cmake pipeline derived from `build.tur` also already
does the right thing (cmake's include paths are set from the manifest).
This plan does not touch either path; it only fixes the per-file driver
to be consistent with them.

### Error-message improvement (SC0) is independent

SC0 makes things *better* for everyone -- including users who do remember
to pass `-I` and still hit a typo. It can land as a standalone PR and
gives us a known-good baseline for the test fixture before SC1 adds the
plumbing it asserts on.

### LSP and the existing JSON output mode

`tur check --json-diagnostics` already exists (`src/main.c:4729-4737`).
LSP servers and editor integrations using it benefit from the same
auto-discovery once SC4 lands -- no LSP-specific code change needed
beyond SC6's verification.

---

## Risks and open questions

1. **Auto-discovery surprises.** Could a stray `build.tur` in a parent
   directory cause the wrong include path to win? Possible but
   unlikely: every existing `build.tur` is at a spice root, and the
   walk-up halts at the *first* one found. If a user has nested
   spices (one spice's `src/` happening to contain another spice's
   `build.tur`), the inner one wins, which is what we want. The
   `--no-auto-spice` flag is the documented escape hatch.

2. **Cross-spice include-path order.** SC5 has to decide: should
   fetched-dep include paths come before or after the local
   `<spice_root>/src/`? Local should win on a name collision (otherwise
   a vendored dep could shadow a local module). Tests in SC5 should
   include a fixture exercising the precedence.

3. **`find_spice_root` performance.** `stat()` once per dir walked
   upward, max 16 calls per check invocation. Negligible. Caching
   across files inside the same `tur lsp` session is a follow-up
   if profiling ever shows it.

4. **`build.tur` parsing.** `find_spice_root` only checks for the
   file's *existence*; it does not parse the manifest. SC5 will need
   to parse `:spices` and `:cmake-deps` -- a small re-use of the
   existing pkg parser in `src/compiler/pkg.c`. No new format.

5. **Stdlib loading noise and SC0 collision.** `tur check` emits five
   `cannot open 'stdlib/...': No such file or directory` lines on
   every invocation, success or failure. SC0's improved diagnostic
   (multi-line "searched: ..." output) would print *below* that noise
   and read poorly until it's silenced. Mitigation: the SN0-SN3
   sub-track (see implementation phases) lands the launcher-side fix
   in parallel; landing SN1+SN2 before SC0 gives the cleanest
   user-visible result. If ordering slips, SC0 still ships -- the
   noise is cosmetic, not blocking.

---

## Shared work

### Main turmeric repo

- `src/main.c` -- handler changes (SC1, SC2, SC4, SC6) and the new
  `parse_include_flags` / `find_spice_root` helpers.
- `src/compiler/elab_module.c:125-126` -- diagnostic improvement (SC0).
- `tests/fixtures/spice-resolver/` -- new fixture (all phases).
- `tests/` -- new test scripts running the cases in the Tests section.
- `CLAUDE.md` -- short paragraph documenting auto-discovery (SC7).
- `docs/guides/` -- spice-authoring guide gains a "running per-file
  commands inside a spice" section (SC7).

### turmeric-spices repo

- CI work is tracked as phase SC8 above (per-spice `tur check`
  regression matrix, blocked by SC4).

### Notebook plan adjustment

- The `tur fmt` companion in `docs/tur-run-plan.md` assumes per-file
  `tur fmt src/file.tur` works inside a spice. Today it would emit the
  same bogus warnings; after SC4 it works transparently. No plan
  change required -- the assumption just becomes valid.

---

## Future work

| Item | Why deferred |
|------|--------------|
| Per-LSP-session include-path cache | Premature optimization until profiling shows it |
| `tur check <dir>` (whole-spice check without codegen) | Easy to add once SC4 lands; parallel to `tur build <dir>` |
| Auto-discover for `--strict-effects` / `--lint-effects` runs | Same code path; lands "for free" with SC4 |
| Workspace / multi-spice resolution | Needed only when one repo contains many interdependent spices; the `:spices` block already covers single-spice deps |
