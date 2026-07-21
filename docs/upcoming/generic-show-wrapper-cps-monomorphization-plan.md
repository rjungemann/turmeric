# Generic `^Show a` / `^Class a` wrapper monomorphization in the CPS backend

**Status:** NOT STARTED. Prepared from the (now-archived) report
`docs/archive/generic-void-show-wrapper-rough-edges.md`, which recorded two
coupled rough edges in the generic `^Show a` wrappers (`show-line`, `print-show`,
`stdlib/typeclass-show.tur`). One attempted point-fix (the Edge 2 void-temp fix)
was landed and then reverted (commit `71b5cef`) because, in isolation, it converts
a loud compile error into a SILENT MISCOMPILE. This plan sequences the work so the
compile-error fix and the dispatch fix land together, never separately.

## Background

`show-line` / `print-show` are generic `^Show a` wrappers: they `(show x)` (an
owned `String`), print it, release it, and return `:void`. They are the idiomatic
replacement for `(println (show x))`. Two shapes miscompile:

- **Edge 1 (unresolved element ICE):** `(show-line (vec-new))` -- an element-less
  parametric container through the wrapper -- ICEs at `emit_reresolve_disp_type`
  (`carrier<->concrete crossing reached code emission with an unresolved
  parametric param`, `src/compiler/emit_core.c:1820`). `(show (vec-new))`
  DIRECTLY is fine (renders `[]`) because it defaults the element tyvar to the int
  carrier; the wrapper path does not.

- **Edge 2 (void wrapper in a CPS-lowered helper):** two COUPLED defects:
  - **2a (compile error):** a `:void`-returning wrapper call reaches the
    `CT_TAILCALL` `cps->direct` arm (`src/compiler/emit_cps_ir.c`) and is bound
    into an `__auto_type` temp -- `__auto_type t = show_hyline(...)` -- which is a
    C `variable declared void` / `void value not ignored` error (the callee emits
    as C `void`).
  - **2b (silent misdispatch):** once 2a is fixed and the call COMPILES, the SAME
    arm resolves the callee to the GENERIC base clone `show_hyline`, whose body
    bakes the int carrier representative `__inst_Show_show_int(x)`, instead of the
    per-element monomorph clone that dispatches to the correct
    `__inst_Show_show_String` / `__inst_Show_show_Vec` / ... . The pointer is
    printed as an int -> silent garbage for every non-int element type.

**The coupling / why 2a cannot ship alone.** Pre-2a, a `(show-line <non-int>)` in
a CPS-lowered helper does not compile, so 2b is latent (masked by the loud error).
Fixing 2a alone unmasks 2b -- a silent garbage render for a common shape
(`show-line`/`print-show` in any helper with a `String`/`Vec`/`Set`/`Map`/`cstr`).
This is exactly the "do not ship a fix that converts a build error into a runtime
miscompile" hazard the codebase flags (cf.
`docs/reported/effectful-fnvalue-param-miscompile.md`). Hence: 2a and 2b land
together.

### Minimal repro (2b, once 2a is applied)

```turmeric
(load "stdlib/typeclass.tur")
;; A -- show-line in a helper. Without 2a: compile error. With 2a alone: garbage
;;      pointer (e.g. 94330540635104) instead of "HelloWorld".
(defn demo [] : int
  (let [c (string/concat (string/from-cstr "Hello") (string/from-cstr "World"))]
    (do (show-line c) 0)))
(defn main [] : int (demo))
;; B -- same let directly in main: prints "HelloWorld" (monomorph resolves).
```

General, not String-specific: `(show-line (vec-of 1 2 3))` in a helper prints a
pointer instead of `[1 2 3]`. NON-void `show` in a helper is fine
(`(let [s (show v)] (println (string/to-cstr s)))` renders correctly) -- the gap
is specific to the void-wrapper `CT_TAILCALL` `cps->direct` callee resolution.

## Root-cause analysis (grounded)

The clone-selection helper is `find_mono_clone_for_call`
(`src/compiler/emit_cps_ir.c:4525`). It walks `ctx->abi_specializations` for a
spec whose `binding == fn` and `n_args == n`, and discriminates candidate clones
by comparing each call arg's **C type name** against `spec->arg_types[j]` --
**but it skips scalar / carrier args** (`if (args[j].ty != TY_APP && != TY_ADT &&
!= TY_STRUCT) continue;`, line 4535) and only returns a clone when EXACTLY ONE
spec matches (`n_hit == 1`, line 4542).

For `(show-line c)` the element (`String`/`Vec`/...) rides the `int64_t` carrier
at the call site (the atom is `(int64_t)(intptr_t)c`), so:

- The per-element clones all present as arg C type `int64_t`. The discriminating
  loop cannot tell `Show[String]` from `Show[Vec]` -- they collide on the carrier.
  The clone mangling itself reflects this: the emitted clone is
  `show_line__spec__void_int64_t` (return `void`, arg `int64_t`), a name that does
  not encode the element type.
- The `CT_TAILCALL` arm only calls `find_mono_clone_for_call` at all when the
  callee's `SEnt` has `mono_template` set (`src/compiler/emit_cps_ir.c:5250-5255`);
  otherwise `fn = callee_name(...)` = the generic base clone directly.

The discriminator that DOES distinguish the clones already exists on the spec but
is unused here: `EmitAbiSpecialization` carries `bindings[]` (the tyvar->concrete
type binding, `emit_internal.h:117`) and `typeclass_inst`
(`struct TypeClassInstance *`, set for typeclass-instance-method specs by
`emit_abi_intern_spec`, `emit_internal.h:163`). The call site carries the resolved
instance via its `dict_arg` (`EX_DICT`). So the fix is to discriminate the
wrapper's clones by the **instance / tyvar binding**, not the carrier arg C type.

**Task 1 of Phase 2 pins the EXACT failure point** among these candidates (they
are not mutually exclusive): (a) `fe->mono_template` false so the lookup never
runs; (b) `find_mono_clone_for_call` returns NULL because the carrier args are
non-discriminating and >1 spec ties; (c) the clone NAME collides across element
types (`show_line__spec__void_int64_t` for both `String` and `Vec`) so a program
using the wrapper at two element types would emit one clone and mis-call the
other. A quick instrumentation of the arm on the repro settles it before code
changes.

## Strategy

Two viable approaches; Phase 2 Task 1 chooses between them on the basis of what
Task 1 pins, but the default recommendation is **A** (least new machinery, matches
the existing spec model):

- **A. Instance/binding-keyed clone selection.** Give the `CT_TAILCALL`
  `cps->direct` (and `CT_LETCALL`) arms a dict/binding-aware clone lookup: match
  the call's `dict_arg` instance (and/or the spec's `bindings[]` tyvar binding)
  against `spec->typeclass_inst` / `spec->bindings[]`, so `Show[String]` and
  `Show[Vec]` resolve to distinct clones even though both args are the `int64_t`
  carrier. Requires the per-element clone NAME to encode the element/instance
  (fix the `void_int64_t` collision) so two element types in one program get
  distinct symbols.

- **B. Runtime dict threading.** Thread the `Show` dict as a runtime argument
  through the wrapper's `cps` entry (like the M4/M6 per-method ABI work) so the
  wrapper dispatches `show` through the passed dict rather than a baked instance.
  Heavier, but removes the monomorph-per-type explosion. Prefer only if Task 1
  shows the clone model cannot be made unambiguous cheaply.

Relevant prior art to mirror / not duplicate:
`docs/upcoming/v2/` M4/M6/M7 typeclass-ABI plans (per-instantiation dict
singletons, HKT carrier carve-out), and `emit_abi_intern_spec`'s existing
`typeclass_inst` tagging.

## Phases

### Phase 0 -- Repro harness & baseline (no code changes)

- **0.1** Add throwaway repro files (not fixtures yet) for: `show-line` on
  `int`, `cstr`, `String`, `Vec`, `Set`, `Map`, each in BOTH a `main`-direct
  `let` and a helper-`defn` context; plus `print-show` variants; plus the Edge 1
  `(show-line (vec-new))`.
- **0.2** Record current behavior per cell (compiles? renders correctly?
  garbage? ICE?) as a table in the plan's progress log. This is the acceptance
  oracle for later phases.
- **0.3** Confirm the direct (`main`) path and non-void `show` path render every
  type correctly (the "known-good" reference emit).

### Phase 1 -- Re-land the void-temp compile fix (Edge 2a), GATED

- **1.1** Reinstate the reverted `CT_TAILCALL` `cps->direct` void handling
  (`emit_cps_ir.c`): when the callee's `FnDef` return type
  (`fd_for_binding` + `fn_ret_type`) is `TY_NIL`/`TY_NEVER`, emit a bare call and
  `emit_deliver(..., "0")` instead of `__auto_type t = fn(...)`. (Verbatim the
  reverted commit `31fa6fa` diff.)
- **1.2** Do NOT ship this as a standalone commit. Keep it on the same branch as
  Phase 2 and only merge once Phase 2 makes the wrapper dispatch correct. If an
  intermediate landing is unavoidable, guard 1.1 so a wrapper call that cannot
  resolve to a concrete per-element clone emits a **hard compile diagnostic**
  (see Phase 3), never the silent carrier-rep call -- i.e. keep the failure loud.

### Phase 2 -- Correct wrapper clone selection (Edge 2b) [core]

- **2.1 (pin the failure)** Instrument the `CT_TAILCALL` arm on repro A: log
  `fe != NULL`, `fe->mono_template`, the `find_mono_clone_for_call` candidate
  count and returned name, and the emitted clone names for `show-line`. Decide
  approach A vs B from the result. Record findings in the progress log.
- **2.2 (clone naming)** If clones collide on `void_int64_t`, extend the clone
  mangling for a typeclass-wrapper spec to encode the element type / instance
  (reuse `spec->bindings[]` / `spec->typeclass_inst`), so `Show[String]` and
  `Show[Vec]` clones get distinct symbols. Verify a single program using
  `show-line` at two element types emits two clones.
- **2.3 (selection)** Add an instance/binding-aware branch to
  `find_mono_clone_for_call` (or a sibling used by the wrapper arm): when the
  callee is a `^Class a` wrapper and args are carrier-erased, discriminate by the
  call's `dict_arg` instance against `spec->typeclass_inst` (and/or the tyvar
  binding), instead of the arg C type. Preserve the existing arg-C-type path for
  non-wrapper generics (no behavior change there).
- **2.4 (gate)** Ensure the `mono_template` gate at `emit_cps_ir.c:5250` admits
  the wrapper so the lookup actually runs; if the wrapper is not registered as a
  mono_template, register it (mirror how `main`/direct path specializes it).
- **2.5** Apply the same selection fix to the `CT_LETCALL` arm
  (`emit_cps_ir.c:5170`) if a non-tail wrapper call exhibits the same collision
  (value-returning `^Class a` wrappers, e.g. a future `render-line`-style).

### Phase 3 -- Guardrail against silent carrier-rep fallback

- **3.1** When a `^Class a` wrapper call in a CPS arm cannot resolve to a
  concrete per-element clone AND the receiver/element is a concrete non-int type,
  emit a hard diagnostic (or route through the dict) rather than silently calling
  the carrier-rep base clone. This closes the whole *class* of silent
  misdispatch, not just the `show-line` instance, and makes any future regression
  loud.
- **3.2** Consider an assertion analogous to
  `emit_abi_assert_routed_concrete` (`emit_core.c:142`) at the wrapper call site:
  a wrapper resolving to the int carrier rep for a non-int concrete element is a
  routing hole.

### Phase 4 -- Edge 1: unresolved-element ICE through the wrapper

- **4.1** Trace how the DIRECT `(show (vec-new))` path defaults the unresolved
  element tyvar to the int carrier (it renders `[]`). Identify the defaulting
  site.
- **4.2** Apply the same default (or a clean diagnostic) when the container flows
  through one `^Class a` generic hop, so `emit_reresolve_disp_type`
  (`emit_core.c:1619`) receives a concrete-enough type instead of tripping the
  deep-side `emit_abi_assert_routed_concrete` invariant (`emit_core.c:1820`).
  This lives in the ABI carrier-crossing recovery machinery -- treat as
  higher-risk; land behind its own review and after Phases 1-3.
- **4.3** If a clean default is not reachable cheaply, at minimum convert the ICE
  into a proper user diagnostic ("cannot infer element type of empty collection
  passed to `show-line`; ascribe it, e.g. `(:: (vec-new) (Vec int))`").

### Phase 5 -- Fixtures, suite, docs

- **5.1** Add regression fixtures under `tests/fixtures/`:
  `show-wrapper-helper-<type>` for `String`/`Vec`/`Set`/`Map`/`cstr`, each
  asserting the correct render from a helper `defn` (the shape that used to
  garble); plus a `show-wrapper-two-element-types` fixture exercising the
  clone-naming fix (2.2); plus the Edge 1 fixture (correct render or clean
  diagnostic).
- **5.2** ASan/valgrind pass on the wrapper-in-helper fixtures (owned-String
  render + release; no leak, no double free).
- **5.3** Full suite green (`bash tests/run.sh`, 12-min timeout). Regenerate any
  codegen snapshots the clone-naming change moves, in the same PR.
- **5.4** Update `stdlib/typeclass-show.tur` docstrings if the wrapper's
  monomorphization contract changes in any user-visible way. Remove the
  interim workarounds from the stage-4 fixtures (`show-collections` type
  ascription; `string-basic`/`string-slice` explicit-release form) where the
  wrapper now works directly.

## Acceptance criteria

- Every `(show-line x)` / `(print-show x)` renders identically whether called in
  `main` or in a helper `defn`, for `int`/`cstr`/`String`/`Vec`/`Set`/`Map`.
- No `__auto_type t = void_fn(...)` in emitted C for a `:void` wrapper call.
- A wrapper call that cannot resolve to a concrete instance is a LOUD error, never
  a silent carrier-rep render.
- Edge 1 either renders `[]` or emits a clean ascribe-hint diagnostic (no ICE).
- Full suite green; wrapper-in-helper fixtures valgrind-clean.

## Progress log

- 2026-07-21 -- Plan created from the archived report. Edge 2a fix authored then
  reverted (commit `71b5cef`) to avoid shipping the 2a-without-2b silent
  miscompile. No plan phase started yet.
