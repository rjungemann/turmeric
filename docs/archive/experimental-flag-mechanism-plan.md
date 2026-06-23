# Experimental Feature Flag Mechanism Plan (XF0--XF6)

> **Status:** Mechanism implemented (XF0--XF4, XF6); `EXPERIMENTS[]` ships
> **empty** per the design constraint below. Successor to the now-retired
> `-X<name>` surface ([../drop-x-flags-plan.md](../drop-x-flags-plan.md)).
> Design only -- no flags ship under this mechanism until a concrete
> experimental feature requests one.
>
> **XF5 (first real consumer) is deferred.** Its named consumer
> (cross-parameter sized-type unification) already shipped and is default-on,
> so there is nothing to "migrate" -- gating it would regress the
> `sized-cross-param-*` fixtures. See
> [../../reported/experimental-flag-xf5-stale-premise.md](../../reported/experimental-flag-xf5-stale-premise.md).
> A genuinely-new candidate (size-index arithmetic) is noted there.
>
> **What shipped:** `src/runtime/experiments.{h,c}` (registry +
> `experiment_enable`/`_is_enabled`/`_warn_if_used`/`_reset_warnings`),
> `--enable=<a,b,c>` + `--allow-experimental` CLI parsing and the
> `:experiments [...]` manifest key (CLI wins on conflict), the `TUR-E0310`
> unknown-experiment error and `TUR-W0060`/`TUR-W0061` lifecycle warnings
> (all three explainable via `tur explain`), the `tur experiments`
> subcommand (`--json`), release-cut expiry enforcement in the
> `/cut-minor-release` + `/cut-major-release` skills, and
> [../../guides/experimental-flags-guide.md](../../guides/experimental-flags-guide.md).
>
> **Prerequisites:** Drop-`-X`-flags cut shipped (0.24.0 -- all 16 legacy
> flags are accept-and-warn no-ops; the implies-graph is gone; help text
> no longer lists per-feature gates).
>
> **Last updated:** 2026-06-23

---

## Motivation

The drop-`-X`-flags plan retired 16 feature gates because every one of
them had either shipped or degenerated into ceremony. That left v1 with
**no mechanism at all** for genuinely-experimental features -- the kind
where:

- The feature is half-built and we want it merged behind a gate so the
  trunk stays buildable while it grows.
- The semantics are still in flux and we do not want spice authors to
  start depending on the surface yet.
- The feature carries a known performance, soundness, or compile-time
  cost we are not ready to impose on every user.

The previous `-X` regime conflated three different lifecycles into one
flag namespace -- "in development", "stable but opt-in", and "stable and
on" -- which is why it accreted 16 entries and then all 16 turned out to
be on-by-default in practice. The replacement mechanism must keep
genuinely-experimental gates without re-creating that drift.

Goals:

- A single, **uniformly-named** opt-in surface for in-flight features
  with a clear "this is not stable" signal at every use site.
- A built-in **expiry policy** -- every experimental flag carries a
  target release by which it either graduates (becomes always-on) or is
  removed; no flag can sit at "experimental" indefinitely.
- A **machine-readable registry** so guides, `tur --help`, fixtures, and
  the docs site agree on what is experimental without anyone restating
  the list.
- Composability with the existing diagnostic flags
  (`--strict-effects`, `--warn-unused-result`, etc.) -- experimental
  gates do not subsume diagnostics.

Non-goals:

- Re-introducing any of the 16 retired flags. Those features are
  standard; if one regresses, it gets fixed, not re-gated.
- A general-purpose user-extensible plugin system. This mechanism is
  for **compiler-internal** experiments only.
- Runtime feature detection inside Turmeric source. Source code does
  not branch on which experimental flags are on; either the feature is
  available (the flag is set at compile time and the syntax/types
  elaborate) or it is not (the syntax errors as unknown).

---

## Design

### Single flag surface: `--enable=<name>`

Replace the historical `-X<name>` shape with a long-form, comma-list
flag:

```
tur build --enable=fancy-rows,deep-refinements src/
```

Why long-form:

- `-X<name>` looked like a stable knob. `--enable=<name>` reads as
  "opt me in to this experiment" at every call site.
- Comma-list parsing collapses multi-flag invocations onto one token
  and matches how `--features=` is spelled in adjacent ecosystems
  (Cargo, Bazel, GCC `-fopt-info=`).
- Leaves the entire `-X` prefix free for genuinely future work
  (cross-compilation targets, e.g.) without name collisions with the
  retired set.

Manifest equivalent: a top-level `:experiments [...]` key in
`build.tur` whose contents are merged with the CLI list at parse time.

```turmeric
(package
  :name "my-spice"
  :version "0.1.0"
  :experiments [:fancy-rows :deep-refinements])
```

Both shapes resolve to the same internal set; the CLI list wins on
conflict (CLI is the more specific override).

### Required metadata per flag

Every experimental flag is declared in a single table in
`src/runtime/experiments.c`:

```c
typedef struct {
    const char *name;          /* kebab-case, no leading '-' */
    const char *summary;       /* one-line, shown in --help */
    const char *plan_path;     /* docs/upcoming/... */
    const char *introduced;    /* version string, e.g. "0.25.0" */
    const char *expires_at;    /* version string, e.g. "0.28.0" */
    enum {
        XF_LIFECYCLE_PROTOTYPE,  /* core algorithm in flux */
        XF_LIFECYCLE_BETA,       /* surface stable, soak period */
    } lifecycle;
    bool *opt_global;            /* points at g_opt_<name> */
} ExperimentDescriptor;

static const ExperimentDescriptor EXPERIMENTS[] = {
    /* populated as features land */
};
```

No flag may be added without all six fields populated. `expires_at`
is a **hard contract**: when the project hits that version, the
release-cut script (`cut-minor-release` / `cut-major-release` skills)
refuses to build until the entry is either graduated (deleted from the
table; the bool stays `true` at runtime, mirroring the drop-`-X` plan's
no-op pattern) or removed entirely (bool stays `false`, the feature is
shelved).

### Lifecycles

| Lifecycle | Meaning | Diagnostic on use |
|---|---|---|
| `XF_LIFECYCLE_PROTOTYPE` | Algorithm or surface still changes between releases. | `TUR-W0060: experimental feature '<name>' (prototype) -- breaking changes likely; see <plan>` -- emitted **once per compile** on first use. |
| `XF_LIFECYCLE_BETA` | Surface frozen, soaking for one release cycle before graduation. | `TUR-W0061: experimental feature '<name>' (beta) -- graduates in <expires_at>; see <plan>` -- emitted **once per compile**. |

The warnings always fire when the flag is enabled. Suppressing them
requires `--allow-experimental` (a deliberately ugly flag), which is
intended for the project's own CI matrix runs and nothing else.

### Status command

```
tur experiments              # human-readable table
tur experiments --json       # machine-readable for the docs site
```

Lists every entry in `EXPERIMENTS[]` with its lifecycle, expiry,
plan link, and current enable state in the active `build.tur` if one
is in scope. This is the single source of truth -- guides do not
restate the list.

### Interaction with the retired `-X` surface

The drop-`-X` plan kept `-X<name>` recognized as accept-and-warn no-ops
for the 16 historical names. Those continue to warn (`TUR-W0050`); the
new mechanism does **not** reuse any of those 16 names. If a future
experiment lives in adjacent territory (e.g. a successor to
`-Xunique-types` UT2+), it picks a fresh name (`unique-affine-checks`,
not `unique-types`) so the warning history stays unambiguous.

### What does **not** become an experimental flag

- **Diagnostic strictness** (`--strict-effects`, `--lint-panic`, etc.)
  stays in the diagnostic-flag namespace. Diagnostics are not gated
  features; they tune noise on already-shipped behavior.
- **Codegen toggles** (`--emit-abi-trace`, `--dump-*`) stay where they
  are. These are operator knobs, not feature gates.
- **Build-system options** (`--build-dir`, `-I`, `--no-auto-spice`)
  stay where they are.
- **Partial-but-shipping features** that already have completion plans
  (`sized-types-completion-plan.md`, `unique-types-completion-plan.md`
  for UT2+). The drop-`-X` plan committed to "partial features ship at
  their current level"; we do not re-gate them retroactively. New
  partial features land behind `--enable=` from day one.

---

## Phases

### XF0 -- Registry skeleton

- Add `src/runtime/experiments.{h,c}` with the `ExperimentDescriptor`
  struct, an empty `EXPERIMENTS[]` table, and `experiment_enable(name)`
  / `experiment_is_enabled(name)` / `experiment_warn_if_used(name)`.
- Add `g_enabled_experiments` (a small string set) to
  `src/runtime/globals.{h,c}`.
- No CLI surface yet -- the table is empty, so this phase is purely
  the scaffolding the next phase wires up.

### XF1 -- CLI + manifest parsing

- Parse `--enable=<a,b,c>` in both scan passes in `src/main.c`. An
  unknown name in the list is an error (`TUR-E0310: unknown
  experiment '<name>'; run 'tur experiments' for the list`), not a
  warning, so typos surface immediately.
- Parse `:experiments [...]` in `pkg_manifest_read`; merge with CLI.
- Add `--allow-experimental` to suppress `TUR-W006x`. Document it as
  "intended for the Turmeric project's own CI; spice users should not
  set this."

### XF2 -- Warning emission

- Wire `experiment_warn_if_used` into the elaboration entry point for
  each gated feature (each feature owns the call). The helper itself
  handles the "once per compile" dedup using a small bitset keyed on
  the table index.
- Add `TUR-W0060` and `TUR-W0061` codes with the messages above; both
  link to the plan path stored in the descriptor.

### XF3 -- `tur experiments` subcommand

- Tabular human output (name | lifecycle | introduced | expires |
  plan).
- JSON output for the docs site to consume; the docs site's
  experimental-features index is generated from this, not maintained
  by hand.

### XF4 -- Release-cut enforcement

- Extend `cut-minor-release` and `cut-major-release` skills:
  before bumping `VERSION`, scan `EXPERIMENTS[]` and refuse to proceed
  if any entry's `expires_at` is `<=` the target version and the entry
  is still present. The release author either deletes the entry
  (graduation) or shelves the feature (separate PR) before the cut
  can continue.
- Document the enforcement in `docs/guides/release-process.md`.

### XF5 -- First real consumer

- Pick the next genuinely-experimental feature that wants a gate
  (candidate: cross-parameter sized-type unification, the one open gap
  called out in [project_sized_types_phase.md](project_sized_types_phase.md)),
  and migrate it onto `--enable=sized-cross-param` as the first
  occupant of `EXPERIMENTS[]`. This phase exists to prove the
  mechanism end-to-end on real work before the next experiment lands.

### XF6 -- Docs + guide

- Add `docs/guides/experimental-flags-guide.md` describing the
  mechanism, the two lifecycles, the warning codes, the
  `tur experiments` command, and the "if you see TUR-W006x in your
  build, here is what to do" runbook for spice users.
- Update `docs/guides/compiler-flags-guide.md`'s "Removed feature
  flags" section with a one-line cross-reference to the new guide
  ("for genuinely experimental features in v1+, see
  experimental-flags-guide.md").
- Update `CLAUDE.md` with the rule: **new in-flight compiler features
  ship behind `--enable=<name>`, with all six descriptor fields filled
  in and a plan in `docs/upcoming/`; they do not ship gateless until
  graduation.**

---

## Verification

- `tur experiments` lists exactly the entries in `EXPERIMENTS[]`; no
  drift between the table and the docs.
- `--enable=does-not-exist src/foo.tur` exits non-zero with
  `TUR-E0310`.
- `--enable=<name>` on a `XF_LIFECYCLE_PROTOTYPE` flag emits exactly
  one `TUR-W0060` per compile (regardless of how many use sites the
  feature has in the file).
- `--allow-experimental --enable=<name>` emits zero `TUR-W006x` lines.
- A synthetic table entry with `expires_at = "0.25.0"` causes
  `cut-minor-release` to refuse a 0.25.0 cut until the entry is
  removed; removing the entry lets the cut proceed.
- `bash tests/run.sh` (10-minute timeout) is unchanged in pass count;
  this mechanism does not gate any existing fixture.

---

## Risk

- **Accretion.** The original `-X` surface grew to 16 entries because
  nothing forced graduation. The mandatory `expires_at` plus the
  release-cut enforcement is the structural answer; if it works in
  practice, no flag stays in the table for more than ~2 minor
  releases.
- **Warning noise.** `TUR-W006x` firing once per compile per flag is
  the design point; if a user opts in to three experiments they see
  three warnings per file. That is intentional -- the cost should
  scale with the riskiness of the opt-in -- but if it becomes
  punishing in practice the dedup could be widened to once per
  invocation. Defer that change until we have a real user complaint.
- **Manifest/CLI merge confusion.** Two surfaces (manifest +
  `--enable=`) means a build can have its experiment set come from
  two places. The "CLI wins on conflict" rule keeps it predictable,
  but the `tur experiments` output should display *effective* state
  alongside *source* (manifest vs CLI) so users can debug a surprise.

---

## Out of scope

- Versioned experimental flags (`--enable=fancy-rows@v2`). Treat
  each iteration as a new name (`fancy-rows`, then `fancy-rows-v2`)
  rather than introducing per-flag versioning machinery.
- User-defined experimental flags inside spices. The mechanism is for
  compiler-internal experiments; spice authors who want feature-flag
  semantics in their own code use ordinary `defn` + a config value.
- Removing the `-X<name>` recognizer entirely. That is the same
  out-of-scope item the drop-`-X` plan called out and remains a
  separate, later v1.0+ decision.
