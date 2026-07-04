---
title: Experimental Feature Flags
category: CLI Tools
description: The `--enable=<name>` surface for genuinely in-flight compiler features, the two lifecycles, the TUR-W006x warnings, and the `tur experiments` command.
---

# Experimental Feature Flags

Turmeric gates genuinely **in-flight** compiler features behind a single,
uniformly-named opt-in surface: `--enable=<name>`. This is the successor to
the retired `-X<name>` regime (see
[compiler-flags-guide.md](compiler-flags-guide.md#removed-feature-flags)).
Unlike `-X`, which conflated "in development", "stable but opt-in", and
"stable and on" into one namespace until it accreted 16 entries, the
`--enable=` mechanism is *only* for features that are not yet stable, and
every flag carries a hard expiry by which it must graduate or be removed.

> **Right now the registry is empty.** No feature ships behind this
> mechanism until a concrete experiment requests a gate. `tur experiments`
> will tell you the current set; this guide describes how the mechanism
> behaves once a flag is present.

## Opting in

On the command line, pass a comma-separated list:

```sh
tur build --enable=fancy-rows,deep-refinements src/
```

Or, per-spice, in `build.tur` with a top-level `:experiments` key (names are
keywords):

```turmeric
(defpackage my-spice
  :name "my-spice"
  :version "0.1.0"
  :experiments [:fancy-rows :deep-refinements])
```

Both shapes resolve to the same internal set. When the same name appears in
both, **the CLI wins** (it is the more specific, per-invocation override).

An unknown name -- on the CLI or in the manifest -- is a **hard error**, not a
warning, so typos surface immediately:

```
error [TUR-E0310]: unknown experiment 'fancy-roows'; run 'tur experiments' for the list
```

Run `tur experiments` to see the exact set of recognized names.

## The two lifecycles

Every experiment is in one of two lifecycle states, which determines the
warning you see at the first use site:

| Lifecycle | Meaning | Warning (once per compile) |
|---|---|---|
| `prototype` | Algorithm or surface still changes between releases. | `TUR-W0060: experimental feature '<name>' (prototype) -- breaking changes likely; see <plan>` |
| `beta` | Surface frozen, soaking for one release cycle before graduation. | `TUR-W0061: experimental feature '<name>' (beta) -- graduates in <expires_at>; see <plan>` |

The warning fires **once per compile**, on first use, regardless of how many
times the feature is used in the file. Each warning links to the feature's
plan document.

The warnings always fire when the flag is enabled -- that is the design
point: the cost of the opt-in scales with its riskiness. There is no gate to
silence them; enabling an experiment (via `--enable=<name>`, `build.tur`, or
`~/.config/turmeric/experiments.tur`) is itself the acknowledgment. The
`--allow-experimental` suppression flag was retired -- passing it is now a
hard error that points you here.

## `tur experiments`

The registry is the single source of truth; this guide does not restate the
list -- the command does:

```sh
tur experiments          # human-readable table
tur experiments --json   # machine-readable (the docs site consumes this)
```

The table shows each entry's name, lifecycle, the version it was introduced
in, its expiry, whether it is enabled in the current invocation, and the
plan link. When an experiment is enabled, the table also shows where the
enable came from (`cli` or `manifest`) so a surprise is easy to debug.

## Expiry policy

No flag can sit at "experimental" indefinitely. Every entry carries an
`expires_at` version, and that is a **hard contract**: the release-cut
process (`/cut-minor-release`, `/cut-major-release`) scans the registry
before bumping `VERSION` and refuses to proceed if any entry's `expires_at`
is at or before the version being cut. At that point the release author, in
a separate reviewed PR, either:

- **graduates** the feature -- deletes the row from the registry; the feature
  becomes always-on (its enable bit stays `true`, mirroring how the retired
  `-X<name>` flags became accept-and-warn no-ops), or
- **shelves** it -- removes the row and the feature.

This structural forcing function is the answer to the `-X` regime's drift:
no experiment lives in the table for more than ~2 minor releases.

## If you see `TUR-W006x` in your build -- runbook

You are seeing a `TUR-W0060`/`TUR-W0061` because something enabled an
experiment. Walk it back:

1. **Did you pass `--enable=`?** Check your invocation and any wrapper
   scripts. Drop the flag if you did not mean to opt in.
2. **Does a `build.tur` in scope list `:experiments`?** A spice you are
   building (or one of its enclosing workspace manifests) may have opted in.
   Run `tur experiments` from that directory -- the `ENABLED` column shows
   the source.
3. **Is the warning expected?** If you genuinely want the feature, the
   warning is informational: the feature works, it is just not stable. Do
   **not** depend on its surface from a published spice yet -- a `prototype`
   feature can change shape in the next release, and a `beta` feature
   graduates (and its flag becomes a no-op) on the version named in the
   `TUR-W0061` message.
4. **The warning is informational, not suppressible.** There is no flag to
   silence it -- `--allow-experimental` was retired. If an experiment is
   enabled, the `TUR-W006x` line fires once per compile; that is intended.

## What is *not* an experimental flag

The `--enable=` surface is for in-flight **features** only. These stay where
they are:

- **Diagnostic strictness** (`--strict-effects`, `--lint-panic`, ...) -- tunes
  noise on already-shipped behavior; see
  [compiler-flags-guide.md](compiler-flags-guide.md#diagnostic-flags).
- **Codegen / operator knobs** (`--emit-abi-trace`, `--dump-*`).
- **Build-system options** (`--build-dir`, `-I`, `--no-auto-spice`).
- **Partial-but-shipping features** that already went always-on at their
  current level (e.g. sized types, uniqueness types). Those are not re-gated
  retroactively; new partial features land behind `--enable=` from day one.

## For compiler contributors

A new in-flight feature ships behind `--enable=<name>`, never gateless until
graduation. Add one row to `EXPERIMENTS[]` in
`src/runtime/experiments.c` with **all seven fields** populated -- `name`,
`summary`, `plan_path`, `introduced`, `expires_at`, `lifecycle`, and an
`opt_global` pointing at a `g_opt_<name>` bool the feature's elaboration
reads -- and a plan in `docs/upcoming/`. Call `experiment_warn_if_used(name)`
from the feature's elaboration entry point (the helper handles the
once-per-compile dedup). The CLI/manifest parsing, the `tur experiments`
listing, and the release-cut enforcement all pick the entry up automatically
from the table.

## See also

- [compiler-flags-guide.md](compiler-flags-guide.md) -- diagnostic/debug
  flags and the retired `-X<name>` set.
- [docs/upcoming/v1/experimental-flag-mechanism-plan.md](../upcoming/v1/experimental-flag-mechanism-plan.md)
  -- the design plan (XF0--XF6).
