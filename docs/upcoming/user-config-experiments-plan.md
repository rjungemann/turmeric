# Plan: read `~/.config/turmeric/experiments.tur` as a user-level experiment default

**Status:** proposal (not started). **Area:** `src/runtime/experiments.{c,h}`,
`src/main.c`, one new small reader.
**Goal:** let a user declare a per-machine baseline of enabled experiments in
`$XDG_CONFIG_HOME/turmeric/experiments.tur`, honored by every turmeric entry
point (CLI, LSP, REPL, `tur build`, `tur check`) exactly when the current
project's `build.tur` does not carry an `:experiments` list. Suppressed
outright by any project that declares `:experiments` at all -- even empty
`:experiments []`. Editors and other tools (Trowel, future GUIs) get
experiment defaults "for free" without reimplementing the parser, the name
validation, the lifecycle-warning wiring, or the `expires_at` gate.

## Background

The experiment registry today (`src/runtime/experiments.h`) knows two
sources:

```c
typedef enum ExperimentSource {
    XF_SRC_NONE = 0,
    XF_SRC_MANIFEST,
    XF_SRC_CLI,
} ExperimentSource;
```

`experiment_enable(name, src)` records origin; CLI can overwrite a prior
manifest enable but not vice-versa (`src/runtime/experiments.c:129`). The
manifest path lives in `apply_manifest_experiments` (`src/main.c:2217`),
called once we have a resolved `PkgManifest`. There is no user-level
default: a developer who wants a flag on across every scratch file,
scratch REPL, and every non-manifested project has to remember
`--enable=<name>` every single invocation, or set an alias.

The Trowel editor (`../turmeric-scite`) hits this pain most acutely --
its plan currently proposes reading `$XDG_CONFIG_HOME/turmeric/experiments.tur`
itself and forwarding `--enable=` to `tur repl`. That works but bakes
name validation, comment parsing, and lifecycle awareness into every
editor that wants the same feature. Moving the read into turmeric solves
it for the whole ecosystem.

The `--allow-experimental` gate is a related but separate wart. It
currently guards nothing structural -- it's a "yes, I know" acknowledgment
next to `--enable=`. In practice every real invocation that uses
`--enable=` also sets `--allow-experimental`, and forgetting it produces
a confusing "experiment not permitted" error rather than a useful one.
This plan retires it: enabling an experiment *is* the acknowledgment.

## Goals

1. If `$XDG_CONFIG_HOME/turmeric/experiments.tur` (fallback
   `$HOME/.config/turmeric/experiments.tur`) exists and lists
   `:enable [name ...]`, every turmeric entry point starts with those
   experiments on.
2. A project's `build.tur` with any `:experiments [...]` key -- including
   the empty list `:experiments []` -- **suppresses the user file
   entirely**. The project owner has stated their intent; user
   preferences do not silently union in. Rationale: different projects
   may be pinned to different turmeric versions, and a user flag that
   toggles a feature the project's pin doesn't understand would surface
   as `TUR-E0310` at build time -- a surprise the project owner cannot
   plan around.
3. CLI `--enable=<name>` still wins over both, exactly as it wins over
   the manifest today.
4. `--allow-experimental` becomes always-on. Enabling an experiment (via
   any source) is itself the acknowledgment; a separate gate adds
   nothing.
5. `tur experiments` gains a new `source` value: `user-config`, so users
   can see where a given flag came from.
6. Unknown experiment names in the user file emit `TUR-E0310` with a
   pointer to the file path, matching the manifest path's contract
   ("typos surface immediately").
7. No new configuration surface beyond one file at one path. No
   environment variable, no per-tool override, no schema versioning.

## Non-goals

- **A general user config for other turmeric knobs.** This file is
  scoped to experiments. If we later grow user-level defaults for
  something else (default optimization level, warning verbosity), that
  gets its own file and its own plan -- not a "one big config" file that
  becomes a bike-shed magnet.
- **Merging with the manifest.** Explicitly rejected; see Goal 2.
- **Per-project user overrides.** A user who wants a different flag set
  per project uses that project's `build.tur`, an editor override, or an
  explicit `--enable=` in their run script. There is no
  `.turmeric/experiments.tur` beside the manifest.
- **A `[trowel]` / `[web-repl]` section, TOML compatibility, or any
  other schema drift.** The file is turmeric-syntax; the manifest reader
  handles it.
- **Grandfathering `--allow-experimental`.** The flag is removed, not
  deprecated. Any leftover in a script becomes an unknown-flag error at
  parse time. Rationale below.

## Design

### File location and format

Path resolution, in order:

1. `$XDG_CONFIG_HOME/turmeric/experiments.tur` when `XDG_CONFIG_HOME` is
   set and non-empty.
2. `$HOME/.config/turmeric/experiments.tur` otherwise.
3. On Windows, `%APPDATA%\turmeric\experiments.tur`.

Format is a turmeric-syntax key-value file, parsed by a subset of the
manifest reader:

```turmeric
;; ~/.config/turmeric/experiments.tur
;;
;; Experiments to enable by default in every turmeric invocation that
;; is NOT running inside a project whose build.tur declares its own
;; :experiments list.  Names must match `tur experiments`.

:enable [forall-kinds
         hkt-hrt
         vl-wide-functor]
```

The key is `:enable` (matching the CLI's `--enable=` naming), not
`:experiments`. Two reasons:

- `:experiments` in `build.tur` names the flags the *project* commits
  to; `:enable` in the user file names the flags the *user* wants
  turned on by default. Different semantic scope, different key --
  reduces the temptation to write a file with both keys and expect
  merging.
- The user file has no other keys today. If we ever add one (a "known
  bad experiments to warn about," say), `:enable` composes cleanly next
  to it; `:experiments` invites the "wait, is this the project or the
  user?" question every time.

No other keys are recognized in v1. Unknown keys emit a
`TUR-W0062: unknown key '<key>' in <path>` warning and are otherwise
ignored (forward-compatible with future additions).

### New source tag

Extend `ExperimentSource` (`src/runtime/experiments.h:43`):

```c
typedef enum ExperimentSource {
    XF_SRC_NONE = 0,
    XF_SRC_USER_CONFIG,   /* ~/.config/turmeric/experiments.tur      */
    XF_SRC_MANIFEST,      /* project build.tur :experiments          */
    XF_SRC_CLI,           /* --enable=<name> or LSP force-enable-all */
} ExperimentSource;
```

Ordering is deliberate: numerically ascending matches precedence
lowest-to-highest, so a future "highest source wins" refactor is
trivial. Today the rule remains explicit in `experiment_enable`:

```c
/* experiments.c:129 -- current */
if (src == XF_SRC_CLI || g_src[idx] == XF_SRC_NONE) g_src[idx] = src;
```

becomes:

```c
/* Higher-numbered source wins.  CLI beats manifest beats user-config
 * beats not-yet-set. */
if (src > g_src[idx]) g_src[idx] = src;
```

Semantically identical for the two existing sources, and the new source
slots in without further edits.

`tur experiments`' source column (`src/main.c:12079`) gains one arm:

```c
case XF_SRC_USER_CONFIG: return "user-config";
```

### The reader

A single new function in `src/runtime/experiments.c` (kept next to the
registry so it lives with the surface it configures):

```c
/* Read $XDG_CONFIG_HOME/turmeric/experiments.tur (or the platform
 * equivalent) and enable every name in its :enable list at
 * XF_SRC_USER_CONFIG.  No-op when the file does not exist.  Exits with
 * TUR-E0310 on an unknown name (path included in the message).
 *
 * Returns true iff a file was actually read (used by callers that need
 * to decide whether to suppress it -- see apply_user_config_experiments).
 */
bool experiments_read_user_config(void);
```

Implementation strategy: reuse `pkg_manifest_read` in a
"user-config subset" mode. The manifest reader already handles
`;`/`#| |#` comments, `:key [values]` shape, and produces a `PkgManifest`
with a `n_experiments` / `experiments[]` array. Two options:

- **A**: teach `pkg_manifest_read` a `mode` flag that accepts `:enable`
  as an alias for `:experiments` and rejects other required manifest
  keys (`:name`, `:version`) with `TUR-W0062` instead of erroring.
- **B**: write a 60-line dedicated reader that skips comments, finds
  `:enable [`, tokenizes symbols until `]`, and stops. No `.tur`
  evaluation, no macros -- the file's grammar is trivial.

Prefer **B** for v1. The manifest reader carries project-shape
assumptions we do not want to leak into the user-config file, and the
grammar is small enough that a dedicated reader stays understandable.
If a second config file lands later, we revisit and share the reader
then.

### Precedence and call ordering

Add `apply_user_config_experiments` next to `apply_manifest_experiments`
in `src/main.c`:

```c
/* Read the user-level experiments file.  Suppressed when the current
 * compile is running inside a manifested project that declared its own
 * :experiments list -- see the plan for rationale. */
static void apply_user_config_experiments(const PkgManifest *m_or_null) {
    if (m_or_null && m_or_null->has_experiments_key) return;
    experiments_read_user_config();
}
```

Note: `has_experiments_key` distinguishes "no key at all" from
"`:experiments []`". The manifest struct grows a `bool
has_experiments_key` alongside `n_experiments` -- one bit, set by the
reader when it saw the token even if the list was empty. Empty-list
suppression is the whole point of Goal 2.

Call ordering, at every entry point that resolves a manifest:

1. CLI parse -- `--enable=<name>` is recorded but not yet applied
   (deferred to after manifest+user, so it can overwrite them).
   *(This is the same shape the current code needs to keep CLI winning
   over the manifest; no functional change here beyond adding one more
   source in the middle.)*
2. Discover manifest (may be `NULL` for scratch files / non-project
   REPL).
3. `apply_user_config_experiments(manifest_or_null)`.
4. `apply_manifest_experiments(manifest)` when the manifest exists.
5. Apply the deferred CLI list.

Call sites to touch:

- `src/main.c:2244` (single-file compile path) -- already runs
  `apply_manifest_experiments`. Insert user-config read before it.
- `src/main.c:1161` (LSP force-enable-all path) -- already turns every
  experiment on for editors; no change needed. LSP does not read the
  user file (redundant; it already enables everything).
- `tur repl` entry point -- add user-config read after CLI parse and
  before REPL startup.
- `tur check` / `tur build <dir>` project mode -- reads manifest,
  should read user file first via the same helper.
- `tur run` -- same as `tur build`.
- WASM entry point (`src/web/wasm_glue.c`) -- **skipped**. The browser
  has no `$XDG_CONFIG_HOME`; the web REPL's opt-in is the
  query-string mechanism from `web-repl-experiment-flags-plan.md`.
  Explicitly do not read a virtual FS file -- that opens a can of
  "which visitor's config" worms.

### Retiring `--allow-experimental`

Concurrent with this plan, remove `--allow-experimental` and the
`g_allow_experimental` bit (`src/runtime/experiments.c:147`,
`src/main.c:1158`, `src/main.c:5935`, `src/main.c:12576`). Every use
site of the bit inside the compiler proper (`experiment_warn_if_used`
guard, etc.) treats it as always true.

Why now, not later: shipping the user-config file with a second
still-required "acknowledgment" flag makes the UX worse than status
quo -- the user now has to know about *two* things
(`experiments.tur` and `--allow-experimental`). Better to remove the
flag in the same commit that introduces the file. Users who want the
"I know these are experimental" reminder still get it via the
lifecycle warnings (TUR-W0060 / TUR-W0061), which fire regardless.

Behavior change is technically breaking for any script that passed
`--allow-experimental`, so the CLI parser rejects it with a targeted
message rather than silently accepting it:

```
error: --allow-experimental was retired in <VERSION>; enabling an
experiment (via --enable=<name>, build.tur, or
~/.config/turmeric/experiments.tur) is now the acknowledgment.
Remove the flag.
```

One release cycle of that message, then drop the arm entirely.

### CLAUDE.md update

The "Experimental Compiler Features -- STRICT RULE" section currently
lists two enable paths (CLI, manifest). Add the user-config file as the
third and note the suppression rule. Also strike the `-x`-alias
sentence that references `--allow-experimental` (if any -- verify
during implementation).

## Milestones

| # | Deliverable | Files touched |
|---|---|---|
| UC-1 | Add `XF_SRC_USER_CONFIG` to the enum; switch `experiment_enable` to "higher source wins"; add the `tur experiments` source arm; no readers yet. | `src/runtime/experiments.{c,h}`, `src/main.c` |
| UC-2 | Dedicated reader `experiments_read_user_config` + platform path resolution helper. Unit test via a small C harness. | `src/runtime/experiments.c`, `tests/unit/experiments_user_config.c` |
| UC-3 | Add `has_experiments_key` to `PkgManifest`; call `apply_user_config_experiments` from every manifest-resolving entry point (CLI compile, `tur repl`, `tur check`, `tur build`, `tur run`). Fixtures cover: no manifest + user file / manifest with key / manifest with empty `:experiments []` / CLI overrides both. | `src/main.c`, `tests/fixtures/experiments-user-config-*` |
| UC-4 | Retire `--allow-experimental`: remove the bit, delete the CLI arm, make `experiment_warn_if_used` unconditional; add the "retired in <version>" targeted error for one release. | `src/runtime/experiments.{c,h}`, `src/main.c`, CLAUDE.md |
| UC-5 | Docs: `docs/guides/experimental-flags-guide.md` grows a "user-level default" section; CLAUDE.md's experiments rule notes the third source. Trowel's plan can drop its "interim" path. | `docs/guides/experimental-flags-guide.md`, CLAUDE.md |

UC-1 through UC-3 form the shipping unit -- they can land in one PR or
three, but UC-2 is dead code until UC-3 calls it. UC-4 is a separate,
observably-breaking PR; land after UC-1..3 have soaked for a release.
UC-5 trails.

## Testing

- **Fixture: no manifest, user file present.** Scratch-file compile of
  a source that requires `forall-kinds` succeeds when
  `experiments.tur` lists it; fails with `TUR-E0310`-adjacent
  diagnostic when the file is absent.
- **Fixture: manifest with `:experiments []` (empty).** Same source,
  same user file: build fails with the compiler's normal
  "requires --enable=forall-kinds" wording. Proves suppression works
  for the empty case, which is the load-bearing corner of Goal 2.
- **Fixture: manifest with `:experiments [some-other-flag]`.** User
  file lists `forall-kinds`; source uses `forall-kinds`. Build fails.
  Proves the manifest suppresses the user file even when they don't
  overlap.
- **Fixture: CLI beats both.** Manifest and user file both empty /
  absent; `--enable=forall-kinds` on the CLI works. `tur experiments`
  shows source `cli`.
- **`tur experiments` source column.** New arm renders `user-config`
  for a flag that came from the file; unaffected for CLI / manifest.
- **Unknown name in user file.** Path is in the error message; exit
  code 2; no partial-enable side effects (idempotent parse).
- **`--allow-experimental` removed.** Passing it produces the targeted
  "retired in <version>" error, not a silent accept and not a generic
  "unknown flag." One test that this message fires; one test that a
  script *without* the flag now compiles a `forall-kinds`-using
  source cleanly.

## Risks and open questions

1. **Users with the flag on globally forget it exists.** If
   `experiments.tur` sets `forall-kinds` on and a shared code snippet
   assumes it is available, other developers may see errors the author
   never sees. Mitigation: the `tur experiments` command already lists
   what is enabled and where it came from; the source column with
   `user-config` makes the situation self-diagnosable. No further
   mechanism.
2. **The user file "leaks" into `bash tests/run.sh`.** The suite
   currently runs with no user file (CI does not create one). A
   contributor whose home directory carries a real
   `experiments.tur` could see local-only test passes / failures that
   don't reproduce elsewhere. Mitigation: `tests/run.sh` sets
   `XDG_CONFIG_HOME=<tmpdir>` or an equivalent env before invoking
   `tur`, forcing the file lookup into an empty directory. Cheap; add
   in UC-3.
3. **Retiring `--allow-experimental` in the same release.** If a
   downstream user has scripts or CI that pass the flag, they break at
   the same moment they lose the ability to sidestep the file. The
   targeted error message mitigates confusion; a release-notes callout
   in the CHANGELOG covers the rest. If we get pushback, keep the
   accept-and-ignore behavior for one release under a `-W`
   deprecation notice and drop the arm the release after.
4. **`XF_SRC_USER_CONFIG` numeric value.** Placing it between `NONE`
   and `MANIFEST` in the enum reorders existing values. Every switch
   on `ExperimentSource` needs to compile-check clean; grep for
   `XF_SRC_MANIFEST` / `XF_SRC_CLI` before landing UC-1 and confirm
   every use is either a comparison (fine) or a switch (needs the new
   arm, added in the same commit).
5. **Editor / LSP double-application.** LSP force-enables everything
   at `XF_SRC_CLI`; the user file would be a redundant no-op on that
   path. Correct behavior -- but worth an explicit early-return in the
   LSP init so `tur experiments` output stays legible (a name should
   read `cli` if that is what won, not "user-config that CLI then
   overwrote").

## Related

- `src/runtime/experiments.{c,h}` -- registry and lifecycle
  machinery.
- `src/main.c:2217` -- current manifest-side apply path.
- `docs/upcoming/web-repl-experiment-flags-plan.md` -- sibling plan
  for query-string opt-in in the browser; explicitly not affected by
  this change.
- `docs/guides/experimental-flags-guide.md` -- user-facing docs to
  update in UC-5.
- `../turmeric-scite/docs/experiment-flags.md` -- Trowel's plan; the
  "preferred path" section is unblocked once UC-3 lands.
