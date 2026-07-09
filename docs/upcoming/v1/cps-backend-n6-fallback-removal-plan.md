---
title: "CPS backend N6 (gate item 7): remove the whole-function fallback -- readiness, measurement, and phased plan"
status: planning
description: Graduation gate item 7 makes the CPS backend the SOLE lowering for colored (may-capture) functions -- no CT_UNSUPPORTED whole-function bail-out, no direct-vs-CPS dual path. This document measures the current fallback surface (which colored functions still bail out and on which forms), shows that removing the fallback today would turn 400+ sites into hard errors, and lays out the phased path: (1) delegate every control-op-free, colored-call-free subexpression to the direct emitter (the big coverage lever, reusing CT_LETRAW), then (2) handle the remaining control-carrying forms (multi-case handle, shift0, cloneable/serial reset, async, capturing/multi-shot continuations), then (3) delete the fallback and turn any residual into a hard error with a form-named diagnostic.
---

## Why this exists

The `cps-backend` graduation gate
([cps-backend-non-scalar-values-plan.md](cps-backend-non-scalar-values-plan.md#graduation-gate----what-must-hold-before-cps-backend-goes-always-on),
item 7 / N6) requires deleting the whole-function fallback: a colored function is
emitted through the CPS backend, full stop, with no second lowering to fall back
to. Any form that still cannot be emitted then becomes a hard compiler error, not
a silent reroute to the fiber/direct path.

Every other gate item is closed (Tier A/B/C, narrow-int, struct/ADT locals,
owning pointers re-scoped to item 4). N6 is the terminal item -- and by far the
largest, because it is gated on **100% form coverage for colored functions**, not
on any single value representation.

## Measurement -- the current fallback surface

Instrumenting the `CT_UNSUPPORTED` catch-all with the offending `Expr` kind and
running `--dump-cps emit-c` over every fixture that uses a control op
(`perform`/`handle`/`shift`/`reset`/`defeffect`) tallies the reasons a colored
function's body leaves the CPS subset:

| Reason | Count | Meaning |
| --- | ---: | --- |
| `form not in CPS2 subset` | 402 | an `Expr` kind the translator's switch does not handle |
| `indirect call` | 6 | `EX_CALL` with no resolvable `fn_binding` (a call through a value) |
| `handle: only single-case handlers` | 3 | a multi-case `handle` |

Breaking down the 402 "form not in subset" by `Expr` kind:

| Kind | Count | Note |
| --- | ---: | --- |
| `EX_FN_TO_FAT` | 384 | a bare fn wrapped as a fat closure (higher-order stdlib: `hamt/map`, `hamt/filter`, ... -- colored because they take an fn arg that *might* perform) |
| `EX_PANIC` | 4 | `(panic ...)` |
| `EX_CLOSURE` | 4 | a closure literal |
| `EX_SHIFT0` | 2 | `shift0` (the other delimiter) |
| `EX_REF` | 2 | `(ref x)` owning-reference constructor |
| `EX_INLINE_C` | 2 | an inline-C block |
| `EX_SET` | 2 | `(set! ...)` |
| `EX_ASYNC` | 1 | `(async ...)` |
| `EX_DEFER` | 1 | `(defer ...)` |

The translator (`src/passes/cps_ir.c`, `cps_bind` / `cps_tail`) currently handles
only 11 `Expr` kinds: `EX_BUILTIN`, `EX_CALL`, `EX_DO`, `EX_HANDLE`, `EX_IF`,
`EX_LET`, `EX_PERFORM`, `EX_RESET`, `EX_RESUME`, `EX_RETURN`, `EX_SHIFT` -- plus
atoms and the `CT_LETRAW`-delegated leaf ops (`make-struct` / `.field` /
`default-of` / `rc/*`, and cps->direct calls with atomic args). Everything else
falls back.

**Conclusion: N6 is not a safe deletion today.** Removing the fallback now would
turn all 411 sites into hard errors -- the suite would fail to compile broadly
(the `EX_FN_TO_FAT` count alone spans the higher-order stdlib that any effect
program pulls in). The fallback must stay until coverage is complete.

### Re-measurement (after the N6.1-N6.3 slices + indirect calls + nil return)

Re-tallying the fallback reasons for every *colored* top-level function across
the 311 control-op fixtures (a temporary `TUR_CPS_MEASURE` instrument; counts are
inflated by per-fixture stdlib re-emission but the relative shape is what
matters), the surface has shifted decisively from "form not in subset" to
**signature rejection** (`fn_sig_ok`): the body-form coverage from the delegation
+ capturing-continuation work absorbed almost all of the old 402. What remains is
dominated by the *types in the signature*:

| Reason | ~Count | Meaning |
| --- | ---: | --- |
| `sig-param TY_APP` | 4352 | a parametric type-app parameter (`option<T>`, `list<T>`, `Cons<int>`, ...) |
| `sig-param TY_FN` | 2504 | a plain (non-rank-2) function-typed parameter |
| `sig-ret TY_APP` | 625 | a parametric type-app return |
| `sig-ret TY_NIL` | 2433 -> 206 | a nil/void return -- **now landed** (the residual 206 also have a bad param) |
| `core:*` (nested perform, ...) | ~50 | genuine control-carrying gaps (small) |

So the next big levers are all *signature* types: parametric type-app params /
returns (the non-scalar-parametric Tier-C work) and plain `TY_FN` params. The
control-carrying core gaps (nested sequential performs, resuming shift bodies,
cloneable/serial, async) are now a small tail by count -- and the hardest.

**Caveat found while scoping these (and a latent miscompile fixed).** The raw
`TY_APP` counts overstate the *real* remaining surface, in two ways:

- A **concrete** parametric type-app is often already handled. `(Option int)`
  monomorphizes to a *by-value* owning-free ADT (`tur_adt_Option__int`) and rides
  the existing Tier C by-value box path -- a colored function taking/returning it
  already CPS-emits correctly (verified: `direct == cps`). Much of the measured
  `sig-param TY_APP` count is *generic templates* (`(Option A)` with a type
  variable, which is carrier-ABI only pre-monomorphization); their monomorphs
  emit. So the true parametric surface is smaller than the raw count.
- A **heap-passed** parametric ADT (`(Vec int)` -> `tur_adt_Vec__int *`) is a
  different case, and it exposed a **pre-existing miscompile**: a heap ADT def
  still has product *shape*, so `type_is_byvalue_adt_product` reported true and
  `slot_box_ty` wrongly classified it as a boxable Tier C value. `fn_sig_ok`
  admitted the function and the return crossing boxed/unboxed the handle
  (`*(T**)__r; free(...)`), double-indirecting it -- a colored `mkvec` returned a
  garbage `vec-len` of 0. **Fixed** by excluding `type_is_heap_adt` /
  `type_is_heap_struct` from `slot_box_ty`, so a heap ADT is never boxed.
  **Heap-handle-in-subset support then LANDED** on top of that: a heap ADT/struct
  is admitted as a plain-cast int64 *carrier* handle (`carrier_handle_ok` in
  `slot_ok_t`, `slot_load` casting via `binder_ctype_full` back to the pointer
  type), so a colored function that produces/returns/threads such a handle
  through a straight-line delegated sequence CPS-emits and moves it correctly
  (`mkvec` now emits, `direct == cps == 1`). The safety property is that
  `carrier_handle_ok` is added to `slot_ok_t` but NOT to `cap_ty_ok`: a handle
  live *across* a control op is captured into a leaked, possibly multi-shot
  continuation env, where sharing the owning pointer would be unsound -- so
  `cap_add` still bails on it and the function falls back (verified: `addto`,
  whose `Vec` param is used after a `handle`, stays fallback). Regression fixture
  `cps-backend-heap-adt-return`. Surfaced and required a separate fix along the
  way: a nil/void *delegated call* (`vec-push!`) was dropping its side effect
  (emit_letraw discarded the un-emitted void-call expression) -- fixed with
  fixture `cps-backend-nil-delegated-call`.

  *Remaining signature surface:* a heap handle *consumed across* a control op
  (still falls back by design -- needs the affine/single-shot ownership analysis,
  same gate as the consumed-owning-capture item), and effectful `TY_FN` callback
  params (colored indirect calls).

## The key architectural lever -- delegate control-op-free subexpressions

Most of the surface is not control flow at all: `EX_FN_TO_FAT`, `EX_CLOSURE`,
`EX_PANIC`, `EX_REF`, `EX_INLINE_C`, `EX_SET`, `EX_DEFER` -- none of these thread
a continuation. They fall back only because the CPS translator's switch does not
enumerate them.

The N3 work already established the pattern for this: `CT_LETRAW` delegates a
subexpression's emission to the direct emitter (`emit_value`), binding its result
as a local. It is used today for `make-struct` / `.field` / `rc` ops. **Generalize
it**: any subexpression that

1. contains no syntactic control op (`perform`/`handle`/`resume`/`shift`/`shift0`
   /`reset`/`cloneable-*`/`serial-*`/`discontinue`), **and**
2. contains no call to a *colored* function (a `cps->direct` call to an
   *uncolored* callee is fine -- it already is delegated; a colored callee must
   thread the continuation, so it cannot be delegated wholesale), **and**
3. is not itself a nested fn/closure body (a call-graph boundary -- colored on its
   own merits),

can be emitted by the direct emitter via `CT_LETRAW`, exactly like `make-struct`.
This collapses `EX_FN_TO_FAT`, `EX_CLOSURE`, `EX_PANIC`, `EX_REF`, `EX_SET`,
`EX_DEFER`, `EX_INLINE_C`, `EX_MATCH`, `EX_FOR`, `EX_WHILE`, literals, casts, etc.
into delegated locals in one move -- the single biggest coverage step.

### Safety conditions (why the predicate is conservative)

- **Condition 2 (no colored call) is load-bearing.** If a delegated subexpression
  called a colored function that performs effect `E` handled by an *enclosing*
  handler in the current function, the direct emitter would call it through its
  entry wrapper (a fresh DK root), so the `perform` would not reach the enclosing
  DK handler -- the machine-split crash. The existing `cps->direct` delegation
  already respects this (`callee_colored` gates `CT_TAILCALL` vs delegation); the
  general predicate must too.
- **The scan must be sound, so its default is "not delegatable."** There is no
  generic `Expr` child-walker in the tree, so the predicate enumerates the safe
  composite forms and recurses into their children; any un-enumerated kind is
  conservatively treated as not-delegatable (it falls back, as today). This can
  only *under*-delegate (safe), never over-delegate (unsound).
- **Captures inside lifted bodies still gate.** A delegated subexpression that
  references enclosing locals is fine in the main function body (they are C locals
  in scope) but is rejected by the existing `has_capture` zero-capture cut inside
  a lifted reset/shift/handler helper -- so it falls back there, as today.

This step is **fallback-safe and blast-radius-contained**: the CPS backend only
activates under `--enable=cps-backend`, so growing coverage only changes the
`cps-backend-*` fixtures, never the default suite. It is worth landing on its own
(more colored functions CPS-emit) well before N6.

## Remaining control-carrying gaps (after delegation)

These genuinely thread a continuation and need real CPS handling before the
fallback can be deleted:

- **Multi-case `handle`** (`handle: only single-case handlers`) -- the translator
  admits one case; a `dk_handler` keyed by effect tag needs to dispatch N cases.
- **`shift0`** and the **cloneable / serial** reset/shift variants -- other
  delimiters with distinct prompt semantics.
- **`async`** -- scheduler-backed continuation capture.
- **Capturing / multi-shot continuations** -- the C3/C4 subset is zero-capture
  abortive + single resume; a handler case or shift body that captures other
  locals, or a genuinely multi-shot resume, must lift with a real env (not the
  zero-capture `k`-as-env shortcut) and copy correctly.
- **Indirect calls** (`indirect call`) -- a call through an fn value whose
  coloring is unknown; needs a conservative "treat as colored" thread or a
  runtime bridge.
- **Non-tail cps->cps with a capturing join** -- the heap-join landed for the
  zero-capture direct-body form; a capturing join needs an env-carrying frame
  (also the `docs/reported/cps-backend-fallback-intermediary-splits-effect-chain.md`
  residual).

## Phased plan

- **N6.1 -- general delegation** (recommended first; fallback-safe, contained).
  Add `is_delegatable_general(b, e)` (conditions above) and route it through
  `CT_LETRAW` in `cps_bind`/`cps_tail` ahead of the `CT_UNSUPPORTED` default.
  Round-trip fixtures: a colored function whose body wraps an effect in a
  `match` / `while` / closure, `direct == cps`. Re-measure the surface.

  **Started -- capture-free fn-values landed.** `is_delegatable_value` (bare fn /
  `EX_FN_TO_FAT` / `EX_POLY_WRAP`, and an `EX_CLOSURE` with `n_captures == 0`) now
  delegates through `CT_LETRAW` in both `cps_bind` and `cps_tail`. A capture-free
  fn value references no enclosing local, so it is sound to delegate anywhere
  (including a lifted zero-capture body); a capturing closure still falls back
  (its free vars need the `has_capture` cut -- that is N6.3). Fixture
  `cps-backend-closure-local`: a colored `f` builds a `Box` holding
  `(fn [n] (+ n 1))` in its perform continuation; the closure delegates and `f`
  CPS-emits (`direct == cps == 10`) where it previously fell back.

  **General control-op-free delegation landed (main-body).** `safe_to_delegate`
  (a sound recursive predicate: control-op-free AND no colored/indirect call,
  with `default: false` for any unrecognized form) now routes `match` / `while` /
  `do` / `let` / `if` / `set!` / `cast` / `deref` / `get-field` / `make-struct` /
  builtins / uncolored calls through `CT_LETRAW` in the `cps_bind`/`cps_tail`
  *default* cases (working paths untouched). Re-measuring the corpus, the "form
  not in subset" surface collapsed from **402 to 18** (the 384 `EX_FN_TO_FAT`
  from the fn-value slice; the rest from composite delegation). `has_capture`'s
  `CT_LETRAW` scan was made **sound**: it scans fn-value captures precisely and
  treats any un-enumerated delegated operand as capturing (`default: return
  true`), so a delegated composite is admitted only in the main function body
  (where `has_capture` is not the gate), never in a lifted zero-capture body it
  could not close over. Fixture `cps-backend-delegate-match`: a colored `f`
  performs `Put` with a `match` argument -- the match delegates and `f` CPS-emits
  (`direct == cps == 105`). Full suite: 2010 passed, 0 failed.

  **Remaining N6.1:** delegated composites in *lifted continuation* positions
  (free vars flowing into a perform/shift/handler body) still fall back -- that
  needs the free-var-aware capture handling of N6.3.

  **Entanglement to plan for (found while scoping):** a delegatable form in a
  colored function most often sits in a *continuation* -- the body of a
  `perform` / `shift` / `reset` / handler case -- not the straight-line main
  body. Those positions are gated by `perform_body_ok` / `shift_body_ok` /
  `reset_body_ok`, which admit only straight-line `letval`/`letprim`/`letcall`/
  `letraw`/`if` today. So N6.1 is not just "route delegatable forms through
  `CT_LETRAW`": the lifted-body subset predicates must also admit the delegated
  `CT_LETRAW` in those positions (they already admit `CT_LETRAW` for the leaf
  ops, so this is widening the operand set, not a new node), **and** the
  `has_capture` zero-capture cut must be satisfied or lifted -- a delegated
  `while`/`match` that references other locals inside a lifted body captures
  them, which the current cut rejects (that is the N6.3 capturing-continuation
  work). Net: N6.1 delivers real coverage in the *main body* immediately, and
  full continuation-position coverage lands together with N6.3.
- **N6.2 -- multi-case handle + shift0.** *Multi-case handle LANDED.* `CT_HANDLE`
  now holds an array of `CHandleCase` (effect / params / k / body) instead of a
  single clause; `build_handle` translates all cases, and `emit_handle` installs
  one `dk_handler` per case chained by effect tag onto the handle continuation
  (`dk_handler(tag0, c0, 0, dk_handler(tag1, c1, 0, dk_frame(k-cont)))`), so
  `dk_perform` dispatches each effect to its own lifted case. term_core_ok /
  has_capture_case / needs_heap_join iterate all cases; each case still admits
  `<=1` effect param and a zero-capture straight-line body. Fixture
  `cps-backend-multi-handle`: `run` handles Ask + Add, `direct == cps`
  (`5` / `101`).

  *shift0 LANDED.* `shift0` differs from `shift` in exactly one way -- it does
  NOT reinstall the delimiting prompt when it captures the continuation -- so the
  CPS IR reuses the `CT_SHIFT` node with a `shift0` flag (set by the `EX_SHIFT0`
  arms in `cps_bind`/`cps_tail`, which share `cps_shift_body_kf` with plain
  shift), and `emit_shift` lowers it to `dk_shift0` instead of `dk_shift`. The
  lifted shift-body helper, its capture env, and the receiver application are all
  shared with plain shift, so shift0 automatically inherits the N6.3 capturing-
  shift-body support. Fixture `cps-backend-shift0`: nested resets, `deep`'s
  shift0 delimited by `deep`'s reset under `outer`'s reset, `direct == cps`
  (`105`).

  *Remaining:* the cloneable/serial reset/shift variants (deep-clone capture
  environments / serialization -- N6.4); and multi-case handlers whose body
  performs *multiple sequential* effects (a nested perform in a perform
  continuation) still fall back -- that is the perform-continuation-can-perform
  widening, adjacent to N6.3.
- **N6.3 -- capturing / multi-shot continuations** -- lift with a real env;
  removes the zero-capture cut's fallbacks. *Started -- capturing PERFORM
  continuations (scalar source captures) landed.* The `LH_PERFORM_CONT` frame's
  env slot (previously unused / `0`) now carries a heap struct of the
  continuation's captured scalar source vars: `collect_caps` gathers them
  (exhaustive-or-bails, mirroring `has_capture_rec`; bails on a captured CPS var,
  a non-scalar capture, or a delegated/nested-control body), `emit_perform`
  allocates + populates a `<helper>_env` struct and passes it as the frame env,
  and the lifted helper reads the captures back into real-typed locals named via
  `name_for_binding`. Scalar captures are Copy, so the shared env is
  multi-shot-safe (read-only) and leaked with the DK nodes. `term_core_ok`'s
  perform check uses `collect_caps` instead of `!has_capture`. Fixture
  `cps-backend-capture-perform`: `(+ x (perform E))` now CPS-emits
  (`direct == cps == 107`), single and multi-scalar captures verified.

  *Capturing RESET / HANDLE continuations landed too.* The `LH_RESET_CONT` env
  slot (which already carries the enclosing continuation `k`) now widens to a
  `{ DK *__k; <captures...> }` struct when the continuation captures scalar source
  vars: `emit_reset` / `emit_handle` collect the caps, and `emit_cont_env`
  allocates + populates the struct (`__k = k`, `fN = capN`) and passes it as the
  frame env (was plain `(intptr_t)k`); the helper reads `k` and the captures back
  from it. `term_core_ok`'s CT_RESET / CT_HANDLE checks use `collect_caps` instead
  of `!has_capture`. Fixture `cps-backend-capture-handle`:
  `(+ x (handle (g) ...))` -- the continuation after the handle captures `x` --
  CPS-emits (`direct == cps == 107`); a capturing `reset` continuation verified
  the same way. Full suite: 2013 passed, 0 failed.

  *Captured CPS vars (intermediate results) landed too.* The collector now
  handles a captured CPS result var (`(+ (+ x y) (perform E))` captures the fresh
  `(+ x y)` result) as well as source-var captures: a fresh CPS var is named the
  same way (`tN`) at its declaration and every reference, so `CapSet` carries it
  by name and it rides the continuation env like a source capture. Fixture
  `cps-backend-capture-intermediate` (`direct == cps == 107`).

  *Capturing HANDLER cases landed too.* A handler case body that references an
  enclosing local -- e.g. `run`'s case `(resume k (+ base 1))` captures the
  parameter `base` -- is now lifted with an env carrying its scalar source
  captures. Two pieces: `collect_caps_case` gathers the case body's captures
  while excluding the case's own params + `k` (bound by the DKHandler signature),
  and `emit_cont_env` grew a k-less mode (`k_expr == NULL`) so a handler-case env
  is `{ f0; ... }` with no `__k` slot; the LH_HANDLER_CASE helper reads its
  captures back the same way LH_RESET_CONT does. The collector also grew a
  `CT_RESUME` arm -- a *resuming* case body (the common shape) previously hit the
  `default:` bail and fell back; now `resume k v` threads its operands and binds
  its result var like any other node. Fixture `cps-backend-capture-handler-case`:
  `(handle (work) (E [] k) (resume k (+ base 1)))` CPS-emits
  (`direct == cps == 41`). Full suite: 2016 passed, 0 failed.

  *Capturing SHIFT bodies landed too.* A shift's delimited body that references
  an enclosing local -- e.g. `inner`'s body `(+ x 5)` captures the parameter `x`
  -- is now lifted with its scalar captures in the LH_SHIFT_BODY helper's env,
  exactly as the reset/handle continuations are:
  `typedef struct { int64_t f0; } inner_s0_env;` with `dk_shift(1, inner_s0,
  (intptr_t)e, k)` and the helper reading `int64_t x = e->f0;`. The captured
  continuation is excluded from the env (it is the shift's `k`, delivered as the
  DK* `subk` arg, not an env slot); this admits abortive capturing shift bodies
  (the common shape) that discard `subk`. `emit_shift` collects the body's caps
  (excluding `shift.k`), builds a k-less env via `emit_cont_env`, and passes it
  as the `dk_shift` env. Fixture `cps-backend-capture-shift-body`:
  `(shift (fn [v] v) (+ x 5))` delimited by an outer `reset` CPS-emits
  (`direct == cps == 105`). Full suite: 2017 passed, 0 failed.

  *Non-scalar (by-value aggregate) captures landed too.* A continuation that
  captures an owning-free by-value product -- e.g. `f`'s post-handle continuation
  `(+ r (.second p))` captures the parameter `p : Pr` -- now rides the env by
  value instead of falling back. Such a value is Copy (`slot_box_ty`: no drop
  glue), so it needs no retain/drop even though the env is leaked and the
  continuation may be multi-shot; each read is an independent value copy. Two
  pieces: `CapSet` now carries the full `Type` alongside the `TypeKind`, so the
  env field is emitted via `binder_ctype_full` (`tur_adt_Pr f0;`) and `cap_add`
  admits a `slot_box_ty` aggregate; and `collect_caps_rec` grew a `CT_LETRAW` arm
  that gathers a delegated op's operand vars (the same enumeration
  `has_capture_rec` walks -- rc/of, get-field, make-struct, call, closure
  captures), so a lifted body containing a delegated op that captures a Copy
  value is admitted instead of hitting the collector's default bail. Fixture
  `cps-backend-capture-nonscalar`:
  `(let [r (handle ...)] (+ r (.second p)))` CPS-emits (`direct == cps == 103`).
  Full suite: 2018 passed, 0 failed.

  *Fat-closure (fn-value) captures landed too.* A continuation that captures a
  rank-2 fn-value parameter and calls it -- e.g. `f`'s post-handle continuation
  `(fnv r)` through the parameter `fnv : (fn [int] int)` -- now rides the env by
  value. A fn value is a fat closure (`tur_poly_fn_t` = `{ fn ptr; env ptr }`):
  16 bytes, wider than the scalar slot, but **Copy** -- a borrowed function value
  the callee never drops (the direct emitter emits no drop for a fn-value param;
  its owner outlives the call), so it needs no retain/drop even under a leaked
  multi-shot env. `CapSet` grew a `polyfn` flag; `cap_add` admits an `is_poly_fn`
  binding with `tur_poly_fn_t` as the env-field type (`cap_ctype`); and
  `collect_caps_rec`'s `CT_LETRAW` `EX_CALL` arm captures the callee value
  (fn_binding- or fn_expr-carried) instead of bailing. Fixture
  `cps-backend-capture-fnvalue`:
  `(let [r (handle ...)] (fnv r))` CPS-emits (`direct == cps == 100`). This also
  **closes the lifted-position indirect-call residual** left by the N6.4
  indirect-call slice -- a captured fat-closure callee no longer forces fallback.
  Full suite: 2021 passed, 0 failed.

  *Borrowed (`^borrow`) owning captures landed.* An owning value (an rc handle,
  a drop-glue by-value aggregate) that fails the Copy slot gate is still safe to
  capture *by value* when its binding is `^borrow`: the type checker guarantees
  the callee only reads it -- never drops or moves it -- so the shared shallow
  copy in the (leaked, possibly multi-shot) env is never released by this
  function, giving no double-free and a refcount identical to the direct path.
  `cap_add` admits an `is_borrow` binding; `fn_sig_ok` admits an `is_borrow`
  *parameter* even when its type fails the slot gate. This is the sound
  admission signal -- a type-system guarantee, not a fragile whole-function
  consumption scan. Fixture `cps-backend-capture-borrow`: `f`'s post-handle
  continuation `(+ v (.tag o))` captures the `^borrow o : Own` (a struct with an
  `rc<int>` field) by value (`tur_adt_Own f0`), `direct == cps == 59`. Full
  suite: 2022 passed, 0 failed.

  *Remaining N6.3 (each root-caused; see the linked report):*

  - **Owning (non-Copy) captures without `^borrow`** -- carrier ADTs / rc handles
    / by-value products with drop glue that the function *does* consume. The
    blocker is not just the retain: the drop-insertion pass sinks the captured
    value's `rc/drop` *into the continuation body*, so a single clone-on-capture
    into the leaked, multi-shot env is dropped once per resume -> underflow.
    Needs a single-shot/affine proof or per-shot cloning. `collect_caps`
    correctly bails today (so it is sound, just conservative); the `^borrow`
    slice above is the safe subset that needs no such analysis. See
    [docs/reported/cps-backend-owning-capture-multishot-double-free.md](../../reported/cps-backend-owning-capture-multishot-double-free.md).
  - **Resuming SHIFT bodies.** The current shift lowering (`cps_shift_body`)
    applies the receiver to the *body value* and delivers to the prompt; the
    captured continuation `subk` is passed to the shift-body helper but ignored
    (abortive only). A shift whose receiver actually invokes the continuation
    needs a different lowering that binds `subk` and threads it into the
    receiver -- not expressible in the receiver-applied-to-body form.
- **N6.4 -- the long tail** -- cloneable/serial reset, async, indirect calls, per
  the re-measured surface.

  - **Indirect calls (main body) LANDED.** A call through a fn *value* (a
    NULL-binding indirect call, or a call through a fn-value parameter) is the
    callee's direct entry point, which installs its own root prompt and never
    joins the caller's delimited-control chain -- so it is sound to delegate to
    the direct emitter via `CT_LETRAW`, exactly as an uncolored direct call is.
    The blocker was a fn-value *parameter* spelling divergence: the CPS backend
    typed it `void *` / id-suffixed while the direct emitter (which generates the
    delegated body, `fnv.fn` / `fnv.env`) uses `tur_poly_fn_t` / bare source
    name. Fixed by (1) setting `ctx->fn_params` during CPS emission so every
    parameter reference resolves to the raw id-less name, (2) emitting a rank-2
    poly fn param as `tur_poly_fn_t` in `emit_params`, and (3) a candidacy guard
    (`param_name_clashes_cps`) excluding a function whose param raw name collides
    with a CPS-synthesized identifier (`k` / `t<N>` / `__*`). Fixture
    `cps-backend-indirect-call` (`direct == cps == 70`). Full suite: 2020 passed,
    0 failed. *Lifted-position indirect calls* (callee captured into a
    continuation env) subsequently landed too, via the fat-closure-capture slice
    above (`cps-backend-capture-fnvalue`) -- a captured `tur_poly_fn_t` rides the
    env by value. See
    [docs/archive/cps-backend-indirect-call-fatclosure-param-divergence.md](../../archive/cps-backend-indirect-call-fatclosure-param-divergence.md).
  - **cloneable / serial** reset/shift need the DK deep-clone (`dk_copy` exists
    in the runtime) plus the direct emitter's cloneable capture-environment glue
    (`live_captures` / `capture_clone_fns` / `capture_drop_fns`), which is
    direct-path-specific today.
  - **async** needs scheduler-backed continuation capture.
- **Signature widening (the dominant remaining surface).** The re-measurement
  above shows `fn_sig_ok` -- not body-form coverage -- is now the main gate.
  - **Nil/void return LANDED.** `fn_sig_ok` admits a `TY_NIL` return; the CPS
    body already delivers a unit (0) to the return continuation, and the
    direct->cps entry wrapper is emitted `void` (matching the direct emitter's
    forward decl) and discards that unit rather than returning an int64. Fixture
    `cps-backend-void-return` (`f : void` performs E, `direct == cps`, both print
    `7`). This alone cleared ~2200 of the measured sig-return fallbacks
    (void-returning loggers / writers / `record!`-style effects are common).
    Full suite: 2023 passed, 0 failed.
  - **Remaining:** parametric type-app (`TY_APP`) params / returns -- the
    non-scalar-parametric Tier-C work (carrier-ABI vs by-value apps) -- and plain
    `TY_FN` params (a non-rank-2 function pointer, passed as `int64_t` / a cfnptr
    typedef in the direct emitter; admit it the way `is_poly_fn` params are).
- **N6.5 -- delete the fallback.** Remove the `CT_UNSUPPORTED` whole-function
  bail-out and the direct-vs-CPS dual path from `emit_cps_ir.c` / the classifier.
  Any residual form becomes a hard error; give it a **form-named diagnostic**
  (the measurement patch that annotates `CT_UNSUPPORTED` with the `Expr` kind is
  the seed for this). Re-run the full suite and the sign-off probe with the
  fallback gone.

Only after N6.5 does `cps-backend` satisfy gate item 7. Until then the fallback
stays and coverage grows monotonically under the flag.

## Depends on / reuses

- `CT_LETRAW` delegation + `emit_value` (N3) -- the mechanism N6.1 generalizes.
- `callee_colored` (`cps_ir.c`) -- the colored-call gate for the delegation
  predicate.
- `cps_directly_uses_control` (`cps.c`) -- the control-op seed enumeration to
  mirror (but with a sound, not-delegatable default).
- Parent: [cps-backend-non-scalar-values-plan.md](cps-backend-non-scalar-values-plan.md)
  (the graduation gate), [cps-ir-to-c-backend-plan.md](cps-ir-to-c-backend-plan.md)
  (the C1-C6 backend).

## Out of scope

- Uncolored functions -- they are never CPS-emitted; N6 is only about colored
  functions.
- Owning-field aggregate / carrier crossings -- gate item 4, not N6.
