## Turmeric Godot Binding -- API Surface Expansion Plan (Tier 3)

> **Status:** Shipped (T3.A-T3.E, 2026-06-25 / 2026-06-26)
> **Last Updated:** 2026-06-26
> **Type:** Integration / Game Engine -- post-v1 follow-up to
> [godot-language-binding-plan.md](../upcoming/v1/godot-language-binding-plan.md).
> Companion to
> [godot-binding-typed-facade-plan.md](./godot-binding-typed-facade-plan.md);
> can land independently but composes cleanly with it.
>
> Landed in the `turmeric-godot` repo across five commits on `main`:
>   * `00d0426` -- T3.A ALLOWLIST 19 -> 53 (curated +34 classes)
>   * `414e1a6` -- T3.B singleton + static method generation
>   * `352f315` -- T3.C Dictionary + Array builders / mutators / typed accessors
>   * `7125bdb` -- T3.D PackedXxxArray (9 families) + RID marshalling & natives
>   * `69c063a` -- T3.E vararg dispatch via godot-call-pack + generator wrappers
>
> Out-of-scope follow-ups still open: per-Packed `-set` in place; real C-ABI
> variadic at the FFI boundary (Tier 4+); 3D-class allowlist pass; the
> long-tail editor-internal class surface.

---

## Why this exists

The G3 codegen exists and works -- the parent plan's "Generated facade
allowlist is 15 of 920 classes" known-gap captures the state. The
ALLOWLIST in `tools/gen_godot_facade.py` is 19 classes today, chosen to
cover the 2D gameplay surface that `paddle-pong-tur` actually exercised.
A real second game project bumps into the cliff almost immediately
(no `AudioStreamPlayer`, no `Tween`, no `CharacterBody2D`, no
`PackedScene`, no `Resource`...).

Three orthogonal expansion axes, each pickable independently:

1. **More classes** -- mechanical ALLOWLIST growth.
2. **More Variant kinds** -- `Dictionary` / `Array` / `PackedXxxArray`
   are not pluggable into the generator yet.
3. **Method-shape coverage** -- statics, varargs, singletons.

---

## Phases

### Phase T3.A -- Curated ALLOWLIST expansion to ~50 classes (~1 week)

Target list, derived from "what a 2D action game actually needs":

```
;; Already in (19)
AnimationPlayer Area2D CanvasItem CanvasLayer CollisionShape2D Control
Input Label Node Node2D Object PhysicsBody2D RichTextLabel RigidBody2D
SceneTree Sprite2D StaticBody2D Timer Viewport

;; Tier-3 additions (~30 more)
AudioStream AudioStreamPlayer AudioStreamPlayer2D
Camera2D CharacterBody2D
Engine Environment
FileAccess FileSystemDock
GPUParticles2D
Image ImageTexture InputEvent InputEventKey InputEventMouseButton
Light2D
MeshInstance2D
NavigationAgent2D NavigationRegion2D
OS PackedScene PhysicsServer2D
RandomNumberGenerator Resource ResourceLoader
Shader ShaderMaterial Skeleton2D
TileMap TileMapLayer Texture2D Tween
Window WorldBoundaryShape2D
```

This isn't 920 -- most of the rest is editor-internal nodes, audio bus
plumbing, physics server internals, 3D-only classes, and singletons the
average game doesn't touch directly. Curating to ~50-75 captures the
practical game surface; the long tail can grow as demand surfaces.

`gen_godot_facade.py`'s ALLOWLIST gains the new entries; nothing else
changes structurally. Re-run, commit, done.

### Phase T3.B -- Singleton + static method generation (~3-5 days)

Generator skips static methods today ("statics need a singleton, not an
instance handle"). Real games hit `Input::is_action_pressed`,
`OS::get_ticks_msec`, `RandomNumberGenerator::randf`,
`Engine::get_frames_per_second` constantly.

Emit a per-singleton namespace:

```turmeric
;; Generated for each class whose JSON marks it `is_singleton: true`.
(defn input/is-action-pressed [action : :cstr] : :bool
  (godot-call-b (godot-singleton "Input") "is_action_pressed" action))

(defn os/get-ticks-msec [] : :int
  (godot-call (godot-singleton "OS") "get_ticks_msec"))
```

For non-singleton statics (`ClassDB::class_exists`), the same pattern
works via `godot-singleton "ClassDB"`.

The curated prelude already has `(input/is-action-pressed ...)` hand-
written; generated code skips that name to avoid the conflict (same
mechanism as the Node prelude conflict skip).

### Phase T3.C -- Dictionary + Array Variant marshalling (~1-2 weeks)

The biggest missing Variant kinds. Today calling any Godot method that
returns a `Dictionary` or `Array` either:

- forces you down to `(godot-call ...)` with a `:int` arena handle, OR
- works for primitives in the container by accident.

Two changes needed:

1. **Runtime**: marshal `Dictionary` / `Array` Variants through the
   arena (same model as Vector2 et al.), returning handles. Add
   `godot-dict-*` / `godot-array-*` builder + accessor natives that
   speak through the handle.
2. **Generator**: map JSON `Dictionary` -> `:DictHandle`, `Array` ->
   `:ArrayHandle` (with the Tier 2 typed-facade rewrite).

User surface:

```turmeric
(let [d (animation/get-animation-list anim-player)]   ; d : ArrayHandle
  (for [i (range (godot-array-size d))]
    (godot-println (godot-array-get-string d i))))
```

`Dictionary` is the painful one for real games (every save/load
pipeline, every JSON API, every signal payload with named fields).
Worth front-loading even if `Array` slips.

### Phase T3.D -- PackedXxxArray + NodePath + RID (~1 week)

The eight `PackedXxxArray` types (`PackedByteArray`, `PackedInt32Array`,
`PackedFloat32Array`, `PackedStringArray`, `PackedVector2Array`,
`PackedVector3Array`, `PackedColorArray`, `PackedInt64Array`) are how
Godot moves bulk data: tilemap cells, vertex buffers, audio samples.
Same arena model as `Array`, but each has a concrete element type the
runtime can fast-path (no per-element Variant boxing).

`NodePath` and `RID` are smaller surface but show up everywhere:

- `NodePath` -> string + arena handle for path resolution.
- `RID` -> opaque server-side resource identifier.

Per-type natives:

```turmeric
(godot-packed-vec2-array-size handle)         : :int
(godot-packed-vec2-array-get  handle i)       : Vec2Handle
(godot-packed-vec2-array-set! handle i v)     : :void
;; ...
```

### Phase T3.E -- Vararg dispatch surface (~1 week)

Generator skips vararg methods today. `Node::call_deferred(method, ...args)`,
`Object::callv(method, args)`, and the entire `Object::emit_signal(name, ...args)`
family are vararg. Workarounds (`emit-signal` in the prelude) cover the
hottest paths but the generic surface is missing.

Two paths:

1. **Build an Array arena handle and pass it through callv.** Simpler.
   Costs a heap allocation per call.
2. **Real variadic at the C ABI.** Requires the typed-variadic Turmeric
   feature (already shipping per CLAUDE.md `& rest :type` notes) to
   land at the FFI boundary too.

Recommended: ship path 1 as the v1.x default ("works, slightly slow"),
plan path 2 as a Tier 4+ optimisation once it matters.

---

## Risks

- **`extension_api.json` schema drift.** Godot 4.x has been adding
  fields and changing JSON shape across minor releases. The generator
  is pinned to a specific 4.3 schema; bumping to 4.5+ may need
  generator adjustments. Mitigation: snapshot the JSON we generate
  against; flag schema mismatch on regeneration.
- **Method-name collisions with the curated prelude.** The prelude is
  growing; the skip-list in `gen_godot_facade.py` needs to track it.
  Easy to miss; add an assertion that the curated set and generated
  set don't overlap.
- **Generated facade file size.** 50 classes x avg ~30 methods = ~1500
  defns. The current single-file model already hit 2197 lines for 19
  classes; expect 6000-8000. Compile time of the generated C++ string
  literal is the bottleneck. Mitigation: split into per-class files,
  or compress runtime-side (the Turmeric source doesn't need to be a
  human-readable C string literal).

---

## Success Criteria

- ALLOWLIST at ~50 curated classes; generator regenerates clean.
- `(input/is-action-pressed "ui_accept")` works without curated-prelude
  hand-wiring.
- A user can read a `Dictionary` returned from any Godot API.
- `PackedByteArray` and `PackedInt32Array` round-trip cleanly (tilemap
  read + write use case).
- `(call-deferred self "move-and-slide")` works via the Array-arena
  vararg path.

---

## Out of Scope

- Auto-completion / hover docs derived from JSON. The data exists in
  `extension_api.json`; surfacing it through the existing
  `complete_code` extension hook is its own work and probably belongs
  with editor-niceties expansion (not in any current tier).
- Engine-internal classes (everything under `EditorPlugin`, the entire
  GDScript-internals surface, audio bus servers). Out of scope of
  "what a game needs".
- 3D-specific classes (`Node3D`, `MeshInstance3D`, ...). The current
  ALLOWLIST is 2D-leaning; a 3D pass is a natural follow-up once a 3D
  demo exists to drive priorities.
