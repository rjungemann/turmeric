# Generic `^Show a` / `^Class a` wrapper monomorphization in the CPS backend

**Status:** DONE (2026-07-21) -- both edges fixed, fixtures + stage-4 workaround
cleanup landed. Only the OPTIONAL Phase 3 guardrail remains (not needed -- the
fixes are sound by construction, see Phase 3). Prepared from the (now-archived)
report
`docs/archive/generic-void-show-wrapper-rough-edges.md`, which recorded two
coupled rough edges in the generic `^Show a` wrappers (`show-line`, `print-show`,
`stdlib/typeclass-show.tur`).

Edge 2 (Phases 1/2/2b): the void-temp compile fix + RC1 (nominal-type
direct-clone routing) + RC2 (colored-clone CPS-body method re-resolution) + 2b.3
(scalar-spec tie-break) shipped together, so `show-line`/`print-show` render
correctly for every element type in a CPS-lowered helper with no silent
miscompile. Edge 1 (Phase 4): a wrapper applied to an element-less container
(`(show-line (vec-new))`) no longer ICEs -- the concrete-head/tyvar-element
dispatch type resolves by head instead of tripping the R3 assertion. This plan
sequenced the Edge 2 work so the compile-error fix and the dispatch fixes landed
together, never separately (an earlier isolated void-temp fix was reverted, commit
`71b5cef`, precisely to avoid the loud-error -> silent-miscompile conversion).

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

### Phase 1 -- Re-land the void-temp compile fix (Edge 2a) -- LANDED

- **1.1 -- done.** The `CT_TAILCALL` `cps->direct` void handling is back
  (`emit_cps_ir.c`): a `TY_NIL`/`TY_NEVER` callee (via `fd_for_binding` +
  `fn_ret_type`) emits a bare call + `emit_deliver(..., "0")`.
- **1.2 -- satisfied.** Landed TOGETHER with RC1 + RC2 + 2b.3 in one pass, so it
  never shipped alone: with all four in place every element type
  (int/bool/cstr/String/Vec/Set/Map) renders correctly in a helper -- there is no
  silent carrier-rep miscompile to guard against. (The interim RC1-only commit
  `4760417` deliberately did NOT include this void fix, keeping the branch's loud
  compile error until the dispatch was correct.)

### Phase 2 -- RC1: route nominal-type wrapper calls to the direct clone [core] -- LANDED

**Status: DONE (2026-07-21), staged.** The `CT_TAILCALL` arm
(`emit_cps_ir.c`) now resolves the per-element DIRECT clone for an UNCOLORED
generic callee (the gate `if (fe && fe->mono_template)` skipped it) and routes it
through the cps->direct path (the clone is direct-only; `clone_is_cps` stays false
so it never hits the cps->cps `<clone>__cps` undefined reference). Implementation
matched the pin exactly:

- **2.1 (admit the wrapper) -- done.** Added an `else if (fe && !callee_colored)`
  branch that runs `find_mono_clone_for_call` even when `mono_template` is 0. The
  lookup already returns the correct clone (proven in the pin); no new
  discriminator was needed.
- **2.2 (route to the DIRECT clone) -- done.** The cps->cps-vs-direct condition is
  now `callee_colored || clone_is_cps` (was `binding_in_s || mclone`), so an
  uncolored generic clone falls to cps->direct calling `<clone>` (which is
  emitted), never `<clone>__cps` (which is not). Chose option (a) from the plan.
- **2.3 (verify) -- done.** `show-line`/`print-show` on `String`/`Vec` in a helper
  now render correctly (`HelloWorld`, `[1 2 3]`) WITH the Edge 2a void fix applied;
  full suite green (2243/0). Non-void `show` in a helper unaffected.

**Coupling note:** RC1 is user-visible only once the Edge 2a void-temp fix (Phase
1) lands -- otherwise the void wrapper call does not compile. Phase 1 is held
until RC2 lands (see below), so this pass committed RC1's routing alone (safe: the
branch keeps the loud void compile error, no carrier-scalar silent miscompile) and
the void fix + RC2 will land together next.

### Phase 2b -- RC2: fix the carrier-scalar clone-body dispatch [core] -- OPEN, root cause refined

**Status: OPEN; root cause refined and found DEEPER than first written.** RC2 is
NOT the direct-emitter `emit_reresolve_method_call` path (that correctly resolves
`String`/`Vec`). Pinned mechanism (instrumented 2026-07-21):

- For a carrier-scalar element (`cstr`), the wrapper's per-element clone
  (`show_line__spec__void_const_char`) is emitted COLORED -- it has a `__cps` body.
  The String/Vec clones are UNCOLORED (plain direct bodies), which is why their
  `(show x)` goes through the direct emitter and re-resolves correctly.
- Inside the cstr clone's CPS body, `(show x)` is a `CT_TAILCALL` whose callee is
  the elaboration-baked carrier rep `__inst_Show_show_int`. The CPS emitter's
  `callee_name` returns that baked name verbatim -- it does NOT run the per-spec
  method re-resolution the direct emitter does. So the body dispatches to
  `Show[int]` and prints the pointer. (Garbles in `main` too -- independent of the
  gate/routing; a routing change cannot fix it, the clone BODY is wrong.)
- The active ABI spec DOES carry the correct binding (`bindings[0] = {name:"a",
  type: TY_CSTR}`), but the CPS IR (`CTerm`) has DROPPED the dispatch `dict_arg`,
  so the callee name `__inst_Show_show_int` alone cannot be soundly re-targeted:
  it is indistinguishable from a genuine `(show 42)` int dispatch that happens to
  live in a spec with a non-int binding. A string-surgery re-target (`_int` ->
  `_cstr`) would MISCOMPILE such a genuine int dispatch.

**RC2 fix -- LANDED (2026-07-21), approach "thread the source Expr through the
CPS IR".** Chosen because it is the soundest of the three candidates (the
coloring-asymmetry lever was not needed):

- **2b.1 -- done.** Added `const Expr *call_expr` to the `CT_TAILCALL`/`CT_LETCALL`
  CTerm structs (`src/passes/cps_ir.h`) and populate it with the source `EX_CALL`
  at the call-lowering sites (`src/passes/cps_ir.c`). Exported
  `emit_reresolve_method_call` (`emit_core.c`/`emit_internal.h`). The CPS emitter's
  `CT_TAILCALL`/`CT_LETCALL` arms now call it on `call_expr`; when it returns a
  concrete instance name, that name is used and the call is FORCED onto the
  cps->direct path (the re-resolved instance method is uncolored). It self-gates
  (`emit_reresolve_disp_type` returns NULL unless the call is a genuine tyvar
  dispatch inside an active spec), so an ordinary `(show 42)` int dispatch is
  untouched -- this is why it is sound where a string-surgery re-target was not.
- **2b.2 -- done (falls out of 2b.1).** `emit_reresolve_method_call` already steers
  by the spec's `bindings[]` via `emit_reresolve_disp_type` -> the concrete element
  type -> `emit_inst_suffix_component` / `emit_concrete_inst_method_name`, so
  `cstr` -> `__inst_Show_show_cstr`, `bool` -> `__inst_Show_show_bool`, `int` stays
  `__inst_Show_show_int`. Re-resolved calls pass args RAW (not cast to the original
  carrier-rep param), since the spec-clone arg already has the concrete C repr.
- **2b.3 -- done, and it bites more than "lower priority".** The collision is not
  just two carrier-mangled types sharing a symbol; whenever SEVERAL scalar-element
  wrapper specs coexist (`int`+`bool`+`cstr`), `find_mono_clone_for_call` SKIPS the
  scalar arg in discrimination, so they all match (n_hit > 1) and the call falls to
  the carrier-rep BASE clone -- `(show-line true)` printed `1`. Fixed with an
  additive tie-breaker: when the primary (scalar-skipping) pass is ambiguous, a
  second pass compares scalar args too by concrete C type and uses the unique
  match. Additive -- it only runs on ambiguity and disqualifies a spec whose arg
  has no concrete atom type, so a genuinely carrier-erased scalar arg still yields
  NULL (no regression to non-wrapper generics).
- **2b.4 -- covered.** The fix is in the shared `CT_TAILCALL`/`CT_LETCALL` arms and
  `find_mono_clone_for_call`, so it applies to `print-show` and any `^Show a`
  wrapper automatically.

**Verified:** `show-line`/`print-show` on int/bool/cstr/String/Vec/Set/Map in a
helper AND in `main` all render correctly; the original report Edge 2 repro
renders correctly; full suite green (2243/0). Regression fixture
`tests/fixtures/show-wrapper-helper-dispatch/`. (A pre-existing, orthogonal
String-local drop leak -- the `string/from-cstr`/`concat` temporaries in a `let`
are not released -- reproduces identically in the direct `main` path; it is NOT a
dispatch issue and is out of scope here.)

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

### Phase 4 -- Edge 1: unresolved-element ICE through the wrapper -- LANDED

**Status: DONE (2026-07-21).** Root cause was simpler than feared and no
carrier-default substitution was needed:

- **4.1/4.2 -- done.** The direct `(show (vec-new))` path renders `[]` because in
  `main` there is no active ABI spec, so `emit_reresolve_disp_type` returns early
  and the elaboration-baked `__inst_Show_show_Vec` (a concrete-head receiver) is
  kept. Through the wrapper, the inner `(show x)` re-resolves inside the Vec clone
  spec, and `emit_reresolve_disp_type` resolves the dispatch type to `(Vec ?)` --
  a TY_APP with a CONCRETE HEAD but a tyvar element -- which tripped the
  deep-side `emit_abi_assert_routed_concrete` (Debug ICE) / would silently pick a
  carrier `__inst_*` (Release). Fix (`emit_core.c`): when the resolved dispatch
  type's spine carries a tyvar but its HEAD is concrete, return it as-is and let
  the caller's authoritative head-match (`emit_concrete_inst_method_name` ->
  `emit_inst_head_matches`, which matches `Show[Vec]` to `(Vec ?)` by head) resolve
  the instance, instead of asserting.  This is sound: a TY_APP dispatch type has
  NO carrier-rep suffix fallback (`emit_inst_suffix_component(TY_APP) == NULL`), so
  the re-resolution either matches by head or yields NULL (keep baked) -- it can
  never silently select the int rep.  If the HEAD itself is a tyvar/unknown there
  is nothing to select, so it keeps base/repr like the existing bare-tyvar bail.
  `type_spine_has_tyvar` was moved out of `#ifndef NDEBUG` so the bail runs in
  Release too (where the assertion is compiled out).
- **4.3 -- not needed.** The empty container renders correctly (`[]`, `#map{}`),
  matching the direct path, so no ascribe-hint diagnostic is required.

Verified: `(show-line (vec-new))` renders `[]`, `(show-line (hamt-of ...))` renders
`#map{}`; the Edge 2 cases are unaffected; full suite green. Fixture
`tests/fixtures/show-wrapper-empty-container/`.

### Phase 5 -- Fixtures, suite, docs -- LANDED

- **5.1 -- done.** `tests/fixtures/show-wrapper-helper-dispatch` covers RC1
  (Vec/Set/Map), RC2 (cstr), and 2b.3 (int/bool/cstr in one helper) in a helper
  `defn`; `tests/fixtures/show-wrapper-empty-container` covers Edge 1. (A single
  multi-type fixture replaced the per-type `show-wrapper-helper-<type>` sketch --
  same coverage, less churn.)
- **5.2 -- done.** The cstr wrapper-in-helper path is valgrind-clean. (The
  String-element cases carry a PRE-EXISTING, orthogonal leak -- the
  `string/from-cstr`/`concat` `let` temporaries are never released -- which
  reproduces identically in the direct `main` path, so it is not a wrapper/dispatch
  regression and is left to the separate String-local-drop work.)
- **5.3 -- done.** Full suite green (`bash tests/run.sh`) at each landing
  (2243/0 for Edge 2, 2244/0 for Edge 1). No `expected.c` snapshots are attached to
  the touched fixtures, so no regen was needed.
- **5.4 -- done.** Removed the interim workarounds now that the wrapper works
  directly: `show-collections` drops the `(:: (vec-new) (Vec int))` /
  `(:: (hamt-of) (Map int int))` ascriptions (`(vec-new)` / `(hamt-of)` render
  `[]` / `#map{}` directly); `string-basic` and `string-slice` replace the
  explicit `(let [__s (show x)] (do (println (string/to-cstr __s)) (string/release
  __s)))` form with a plain `(show-line x)`. All three are output-neutral (same
  `expected.stdout`). No `stdlib/typeclass-show.tur` docstring change was needed --
  the wrapper's public contract is unchanged; only its codegen was fixed.

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
- 2026-07-21 -- **Phase 2 (RC1) LANDED, staged.** `CT_TAILCALL` arm resolves the
  uncolored generic's DIRECT clone and routes cps->direct (option (a)). With the
  Edge 2a void fix temporarily applied, `show-line`/`print-show` on `String`/`Vec`
  in a helper render correctly; full suite green (2243/0). Committed RC1 routing
  ALONE (void fix held), so the branch keeps the safe loud void compile error and
  introduces no carrier-scalar silent miscompile. RC1 is therefore staged
  infrastructure until the void fix + RC2 land together.
- 2026-07-21 -- **RC2 root cause REFINED (deeper than first written).** Instrumented
  `emit_reresolve_method_call` (direct emitter) and the CPS `CT_TAILCALL`/`CT_LETCALL`
  arms. Findings (instrumentation removed afterward, nothing of it landed): the
  cstr wrapper clone is emitted COLORED, so its `(show x)` runs through the CPS
  emitter's `callee_name` (baked `__inst_Show_show_int`), NOT the direct emitter's
  per-spec `emit_reresolve_method_call` that correctly handles String/Vec. The CPS
  IR has dropped the `dict_arg`, so the baked callee name cannot be soundly
  re-targeted (indistinguishable from a genuine int dispatch). RC2 thus needs
  dispatch-info threaded through the CPS IR, or method re-resolution before CPS
  lowering, or removing the spurious coloring of the scalar clone -- see Phase 2b.1.
  The active spec DOES carry `bindings[0] = {a: TY_CSTR}`, so once the CPS side can
  re-resolve, steering to `__inst_Show_show_cstr` is straightforward.
- 2026-07-21 -- **EDGE 2 COMPLETE. Phase 1 (void) + RC2 + 2b.3 LANDED with RC1.**
  Implemented RC2 via the "thread the source Expr through the CPS IR" approach:
  added `CTerm.call_expr` (`cps_ir.h`), populate at lowering (`cps_ir.c`), exported
  `emit_reresolve_method_call`, and call it from the CPS `CT_TAILCALL`/`CT_LETCALL`
  arms (forcing cps->direct + raw args for a re-resolved instance callee). Re-added
  the Edge 2a void bare-call. Discovered 2b.3 bites harder than expected -- multiple
  scalar-element specs make `find_mono_clone_for_call` non-unique (skips scalar
  args) so `(show-line true)` fell to the base clone and printed `1`; fixed with an
  additive scalar-comparing tie-break pass. All element types now render correctly
  in helper and `main`; full suite green (2243/0); fixture
  `show-wrapper-helper-dispatch` added. Only Phase 4 (Edge 1 ICE) remains. Noted a
  pre-existing, orthogonal String-local-drop leak (reproduces in the direct `main`
  path too), out of scope for this dispatch work.
- 2026-07-21 -- **EDGE 1 (Phase 4) LANDED.** `(show-line (vec-new))` no longer
  ICEs. In `emit_reresolve_disp_type`, a dispatch type with a concrete HEAD but a
  tyvar element (`(Vec ?)` from an empty container) is now returned as-is so the
  caller's authoritative head-match resolves `Show[Vec]`, instead of tripping the
  R3 assertion; a TY_APP has no carrier-rep suffix fallback so this cannot silently
  misdispatch (matches by head or keeps baked). `type_spine_has_tyvar` moved out of
  `#ifndef NDEBUG` so the bail also runs in Release. Renders `[]` / `#map{}` like
  the direct path; Edge 2 unaffected; full suite green. Fixture
  `show-wrapper-empty-container`. Both edges of the original report are now fixed.
- 2026-07-21 -- **Phase 5.4 cleanup LANDED.** Removed the interim workarounds the
  wrapper bugs had forced: `show-collections` now passes `(vec-new)` / `(hamt-of)`
  unascribed; `string-basic` / `string-slice` use `(show-line x)` instead of the
  explicit `show`+`to-cstr`+`release` form. All output-neutral (same
  `expected.stdout`), suite green. Also removed a stale untracked
  `cps-void-show-wrapper-midbody` fixture dir (leftover run-artifacts from the
  reverted Edge 2a commit). Plan complete except the optional Phase 3 guardrail.
