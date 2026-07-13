# Language/codegen gaps surfaced by the pure-Turmeric `logic.tur` port

> **Status:** Partially fixed. **GAP 1 fixed 2026-07-13** (see its section); the
> `logic.tur` goal-handle `:int`-carrier workaround has been removed. GAP 2, 3,
> and the residual arg-passing facet of GAP 4 remain open.
> **Reported:** 2026-07-13 (during the `stdlib/logic.tur` pure-Turmeric port).
> **Context:** `logic.tur` is now inline-C-free and runs under both harnesses,
> but getting there required four workarounds that paper over real gaps. None
> blocked v1 (the engine is correct and green), but each one made the code less
> honest than it should be -- notably forcing `:int`-carrier plumbing the
> "No Lazy `:int`" rule otherwise discourages. This report captures the gaps and
> a plan to retire the workarounds once they are fixed.

The four are independent; fix and clean up in any order. Severity is "how much
it hurt this port," not a release gate.

---

## GAP 1 -- calling an opaque HKT handle cast to a `fn` type emits a *thin* call (SIGSEGV) -- **HIGH** -- FIXED 2026-07-13

**Fix (as landed).** Two changes in `elab_ascribe` (`src/compiler/elab_types.c`),
both keyed off a new `ascribe_type_is_opaque_handle` helper:

- **Consumer side.** The existing "fat-handle ascription" rule (a `:int` /
  `:ptr<void>` carrier ascribed to a `fn` type is marked `boxed` for slot-0
  dispatch) now also fires when the *source* is an opaque newtype whose carrier
  is the int64/pointer slot -- so `(:: g (fn [Subst] Stream))` on a `(Goal int)`
  fat-dispatches instead of emitting a thin call.
- **Producer side.** A *bare, captureless* closure (`TY_FN`, unboxed) ascribed to
  an opaque handle or `:ptr<void>` is now wrapped in `EX_FN_TO_FAT` so it crosses
  as a uniform `{ thunk, env }` fat box -- mirroring the captureless-closure
  boxing already done for `TY_TYVAR` escapes. This also fixed a *latent* variant
  the report missed: a no-capture goal like `logic.tur`'s `succeed` / `fail`
  segfaulted when dispatched compiled, because it was stored as a raw code
  address and slot-0-dispatched. Both now work.

The `logic.tur` workaround (goal handles carried as `:int`) has been removed:
`apply-goal` is typed `[g : (Goal int) ...]` and the `(:: g :int)` erasures are
gone from the combinators and instances. See the cleanup checklist below.

Repro below now prints `15` (was SIGSEGV). Original report follows.

**Summary.** When a value typed as an opaque HKT handle (`(Goal A)`,
`(Box A)`, carrier `:ptr<void>`) is cast to a function type and applied, codegen
emits a **thin** function-pointer call that ignores the fat-closure env. If the
underlying closure captured anything, the call jumps into the env block and
segfaults. Casting the *same* value through its erased `:int` carrier first
takes the fat-dispatch path and works. Silent miscompile, not a diagnostic.

**Minimal repro.**

```turmeric
(defopaque Box [A] :ptr<void>)
(defn apply-box [b : (Box int)] : int ((:: b (fn [int] int)) 10))
(defn mk [cap : int] : (Box int)
  (:: (let [c cap] (fn [x] (+ x c))) :Box))   ; fat closure (captures c)
(defn main [] : int (println (apply-box (mk 5))))  ; want 15
```

`tur run` -> **Segmentation fault**. Change `apply-box`'s parameter to `b : int`
and call `(apply-box (:: (mk 5) :int))` and it prints `15`.

**Root cause (where known).** `elab_call_head_expr`
(`src/compiler/elab_call.c:984`). It sets the head temp's `closure_fn_binding` /
`type.as.fn.boxed` (the fat/slot-0 dispatch markers) only when the head is
`TY_PTR_VOID` *or* a curried/poly/tyvar-carrier `EX_CALL`. When the head is an
opaque handle ascribed to a `fn` type, `head_kind` resolves to `TY_FN` and the
`source_expr` is a plain binding ref (not `EX_CALL`), so neither branch fires;
`closure_fn_binding` stays NULL and emit falls back to a thin pointer call. The
`:int`-carrier form works precisely because the erased-carrier path is what the
existing "uniform fat box / slot-0 dispatch" logic already handles.

**Fix directions.** In `elab_call_head_expr`, when `source_expr`'s *static* type
is an opaque whose carrier is `:ptr<void>` (or otherwise an erased handle),
route it through the same fat-dispatch path as the `TY_PTR_VOID` head: set
`closure_fn_binding = expr_closure_fn_binding(source_expr)` (or mark the head
temp `boxed`). Minimum acceptable outcome: never emit a thin call for a value
whose runtime representation may be a fat box -- if fat-dispatch genuinely
cannot be determined, emit a diagnostic rather than a silent thin call.

**Workaround in the tree.** `stdlib/logic.tur`: `apply-goal [g : int ...]` and
`apply-fat`/goal combinators all carry the goal handle as its `:int` carrier and
apply via `(:: g (fn [Subst] Stream))`, with `(:: g :int)` erasures sprinkled
through `conjoined`/`disjoined`/`fresh`/`run-logic` and the four instances.

---

## GAP 2 -- forward/mutually-recursive `defn` return type defaults to `int` -- **MEDIUM**

**Summary.** A `defn` that calls a *later-defined* `defn` types that forward
reference as returning `int`, regardless of the callee's declared return type.
For a mutually-recursive pair this poisons `match`-arm unification.

**Minimal repro.**

```turmeric
(defdata T :copy (A :int) (B :T))
(defn f [x : T] : T
  (match x (A n) (g n) (B inner) (f inner)))   ; g is defined below
(defn g [n : int] : T (A (+ n 1)))
(defn main [] : int (match (f (B (A 5))) (A n) (println n) (B _) (println -1)))
```

`tur run` ->
`TUR-E0001: match: arm types are incompatible -- expected int (from earlier
arm), got adt`. The first arm `(g n)` is assumed `int`; the second arm is `T`.
Reordering so no forward reference exists (or making `f` self-recursive) fixes
it.

**Root cause (where known).** Not pinned to a line. The elaborator resolves a
call to a not-yet-elaborated `defn` with a default/placeholder result kind
(`int`) instead of its declared signature. Two-pass signature collection (bind
every top-level `defn`'s declared type before elaborating any body) would remove
the ordering dependence.

**Fix directions.** Pre-register all top-level `defn` return types in a first
pass so forward references resolve to the declared type. Until then, mutually
recursive helpers must be written self-recursively or ordered callee-first.

**Workaround in the tree.** `stdlib/logic.tur`: `logic-walk` was written to
recurse only on itself via a `subst-lookup` helper returning a `Lookup` ADT,
specifically to avoid a `walk-var <-> logic-walk` mutual cycle.

---

## GAP 3 -- non-recursive (by-value) `defdata`/`defstruct` cannot erase to/from `:int` -- **MEDIUM**

**Summary.** The compiled path represents a non-recursive ADT/struct **by
value** (`tur_adt_P`), so there is no int handle to cast to. `(:: v :int)` and
the reverse `(:: someInt :SomeByValueAdt)` fail -- and fail as a raw `cc` type
error, not a Turmeric diagnostic. A *recursive* ADT is heap-boxed and erases
cleanly, so the fix is often "make the type recursive," which is a
representation hack rather than an intent.

**Minimal repro.**

```turmeric
(defdata P :copy (P :int :int))
(defn main [] : int (println (:: (P 3 4) :int)))
```

`tur run` ->
`error: incompatible types when initializing type 'int64_t' ... using type
'tur_adt_P'` (raw C error from the generated file).

**Root cause (where known).** By-value aggregate representation for
non-recursive ADTs (see the by-value-HKT monomorphization notes in
`src/compiler/elab_call.c`). `::` between a by-value aggregate and `:int` has no
lowering. Same class of issue bites a poly `:fn` result (a boxed `int`) fed into
a by-value struct slot: `(:: (f s) :SomeByValueAdt)` also fails.

**Fix directions.** Either (a) reject `::` between a by-value aggregate and
`:int` with a real Turmeric diagnostic (never leak a `cc` error), and/or (b)
support an explicit boxing cast (heap-box the aggregate, hand back the handle)
so erased-carrier plumbing can round-trip a by-value value without needing a
sham recursive arm.

**Workaround in the tree.** `stdlib/logic.tur`: the fresh-var counter is folded
into the recursive `Subst` base node (`(SNil next)`) instead of a natural
`UState { subst next }` product -- chosen specifically because a non-recursive
`UState` is by-value and breaks the `:int`-carrier goal/stream plumbing. (This
also happens to be a tidy design, but the *reason* is this gap.)

---

## GAP 4 -- `:fn`-typed parameters erase the callback's argument types -- **LOW**

**Summary.** A parameter declared `:fn` (the first-class untyped function type)
loses its argument types: a lambda passed in has its parameters inferred as
`int`, so passing a real ADT through it needs an explicit annotation on the
lambda parameter. Ergonomic papercut, surfaced at every `fresh` call site.

**Minimal repro.**

```turmeric
(defdata Term :copy (TInt :int))
(defn call-it [f : fn] : int (f (TInt 7)))
(defn use-int [t : Term] : int (match t (TInt n) n))
(defn main [] : int (println (call-it (fn [t] (use-int t)))))  ; t inferred :int
```

`tur run` ->
`TUR-E0001: function 'use-int' arg 1: expected Term, got int`. Annotating the
lambda (`(fn [t : Term] (use-int t))`) fixes it.

**Root cause (where known).** `:fn` is intentionally arity/type-erased at the
call boundary (boxed args). A typed function-type parameter
(`(fn [Term] (Goal int))`) *does* now preserve the arg type -- after the GAP 1
fix, a typed callback param even infers an unannotated lambda's parameter type
(`(fn [t] ...)` binds `t : Term`). **But a residual facet remains:** a
*capturing* (fat) closure passed into such a typed parameter is still
mis-dispatched by the callee's *direct* call `(f x)` -- nested `fresh`
(`(fresh (fn [x : Term] (fresh (fn [y : Term] ...))))`) segfaults compiled if
`fresh`'s parameter is typed `(fn [Term] (Goal int))`. This is GAP 1's dispatch
problem on the *argument-passing / direct-call* side (`elab_call`), which the
`elab_ascribe`-only fix does not cover: a fat-closure argument crossing into a
plain typed `fn` parameter is not marked `arg_fat`, so the callee calls it thin.

**Fix directions.** Mark a `(fn ...)` parameter that may receive a fat closure
as `arg_fat` (box the argument via `EX_FN_TO_FAT` at the call site, slot-0
dispatch in the callee) -- the same producer/consumer pairing GAP 1 got, applied
to the `elab_call` argument path rather than the `::` path. Until then, a
callback parameter that can receive a *capturing* closure must stay `:fn`.

**Workaround in the tree (still present).** `stdlib/logic.tur`: `fresh` /
`apply-fat` / `fresh-impl` keep the callback typed `:fn` (uniform thin/fat
dispatch); migrated fixtures annotate their lambdas `(fn [x : Term] ...)`. The
goal-*handle* carriers, by contrast, are now honestly `(Goal int)` (GAP 1 fixed).

---

## Cleanup plan -- retire the workarounds once the gaps are fixed

Do these opportunistically as each gap lands; none is a prerequisite for the
others except where noted. The end state is a `logic.tur` whose goal/stream
plumbing is typed end-to-end with no `:int` type-erasure.

- [x] **GAP 1 fixed** (done 2026-07-13) -> `apply-goal` retyped
      `[g : (Goal int) state : Subst] : Stream`; the `(:: g :int)` erasures are
      gone from `conjoined`, `disjoined`, `run-logic`, `bind-goal-raw`,
      `fmap-goal-raw`, and the `Functor`/`Applicative`/`Monad`/`Alternative`
      instances (now `(:: ... (Goal int))`). Both harnesses green; no thin-call
      segfault.
- [ ] **GAP 4 residual (arg-passing dispatch) fixed** -> only then retype
      `fresh`/`apply-fat`/`fresh-impl` callbacks as `(fn [Term] (Goal int))` and
      drop the `:fn` erasure; then the migrated `logic-*` fixtures can drop the
      `(fn [x : Term] ...)` annotation. Blocked today: a *capturing* callback
      into a typed `fn` parameter still mis-dispatches (nested `fresh`
      segfaults), so `:fn` is retained.
- [ ] **GAP 3 fixed** -> optionally reintroduce an explicit
      `UState { subst next }` product if it reads more clearly than the
      counter-in-`SNil` encoding (the current encoding stays correct either way;
      this is a readability call, not a correctness fix).
- [ ] **GAP 2 fixed** -> `logic-walk`/`subst-lookup` may be re-expressed as a
      mutually-recursive `walk` / `walk-var` pair if that is clearer (again,
      cosmetic).
- [ ] After any of the above, regenerate fixture snapshots / docstrings in the
      same change and re-run `bash tests/run.sh` + `bash tests/run-turi.sh`.

Grep anchor for the workarounds: every `(:: ... :int)` and the `: fn` / `: int`
carrier annotations in `stdlib/logic.tur`'s goal-machinery section
(`apply-goal` through the instances) exist because of GAP 1/3/4 above.

---

## See Also

- `stdlib/logic.tur` (the workarounds live in the "Goal application" through
  "HKT typeclass instances" sections).
- `docs/archive/history/logic-pure-turmeric-port-plan.md` -- the port plan, whose
  "Implementation notes" section records the same deviations from the caller's
  perspective.
- `src/compiler/elab_call.c:984` (`elab_call_head_expr`) -- GAP 1 dispatch site.
