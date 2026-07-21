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

### Pinned failure points (instrumented 2026-07-21)

The `CT_TAILCALL` arm was instrumented on the repros (`show-line` on
`String`/`Vec`/`cstr`, in a helper vs `main`; the void-temp fix temporarily
re-applied so they compile). Findings, with evidence:

| case | ctx | `fe->mono_template` | forced `find_mono_clone_for_call` | clone-body dispatch | `<clone>__cps` emitted | render |
| --- | --- | --- | --- | --- | --- | --- |
| String | helper | **0** | returns `show_line__spec__void_int64_t` (correct) | `__inst_Show_show_String` (correct) | **no** | garbage |
| Vec | helper | **0** | returns `show_line__spec__void_tur_adt_Vec__int__` (correct) | `__inst_Show_show_Vec__spec__...` (correct) | **no** | garbage |
| cstr | helper | 1 | returns `show_line__spec__void_const_char` | **`__inst_Show_show_int` (WRONG)** | yes | garbage |
| cstr | main | (clone used) | -- | **`__inst_Show_show_int` (WRONG)** | -- | garbage |
| String | main | (never reaches arm; direct-inlined) | -- | inlined `__inst_Show_show_String` | -- | correct |

This pins **two distinct root causes** (the earlier "three candidates" guess was
wrong -- clone-name collision and `find_mono_clone_for_call` returning NULL are
NOT the cause):

- **RC1 -- nominal element types (`String`, `Vec`, ...): the `mono_template`
  gate.** `ent_of_binding(fn)->mono_template` is **0** for the wrapper, so the
  arm's `if (fe && fe->mono_template)` guard (`emit_cps_ir.c:5252`) skips
  `find_mono_clone_for_call` entirely and falls to
  `fn = callee_name(...)` = the generic base clone `show_hyline`, whose body bakes
  the int carrier rep `__inst_Show_show_int`. The correct per-element clone EXISTS,
  the lookup RETURNS it when the gate is bypassed, and the clone's BODY dispatches
  correctly (String -> `__inst_Show_show_String`, Vec ->
  `__inst_Show_show_Vec__spec__...`). So RC1 is *purely* the gate.
  - **Coupled catch (RC1b):** naively removing the gate is not a fix. With the
    gate bypassed the arm takes its `mclone` -> cps->cps branch, emitting
    `return <clone>__cps(...)` -- but the `__cps` variant of the wrapper spec clone
    is **not emitted** (`show_line__spec__void_int64_t__cps` is an undefined
    reference; link error confirmed). The correct routing is the DIRECT clone
    (`<clone>`, which IS emitted) via a cps->direct call, not cps->cps.

- **RC2 -- carrier-scalar element types (`cstr`, and by inference `bool`/sized
  ints): the mono-clone BODY dispatches to the int rep.** For `cstr` the gate is
  fine (`mono_template=1`) and the correct-named clone `show_line__spec__void_const_char`
  is selected, but the clone's own body calls `__inst_Show_show_int((int64_t)x)`
  instead of `__inst_Show_show_cstr` -- the internal `show` dispatch fell to the
  int carrier representative among the primitives that share the int64 carrier.
  This garbles in `main` too (independent of the CPS gate). It is a mono-clone-body
  ABI-specialization bug, NOT a call-site resolution bug, and is the same
  carrier-representative dispatch class as
  `docs/reported/method-dispatch-missing-instance-falls-back-to-carrier-representative.md`
  -- surfacing here inside a wrapper clone body.

The discriminator that distinguishes the per-element clones already exists on the
spec: `EmitAbiSpecialization.bindings[]` (the tyvar->concrete-type binding,
`emit_internal.h:117`) and `typeclass_inst` (`emit_internal.h:163`, NULL for an
ordinary `^Show a` defn like `show-line`). The call site also carries the resolved
instance via `dict_arg` (`EX_DICT`). RC2's fix must key the clone body's inner
`show` dispatch on that binding/instance rather than the carrier rep.

## Strategy

The pin above resolves what was uncertain, so the strategy is now concrete and
split by root cause:

- **RC1 (nominal types -- `String`/`Vec`/...): make the wrapper participate in
  mono-clone dispatch, routed to the DIRECT clone.** The clone and its correct
  body already exist; the fix is to (i) admit the wrapper through the
  `mono_template` gate (or add a wrapper-aware branch that runs
  `find_mono_clone_for_call` regardless), and (ii) when a clone resolves for a
  callee that has no emitted `__cps` variant, route to the direct clone via the
  cps->direct path (bare/`__auto_type` call, or the void bare-call from Edge 2a)
  rather than the cps->cps `<clone>__cps` branch. No new discriminator machinery
  is needed for RC1 -- `find_mono_clone_for_call` already returns the right clone;
  the clone-name-collision worry only bites when two DISTINCT carrier-mangled
  element types (e.g. `String` and an opaque `:int` newtype, both `void_int64_t`)
  appear in ONE program (Task 2.5 covers it, lower priority).

- **RC2 (carrier-scalar types -- `cstr`/`bool`/sized ints): fix the clone BODY's
  inner `show` dispatch.** The clone is selected correctly; its body must dispatch
  `(show x)` to the element's instance (`__inst_Show_show_cstr`) rather than the
  int carrier representative. This is the harder half -- it lives in the
  ABI-specialization of the clone body (the same carrier-representative dispatch
  machinery as the method-dispatch report) and may need the spec's `bindings[]`
  to steer the inner method dispatch. Prior art:
  `docs/upcoming/v2/` M4/M6/M7 typeclass-ABI plans (per-instantiation dict
  singletons, HKT carrier carve-out); `emit_abi_intern_spec`'s `typeclass_inst`
  tagging; and `emit_reresolve_method_call` / `emit_reresolve_disp_type`, which
  already re-dispatch a carrier-erased method per ABI spec elsewhere.

RC1 and RC2 are independent and can land in either order, but the void-temp fix
(Phase 1) must not ship until BOTH are done (RC1 unmasks nominal-type garbage;
RC2 the carrier-scalar garbage) -- else Phase 1 alone is a silent miscompile.

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

### Phase 2 -- RC1: route nominal-type wrapper calls to the direct clone [core]

(The failure is already pinned -- see "Pinned failure points". This phase fixes
RC1: `String`/`Vec`/... in a helper. Task 2.1's old "instrument to pin" job is
DONE; the instrumentation approach is preserved below as the verification method.)

- **2.1 (admit the wrapper through the gate)** In the `CT_TAILCALL` arm
  (`emit_cps_ir.c:5250-5255`), the `if (fe && fe->mono_template)` guard is what
  skips the (working) `find_mono_clone_for_call` for `show-line`. Either (a) set
  `mono_template` on the wrapper's `SEnt` where the direct/`main` path already
  specializes it, or (b) add a wrapper-aware condition that runs the lookup even
  when `mono_template` is 0. Confirm with the instrumentation (`TUR_DBG_WRAP`-style
  probe used in the pin) that `find_mono_clone_for_call` now returns the clone.
- **2.2 (route to the DIRECT clone, not cps->cps)** Critical coupling (RC1b): when
  a clone resolves but its `<clone>__cps` variant is NOT emitted (true for these
  wrapper specs -- `show_line__spec__void_int64_t__cps` is an undefined reference),
  the arm must take the cps->direct path calling the DIRECT clone `<clone>`, not
  the cps->cps `return <clone>__cps(...)` branch (`emit_cps_ir.c:5257,5271`).
  Options: (a) gate the cps->cps branch on "the callee has an emitted `__cps`
  variant" and fall to cps->direct otherwise; or (b) emit the `__cps` variant for
  wrapper spec clones. Prefer (a) -- smaller, and the direct clone body is already
  correct for nominal types. Combine with the Edge 2a void bare-call so a
  `:void` direct clone call emits `<clone>(...)` + deliver `0`.
- **2.3 (verify)** `show-line`/`print-show` on `String`/`Vec`/`Set`/`Map` in a
  helper now render correctly and link. Non-void `show` in a helper (already
  correct) must be unaffected.

### Phase 2b -- RC2: fix the carrier-scalar clone-body dispatch [core]

(Independent of Phase 2; fixes `cstr`/`bool`/sized-int, which garble even in
`main` because the clone BODY bakes `__inst_Show_show_int`.)

- **2b.1** Trace how the wrapper clone body's inner `(show x)` is ABI-specialized
  when the element is a carrier-scalar (`cstr`). Identify where it resolves to the
  int carrier representative instead of the element's instance -- the
  representative-selection is the same class as
  `method-dispatch-missing-instance-falls-back-to-carrier-representative.md`, here
  reached inside a clone body via `emit_reresolve_method_call`.
- **2b.2** Steer the inner dispatch by the spec's `bindings[]` (the tyvar->`cstr`
  binding for this clone) so the body calls `__inst_Show_show_cstr`. Ensure `int`
  itself still resolves to `__inst_Show_show_int` (do not break the one case that
  currently works).
- **2b.3 (clone-name collision, lower priority)** Two DISTINCT carrier-mangled
  element types in ONE program share the `void_int64_t` clone symbol (e.g. `String`
  + an opaque `:int` newtype). Encode the element/instance in the wrapper clone
  mangling (reuse `spec->bindings[]`) so they get distinct symbols. Only bites
  multi-element-type programs; verify with a fixture that uses `show-line` at two
  carrier-mangled types.
- **2b.4** Apply the same body-dispatch fix to `print-show` and any other
  `^Show a` wrapper, and to the `CT_LETCALL` arm (`emit_cps_ir.c:5170`) for a
  value-returning `^Class a` wrapper if one exists.

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
  clone-naming fix (2b.3); plus the Edge 1 fixture (correct render or clean
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
  miscompile.
- 2026-07-21 -- **Failure point PINNED** (Phase 0 baseline + old Phase 2.1
  instrumentation, done). Temporarily re-applied the Edge 2a void fix and
  instrumented the `CT_TAILCALL` arm (`TUR_DBG_WRAP` probe: callee, `fe`,
  `mono_template`, a forced `find_mono_clone_for_call`, and the matching specs;
  plus a `TUR_DBG_NOGATE` probe bypassing the gate). All instrumentation removed
  afterward (`emit_cps_ir.c` back to HEAD; nothing landed). Results are in
  "Pinned failure points": TWO root causes -- **RC1** = the `mono_template=0` gate
  skips a working clone lookup for nominal types (`String`/`Vec`), coupled with a
  missing `<clone>__cps` variant (route to the DIRECT clone); **RC2** = the
  carrier-scalar (`cstr`) clone BODY bakes `__inst_Show_show_int` (garbles in
  `main` too, independent of the gate). The earlier "three candidates" guess was
  wrong: clone-name collision and `find_mono_clone_for_call` returning NULL are
  NOT the cause. Phases 2 / 2b rewritten around RC1 / RC2.
