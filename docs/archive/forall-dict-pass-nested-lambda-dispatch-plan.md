# Lowering constraint-method dispatch inside a nested lambda (forall-dict-pass)

**Status:** LANDED (primary case) 2026-07-06 -- the canonical captureless
single-dispatch mapper `(fn [x] (show x))` is lowered end to end (Phases 1-3);
the TUR-E0311 guard is NARROWED to a residual (a mapper dispatching >1 class, a
capturing mapper, or a dispatch in a deeper nested lambda), covered by the
negative fixture `tests/fixtures/errors/forall-dict-nested-lambda-multiclass/`.
Positive fixture: `tests/fixtures/van-laarhoven-lens-show-mapper/`.  Phase 4's
"remove the guard entirely" is deferred to the residual follow-up below.
**Predecessor:** `docs/archive/forall-dict-pass-multi-constraint-hkt-plan.md`
(graduated the flag; this shape was guarded with TUR-E0311 rather than lowered).
**Tracked bug:** `docs/archive/forall-dict-pass-nested-lambda-method.md` (resolved).

## Residual follow-up (still guarded)

The captureless-mapper-to-closure conversion carries exactly ONE env dict and
promotes a mapper that has no other captures.  Three shapes remain guarded by
TUR-E0311 -- a **multi-class mapper** (N dicts), a **capturing mapper**, and
**deeper nesting** -- and are broken out into their own tracked plan so they do
not get lost against this landed one:

**->** `docs/upcoming/v1/forall-dict-pass-nested-mapper-general-plan.md`.

## The hole

A dict-clone body dispatches each class method on its constrained type variable
through a runtime dictionary param. This works when the call sits **directly in
the clone body**. It does **not** work when the call sits inside a **nested
lambda** the clone body passes to another call -- the van Laarhoven mapper:

```turmeric
;; (Functor f, Show a) => (a -> f a) -> a -> (f cstr)
(defn show-lens [^f a] [^Functor f ^Show a g : (-> a (f a)) s : a] : (f cstr)
  (fmap (g s) (fn [x : a] : cstr (show x))))   ; <-- `show` inside the mapper
```

`fmap` (directly in the body) dispatches through the `Functor` dict param.
`show`, inside the mapper, is lambda-lifted to its own top-level C function with
no dict param in scope, so it silently mis-resolved to the carrier
representative (`Show int`) for every focus type. The graduation guarded this
with **TUR-E0311**; this plan lowers it and removes the guard.

## The mechanism it already rides on

`make_dict_clone` shares the original's body across every clone of the same
function; each clone is a separate C function with its own leading dict param(s).
The mapper is lambda-lifted **once** and shared across all clones. The crucial
precedent is `van-laarhoven-lens-concrete`, whose mapper
`(fn [nx] (make-struct Point :x nx :y (.y s)))` **captures `s`** (a clone param):

```c
struct __env_1292 { tur_thunk...t __fn; tur_adt_Point *s; };
static tur_adt_Point *__fn_1290(void *env_p, int64_t nx) {
    struct __env_1292 *env = (struct __env_1292 *)env_p;
    return ctor_Point(nx, ...(env->s)->y);          /* reads capture from env */
}
static int64_t point_hyx_un_undict_un1319(int64_t __dict_1320, int64_t g, int64_t s) {
    struct __env_1292 *e = malloc(sizeof *e);
    e->__fn = __fn_1290;  e->s = s;                 /* clone body builds the env */
    return fmap_via(__dict_1320, g(s), (tur_poly_fn_t){ e, thunk });
}
```

One **shared** mapper `__fn_1290`; five distinct clones each build their **own**
env at runtime and read `s` back through it. That is exactly the shape `show`
needs: make the mapper **capture the dict** the same way it captures `s`, and
each clone body will inject its own dict param into the env. No per-clone mapper
copy is required -- the env is per-clone-instance even with a shared mapper.

## Design: the dict is a captured value, not emit-time ctx

Today, in-body dispatch is emit-time magic: `emit_call_name` reads
`ctx->dict_dispatch_param_cnames[k]` while the clone body is being emitted. That
context is gone when the lifted mapper's own body is emitted. The fix threads
the dict to the mapper as a **captured env value** so the mapper is
self-contained.

Two facts make this tractable:

1. **Dict param bindings must be stable per original function.** A shared mapper
   can only capture one binding; if every `make_dict_clone` call mints fresh
   dict param bindings, the shared mapper cannot name one consistently.
   Memoize the per-constraint dict `Binding`s on the original `FnDef`
   (`elab_call.c:make_dict_clone`, ~5537). Every clone of the same original
   declares its leading dict params from those shared bindings, so their cname is
   stable and a capture resolves for every clone.

2. **The closure-env + fat-dispatch path already carries captures.** A mapper
   that captures the dict becomes an ordinary `EX_CLOSURE` whose env the clone
   body builds. The only genuinely new emit behavior is *dispatching a method
   through an env-loaded dict* (vs a param or ctx).

## Phase 1 -- stable per-original dict bindings

- `make_dict_clone`: memoize `dict_clone_params`/`dict_clone_classes` on the
  original `FnDef` the first time it is cloned, and reuse them on subsequent
  clones. Keep the current per-clone `FnDef` (multiple clones are fine); only
  the dict **Binding identities and cnames** become shared.
- Acceptance: the existing multi-clone fixtures
  (`van-laarhoven-lens-concrete`, the `*-wide-*` set, `forall-dict-two-scalar`)
  regenerate byte-identical or trivially-renamed snapshots and stay green.
  (If cname stability shifts snapshots, regenerate in the same change.)

## Phase 2 -- capture the dict into the mapper

- In `make_dict_clone`, replace the TUR-E0311 rejection
  (`dict_clone_dispatch_in_nested_lambda`, elab_call.c) with a **transform**:
  for each nested lifted lambda whose body dispatches a constraint class `C` on
  a bare-tyvar receiver, append the (memoized) class-`C` dict binding to that
  lambda `FnDef`'s `captures` (promoting a captureless `EX_FN` mapper to a
  closure), and tag each such call with "dispatch through captured dict `C`".
- Add a small tag to the `EX_CALL`/dict node (e.g. `dict_capture_class` +
  `dict_capture_binding`) so emit can tell "dispatch through this env-loaded
  dict" from "dispatch through `ctx->dict_dispatch`".
- The mapper is shared; do this transform **once** per original (idempotent:
  skip if already captured), keyed on the memoized dict binding.

## Phase 3 -- emit: dispatch through the env-loaded dict

- `emit_call_name` (emit_core.c ~1697): when a call is tagged
  "dispatch through captured dict `C`", emit
  `((<ret>(*)(...))((void **)(intptr_t)<envload>)[<slot>])(...)` where
  `<envload>` reads the dict field from the closure env (the same
  `env->field` load the mapper already uses for `s`), and `<slot>` is the
  method index in `C`'s dict layout (unchanged from the in-body path).
- The env-build at the construction site is the existing closure path: the dict
  field is filled from the clone's dict param binding, which -- being the
  memoized binding declared as a leading clone param -- is in scope in the clone
  body exactly where the env is built.
- Verify the mapper's poly-wrapper (`__poly_M(void *env, ...)`) forwards `env`
  to the closure body (it already receives it); the raw captureless
  `__fn_N(x)` shape is replaced by the closure `__fn_N(env, x)` shape the
  capture promotion produces.

## Phase 4 -- fixtures, guard removal, docs

- Delete the TUR-E0311 guard (`dict_clone_dispatch_in_nested_lambda` + its call
  site) and the E0311 code reservation note.
- Convert `tests/fixtures/errors/forall-dict-nested-lambda-method/` into a
  **positive** fixture (move it out of `errors/`, add `expected.stdout` +
  `expected.c`) asserting `bool:true` (the previously-miscompiled focus).
- Extend `van-laarhoven-lens-show/` with a `show`-in-the-mapper variant, or add
  a sibling fixture, so both the in-body and in-mapper dispatch are covered.
- Close `docs/reported/forall-dict-pass-nested-lambda-method.md` (move to
  `docs/archive/`), and archive this plan.
- `bash tests/run.sh` green (10-min timeout).

## Risks / notes

- **Snapshot churn from stable cnames (Phase 1).** Memoizing dict bindings may
  renumber `__dict_N` params in existing snapshots. Regenerate in the same
  change; it is a rename, not a semantic move.
- **Two dispatch representations coexist.** In-body dispatch keeps the
  `ctx->dict_dispatch` path; only nested-lambda dispatch uses the captured-dict
  tag. Keep them disjoint (the tag is set only by the Phase 2 transform) so the
  working in-body path is untouched.
- **Deeper nesting.** A method dispatched two lambdas deep captures the dict
  through each intermediate lambda's env. The capture-append must run at every
  lifted-lambda boundary on the path; the Phase 2 walker already descends
  through lifted lambdas (it is the same walk the guard used).
- **Capturing mappers (set/over).** A mapper that already captures a value
  (`b`/`h`) simply gains one more env field; the existing env-build handles an
  N-field env. No special case.
