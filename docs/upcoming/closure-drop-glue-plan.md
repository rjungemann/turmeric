# Closure env drop glue -- freeing captured fat-closure environments

**Status:** S1.2 (borrowed HOF-arg free) LANDED; the rest of S1 (owning-capture
walk-glue + capture-clone) and S2 remain. Prepared from
`docs/reported/escaping-fat-closure-env-leak.md` and the B2 residuals in
`cps-runtime-finish-plan.md` (Progress-log PD).

> **Progress note (2026-07-21) -- local fn-field struct drop LANDED (direct
> path); the "Remaining S2 gap" below is closed for uncolored functions.** A
> by-value struct local that owns a BOXED fn-field now frees that heap fat handle
> at scope exit. Mechanism:
>
> - `elab_forms.c` flags the local (`Binding.drops_fn_fields`) when it passes the
>   SAME moved / consumed / escape guards that admit the existing rc/ref
>   `byvalue-struct-field-leak` auto-drop (`elab_field_is_boxed_fnfield` +
>   `is_binding_consumed` / `is_field_consumed` / `binding_moved_during_init`), so
>   a struct that escapes (returned / moved / consumed) is never flagged.
> - The DIRECT emitter (`emit_let_value`) frees the box via a new
>   `drop_fnfields_<T>(&local)` glue (`emit_module.c`) -- fn-fields ONLY (rc/ref
>   are still discharged by the injected `(defer (drop! (.f o)))`), and NO
>   `free(&local)` (the struct is stack-resident).
>
> Crucially this is **not** a `(defer (drop! (.fn o)))`: an fn-field-drop defer
> reads a fat-fn field that the CPS/DK backend's continuation-capture admission
> rejects, evicting a COLORED fn to the retired direct/fiber path (hard build
> failure -- reproduced on `cps-backend-closure-local` with the defer approach).
> Emitting the free directly in the direct emitter leaves colored functions
> untouched: CPS lowering never runs `emit_let_value`, so a fn-field box in a
> colored fn leaks exactly as it did before local drops existed (no regression,
> no eviction). Uncolored functions release it.
>
> Verified valgrind-clean (definitely-lost 0, exactly-once free, no double-free)
> for: pure-fn-field local (call + drop), mixed rc+fn-field local (rc via defer +
> fn via emit), capturing-closure env box, and the escape case (a returned struct
> is NOT dropped by the producing fn). Suite 2220/0 (one snapshot regenerated:
> `defstruct-field-arrow`, whose local `Cell` fn-field box now frees). Fixture
> `local-struct-fnfield-drop`.
>
> **Still open:** (1) a fn-field box in a COLORED function still leaks (needs the
> CPS backend to admit an fn-field auto-drop, or a scalar-box-pointer capture
> form). (2) A boxed fn-field holding a capturing env with OWNING captures leaks
> those captures (only the env box is freed) -- the S1 walk-glue work. (3)
> Pre-existing, orthogonal: reading an rc field into a var (`(let [s (.r o)] ...)`)
> double-frees the control block -- the field read aliases without an incref while
> both `o`'s field-drop and `s`'s rc-drop decrement it. Filed separately.
>
> **Progress note (2026-07-20f) -- S2 Model U drop glue + move landed for
> fn-fields (rc-wrapped path verified sound).** A boxed fn-field is now an owning
> field: `resolve_ctor_field` sets `needs_drop_glue`, so the holding struct's
> by-value drop glue `free`s the field's heap fat handle (a capturing env, or the
> `{shim, fn}` box for a bare fn). Storing a CAPTURING closure variable into such
> a field MOVES it (`binding_mark_moved` in the constructor arg loop), so aliasing
> -- the same closure in two structs, which valgrind confirmed double-frees the
> shared env -- is now a compile-time `use-after-move` error. A thin fn re-shims
> to a FRESH box per store and an inline closure has no source, so neither is
> consumed. Verified valgrind-clean (0 errors, exactly-once free) for rc-wrapped
> closure structs, thin-fn structs, and rc-cloned structs; the struct-copy path is
> compile-rejected by rc uniqueness. Suite 2219/0 (one snapshot regenerated:
> defstruct-field-arrow). Fixtures `capturing-closure-struct-field` (store+call)
> and `errors/closure-struct-field-move` (aliasing rejected).
>
> **Remaining S2 gap (NOT closure-specific):** a LOCAL by-value fn-field struct
> does not invoke its drop glue at scope exit -- the same local-owning-value drop
> machinery that is deferred for `:heap` structs generally (see
> `docs/archive/drop-glue-shallow-nested-owning-aggregate.md`, verified only via
> rc-wrapping for the same reason). So a closure stored in a plain LOCAL struct
> still leaks its handle (no double-free -- just the pre-existing local-drop gap).
> When local-struct drop invocation lands, this S2 drop glue frees those too with
> no further work.
>
> **Update (2026-07-20e) -- the S2 blocker is FIXED; S2 is now unblocked.** Parts
> 1+2 of `docs/archive/capturing-closure-in-struct-field-segv.md` landed: a
> concrete `(fn ...)` struct/ADT field now uses the fat representation uniformly
> (field type `boxed`; make-struct shims thin fns to fat; field-calls dispatch via
> `TUR_APPLY*`). A capturing closure stored in a struct field now RUNS (no SEGV).
> Suite 2218/0; fixture `capturing-closure-struct-field`. fn-field values are
> intentionally uniformly HEAP-allocated (malloc'd fat handles) so the S2 drop
> glue below can free them uniformly -- so a stored fn/closure currently leaks its
> heap handle (shim box or capturing env) until that drop glue lands. That is the
> remaining S2 work, now buildable on a working store-and-call path:
>   - **S2 Model U:** storing a closure/fn into a struct field MOVES it (source
>     consumed; a second store is a move-check error, preventing the aliasing
>     double-free); the holding struct's drop glue frees the heap fat handle
>     (`free(field)` for the shim box or `drop_glue_env_N` for a capturing env).
>   Without the move check, a closure stored into two structs would double-free,
>   so drop glue must land WITH move semantics, not before.
>
> **Blocker note (2026-07-20d) -- S2 is blocked on a struct fn-field dispatch
> bug, NOT a leak.** Scoping S2 (Model U: a stored closure freed by the holding
> struct's drop glue) surfaced that the store-and-call path does not even work:
> storing a CAPTURING closure in a `defstruct` fn-field and calling it via
> `(.f box)` **SEGVs** -- the fat env pointer lands in the field but the read+call
> emits a THIN function-pointer call (`((R(*)(A))env)(args)`), executing the env
> as code (`emit_expr.c:1153/1470` fat-vs-thin keys on `type.as.fn.boxed` /
> `is_fat`, both false for a field read). A thin top-level fn in the same field
> works; only fat closures crash. Filed as
> `docs/reported/capturing-closure-in-struct-field-segv.md`. S2 CANNOT proceed
> until the field uses the fat representation uniformly (mark the field `boxed`;
> auto-shim thin fns to fat on store -- the "closure-representation-unification
> Phase 0" this plan already names). Freeing a stored closure is moot while it
> mis-dispatches. So the next S2 step is that unification bug, then move + drop
> glue on top.
>
> **Progress note (2026-07-20c) -- S1c fresh-closure-returning CALL args
> (headline `make-scaler` CLOSED).** The other half of S1c landed: a call to a
> fresh-closure-returning fn, passed to a non-retaining fn-param, is now hoisted +
> freed. New `Binding.returns_fresh_closure`, inferred when a fn is elaborated:
> its body is a bare capturing `EX_CLOSURE` with ONLY scalar (Copy) captures and a
> scalar result -- so every call mallocs a fresh, uniquely-owned env whose bare
> `free` is fully safe (no owning capture to double-free, result cannot alias the
> env). `hoist_borrowed_closure_args` (via a shared `arg_is_freeable_closure_source`
> predicate) and `let_binding_env_freeable` both accept such a call arg/init. The
> report's minimal repro `(use-it (make-scaler 2.0))` is now ASan/LSan-clean.
> Suite 2217/0 (3 snapshots regenerated -- kebab-case-capture + two bare-fat --
> where the same hoist now frees a previously-leaked env, all ASan-verified).
> Fixture `closure-env-free-fresh-returning-call`. Guards: a struct-storing
> callee and a fn returning an rc-capturing closure are BOTH correctly left
> unfreed (leak-safe, no UAF). **Still open:** S2 stored/escaping closures
> (httpd middleware, parser combinators; `cps-backend-fn-param`, `free-lift-bind`,
> `unsafe-closure-capture` keep `requires.no-leak-check` -- different shapes, not
> the fresh-consumed-once pattern).
>
> **Progress note (2026-07-20b) -- S1c inferred non-retention (INLINE args).**
> The non-retaining-callee half of S1c landed for INLINE capturing-closure
> arguments. A new `Binding.nonretain_param_mask` records, per fn-typed / `^fat`
> parameter, whether the callee body only CALLS it (inferred at defn elaboration
> via `!closure_binding_escapes(body, param)`). A body containing ANY inline-C is
> excluded (`expr_subtree_has_inline_c`, conservative default-true) -- C text can
> store a param invisibly to the AST, the exact unsoundness that first regressed
> `schema-transform-closure` + the httpd middleware set (they store `^fat` params
> via inline-C). The emit-side escape analysis and `hoist_borrowed_closure_args`
> both consult the mask, so an inline capturing closure passed to a non-retaining
> `^fat`/fn param is now hoisted + freed at scope exit (like the landed `^borrow`
> S1.2 path), no annotation needed. Suite 2216/0; fixture
> `closure-env-free-nonretain-fatparam`; guards verified ASan-clean (freed) and
> leak-safe (struct/inline-C/return retention conservatively NOT freed, no UAF).
> **Still open:** the report's headline `make-scaler` repro passes the closure as
> a CALL result `(use-it (make-scaler ...))`, not an inline `EX_CLOSURE`; hoisting
> a fresh-closure-returning CALL arg (make-scaler's binding already carries
> `returns_closure_fn_binding`) + letting `let_binding_env_freeable` accept a
> `ptr<void>`-typed fresh-env call init is the next slice. S2 (stored/escaping
> closures) unchanged.
>
> **Progress note (2026-07-20).** A second, adjacent leak landed:
> `binding_escapes_impl` (`emit_core.c`) fell to its conservative
> `default: escape` for `EX_DEFER` and the rc/weak/ref-family nodes (`EX_RC_OF`,
> `EX_WEAK`, ...). An owning let-binding lowers its auto-drop to a
> `(defer (drop r))` and its init is `(rc/of ...)`, so BOTH tripped the default
> and flagged every sibling closure as escaping -- a non-escaping closure's env
> leaked (16 B) in any `let` that also bound an `rc`/`ref`, even for a
> scalar-capture closure. Fixed by modeling `EX_DEFER` via its capture set and
> walking the rc/weak/ref operands (strictly more precise; never greenlights a
> free of a referenced env). Suite 2215/0; fixture
> `closure-env-free-with-owning-sibling`; write-up in
> `docs/archive/history/fat-closure-env-free-owning-sibling.md`. This is NOT one
> of the S1/S2 slices below (those are the ESCAPING / inline-HOF-arg cases); it
> is an orthogonal false-escape bug in the same env-free machinery.
>
> **Progress note (2026-07-19).** Verified against the tree: S1.2 is the only
> landed slice. `hoist_borrowed_closure_args` (`elab_call.c:773`), the
> `binding_escapes_impl` FA_BORROW relaxation (`emit_core.c:573`), and the
> `let_binding_env_freeable` scope-exit free (`emit_expr.c:1267`) are all
> present. The **`drop_glue_env_N` walk-glue does NOT exist** (no such symbol
> anywhere in `src/`), and there is no capture-time retain/clone -- so S1 (a/b/c)
> and both S2 models are entirely unbuilt. The `requires.no-leak-check` markers
> still sit on `free-lift-bind`, `unsafe-closure-capture`, `cps-backend-fn-param`,
> and `hkt-stdlib-parser-instances`; `currying-effect-partial` (re-classified out
> of S1) carries no marker. This plan remains **OPEN** -- the ownership feature
> (capture-clone + walk-glue) has not started.

## Landed: S1.2 -- borrowed HOF-arg closure free

A capturing closure passed INLINE to a `^borrow` fn-param now has its heap env
reclaimed at scope exit. `free-lift-bind` / `unsafe-closure-capture` (the
`(free-run (fn [inner] (* inner scale)) ...)` shape) dropped from a 32 B leak to
16 B -- the closure env is freed; the residual 16 B is a SEPARATE free-monad
`Suspend` ADT leak (not a closure), so those fixtures keep `requires.no-leak-check`
for that reason now. Suite 2179/0. Mechanism (no ownership hazard -- these
captures are scalar):
1. `free-run`'s interp param is annotated `^borrow` (it invokes but does not
   retain the closure -- a natural transformation is reused, so the CALLER owns
   and frees it, not the callee).
2. `binding_escapes_impl` (emit_core.c) treats a closure passed to a `FA_BORROW`
   param as NON-escaping (same only-greenlights-a-free posture as the box-accessor
   whitelist).
3. `hoist_borrowed_closure_args` (elab_call.c, applied in the `elab_call_fn`
   wrapper) hoists an inline capturing-closure `^borrow` arg into a fresh
   let-binding, so the existing `let_binding_env_freeable` scope-exit `free`
   reclaims it -- the inline env otherwise has no name to target.

Also fixed a pre-existing latent bug this surfaced: `elab_unsafe` allocated its
`HandleExpr` via `arena_alloc` and never initialized `shallow`, so effect_check
read an uninitialized bool (UBSan `load of value 190`); the arena layout shift
from the hoist made the garbage non-zero. Now `handle->shallow = false`.

Remaining S1: the OWNING-capture case still needs capture-time clone + the
`drop_glue_env_N` walk-glue (Implementation findings below); the `^borrow` free
here is hazard-free only because these payload captures are scalar.

**One-line:** give a captured ("fat") closure's heap env struct a real lifecycle
-- freed when the closure dies, dropped-through when stored, walk-glued when its
captures are themselves owning -- so escaping and HOF-passed closures stop
leaking and the two remaining B2 fixtures (`currying-effect-partial`,
`hkt-stdlib-parser-instances`) CPS-emit instead of evicting on `EX_CLOSURE`.

## What this unblocks

- **The escaping-fat-closure-env leak** (`docs/reported/escaping-fat-closure-env-leak.md`):
  one `malloc`'d `struct __env_N` leaked per capturing-closure construction that
  escapes (returned / stored / passed `^fat`). Currently carries
  `requires.no-leak-check` on `cps-backend-fn-param`, `free-lift-bind`,
  `unsafe-closure-capture`.
- **B2 residuals (2)** in the CPS backend: `currying-effect-partial` (a
  partial-application closure `add10 = (log-add 10)` called in a `Log` handle
  body) and `hkt-stdlib-parser-instances` (closures stored in `Parser` values).
  Both evict on `EX_CLOSURE` today because the closure cannot be admitted without
  a free.
- **The httpd middleware family** (`httpd-async-mw-attr` / `-mw-compose`) and any
  code that stores middleware/handler closures in a chain.

## Current state

A capturing closure lowers to (`emit_expr.c` ~5785):

```c
struct __env_N { int64_t __fn; <captures...> };
struct __env_N *tmp = malloc(sizeof(struct __env_N));
tmp->__fn = <thunk>; tmp->cap0 = ...; ...
```

The fat value carries `tmp` (a one-word env pointer, or a 2-word `tur_poly_fn_t`
for the rank-2 poly protocol). The ONLY free that exists today is
`let_binding_env_freeable` (`emit_expr.c:1267`): a let-bound closure is freed at
scope exit iff it is an **`EX_CLOSURE` literal**, returns a **scalar**, and
**provably does not escape** (`closure_binding_escapes`, conservative -- only ever
greenlights a free). Everything outside that narrow gate leaks:

- a **partial-application** closure (`(log-add 10)` -- init is a CALL, not an
  `EX_CLOSURE` literal),
- a closure passed as a **HOF argument** (`(free-run (fn ...) ...)` -- the arg is
  conservatively flagged escaping),
- a **stored / returned** closure (parser combinators, httpd middleware -- it
  genuinely escapes).

The CPS backend inherits this: it can only admit a capturing closure it can
free, so the un-freeable shapes evict on `EX_CLOSURE`.

## Two sub-problems (different fixes)

The residuals split cleanly by whether the closure ESCAPES its constructor:

**S1. NON-escaping closures that just aren't freed yet.** `currying-effect-partial`
(`add10` called once, locally), the `free-run` HOF args (`free-run` calls the
closure and discards it). These have a single owner and a clear scope-exit death
point; they leak only because the current gate is too narrow (`EX_CLOSURE`-literal
+ scalar-return + a conservative escape check that flags any call argument). Fix
is a scoped free, no ownership tracking.

**S2. ESCAPING closures.** `hkt-stdlib-parser-instances` (the closure is stored in
a `Parser` value that is returned / threaded), httpd middleware (stored in a
chain). Ownership transfers to the holder; the holder must drop it, and a closure
stored in two places must not double-free. Fix needs the closure to participate in
the drop / uniqueness system.

## Design

### The env drop-glue function (shared by S1 + S2)

Emit, per env type that needs it, a `drop_glue_env_N(void *p)`:

```c
static void drop_glue_env_N(void *p) {
    struct __env_N *e = (struct __env_N *)p;
    /* walk-glue: drop each OWNING capture in reverse order, mirroring the
     * ADT/struct drop-glue (emit_module.c emit_adt_byval_drop_glue). */
    <drop e->capK for each owning capture>   /* rc_strong_decrement / drop_glue_* / free */
    free(e);
}
```

- A **scalar-only** env (captures are all Copy scalars) needs no walk -- the glue
  is a bare `free(e)`; the current `let_binding_env_freeable` already emits that
  inline. The glue function matters when captures are themselves owning (an `rc`,
  a `ref`, a NESTED closure -- an env-in-env), exactly the case that leaks worst
  today.
- Reuse the existing owning-value drop machinery keyed off each capture's type
  (`needs_drop_glue`, `rc_strong_decrement`, `drop_glue_<adt>`), so a closure that
  captures an `rc<Foo>` decrements it, and a closure that captures another closure
  recurses into `drop_glue_env_M`.

### S1 -- scoped free for non-escaping closures (bounded, land first)

1. **Widen `let_binding_env_freeable`**: admit a partial-application closure (init
   is an `EX_CALL` whose `returns_closure_fn_binding` is set -- a curried under-
   saturation producing a closure) and drop the scalar-result restriction where
   the closure result cannot alias the env (needs the walk-glue so a non-scalar
   capture is dropped, not just the env freed).
2. **A HOF-arg free**: a closure passed as a call argument whose callee does NOT
   retain it (`free-run` calls-and-discards) can be freed after the call returns.
   This needs a callee "does not retain fn-param" property -- start conservative:
   a `^fat`/`(fn ...)` param that the callee only CALLS (never stores/returns) is
   non-retaining. `free-run`'s inline-C calls the interp once and returns an int
   -- non-retaining. Emit the env free after the call.
3. **CPS interaction**: the CPS backend already has the boundary-reap mechanism
   (`__dk_reap_ptr`, P3.c/P3.d). A non-escaping closure admitted on the CPS
   delegation path registers its env (and, via the glue, its owning captures) for
   reap at the entry boundary -- the analogue of `cps_closure_env_freeable`
   (which today handles only the scalar-capture let case). Wire the glue so the
   reap drops captures too.

S1 alone clears `currying-effect-partial`, `free-lift-bind`, `unsafe-closure-capture`
(dropping their `requires.no-leak-check`) without any ownership-tracking.

### S2 -- drop glue for escaping closures (the real feature)

An escaping closure is an owning heap value whose owner is the value it is stored
in (a struct field, an ADT payload, a return value). Two sound models:

- **Model U (uniqueness / move) -- preferred for STORED closures.** Plug the
  closure into the existing affine/move system (`is_moved`, `is_linear_consumed`,
  `is_affine`, `CK_MOVE`, the alias-state UT1 machinery). Storing a closure in a
  struct MOVES it (the source binding is consumed); the holding struct's drop
  glue (`needs_drop_glue`) calls `drop_glue_env_N` on the field. No refcount, no
  per-closure overhead; a double-store is a move-check error (as it already is for
  other affine values). This is how `hkt-stdlib-parser-instances` (closure stored
  in a `Parser`) and httpd middleware (closure stored in a chain node) should
  work -- the `Parser` / chain-node drop glue owns the closure.
- **Model R (refcount) -- fallback for genuinely SHARED closures.** Add a
  refcount word to the env (`struct __env_N { int64_t __rc; int64_t __fn; ... }`);
  a clone/dup increments, a drop decrements and runs `drop_glue_env_N` at zero.
  Uniform and sharing-safe, but adds a word + rc ops to every fat closure and an
  ABI change to the fat-closure protocol (the `^fat` layout, the HKT thunk
  recovery, `tur_poly_fn_t`). Reserve for closures the uniqueness model rejects
  (a closure legitimately shared by two owners).

Recommendation: land **Model U** for the stored-closure cases (covers the corpus
residuals) and only reach for **Model R** if a shared-closure fixture appears --
the ABI cost of R is high and the corpus does not yet need it.

## Phasing

- **Phase 1 (S1) -- NOT bounded; must land as one atomic ownership unit (see
  Implementation findings).** Order forced by the double-free hazard:
  (1a) capture-time retain/clone for OWNING captures (a bare capture aliases
  today, so an env-drop would double-free), (1b) `drop_glue_env_N` walk-glue on
  top, (1c) the non-retaining-callee (`^once`) annotation + a post-call free hook
  in EX_CALL emission for inline HOF args. 1a+1b are atomic (1a alone leaks MORE;
  1b alone double-frees). Clears the two PD leak fixtures (scalar-capture, so 1c +
  the emit hook, not the walk-glue, is what they need). NOT a "start here quick
  win" -- it is the ownership feature. `currying-effect-partial` is RE-CLASSIFIED
  out of S1 (it is a partial-app of a colored fn -- a B1-style colored closure,
  not a value closure).
- **Phase 2 (S2 / Model U):** closures participate in the move system; struct/ADT
  drop glue drops closure-typed fields via `drop_glue_env_N`. Clears
  `hkt-stdlib-parser-instances` and the httpd middleware family.
- **Phase 3 (S2 / Model R, only if needed):** refcounted env for genuinely shared
  closures. ABI change; deferred until a fixture demands it.

## Implementation findings (verified before starting S1)

A tractability pass on S1 established that it is NOT a quick bounded slice -- every
sub-path has either a soundness hazard or needs new analysis/machinery. Three
facts, each verified against the emitter:

1. **Capturing an owning value does NOT clone it.** The env-fill emission
   (`emit_expr.c` ~5793) is a bare `fat_tmp->field = <value>;` per capture -- no
   `rc` increment, no closure retain. So the walk-glue (dropping owning captures
   in `drop_glue_env_N`) is UNSOUND on its own: dropping a captured `rc` that the
   original owner still drops is a double-free. **The walk-glue REQUIRES
   capture-time retain/clone first** (the "retain when duplicated" half of the
   fix). Scalar (Copy) captures are safe (no ownership) -- so a scalar-only env
   drop is a bare `free`, hazard-free; an owning-capture env drop is blocked on
   capture-cloning.

2. **No post-call free hook exists for inline HOF-arg closures.** `free-lift-bind`
   / `unsafe-closure-capture` pass the closure INLINE to `free-run` (not a let
   binding), so `let_binding_env_freeable`'s scope-exit free (the only closure
   free that exists) does not reach it. Freeing it needs (a) a new "free this
   malloc'd env after the enclosing call/statement" mechanism in the EX_CALL
   emission, AND (b) proof the callee does NOT retain the closure -- `free-run` is
   inline-C whose non-retention is not analyzable; it needs a `^once`/non-retaining
   fn-param annotation or a whitelist. Even though these closures capture only a
   scalar (hazard-free to free), the emit hook + non-retention property are real
   prerequisites.

3. **`currying-effect-partial` is a partial-application of a COLORED fn.** `add10 =
   (log-add 10)` where `log-add` performs `Log`; the "closure" performs when
   called, so it is not a value closure at all -- it belongs with the B1-style
   colored-call handling, not S1 value-closure drop. It should be re-classified
   out of S1.

Net revised S1 order: (a) capture-time retain/clone for owning captures, then (b)
`drop_glue_env_N` walk-glue on top of it, then (c) the non-retaining-callee
annotation + post-call free for HOF args. Only step (a) unblocks a hazard-free
`drop_glue_env_N`; steps done out of order double-free.

## Risks / open questions

- **Double-free** is the cardinal risk. The escape analysis
  (`closure_binding_escapes`) is conservative (only greenlights a free), which is
  the right posture -- extend it carefully; a false "does not escape" frees a live
  env. Model U's move-check is the structural guard for S2.
- **Non-retaining callee property (S1.2):** deciding a callee does not retain its
  fn-param. Start with the syntactic "only calls it" rule (covers `free-run`);
  a general effect/escape signature on fn-params is a larger analysis -- keep it
  out of Phase 1.
- **Walk-glue ordering / cycles:** a closure that captures itself (letrec self-
  capture, already handled specially at construction -- `emit_expr.c` "Edge 1")
  must not recurse infinitely in the glue; mirror the ADT walk-glue's
  cycle-awareness or exclude self-captures from the drop walk.
- **Fat-closure ABI (Model R only):** adding an `__rc` word changes the `^fat`
  layout, HKT thunk recovery, and `tur_poly_fn_t`. Audited in
  `docs/archive/fat-closure-abi-audit-plan.md` -- coordinate there if R is ever
  needed.
- **`tur_poly_fn_t` (2-word) vs one-word env:** the drop must free the right
  object for both the plain env-pointer closures and the rank-2 poly-fat
  closures; confirm which allocation each frees.

## Test targets & exit gate

- `currying-effect-partial`, `hkt-stdlib-parser-instances` flip from
  `BODY-UNSUPPORTED` (`EX_CLOSURE`) to CPS-emitted (direct == cps == turi).
- `free-lift-bind`, `unsafe-closure-capture`, `cps-backend-fn-param` become
  ASan-clean and DROP their `requires.no-leak-check` markers.
- The minimal no-effects repro in `escaping-fat-closure-env-leak.md`
  (`make-scaler`) is ASan-clean.
- httpd middleware fixtures stay green and leak-clean.
- Full `bash tests/run.sh` green; the report moves to `docs/archive/` when closed.
