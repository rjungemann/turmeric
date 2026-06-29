# Trowel: Toggle Between `tur` (AOT) and `turi` (Interpreter) Backends -- Plan

> **Status:** Proposed
> **Last Updated:** 2026-06-28
> **Type:** Editor / Tooling

## Terminology Note

In this plan **"turi" is colloquial shorthand for `tur --interpret`**
(the tree-walking interpreter mode already built into `tur`). It is
**not** a request for a separate `turi` binary, a new executable, or
any split of the compiler tree. There is exactly one binary today
(`tur`); this plan does not add a second one and explicitly does not
want one. Wherever the text says `turi` below, read it as
"`tur --interpret` / `tur repl` mode."

## Goal

Let a Trowel user choose, per session and per file, whether
`turmeric:run-file` (and optionally `turmeric:check-file` /
`turmeric:start-repl`) drive the **compiled** path (`tur run` / `tur
build` -> native) or the **interpreted** path (`tur --interpret` /
`tur repl`, which is the `turi` substrate today). Today's plugin
hard-wires the interpreter path: Run binds to a `:run <abs-path>`
form fed into a long-lived `tur repl` subprocess
(`tools/trowel/turmeric.lua:601`-ish, and `turmeric:run-file` at
`:769`). There is no exposed switch.

This plan is **purely an editor-side feature**. No compiler changes,
no new binaries -- `turi` already lives inside `tur` as
`tur --interpret` / `tur repl`. The toggle just picks which subcommand
Trowel spawns.

---

## Why

- **DrRacket-style fast iteration** is the default today (REPL `:run`),
  which is what an interactive workflow wants. But when the user is
  debugging codegen, validating an AOT path, or measuring real
  runtime, the interpreter is the wrong backend -- and there is no
  in-editor way to flip.
- **Codegen reproduction loop**: snapshot mismatches and inline-C
  bugs only manifest through `tur build`/`tur run`. Forcing a
  drop-to-terminal for every such check is friction.
- **REPL-vs-script parity**: the interpreter and compiled paths
  diverge in known ways (TUR_M7_HKT=0 carrier path, leak posture,
  effect handlers' behavior under TCO). Letting the editor pick
  exposes the divergence early instead of after a release cut.
- **Aligns with the existing two-mode story** in the Godot binding
  (`godot-language-binding-plan.md` already ships AOT + interpreter
  per-script). Trowel should match.

---

## Non-Goals

- **No new binary.** `turi` is not a separate executable; it is
  `tur --interpret` / `tur repl`. The plan does not introduce a
  standalone `turi`, does not split the compiler tree, and does not
  add a second build artifact. This is a hard non-goal -- if a
  future revision of this plan looks like it's drifting toward a
  separate binary, stop and revisit. The toggle is a one-binary,
  two-subcommand-shapes feature.
- **No automatic backend selection** based on file contents,
  pragmas, or build state. The toggle is explicit.
- **No mode for `lsp-lite`.** The LSP path stays on `tur` and is out
  of scope here.
- **No project-manifest plumbing.** A `build.tur` field like
  `:trowel-backend` is a follow-up; v1 of this toggle is
  user-config + UI only.
- **No hot reload of the toggle's effect on a running REPL.**
  Switching the REPL backend tears down the current REPL subprocess
  and starts a fresh one.

---

## Today's Surface (for orientation)

`tools/trowel/turmeric.lua`:

- `config.plugins.turmeric.tur` (default `"tur"`) -- path/name of the
  compiler binary (`:126`).
- `config.plugins.turmeric.repl_subcommand` (default `"repl"`) --
  what the long-lived REPL spawns (`:488`).
- `run_tur(args, label)` (`:145`) -- one-shot spawn helper used by
  `turmeric:check-file`.
- `turmeric:run-file` (`:769`) -- writes `:run <abs-path>` into the
  open REPL view; spawns one if absent.
- `turmeric:check-file` (`:203`) -- runs `run_tur({"check"}, ...)`.
- REPL subprocess: `{tur, repl_subcommand}` at `:538` (interpreter
  path today).

All three commands need a shared backend selector.

---

## Design

### Backend identifiers

Two values: `"tur"` (compiled / AOT) and `"turi"` (interpreted /
tree-walking). Keep these strings stable -- they show up in config
files, status-bar text, and command names.

| Backend | Run a file | Type-check | REPL |
|---------|------------|------------|------|
| `tur`   | `tur run <abs-path>` | `tur check <abs-path>` | `tur repl` *with codegen-backed evaluator* (new -- see Open Questions) |
| `turi`  | feed `:run <abs-path>` to a long-lived `tur repl` (today's behavior) | `tur check <abs-path>` (same -- type-check doesn't fork the two paths) | `tur repl` (today's interpreter REPL) |

`tur check` is identical in both modes -- the type checker is
upstream of the run-vs-interpret split, so the toggle only affects
**run** semantics, not **check**. Keep `turmeric:check-file`
unchanged.

### Config

Three new keys on `config.plugins.turmeric`, all user-overridable in
`init.lua` and persisted by Trowel's normal config layer:

```lua
config.plugins.turmeric.backend          = "turi"   -- "tur" | "turi", default "turi" to preserve today's behavior
config.plugins.turmeric.run_subcommand   = "run"    -- only consulted when backend == "tur"
config.plugins.turmeric.interpret_repl   = true     -- when backend == "tur", control REPL behavior:
                                                    --   true  -> REPL stays interpreter-backed even on `tur` run
                                                    --   false -> REPL also follows the backend (requires the
                                                    --            compiled-REPL substrate; see Open Questions)
```

Default `backend = "turi"` keeps the current behavior bit-for-bit;
users opt into `"tur"` explicitly.

### Commands

Add three named commands so users can rebind freely:

- `turmeric:set-backend-tur`   -- sets `backend = "tur"`
- `turmeric:set-backend-turi`  -- sets `backend = "turi"`
- `turmeric:toggle-backend`    -- flips between the two

Each command logs the new value via `core.log` and, if a REPL is
open and the change affects the REPL subprocess shape (see
`interpret_repl`), prompts to restart it.

### Status-bar indicator

Append a small segment to the existing Trowel status-bar widget
(`tools/trowel/turmeric.lua:1034` area, where the `▶ Run` / `>_ REPL`
chips live) showing the active backend: `[tur]` or `[turi]`,
clickable to invoke `turmeric:toggle-backend`. Use the same chip
styling as the existing buttons so it doesn't visually drift.

### Run-file dispatch

`turmeric:run-file` becomes a thin router:

```lua
["turmeric:run-file"] = function()
  local doc = active_doc_or_warn(); if not doc then return end
  local path = doc.abs_filename or doc.filename
  if config.plugins.turmeric.backend == "tur" then
    run_tur({ config.plugins.turmeric.run_subcommand, path }, "run")
  else
    -- existing :run <abs-path> path through the long-lived REPL
    feed_repl_run(path)
  end
end
```

The `tur run` branch reuses `run_tur` so output streams into the
same log channel `turmeric:check-file` already uses. The `turi`
branch is the existing code, lifted into a helper.

### Per-file override

A file-local override via a top-of-file comment, parsed lazily on
run:

```turmeric
;; trowel: backend = tur
```

When the active doc carries this directive, it wins over the global
setting for that one invocation. Implementation: scan the first
~40 lines on every Run dispatch (cheap; no caching needed since
files are small and Run is user-initiated).

---

## Open Questions

1. **Is there a `tur` (compiled) REPL substrate today?** The current
   `tur repl` is interpreter-backed. If we want `backend = "tur"` +
   `interpret_repl = false` to mean "compiled REPL," that requires a
   separate AOT-backed REPL mode upstream. Until that exists, force
   `interpret_repl = true` when `backend = "tur"` and surface a one-time
   notice the first time the combination is selected. (Documented as a
   v1 limitation in the user-facing guide.)
2. **Should `tur run` output stream into the REPL view or the log
   panel?** `run_tur` currently goes to `core.log`; the REPL feed
   surfaces inside the REPL split. Default to `core.log` for the
   compiled path (matches `check`) and add a future opt-in to mirror
   it into the REPL view.
3. **macOS / Linux path conventions.** `config.plugins.turmeric.tur`
   already covers an absolute path override; the toggle reuses it
   verbatim. No new path config needed unless we later split the
   binaries.
4. **`tur build` cache hygiene.** `tur run` writes into the project's
   `build/` (per `CLAUDE.md` build-output rules) -- Trowel does not
   need to manage that, but the docs should call out where artifacts
   land so users aren't surprised.

---

## Phases

### Phase T0 -- Plumbing (small)

- Lift the `:run <abs-path>` -> REPL logic in `turmeric:run-file`
  into a `feed_repl_run(path)` helper.
- Add `config.plugins.turmeric.backend` (default `"turi"`),
  `run_subcommand` (default `"run"`), `interpret_repl` (default `true`).
- Branch `turmeric:run-file` on `backend`; the `"turi"` branch is
  exactly today's behavior.
- Verify the default-config path is byte-identical to today via
  `tests/trowel/*` harness (or a new headless smoke test if one
  doesn't exist for the plugin yet).

### Phase T1 -- Commands + status bar

- Add `turmeric:set-backend-tur`, `turmeric:set-backend-turi`,
  `turmeric:toggle-backend`.
- Add the `[tur]` / `[turi]` chip to the status bar, clickable to
  toggle.
- Default keybinding suggestion (not added by default; documented):
  `cmd+shift+b` / `ctrl+shift+b` -> `turmeric:toggle-backend`.

### Phase T2 -- Per-file directive

- Parse `;; trowel: backend = (tur|turi)` from the first ~40 lines
  on every Run dispatch.
- Per-file wins over global. No persistence beyond the file itself.
- Document in `tools/trowel/README.md`.

### Phase T3 -- Documentation

- Update `tools/trowel/README.md` with the toggle, defaults,
  per-file directive, and the open-question limitations.
- Add a "Backends" section to the future
  `docs/guides/trowel-guide.md` (if that guide exists; otherwise
  inline in the README).
- Mention in the v1 release notes.

Total: small editor-side change -- realistically a single session of
focused work for Phases T0-T2 plus a docs pass.

---

## Success Criteria

- A fresh Trowel install behaves identically to today
  (`backend = "turi"`, REPL-driven Run).
- Toggling to `tur` makes `cmd+r` spawn `tur run <file>` and stream
  its stdout/stderr to the log panel, with non-zero exits surfaced
  as `core.error`.
- Status-bar chip reflects the active backend and toggles on click.
- A file with `;; trowel: backend = tur` runs through `tur run` even
  when the global setting is `turi` (and vice versa).
- `turmeric:check-file` behavior is unchanged in both modes.

---

## Risks

- **Confusion between "the REPL" and "Run a file."** Today they are
  the same subprocess; the toggle splits them when `backend = "tur"`.
  Mitigation: status-bar chip + explicit command names + a one-time
  notice the first time the user picks the compiled path.
- **`tur run` startup latency** on large projects could make `cmd+r`
  feel sluggish compared to the REPL `:run`. Mitigation: the toggle
  itself surfaces the trade-off; if it bites, the per-file directive
  lets the user stay on `turi` for files where they want fast
  reloads.
- **Drift from the Godot binding's mode naming.** The Godot plan
  uses "AOT" / "interpreter"; this plan uses "tur" / "turi" because
  those are the visible commands. Mitigation: document both spellings
  side-by-side in the guide.
