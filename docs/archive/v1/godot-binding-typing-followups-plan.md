# Godot Binding -- Typing Follow-Ups Plan

> **Status:** Resolved (2026-06-25)
> **Type:** Integration / Game Engine -- Phase G6 of the
> [godot binding plan](../../upcoming/v1/godot-language-binding-plan.md)
> **Companion to:** [godot-binding-guide.md](../../guides/godot-binding-guide.md)

## Resolution Notes

Landed in `../turmeric-godot/` on 2026-06-25:

- **G6.1** -- `godot-connect-typed` native + 59 generated
  `(class/on-<signal> ...)` wrappers with `(fn [argT...] void)` handler
  parameter typing. Closures bind into the script env under a
  synthesized symbol; Godot's Callable targets that symbol so dispatch
  routes through existing `cb_call`. Demos:
  `examples/spike/scripts/typed_signal{,_bad}.{tur,gd}`.
- **G6.2** -- `(defopaque <C>Handle :int)` emitted per allow-listed
  class plus an up-coercion helper to the nearest allow-listed
  ancestor. Generated method wrappers now use `<C>Handle` for self,
  for arguments whose JSON type names an allow-listed class, and for
  Object-typed returns; bodies ascribe back to `:int` at the
  `godot-call` boundary. Demo:
  `examples/spike/scripts/handle_hierarchy.{tur,gd}`.
- **G6.3** -- Curated one-shot prelude entries: `(get-tree)`,
  `(tree/quit)`, `(tree/get-root)`, `(tree/change-scene-to-file)`,
  `(timer/one-shot)`, `(engine)` + accessors, `(os)` +
  `(os/get-system-time-msecs)`, `(log/info)`, `(log/error)`, `(after)`.
  Demo: `examples/spike/scripts/oneshot_prelude.{tur,gd}`.

### Deferred

- Arena types (Vector2/3, Color, Rect2, Transform2D/3D, Array,
  Dictionary) still flow as bare `:int` through the generated facade.
  Promoting them needs typed-native re-registration or a prelude
  wrap layer.
- Down-coercion stays explicit + unchecked (`(:: h :Sprite2DHandle)`).
  A runtime-checked `(checked-cast h :Sprite2DHandle) -> option<...>`
  via `Object::is_class` is left for later.
- Headless `.gd` drivers were authored but not run in this session
  (no Godot CLI in the harness). The C++ build is clean and the
  codegen produces the expected facade; end-to-end verification
  falls to the next session that can launch Godot.

---

## Overview

G3-G5 of the binding plan landed at MVP. Scripts attach to nodes,
dispatch lifecycle hooks, marshal Variants, emit + subscribe to
signals, and run a paddle-pong demo end-to-end. The remaining
type-system surface is what'll separate v1 from a real
production-grade scripting story.

This plan covers three follow-ups, in priority order:

1. **Typed signal-connect wrappers** -- generated from each class's
   `signals` block in `extension_api.json`. Smallest, immediate win:
   the compiler catches handler/signal mismatches before runtime.
2. **Class-hierarchy `defopaque` handles** -- generated per Godot
   class, with subclass-to-superclass coercions. Real type safety
   over the engine's class hierarchy at the cost of explicit
   ascriptions at handle boundaries.
3. **Curated one-shot pattern wrappers** -- prelude entries for
   things every script reaches for (`(get-tree)`, `(tree/quit)`,
   `(timer/start ...)`, `(scene-tree/change-scene-to-file ...)`).
   Pure ergonomics; complements both typing changes above.

The three are independent and can land in any order, but #1 should
go first because it pays off without a user-visible change to script
ergonomics. #2 is the bigger semantic shift. #3 piggybacks on #2.

Each phase below is sized for one MVP commit (a few days each), not
a multi-week milestone.

---

## Goals / Non-Goals

### Goals (v1)

- Compile-time rejection of signal handler / signal-args arity or
  type mismatches.
- A `defopaque` handle per allow-listed Godot class, generated from
  `extension_api.json`, with coercions following the JSON's
  `inherits` chain.
- A prelude entry for the ~10 single-line patterns that show up in
  every script (no manual `(godot-call (singleton "Engine") ...)`).

### Non-Goals (v1)

- **Per-method typed C natives** -- the brute-force option. ~30k
  methods, no clear win over the wrapper-level type annotation
  Codegen v2 already does.
- **Linear / affine handle types** for Ref<T> lifetime. Variant's
  refcount + arena lifetimes cover us today; the right time to
  revisit is when scripts start managing long-lived Resource refs
  outside the per-call arena.
- **Class-hierarchy methods inherited up the chain.** The codegen
  emits per-class wrappers today; a Sprite2D method appears as
  `sprite2d/...` and not also as `node2d/...`. Adding the inherited
  emissions is a separate concern from the type-handle work.

---

## Phase G6.1 -- Typed Signal-Connect Wrappers

### Surface

For each class C and each signal `S(args...)` declared on C,
generate:

```turmeric
(defn classname/on-S [self : int handler : (fn [arg1-types...] void)]
  (godot-connect-typed self "S" handler))
```

The wrapper takes a Turmeric function value -- not a string -- so the
elaborator type-checks the handler's signature against the signal's
declared args.

Example:

```turmeric
;; today (G5):
(godot-connect timer "timeout" "on-tick")

;; G6.1 -- handler is a fn value, arity / types checked:
(timer/on-timeout timer
  (fn [] (godot-println "tick")))
```

A wrong-arity or wrong-type handler is `TUR-E0001` at the call site
instead of "handler did nothing / Variant printerr" at runtime.

### Mechanism

- `godot-connect-typed` (new native, registered `TUR_NRT_VOID`):
  takes (OBJ, SIGNAL-NAME, fn-value). Stores the closure on the
  current instance (TurmericInstance grows a `signal_handlers`
  vector keyed by signal name) and binds a synthesized method name
  to it -- the existing dispatcher in `cb_call` already looks up
  methods by name in the TuriEnv, so we register the closure as a
  callable under a generated stub name.
- Codegen reads `class.signals` from `extension_api.json`; emits one
  `classname/on-SIGNAL` wrapper per (class, signal) pair.
- Arg-type mapping reuses Codegen v2's `param_type` table: scalar
  args (`int`/`float`/`bool`/`String`) get their proper Turmeric
  types; aggregate / Object args become `:int` (arena or Object
  handles), same as the rest of the generated facade.

### Demo

`examples/spike/scripts/typed_signal.{tur,gd}` -- attach to a Node
with a Timer sibling, call `(timer/on-timeout (fn [] ...))`, the
sibling fires `timeout`, the closure runs, the driver asserts a
flag was set. A second test attempts to pass a wrong-arity closure
and confirms the script *fails to load* (elaboration error), not
that the signal silently no-ops.

### Estimate

~3 days. Mechanism is mostly codegen + one new C native; the
closure-storage path on TurmericInstance is the load-bearing piece.

### Risk

`extension_api.json`'s signal definitions are sparse -- some
classes inherit signals from base classes and the JSON only records
them at the originating level. Either generate the wrapper at every
class in the inheritance chain that *could* receive it (duplicates),
or only at the declaring class (forces users to up-cast). Resolve
during Phase G6.2 (when the handle hierarchy lands, the right thing
is "wrappers at the declaring class only; users coerce up").

---

## Phase G6.2 -- Class-Hierarchy `defopaque` Handles

### Surface

For each allow-listed class C with parents P1, P2, ... up to Object:

```turmeric
(defopaque CHandle :int)
(defn c-handle->parent [h : CHandle] : ParentHandle (:: h :ParentHandle))
;; reverse: parent->c is unchecked (downcast), explicit:
(defn parent-handle->c [h : ParentHandle] : CHandle (:: h :CHandle))
```

Generated facade signatures upgrade from bare `:int` to the right
handle type:

```turmeric
;; today (G5):
(defn node2d/set-position [self : int pos : int] ...)

;; G6.2:
(defn node2d/set-position [self : Node2DHandle pos : Vec2Handle] ...)
```

`godot-self` returns a `NodeHandle` (the lowest common ancestor of
attach-time types); user scripts coerce to the specific class with
the generated `nodehandle->node2dhandle` etc.

### Mechanism

- Walk every allow-listed class in `extension_api.json`; emit
  `(defopaque <Class>Handle :int)` and the up-coercion chain.
- Arena types (Vector2 / Vector3 / Color / Rect2 / Transform2D /
  Transform3D / Array / Dictionary) get their own `defopaque` family
  -- `Vec2Handle`, `ColorHandle`, etc. -- already prototyped in the
  prelude post-typed-natives. Promote them out of the prelude into
  the generated facade so they're authoritative.
- Update `godot-self` to return `NodeHandle` (not bare `:int`) by
  registering it `TUR_NRT_PTR` with a per-call ascription. The
  underlying value is still an `:int`; the ascription is purely
  type-system.
- `godot-call` stays variadic and untyped at its core. The generated
  wrappers gain the handle-typed signatures.

### Demo

`examples/spike/scripts/handle_hierarchy.{tur,gd}` -- script that
takes a `(self : Node2DHandle)`, calls a NodeHandle-typed method via
explicit `(:: self :NodeHandle)`, and a Sprite2D-only method
directly. A second pass attempts to pass a NodeHandle where a
Sprite2DHandle is required and confirms the script fails to load.

### Estimate

~4 days. Codegen is mechanical (walk `inherits`, emit ascription
helpers); the load-bearing surface is updating the *existing*
generated facade to use the new types without breaking the demo
scripts.

### Risk

**Verbose user code.** Every `(godot-self)` becomes
`(:: (godot-self) :Node2DHandle)`, every position result needs
`(:: ... :Vec2Handle)`. Mitigate with prelude shortcuts:
`(self-as :Node2DHandle)` macro, or auto-coerce at the script-method
boundary (the `(defn _ready [self : Node2DHandle] ...)` could have
`self` synthetically wrapped). Need to design this carefully so the
common-case script doesn't read like Ada.

**Coercion direction.** Up-coercion (child→parent) is always safe.
Down-coercion (parent→child) is a runtime check we can't enforce in
the type system without dynamic dispatch. For v1, down-coercion is
explicit + unchecked (`(:: h :Sprite2DHandle)`); a future pass could
add `(checked-cast h :Sprite2DHandle)` returning `option<Sprite2DHandle>`
via a runtime `Object::is_class` probe.

---

## Phase G6.3 -- Curated One-Shot Wrappers

### Surface

The prelude grows entries for the ~10 patterns every script reaches
for at least once. Concrete starter set:

```turmeric
;; SceneTree access (Node.get_tree returns SceneTree).
(defn get-tree [] : int (godot-call (godot-self) "get_tree"))
(defn tree/quit [] : void
  (godot-call-v (get-tree) "quit"))
(defn tree/get-root [] : int
  (godot-call (get-tree) "get_root"))
(defn tree/change-scene-to-file [path : cstr] : void
  (godot-call-v (get-tree) "change_scene_to_file" path))

;; Timer one-shot creation -- common enough to deserve a helper.
(defn timer/one-shot [seconds : float handler : (fn [] void)] : int
  (let [t (godot-call (get-tree) "create_timer" seconds)]
    (godot-connect-typed t "timeout" handler)
    t))

;; Engine.
(defn engine [] : int (godot-singleton "Engine"))
(defn engine/get-frames-drawn [] : int
  (godot-call (engine) "get_frames_drawn"))
(defn engine/get-process-fps [] : float
  (godot-call-f (engine) "get_process_fps"))

;; OS.
(defn os [] : int (godot-singleton "OS"))
(defn os/get-system-time-msecs [] : int
  (godot-call (os) "get_system_time_msecs"))

;; Logging that's not just println.
(defn log/info [msg : cstr] : void
  (godot-call-v (engine) "print_verbose" msg))
(defn log/error [msg : cstr] : void
  (godot-call-v (godot-singleton "UtilityFunctions") "push_error" msg))
```

Plus the small composition macros that get re-derived in every
real script:

```turmeric
(defn after [seconds : float handler : (fn [] void)] : void
  (let [t (timer/one-shot seconds handler)] (void)))
```

### Mechanism

- Pure prelude additions. No new C natives, no codegen changes.
- Each entry composes Phase G6.1's `godot-connect-typed` or the
  existing generated facade.

### Demo

Extend `examples/paddle-pong-tur/` with a "press R to restart" path:
on `_input` detecting `ui_select`, call `(tree/change-scene-to-file
"res://main.tscn")`. Headless driver verifies the call doesn't error.

### Estimate

~1 day -- pure typing + a smoke test.

### Risk

Surface area grows. The prelude is the right home for now, but
graduating it into a spice (`turmeric-godot-spice/src/godot/...`)
is a follow-up if entries accumulate past ~50.

---

## Phases

| Phase | Title | Scope | Days |
|---|---|---|---|
| G6.1 | Typed signal-connect wrappers | New native + codegen for `class.signals` | ~3 |
| G6.2 | Class-hierarchy `defopaque` handles | `inherits`-aware generation; update facade | ~4 |
| G6.3 | One-shot pattern wrappers | Prelude entries; pong demo extension | ~1 |

Total: ~1.5 weeks of focused work. G6.1 is load-bearing for G6.3's
`timer/one-shot`; G6.2 is independent but its handle types compose
cleanly with G6.1's `(fn [args...] void)` signatures.

---

## Risks (cross-phase)

- **`extension_api.json` signal-arg type coverage.** Some signals
  use `Variant`-typed args; codegen those as `:int` and document.
- **defopaque + typeclasses interaction.** Turmeric typeclasses
  don't currently dispatch on `defopaque` newtypes (the underlying
  `:int` is what matters). If a future facade pass wants
  `Show[NodeHandle]`, that's a typeclass-system question, not a
  binding question.
- **Codegen size.** G6.1 + G6.2 together roughly double the
  generated facade (the current ~675-wrapper allow-list grows
  ~1.5x with on-SIGNAL wrappers, then 2x again if we emit per-class
  inherited methods). Watch the embed-string size; if it
  approaches 5MB the eval cost at script-load becomes a real
  concern and we'll want to defer to a loader-from-disk story.
- **User-side verbosity.** Phase G6.2's biggest risk. Mitigate via
  prelude macros and `(self-as :C)` shortcuts before declaring the
  surface "done."

---

## Out of Scope (Tracked Elsewhere / Later)

- **Per-method typed C natives** -- a fully dispatched-by-method
  facade where every method has its own typed entry point. ~30k C
  functions. Rejected.
- **Linear / affine handle lifetimes** -- Ref<T> tracking via
  `:linear` defopaque. Worth revisiting when a real script needs
  to hold a Resource across method calls outside the arena.
- **Auto-generated typeclass instances per class** -- `Show
  [Node2DHandle]` etc. Falls out cleanly only once typeclass
  dispatch over `defopaque` newtypes lands.

---

## Success Criteria

- Phase G6.1: at least one signal-handler arity mismatch surfaces
  as a `TUR-E` at script-load time. Headless driver demonstrates.
- Phase G6.2: every script in `examples/spike/scripts/` compiles
  with handle-typed signatures and no `:int` for any Object /
  arena handle position. The `paddle-pong-tur` demo upgrades
  cleanly.
- Phase G6.3: `paddle-pong-tur` uses `(tree/quit)` and
  `(timer/one-shot)` at least once; no script in the repo reaches
  for `(godot-singleton ...)` directly anymore.

End-state: a user-facing typing story where the script saying
`(node2d/set-position self pos)` is rejected at compile time if
`self` came from `(input)` or `pos` is a `ColorHandle`, with the
prelude composing the common patterns without ceremony.
