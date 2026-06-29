# `tur` CLI: `interpret` subcommand + shortest-unambiguous-prefix dispatch

## Motivation

Two small ergonomic fixes to the top-level `tur` CLI:

1. `tur --interpret <file.tur>` is the odd one out — every other mode is a
   bare subcommand (`tur build`, `tur run`, `tur check`, ...). Promote it to
   `tur interpret <file.tur>` and treat `--interpret` as a quiet
   backwards-compat alias.
2. Typing the full subcommand name is unnecessary friction when the prefix
   is unambiguous. `tur int`, `tur bui`, `tur ch` should dispatch to
   `interpret` / `build` / `check`. Ambiguous prefixes (`tur i`, which could
   mean `init`, `install`, `interpret`, `image-info`, `image-verify`) fall
   back to the existing top-level usage output.

Both changes are dispatch-layer-only. No subcommand semantics change.

## Scope

### In scope

- Add `interpret` to the subcommand table in `src/main.c` (currently
  `--interpret` at `src/main.c:13009`).
- Keep `--interpret` working silently (no deprecation warning yet — we have
  no external consumers and the churn isn't worth a TUR-W code).
- Update `usage()` in `src/main.c` to list `tur interpret <file.tur>` in
  place of the current `tur --interpret` line.
- Replace user-facing references to `tur --interpret` with `tur interpret`
  in `docs/`, `README.md`, guides, and any fixture-adjacent scripts that
  are part of the documented surface. Test fixture invocations and
  `tests/run.sh` plumbing that pass `--interpret` internally can stay on
  the flag (it still works); only docs and examples must move.
- Implement shortest-unambiguous-prefix dispatch for the top-level
  subcommand token (`argv[1]`). On ambiguity, print the existing
  `usage()` output and exit non-zero, same as an unknown command today.
- Exact matches always win over prefix matches (so `tur run` keeps working
  even though `runner` could hypothetically be added later).
- Flags that start with `-` (`--help`, `--version`, `--interpret`,
  `--explain`, ...) are NOT prefix-matched — they require exact spelling.

### Out of scope

- Did-you-mean suggestions on ambiguity (just dump usage for now, as the
  task specifies).
- Prefix matching inside subcommand argument parsing (e.g. `tur fetch
  --upd` for `--update`). Top-level subcommand only.
- Deprecating `--interpret` with a warning. Silent alias is enough.
- Renaming any other dashed top-level mode (none remain after this).
- Shell completion updates. (If/when we ship completions, they can list
  the canonical names; prefix dispatch is a runtime convenience, not a
  completion contract.)

## Design

### 1. `interpret` subcommand

Add a branch alongside the existing `--interpret` handler at
`src/main.c:13009`:

```c
if (strcmp(cmd, "interpret") == 0 || strcmp(cmd, "--interpret") == 0) {
    /* existing body */
}
```

The `--interpret` arm of the OR is the back-compat path. Both spellings
route through the same code; no behavioral split.

Update the `usage()` text at `src/main.c:11569` from

```
  tur --interpret <file.tur>        run a file through the tree-walking interpreter
```

to

```
  tur interpret <file.tur>          run a file through the tree-walking interpreter
```

### 2. Shortest-unambiguous-prefix dispatch

Today, `argv[1]` is matched against ~34 hard-coded `strcmp` branches in
`main()`. The cleanest minimal change: build a small canonical command
table once at startup, resolve `argv[1]` against it, and rewrite `argv[1]`
to the canonical name before falling into the existing `strcmp` chain.

```c
static const char *const CANONICAL_COMMANDS[] = {
    "emit-c", "emit-h", "emit-cmake", "check", "audit-spans",
    "lsp", "mcp", "dap", "lsp-lite",
    "build", "run", "repl", "worker", "interpret", "debug",
    "eval", "doc", "image-info", "image-verify", "explain",
    "format", "fmt", "parse-check", "test",
    "new", "init", "add", "add-cmake", "fetch",
    "install", "uninstall", "list", "upgrade", "experiments",
    NULL,
};

/* Returns the canonical name if `tok` is an exact match OR a unique
 * prefix; returns NULL if no match; returns the sentinel
 * COMMAND_AMBIGUOUS if more than one canonical name starts with `tok`. */
static const char *resolve_command(const char *tok);
```

Resolution rules (in order):

1. If `tok` starts with `-`, return `NULL` (flags don't prefix-match).
2. If `tok` exactly equals a canonical name, return that name.
3. Otherwise, scan the table for canonicals starting with `tok`:
   - 0 hits → `NULL` (unknown; falls through to today's `usage()`).
   - 1 hit → return that canonical.
   - >1 hits → return `COMMAND_AMBIGUOUS`.

In `main()`, immediately after `const char *cmd = argv[1];`
(`src/main.c:12573`):

```c
const char *resolved = resolve_command(cmd);
if (resolved == COMMAND_AMBIGUOUS) return usage();
if (resolved != NULL) cmd = resolved;
```

The existing `strcmp` chain is untouched; it just sees the canonical
spelling. Unknown tokens (resolved == NULL) fall through and hit the
trailing `return usage();` at the end of `main()` exactly like today.

### Edge cases

- `tur --interpret` is a flag (starts with `-`), so prefix logic skips it
  and the existing handler runs unchanged.
- `tur --help` / `tur --version` are handled before we reach the prefix
  layer (`src/main.c:12561-12569`), so they're unaffected.
- `tur build` and `tur build-something-future` would both start with
  `build` — exact match wins, so adding new commands later doesn't break
  the bare `build` form.
- `tur emit` is ambiguous (`emit-c`, `emit-h`, `emit-cmake`) → prints
  usage, exit non-zero.
- Empty `argv[1]` already short-circuits via `if (argc < 2) return
  usage();` before resolution runs.

## Implementation steps

1. In `src/main.c`:
   - Add `CANONICAL_COMMANDS[]` + `COMMAND_AMBIGUOUS` sentinel +
     `resolve_command()` near the top of the file (next to `usage()`).
   - Wire the resolver into `main()` after `argv[1]` is bound.
   - Add `interpret` to the existing `--interpret` branch.
   - Update the `usage()` line for interpret.
2. Sweep docs:
   - `grep -rn 'tur --interpret' docs/ README.md` and rewrite to
     `tur interpret`.
   - Spot-check guides under `docs/guides/` for examples.
3. Add fixtures under `tests/fixtures/`:
   - `cli-interpret-subcommand/` — runs `tur interpret tiny.tur`,
     expects success.
   - `cli-prefix-unique/` — runs `tur int tiny.tur` (unique prefix for
     `interpret`), expects success.
   - `cli-prefix-ambiguous/` — runs `tur i tiny.tur`, expects non-zero
     exit and usage on stderr.
   - `cli-interpret-flag-backcompat/` — runs `tur --interpret tiny.tur`,
     still works.
   These belong with the other CLI smoke fixtures; check
   `tests/fixtures/` for the closest existing pattern and match it (some
   CLI fixtures use a `cmd` file + `expected.out`).
4. Regenerate any fixture snapshots that drift from the dispatch change
   (none expected — codegen is untouched — but verify with
   `bash tests/run.sh 2>&1 | grep '^FAIL'`).
5. `bash tests/run.sh` (10-minute timeout per CLAUDE.md), confirm zero
   FAILs.

## Risks

- **Future ambiguity**: adding a new subcommand could turn a previously
  unique prefix into an ambiguous one and silently break someone's muscle
  memory. We accept this — there are no external consumers yet, and
  scripts should use canonical names anyway. If/when we have consumers,
  add a CI lint that warns when a new command would shorten an existing
  unique prefix.
- **`--interpret` ossifying**: keeping the flag forever is a small wart.
  Acceptable; can deprecate later under a TUR-W code if it ever matters.
- **Docs drift**: the doc sweep is the only mechanical risk. `grep -rn`
  catches the obvious cases; the rest will surface when readers hit
  them.

## Done when

- `tur interpret <file>` works; `tur --interpret <file>` still works.
- `tur int <file>` dispatches to `interpret`; `tur i <file>` prints
  usage and exits non-zero.
- `tur --help` lists `tur interpret`, not `tur --interpret`.
- `grep -rn 'tur --interpret' docs/ README.md` returns nothing.
- `bash tests/run.sh` is green with the four new CLI fixtures.
