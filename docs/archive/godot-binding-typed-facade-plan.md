## Turmeric Godot Binding -- Typed Facade Plan (Tier 2)

> **Status:** Draft Plan
> **Last Updated:** 2026-06-25
> **Type:** Integration / Game Engine -- post-v1 follow-up to
> [godot-language-binding-plan.md](./v1/godot-language-binding-plan.md).
> Identified there as Out-of-Scope item "Typed Godot facade -- worth a
> follow-up plan after v1 ships."

---

## Why this exists

The v1 facade is structurally untyped. Every Godot Object handle, every
arena handle (Vector2, Vector3, Color, Rect2, ...), every Variant
return value is `:int` to the Turmeric elaborator. That was the v1
compromise documented in the parent plan; the compiler block that
forced it -- "untyped native registration blocks curated facades" --
landed in
[`docs/archive/untyped-native-registration-blocks-curated-facades.md`](../archive/untyped-native-registration-blocks-curated-facades.md).
The codegen just hasn't been rewired to consume the new typed-native
registration API.

The user-visible cost today:

```turmeric
;; What we have: opaque ints. Elaborator can't catch this.
(node/set-position (some-other-node-handle) (input/is-action-pressed "fire"))
;;                  ^ wrong arity / wrong handle kind / wrong-shape arg --
;;                  all type-check, all explode at runtime.
```

```turmeric
;; What we want: real types. TUR-E0001 at compile time.
(node/set-position my-node fire-down?)
;;                  ^ :Node                      ^ :bool, expected :Vec2Handle
```

This is the biggest single ergonomic / correctness gap between Turmeric
and GDScript inside Godot today. It's also the change that meaningfully
demonstrates *why* you'd write a game in Turmeric vs GDScript -- a
typed lisp without typed engine APIs is a typed lisp with a giant
`:int` puddle in the middle.

---

## Phases

### Phase T2.A -- defopaque newtypes per Godot class -- DONE (narrowed)

**Status (2026-06-25):** landed in a narrower form than originally
drafted. The investigation that opened this phase surfaced that G6
already shipped per-class defopaque handles (`<Class>Handle`) for every
ALLOWLIST entry, the Castable-style up-coercion helpers
(`<class>-handle-><parent>-handle`), and typed method signatures
through the generator. See the G6 archive at
[`docs/archive/v1/godot-binding-typing-followups-plan.md`](../archive/v1/godot-binding-typing-followups-plan.md).

The genuine remaining gap from G6's "Deferred" list was the arena
Variant types -- `Vector2 / Vector3 / Color / Rect2 / Transform2D /
Transform3D / Array / Dictionary` still flowed as bare `:int` through
the generated facade. That's what landed in this session:

- `bridge/prelude.cpp` -- added `Transform2DHandle`,
  `Transform3DHandle`, `ArrayHandle`, `DictHandle` defopaques
  alongside the existing `Vec2Handle / Vec3Handle / ColorHandle /
  Rect2Handle`. Builders (`node/vec2`, `node/color`, ...) now return
  the Handle newtype via ascription; accessors and prelude facade
  wrappers (`node/set-position`, `node/set-modulate`, ...) take Handle
  types with `(:: x :int)` demotion at the `godot-call` boundary.
- `tools/gen_godot_facade.py` -- new `ARENA_TYPES` map that
  `param_type` and the return-type branch both consult. JSON
  `Vector2`/`Vector3`/`Color`/`Rect2`/`Transform2D`/`Transform3D`/
  `Array`/`Dictionary` now generate `Vec2Handle`/etc. signatures with
  the same demote-at-boundary pattern as the existing ALLOWLIST
  class handling.
- `bridge/generated_facade.cpp` -- regenerated; 214 new arena-handle
  occurrences across the 19 classes / 980 methods.

What did NOT change: per-class defopaques (G6 owns those), up-coercion
helpers, typed signal connect (T2.D scope, also already G6-shipped).
The remaining genuine work in this plan is T2.B (curated prelude
audit), T2.C (Castable typeclass over the existing up-coercion helpers),
and T2.E (inspector property-type expansion). T2.D is closed by G6.1.

### Phase T2.B -- Generator rewrites with real types -- DONE

**Status (2026-06-25):** the generator-side rewrite already shipped
under G6.2 (per-class handles) and T2.A (arena handles). T2.B's
remaining work was the curated prelude audit. Landed:

- `bridge/prelude.cpp` -- tightened `node/get-node` to return
  `:NodeHandle`, `get-tree` to return `:SceneTreeHandle`,
  `tree/get-root` to return `:NodeHandle`. Internal callers of
  `get-tree` (`tree/quit`, `tree/change-scene-to-file`,
  `timer/one-shot`, `after`) demote with `(:: (get-tree) :int)` at the
  `godot-call` boundary; user-facing returns stay typed.

Leaves the loose-typed entries that compose with arbitrary Object
handles (`singleton`, `engine`, `os`, `time`, `node/self`) as `:int`
on purpose: their callers ascribe to the specific Handle they want.

### Phase T2.C -- Class hierarchy + downcast -- DONE (narrowed)

**Status (2026-06-25):** landed in a narrower form. Upcast is already
covered by G6.2's per-pair `<class>-handle-><parent>-handle` defns;
elevating those to a multi-param `Castable [a b]` typeclass with `: b`
returns is not supported by the current elaborator (multi-param
typeclasses' methods can only return concrete types like `:int`, per
the `From [a b] : int` precedent in `stdlib/typeclass.tur`). The
generator-emitted helpers stay as the supported upcast surface;
defopaque ascription `(:: child :ParentHandle)` is also still
available as the bare metal.

Downcast was the genuine gap and landed:

- `bridge/prelude.cpp` -- added `is-class?` curated wrapper over
  `Object::is_class` via the typed `godot-call-b` path.
- `tools/gen_godot_facade.py` + regenerated facade -- one
  `(try-as-<class> h : int) : <Class>Handle` per ALLOWLIST entry.
  Returns the ascribed handle on `is-class?` hit, or `(:: 0 :<Class>Handle)`
  as the wrapped-null sentinel on miss. Caller checks via
  `(= (:: result :int) 0)` -- same null-sentinel pattern as
  `node/get-node`.

Deliberately not implemented: `option<T>`-returning downcasts. The
wrapped-null sentinel avoids the elaborator complexity of resolving
`option<TargetHandle>` per call while still surfacing "wrong class"
without UB.

Implicit upcast at call sites (the original option 2) remains
deferred to Tier 5+ ergonomic work.

### Phase T2.D -- Typed signal connect -- DONE under G6.1

**Status (2026-06-25):** closed by G6.1 before this plan was drafted.
See [G6 archive](../archive/v1/godot-binding-typing-followups-plan.md):

> G6.1 -- `godot-connect-typed` native + 59 generated
> `(class/on-<signal> ...)` wrappers with `(fn [argT...] void)` handler
> parameter typing.

Nothing further needed under this plan; left in place as a directory
of what already exists.

### Phase T2.E -- Inspector property type expansion -- DONE (narrowed)

**Status (2026-06-25):** landed. The C++-side `tg_turi_to_variant_typed`
already routed VECTOR2/VECTOR3/COLOR/RECT2/OBJECT through the variant
arena from earlier work; what was missing was the type-string parser
accepting the new `<Type>Handle` names the `defgodot-export` macro
stringifies.

`turmeric_language.cpp::tg_parse_export_type` now also accepts:

- `Vec2Handle` / `Vec3Handle` / `ColorHandle` / `Rect2Handle` -- direct
  aliases to the existing `vec2/vec3/color/rect2` shortcuts.
- Any `<X>Handle` suffix (NodeHandle / Node2DHandle / Sprite2DHandle /
  ...) -> `Variant::OBJECT`. Class handles all show up as a typed
  Object slot in the inspector; that's the right Variant kind for
  Godot's existing OBJECT-typed property display.

```turmeric
(defgodot-script Player :extends Node2D
  :exports ((speed : float       200.0)
            (start : Vec2Handle  (node/vec2 0.0 0.0))
            (tint  : ColorHandle (node/color 1.0 0.5 0.0 1.0))
            (target : NodeHandle nil)))
```

Deferred: `Transform2DHandle / Transform3DHandle / ArrayHandle /
DictHandle` -- those need arena builders (no `(godot-transform2d ...)`
native exists yet) before they can be used as inspector defaults.
The parser explicitly excludes them from the `Handle`-suffix fallback
so the misuse "user picked TransformHandle but there's no default
constructor" surfaces as a clean `unsupported type` printerr, not a
silent OBJECT-shaped accidental binding.

---

## Risks

- **defopaque cost at runtime.** They wrap `:int` and the compiler
  generally erases the wrapper, but any spot that still pattern-matches
  on `TY_INT` for facade values needs auditing. Spot-check the existing
  `tur-signal` / `paddle-pong-tur` paths early.
- **Generated source size.** A defopaque + Castable instances for ~50
  classes plus per-method typed wrappers is significantly larger
  generated code than today. Bake-time risk if it lands as a single
  blob in `bridge/generated_facade.cpp`. Mitigation: split per-class
  files included by topic.
- **Manifest schema for AOT.** The AOT path's `exports.manifest` tags
  every typed arg as `:int` (since the codegen erased). With defopaque,
  the manifest could carry richer info, but the `tur_ffi_thunk_call`
  shape encoding still only needs the i/f/v classes -- so the manifest
  format does NOT need to change for AOT. Worth a comment in the AOT
  archive to that effect.
- **Backward compat for paddle-pong-tur.** The demo script uses bare
  `:int` for node handles today. Either migrate it as part of T2.B's
  generator rollout, or keep an untyped-pass-through escape hatch
  during transition.

---

## Success Criteria

- A user attempting to call `(node/set-position area2d-handle :float)`
  gets a TUR-E0001 at compile time, not a runtime crash.
- All ALLOWLIST classes have defopaque types; all generated methods
  use them at arg + return positions.
- `paddle-pong-tur` (or a successor demo) compiles + runs end-to-end
  using only typed handles.
- `(node/queue-free some-node2d)` works -- the Castable hierarchy
  resolves `Node2D` widening to `Node` at compile time.

---

## Out of Scope

- Implicit upcast (option 2 from T2.C). Tracked as future ergonomic.
- Typed signal *emit* (mirror of T2.D). Same shape, different
  direction; lands in the same codegen pass if time permits.
- Refinement types ("Node that's actually in the scene tree"). Tracked
  in [refinement-types-plan.md](./refinement-types-plan.md).
- Compile-time validation of node paths (`(get-node "../Player")`).
  Needs scene-graph introspection at build time; separate work.
