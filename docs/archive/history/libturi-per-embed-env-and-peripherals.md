# libturi: per-instance environments and the peripheral embed-API gaps that block them

> **Status:** Resolved -- Gaps 1-4 landed first (the near-term cluster that
> blocks a per-script Godot env path); Gaps 5-8 have now landed too.
> **Severity:** Medium (architectural; defers what an MVP needs but blocks a real multi-script binding)
> **Found by:** Scoping `TurmericInstance` for the Godot binding's G1 follow-up
> **Date:** 2026-06-24

## Resolution (Gaps 5-8)

The remaining medium-term / speculative gaps now have embed API:

- **Gap 5** -- `turi_env_register_native_ex(env, name, fn, ud, free_ud)` plus a
  `free_ud` field on `TuriNativeSpec`. The finalizer fires once, LIFO, from
  `turi_env_free` (never from `turi_env_reset`), so a native whose `ud` is a
  per-script object is reclaimed with the env. Finalizer list on `TuriEnv`
  (`native_finalizers`), fired in `turi_env_free`. `src/turi/env.c`, decls in
  `src/turi/eval.h`.
- **Gap 6** -- debug-only `origin_env` tag on `struct TuriClosure`
  (`#ifndef NDEBUG`), claimed on first application and asserted on every later
  one in `eval_apply_driven`; cross-env closure reuse aborts cleanly instead of
  a heap use-after-free. Compiled out entirely in release. `src/turi/eval.c`.
- **Gap 7** -- per-env `interpret_mode` bit on `TuriEnv` (defaults true),
  snapshotted into the process-global `g_interpret_mode` around each
  `turi_eval` (`turi_eval_with_sink`) and restored, plus
  `turi_env_set_interpret_mode`. This removes the cross-eval clobber between
  co-resident embedders; the deeper per-env elaborator threading stays future
  work (documented). `src/turi/env.c`, `src/turi/eval.c`.
- **Gap 8** -- `turi_env_set_shared_spice_image(env, image)` borrow hook plus a
  `spice_image_borrowed` flag so `turi_env_free` leaves a shared image alone.
  Measure-first: provides the sharing knob without copy-on-write arena
  machinery. `src/turi/env.c`, decl in `src/turi/env.h`.

Covered by the extended `tests/turi/embed-peripherals.c` (ctest target
`tur_embed_peripherals`, leak detection ON) and documented in
`docs/guides/eval-api.md` ("Per-embed-env peripherals").

## Resolution (Gaps 1-4)

The four near-term gaps now have embed API:

- **Gap 1** -- `turi_register_default_native` / `turi_clear_default_natives`
  (a process-global registry every `turi_env_new` consults) plus
  `turi_env_new_with_natives(specs, n)`. `src/turi/env.c`, decls in
  `src/turi/eval.h`.
- **Gap 2** -- `turi_env_reset(env)`: rebuilds globals keeping only native
  closures, clears accumulated source + deferred/handler/return/throw/abort/
  catch state. `src/turi/env.c` (uses `turi_value_is_native` in
  `src/turi/eval.c`).
- **Gap 3** -- `turi_env_set_diag_sink(env, cb, ud)` on top of a diag-layer
  sink (`diag_set_sink` / `diag_get_sink` in `src/compiler/diag.{h,c}`).
  `turi_eval` installs the env's sink around the call via a trampoline.
- **Gap 4** -- `turi_env_set_module_base_dir(env, path)` (owns a copy, freed by
  `turi_env_free`) plus a sentence in the `turi_eval` docblock.

Covered by `tests/turi/embed-peripherals.c` (ctest target
`tur_embed_peripherals`, leak detection ON) and documented in
`docs/guides/eval-api.md` ("Per-embed-env peripherals"). Gaps 5-8 below stay
open.

## Summary

The `turmeric-godot` G1 slice uses **one shared `TuriEnv`** across every
attached script in the project. That works for one script; with two it
breaks, because `(defn _ready ...)` from script B silently overwrites
script A's `_ready` in the shared globals map.

The obvious fix -- one `TuriEnv` per `TurmericScript` -- runs into a
cluster of smaller embed-API gaps that together make per-script envs
more expensive (in code and at runtime) than they should be. Filing
them as a cluster so they can be addressed together; individually they
are each small enough that none would justify a standalone report.

This is **not** blocking the next G1 increment (a single-script
end-to-end demo). It *is* blocking G2 onwards, where a real game
project will have dozens of attached scripts.

---

## Why one env doesn't scale

`turi_eval(env, src)` parses, elaborates, and installs every top-level
`defn`/`def` into `env->globals`. Two scripts that each export `_ready`
land at the same name. The second `turi_eval` wins; the first script's
`_ready` is unreachable. The same holds for `_process`, `_input`, any
shared helper name, and any module-private symbol that elaboration
promotes to the env.

Workaround in the binding today: pretend the problem doesn't exist
(single shared env, single script per session). Acceptable for the
G1 demo, dishonest for anything real.

The structural fix is a `TuriEnv` per `TurmericScript`. With per-script
envs, scripts can't accidentally see each other's bindings -- which is
what users will expect.

---

## The peripheral gaps

### 1. No way to install natives onto every new env  -- RESOLVED

`turi_env_register_native(env, name, fn, ud)` mutates one env. An
embedder that ships ten natives (`godot-println`, `godot-emit`, ...)
and creates 50 per-script envs has to re-register all ten on every
new env -- boilerplate, with the risk of forgetting one.

**Fix shape:** either a global "default natives" registry the
embedder seeds once and every `turi_env_new` consults, or a
`turi_env_new_with_natives(NativeSpec[], n)` constructor that takes
the table explicitly. Either keeps registration in one place.

### 2. No `turi_env_reset(env)` — `_reload` has to destroy + recreate  -- RESOLVED

When a `.tur` file changes and Godot calls `Script::_reload`, the
right behaviour is "start the script's interpreter state from
scratch, then re-eval the new source." Today the only way to do
that is `turi_env_free(env); env = turi_env_new(); /* re-register
every native */`. That's both verbose and reintroduces gap #1 on
every reload.

**Fix shape:** `void turi_env_reset(TuriEnv *env)` -- clear globals,
clear deferred state, clear handler stacks -- but keep already-
registered natives alive. Roughly "drop everything `turi_eval`
created; preserve everything the embedder configured."

### 3. Diagnostics route to global stderr, not per-env  -- RESOLVED

`turi_eval` writes parse/elaboration errors to stderr via the diag
subsystem. In Godot we want each script's compile errors to show up
in the editor's Output panel attributed to *that script*, not in a
global firehose where users can't tell which file produced which
error.

**Fix shape:** a `turi_env_set_diag_sink(env, callback, ud)` hook
that intercepts diagnostics before they hit stderr; the embedder
buffers them and pushes them to its own output channel.

### 4. `env->module_base_dir` is undocumented but load-bearing  -- RESOLVED

A script that uses `(import std/list)` needs `env->module_base_dir`
set before the `turi_eval` call, or the import resolves relative to
cwd (which for a Godot game is whatever directory the binary was
launched from -- almost never what the user means). The field is a
public struct member with one line of header doc; nothing in
`turi/eval.h` mentions it.

**Fix shape:** a `turi_env_set_module_base_dir(env, path)` setter
plus a sentence in `turi_eval`'s docblock pointing to it.
Documentation-only is acceptable; an API setter is cleaner.

### 5. Native `void *ud` lifetime vs env lifetime  -- RESOLVED

If the embedder registers a native with `ud = TurmericScript*` to
let the native call back into the script (for property routing,
signal emission, ...), and the script is destroyed before its env,
the next native invocation dereferences a dangling pointer.
Per-script envs make the lifetime *easier* to reason about (env
lifetime == script lifetime, so ud lifetime can ride along), but
nothing in the API surface tells the embedder that's the rule.

**Fix shape:** doc-only ("registered native ud must outlive the
env"), or accept an optional `(*free_ud)(void *)` callback in
`turi_env_register_native` that fires from `turi_env_free`.

### 6. Closure values pinned to their originating env  -- RESOLVED

`eval.h:21-22` notes that closures "hold internal pointers into
env-owned arenas and must not outlive env." For an embedder caching
`_ready` closure values across calls, the rule is fine -- but it
means caching across envs is forbidden, and there is no compile-time
or runtime check. A bug here would surface as a heap-use-after-free
on the first call to a stale closure.

**Fix shape:** no source-level fix needed; an ASan-guarded
"env tag" debug-only field on `TuriClosure` that the eval loop
asserts against would catch misuse early.

### 7. `g_interpret_mode` is still process-global  -- RESOLVED (cross-eval clobber; deep threading deferred)

The recent fix (libturi-embed-interpret-mode-flag) makes
`turi_env_new` flip it, which solves the embedder-versus-CLI case.
But two libturi embedders in the same process (e.g. the Godot
binding and a future MCP server) still share one flag. Today this
is fine -- both want `true`. The day someone wants compile-mode
elaboration alongside interpret-mode eval, the global will need to
become per-env.

**Fix shape:** move `g_interpret_mode` onto `TuriEnv` and have the
elaborator read it from the env pointer threaded through the elab
context. Substantially larger change than the others; flag it now,
defer the work.

### 8. Per-env memory cost: each `TuriEnv` carries its own arenas + spice image  -- RESOLVED (borrow hook; COW deferred)

`TuriEnv` allocates a fresh `sym_arena`, `value_arena`,
`globals_ht`, async scheduler state, and (if spice auto-discovery
fires) a full `TurSpiceImage`. With 50 attached scripts and a
nontrivial spice tree, that's 50 copies of the same loaded image.

**Fix shape:** an embedder-controlled "share spice image across
envs" hook, or a `turi_env_new_sharing(prototype_env, share_mask)`
constructor where mask selects which arenas to share (read-only)
versus copy-on-write versus per-env. Worth measuring before
optimizing -- might not matter in practice.

---

## Severity calibration

Items 1-4 are **near-term** -- they bite the first time a real
multi-script Godot project is built. Items 5-7 are **medium-term**
correctness/scaling concerns, not active footguns. Item 8 is
**speculative** -- worth measuring before optimizing; might never matter.
**All eight have now landed** (see the two Resolution sections above): 1-4
as the standalone API surface, 5-8 as the follow-up cluster. For 7 and 8
the landed change is the tractable, low-risk slice the report scoped (the
cross-eval mode clobber; a measure-first image-sharing hook); the heavier
work each names -- threading interpret mode through the elaborator per-env
(7), and copy-on-write arena sharing (8) -- stays deferred until a concrete
need or measurement justifies it.

With the near-term items landed, the Godot binding can ship a per-script
env path in G2 without further embed-API work; the medium-term items keep
that path honest as the project scales.

---

## Workaround in use

`turmeric-godot/src/turmeric_language.cpp` (G1) keeps one shared
`TuriEnv` on the language singleton and registers `godot-println`
on it once. `TurmericScript::_reload` evals into the shared env.
This is correct for one script; documented as a footgun in the
binding's README for anyone tempted to attach a second.

The next G1 follow-up slice (TurmericInstance + `_ready` dispatch)
**does not** address this -- it stays on the shared env and ships
the function-table plumbing for per-node instances. Per-script
envs land in G2 alongside the resource loader (see
`docs/guides/godot-resource-loader-guide.md`).

---

## Related

- [`docs/archive/libturi-embed-include-paths.md`](libturi-embed-include-paths.md) -- fixed.
- [`docs/archive/libturi-embed-interpret-mode-flag.md`](libturi-embed-interpret-mode-flag.md) -- fixed.
- [`docs/upcoming/v1/godot-language-binding-plan.md`](../upcoming/v1/godot-language-binding-plan.md) -- v1 binding plan; G2 phase is where this matters.
- [`docs/guides/godot-resource-loader-guide.md`](../guides/godot-resource-loader-guide.md) -- the other piece of G2.
