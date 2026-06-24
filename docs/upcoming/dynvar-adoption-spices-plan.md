# Dynamic Var Adoption -- Spices Plan

> **Status:** Proposed
>
> **Prerequisite:** DV0-DV4 complete (`-Xdynamic-vars`); see
> [docs/archive/history/dynamic-vars-plan.md](../archive/history/dynamic-vars-plan.md)
> and [docs/guides/dynamic-vars-guide.md](../guides/dynamic-vars-guide.md).
>
> **Sibling plan:** [dynvar-adoption-plan.md](dynvar-adoption-plan.md)
> covers compiler-tree adoption. This plan covers spices in
> `../turmeric-spices/spices/`.
>
> **Scope:** Replace ambient handles, threaded ctx/conn params, and
> hand-rolled "current X" globals in selected spices with `defdynamic`.
> Singleton-busting and test-replacement only; no API redesign beyond
> what dynvars naturally remove.

---

## Motivation

Several spices independently re-invent the binding-stack pattern: tourist
threads `Ctx` through every handler, postgres threads `Conn`, raygui
hand-rolls a state save/restore around C statics. The dynvar machinery
already exists -- the work here is conversion, not design.

Non-goals: rewriting spice APIs, removing typed handles, or "everything
should be a dynvar." Each target below has a concrete win that justifies
the change.

---

## Targets (ranked)

### A. `tourist` + `httpd` -- request context -- HIGH payoff

**Sites:** `../turmeric-spices/spices/tourist/src/tourist/app.tur:79-88`
and ~29 handler/middleware/matcher functions that take `[ctx : Ctx]` as
their first parameter. `httpd` is the producer (sets per-request state in
its worker thread).

**Proposed var (in a new `tourist/src/tourist/dynvars.tur`):**

```turmeric
(defdynamic *current-ctx* :ptr<Ctx> nil-ctx)
```

**Why dynvar:** erases the `ctx` parameter from nearly every handler
signature; `httpd`'s worker dispatch establishes the binding once per
request, and every downstream call reads it directly. Test injection
becomes one `binding` form.

**Conversion:**

1. Declare `*current-ctx*` in tourist.
2. `httpd`'s request dispatch wraps the user handler in
   `(binding [*current-ctx* req-ctx] (user-handler))`.
3. Tourist's middleware/matcher API gains an arg-less reader
   (`(current-ctx)`) and old explicit-ctx entry points become thin
   wrappers that pass `*current-ctx*`.
4. Migrate handlers opportunistically -- both call styles work during
   transition.

**Caveat:** `Ctx` must be non-substructural to be carried by a dynvar
(TUR-E0603 rejects substructurals). Today it is; preserve that.

**Estimated size:** medium-large; touches both spices, but the change
per call site is mechanical.

### B. `raygui` -- GUI state and style stack -- HIGH payoff, low risk

**Sites:** `../turmeric-spices/spices/raygui/src/raygui/core.tur:102-206`.
`gui-set-state`/`gui-set-style`/`gui-load-style` already manipulate C
statics with hand-rolled save/restore.

**Proposed vars:**

```turmeric
(defdynamic *gui-state* :ptr<GuiState> default-gui-state)
(defdynamic *gui-style* :ptr<GuiStyle> default-gui-style)
```

**Why dynvar:** the existing pattern IS a binding stack. Replacing it
with dynvars deletes the boilerplate and makes `(with-style s ...)` /
`(with-state s ...)` natural one-liners. Tests get scoped overrides for
free.

**Conversion:** convert the existing setter/getter pairs to read/write
the dynvar; offer `with-state`/`with-style` macros that expand to
`binding`. Keep the underlying C `Raygui*` calls -- only the Turmeric
wrapper changes.

**Estimated size:** medium; isolated to one spice.

### C. `ansi` -- terminal sink and color mode -- MEDIUM payoff

**Sites:** `../turmeric-spices/spices/ansi/src/ansi/color.tur:27-95`.
Every color/style helper writes to stdout via `printf`.

**Proposed vars:**

```turmeric
(defdynamic *ansi-sink*       :ptr<FILE> stdout-handle)
(defdynamic *ansi-color-mode* :int 0)   ; 0 = auto, 1 = always, 2 = never
```

**Why dynvar:** CI captures output without rewiring fds; `--no-color`
flips a mode flag instead of branching at every emit. Pairs naturally
with the compiler-side `*current-out*` dynvar from the main plan.

**Conversion:** rewrite the emit primitives to read both vars; leave
the high-level color API unchanged.

**Estimated size:** small; ~10-15 emit sites.

### D. `valkey` -- Redis client handle -- MEDIUM payoff

**Sites:** `../turmeric-spices/spices/valkey/src/valkey/` (cmd, reply,
client, pubsub modules). Pub/sub and queue helpers thread the client.

**Proposed var:**

```turmeric
(defdynamic *valkey-client* :ptr<ValkeyClient> nil-client)
```

**Why dynvar:** test fakes, request-scoped client selection (different
namespaces per tenant), and removes the most common first param.

**Conversion:** same shape as tourist; add arg-less readers, keep the
explicit-client API as wrappers.

**Estimated size:** medium.

### E. `postgres` -- connection handle -- MEDIUM payoff, design question

**Sites:** `../turmeric-spices/spices/postgres/src/postgres/db.tur:197-322`.
25+ functions take `[^borrow conn : Conn]`.

**Proposed var:**

```turmeric
(defopaque ConnHandle :ptr<void>)        ; non-substructural carrier
(defdynamic *current-conn* :ConnHandle nil-conn)
```

**Why dynvar:** test fixtures bind a fixture connection for the whole
suite; `(with-transaction ...)` becomes a `binding`.

**Design caveat:** `Conn` is `^borrow`-typed today and `defdynamic`
rejects substructural types (TUR-E0603). The dynvar must carry a
non-substructural handle (a `defopaque` wrapper). Linearity then shifts
from the type system to discipline at the dynvar boundary -- production
code continues to use the typed API; the dynvar is the test/scoping
seam, not the primary handle.

**Decision required before starting:** are we OK with the dynvar
carrying an un-borrowed handle, or do we want to lift the substructural
restriction on `defdynamic` for this case? Recommend the former -- the
restriction exists for sound reasons.

**Estimated size:** medium, plus the design call.

### F. `opengl` -- current program / VAO / VBO -- LOW-MEDIUM payoff

**Sites:** `../turmeric-spices/spices/opengl/src/opengl/shaders.tur:147-150`
and bind helpers.

**Proposed vars:**

```turmeric
(defdynamic *gl-program* :int 0)
(defdynamic *gl-vao*     :int 0)
(defdynamic *gl-vbo*     :int 0)
```

**Why dynvar:** mirrors the GL state machine into a scope so
`(with-program p ...)` can pop back on exit without the manual unbind
footgun. GL itself still owns the real state; the dynvar is a hygiene
layer.

**Estimated size:** small per var; defer unless someone is actively
hitting the unbind footgun.

### Explicitly skipped

- `math`, `linalg`, `regex`, `json`, `frame`: pure/data, no ambient state.
- `c-dsl`, `glsl`: compile-time codegen, no runtime singleton.
- `ecs`/`ecs-raylib`: the world pointer is a candidate, but ECS APIs
  already lean on an explicit world; the win is smaller than tourist's
  ctx threading. Revisit after A.

---

## Phases

| Phase | Targets | Gate |
|---|---|---|
| SPA-1 | C (ansi sink + color mode) | smallest spice; validates the recipe |
| SPA-2 | B (raygui state stack) | self-contained; deletes existing boilerplate |
| SPA-3 | A (tourist + httpd ctx) | biggest payoff; coordinate across both spices |
| SPA-4 | D (valkey client) | mirrors A on smaller scope |
| SPA-5 | E (postgres) | gated on the substructural-vs-dynvar design call |
| SPA-6 | F (opengl) | only if a real consumer asks for it |

Each phase is independently shippable in `../turmeric-spices/`. Stop at
any phase where the next target's payoff falls below its conversion
cost -- the goal is leverage, not coverage.

---

## Sequencing relative to the compiler-tree plan

The compiler-tree plan ([dynvar-adoption-plan.md](dynvar-adoption-plan.md))
should land its early phases first -- mock-time and diag-mode validate
the conversion recipe inside this repo where cadence is fully
controlled. SPA-1 and SPA-2 are safe to run in parallel with that;
SPA-3 (tourist) wants the compiler-side output-sink work bedded in
first because tourist tests will reuse that capture machinery.

---

## Test plan per phase

- Add a fixture under
  `../turmeric-spices/spices/<spice>/tests/dynvar-adopt/` that
  exercises both the root value and a `binding` override.
- For A and D, add a `spawn-conveying` fixture: parent binds a fake,
  child worker sees it. Doubles as a real-world DV3 smoke test outside
  the existing convey-isolation fixture in this repo.
- Each spice's existing suite must stay green
  (`bash tests/run.sh 2>&1 | grep "^FAIL"` empty) after each phase.

---

## Open questions

- **Cross-spice dynvar declarations.** `*current-ctx*` belongs to
  tourist but is set by httpd. Either tourist exports the var and
  httpd imports it, or both depend on a small shared
  `tourist-dynvars` spice. Recommend the export-from-tourist route --
  fewer moving parts, and httpd already depends on tourist in
  practice.
- **`Ctx` shape compatibility.** Confirm `Ctx` is non-substructural
  and that a `:ptr<Ctx>` (or the existing handle representation) fits
  the dynvar type rules without lifting TUR-E0603.
- **Root-init known bug.** The DV2 "root-value initializer not emitted
  when explicit `defn main` is present" limitation still applies. For
  targets whose root must be a real handle (default sink, default
  state, default client), either fix the root-init bug first or
  initialize from spice glue before user code runs.
