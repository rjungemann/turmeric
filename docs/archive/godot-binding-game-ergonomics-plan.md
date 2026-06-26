## Turmeric Godot Binding -- Game Ergonomics Plan (Tier 4)

> **Status:** Shipped -- starter slices for T4.A/T4.B, full impls for
> T4.C/T4.D (2026-06-26). The two heavy substrate pieces (T4.A approach 2
> direct cross-script dispatch + T4.B in-flight method safety / schema
> migration) stay on the follow-ups list below; the recommended-scope
> starters cover the friction the plan was written to address.
> **Last Updated:** 2026-06-26
> **Type:** Integration / Game Engine -- post-v1 follow-up to
> [godot-language-binding-plan.md](../upcoming/v1/godot-language-binding-plan.md).
>
> Landed in the `turmeric-godot` repo on `main`:
>   * `5b48105` -- T4.A starter: (cross-call ...) / (cross-call-pack ...)
>     prelude helpers naming the Callable fallback
>   * `351d96f` -- T4.B starter: cb_get_property_state wired so Godot's
>     capture/replay dance preserves @export inspector values across reload
>   * `d1066a4` -- T4.C: typed (cls/emit-<sname>) wrappers for all 102 signals
>     (paired with the existing (cls/on-<sname>) connect side)
>   * `9a58244` -- T4.D: (preload "res://...") native + ResourceHandle wrapper
>     with load-time path validation and process-lifetime caching
>
> Companion doc edit in `turmeric` `main`:
>   * `286fa37f9` -- godot-binding-guide.md cross-script-calls section,
>     "Shipped since the original v1 plan" notes, allowlist count fix
>
> Follow-ups still open (deferred per the recommended scope):
>   * T4.A approach 2 -- per-script build-graph + RTLD_GLOBAL exports for
>     direct cross-script symbol dispatch (the 3-4 week piece).
>   * T4.B in-flight method safety + dropped-property logging on schema
>     change + structured property-schema migration.

---

## Why this exists

Tier 2 (typed facade) and Tier 3 (API surface) make individual scripts
better. This plan addresses two language-level walls a real game project
hits almost immediately, both explicitly listed as non-goals in the v1
parent plan:

1. **Cross-script calls.** Player.tur and Enemy.tur each compile to
   their own AOT `.so`. Today there is no symbol-resolution path
   between them; if Player wants to call into Enemy, it has to go
   through Godot's signal / Callable system, paying the same Variant
   marshalling overhead that's already the dominant per-call cost.
2. **Editor-time hot reload for stopped scripts.** v1 says "Edit -> Stop
   -> Play is fine"; in practice, that means closing and reopening the
   running game per source edit. Real iteration loops -- in Godot's
   own scripting languages too -- reload stopped scripts in place. The
   AOT cache invalidation is already source-byte-keyed, so the cost of
   wiring this is mostly engine-side integration, not new
   substrate.

Order matters: cross-script calls are the higher-leverage piece (a
project of more than 1-2 scripts hits it; iteration speed is a comfort
issue, real but smaller).

---

## Phases

### Phase T4.A -- Cross-script symbol resolution (~3-4 weeks)

The constraint: each `.tur` script today is built as an isolated
shared library under `.godot/turmeric-cache/<hash>/`. dlopen'ing one
library does not put another library's symbols into reach.

Three feasible approaches:

1. **Route everything through Godot Callable.** What GDScript does:
   `(node/call other-node "do-thing" args...)` runs through Godot's
   signal machinery. Slow, but works today with zero new substrate.
   Document as the v1.x default; let users opt into faster paths.
2. **Explicit cross-script imports.** A script declares
   `(godot-import "res://other.tur" :refer [do-thing])`. The AOT
   builder resolves the path, ensures the dependency is built first,
   and emits a manifest entry that the loader links. Same model as
   spices.
3. **One big library per project.** Aggregate every `.tur` in the
   project into a single `tur build --shared` invocation. Cross-script
   calls become same-library calls; the AOT cache invalidates if *any*
   script changed. Simple but punishes incremental builds.

Recommended: **ship (1) immediately as the always-available fallback,
build (2) as the opt-in fast path**. Approach (3) is tempting but the
"rebuild everything per edit" cost is steep enough that real games
would split projects to avoid it.

Substrate for approach (2):

- Each per-script `tur build --shared` discovers `(godot-import ...)`
  forms and emits a build-order dependency edge.
- The cache layout grows a `<project>/.godot/turmeric-cache/_build-graph.tsv`
  that records (importer-hash, importee-hash) edges so reloads can
  invalidate downstream when an upstream changes.
- The runtime loads images in dependency order; a script's dlopen
  carries `RTLD_GLOBAL` for symbols it exports to importers, while
  `RTLD_LOCAL` is preserved for everything else.
- Type-checking across scripts uses the existing module resolution
  path (the script becomes a synthesised module the importer's
  elaborator can lookup against).

### Phase T4.B -- Editor-time hot reload for stopped scripts (~2-3 weeks)

Today: edit a `.tur` file, save, the editor calls `Script::_reload`,
the slow path (or fast-path) rebuilds the AOT image and dlopens. Per-
instance state is reset.

The friction is per-instance state -- `@export` properties get reset to
their defaults on every reload, even though the inspector value was
edited by the user. GDScript preserves these via the `p_keep_state`
parameter of `_reload`; turmeric-godot's `_reload` ignores it today.

What "hot reload" means here:

- Capture per-instance `property_values` before tearing down the old
  AotImage.
- Reload + rebuild + dlopen as today.
- Replay captured `property_values` onto the new instance.
- Any methods currently in flight: not safe to reload; defer until the
  current Godot frame completes. (Editor reload only fires outside
  Play, so "currently in flight" is normally empty anyway.)

The "running scenes" hot reload (in-Play, hot-patch a script while a
game is running) stays out of scope -- that's the v1 non-goal and
requires substrate that doesn't exist (function-replacement in
dlopen'd code is platform-specific and gnarly).

### Phase T4.C -- Signal-emit ergonomics (~1 week)

Today: `(emit-signal self :hit damage)` works for the bare signal
case. With multiple args + typed payloads, the surface is awkward.
With Tier 2 typed signals (`(area2d/connect-body-entered ...)`),
there's a matching gap on the emit side:

```turmeric
;; Generated alongside Tier 2's typed connect.
(defn area2d/emit-body-entered [self : Area2D body : Node2D] : :void
  (godot-emit-signal self "body_entered" body))
```

Cheap; lives in the same generator pass as Tier 2 typed signals.

### Phase T4.D -- Resource preloading (~1 week)

GDScript has `preload(...)` -- a compile-time resource load that errors
at parse time if the resource is missing, rather than at runtime when
the load call fires. The Turmeric equivalent would be a `(preload
"res://path.tscn") : PackedScene` form that:

- Verifies the path exists at script-compile time (file-system check
  during `_reload`).
- Caches the load through the existing resource loader.
- Returns a typed handle the user can pass to `instantiate` / etc.

Small but real: misnamed asset paths today fail silently or crash at
gameplay time.

---

## Risks

- **Cross-script ABI versioning.** If script A is built against script
  B's old signature, then B changes its exported signature, A's
  manifest expectations stop matching B's symbols. Mitigation: include
  B's source hash in A's import record; force A to rebuild when B's
  hash changes. The existing AOT hash already keys on source bytes;
  extending to dependency hashes is a natural step.
- **Hot reload + per-instance state shapes.** If a script edit changes
  the shape of `property_values` (renames an export, changes a type),
  replaying the captured state mid-edit either fails the replay or
  silently silently keeps the old slot. Mitigation: drop captured
  state slots whose name+type doesn't match the new export decl; log
  a clear "[turmeric ... reload] dropped stale property `X`" line.
- **Multi-script cache invalidation storm.** Approach (2) in T4.A means
  changing a "foundation" script (player utilities, base classes)
  cascades rebuilds across every importer. For a 50-script project,
  that's a slow edit. Mitigation: parallel build of independent
  subgraphs; serial only within a dependency chain.

---

## Success Criteria

- A script can `(godot-import "res://shared/utils.tur" :refer [...])`
  and call directly into the imported script's defns with no Variant
  marshalling on the call path.
- Hot reload of a stopped script preserves inspector-edited `@export`
  values across the reload.
- A `(preload "res://nonexistent.tscn")` produces a compile-time error
  citing the missing path, not a runtime crash.

---

## Out of Scope

- Running-scene hot patching (replace a function while gameplay is
  live). Not v1.x; needs dlopen-replace substrate that's a separate
  research effort.
- Per-instance state migration through script schema changes (rename a
  property AND keep the value). The dropped-with-log behaviour above
  is the v1.x answer; structured migration is later.
- Project-wide whole-build mode (T4.A approach 3). Documented as a
  considered alternative; not the recommended path.
