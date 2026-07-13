# `stdlib/logic.tur` pure-Turmeric port (miniKanren) -- Plan

> **Status:** Done (2026-07-13). `stdlib/logic.tur` is now pure Turmeric with
> zero inline-C; the `hkt-stdlib-logic-instances` carve is dropped and all
> `logic-*` fixtures run under both harnesses. See "Implementation notes" below
> for the two deviations from the sketch (a self-contained pure Stream ADT
> instead of importing `backtrack.tur`, and the fresh counter folded into the
> `Subst` tail rather than a separate `UState`).
> **Last Updated:** 2026-07-13
> **Type:** stdlib / interpreter parity
> **Scope:** Reimplement `stdlib/logic.tur`'s ~20 inline-C primitives in pure
> Turmeric so the miniKanren engine runs under both the compiled path and the
> tree-walking interpreter (`tur --interpret` / WASM REPL), then drop the
> `tests/fixtures/hkt-stdlib-logic-instances` `requires.tur-only` carve.
> **Headline finding:** **no missing language feature.** Everything the C
> primitives do is already expressible with `defdata` + `match` + `backtrack.tur`
> (all interpreter-native). The one real wrinkle is a global mutable fresh-var
> counter, fixed by threading state, not by new machinery.

---

## Motivation

`logic.tur` is carved `requires.tur-only` because it carries ~22 inline-C
primitives (term constructors/accessors, unification, the substitution store,
walk, a global fresh counter). The carve note (correctly) judged re-implementing
that engine *as interpreter natives* disproportionate. But the note's own
contrast points at the better fix: its companion `backtrack.tur` "IS fixed... it
reduces to a handful of natives." A **pure-Turmeric** `logic.tur` needs no
natives at all -- it becomes ordinary library code that runs everywhere, and it
is a better-typed engine than the pointer-casting inline-C it replaces (killing a
pile of `:int`-as-eraser handles per the "No Lazy `:int`" rule).

---

## Current inline-C inventory (`stdlib/logic.tur`)

Twenty `\`\`\`c` blocks, in three groups. Everything else in the file
(`apply-fat`, `unify-goal-impl`, `lequal`, `succeed`, `fail`, `conjoined*`,
`disjoined*`, `fresh*`, `reify-walk`, `run-logic`, all HKT instances) is already
pure Turmeric.

**Group A -- solution stream (the backtrack/list monad), lines 53-140:**
`mzero`, `mreturn`, `mplus`, `mbind`, `bt-length`. These are an inlined copy of
`backtrack.tur`'s `{value,next}` cons-stream, identical in name, layout, and
semantics.

**Group B -- terms, lines 154-260:** `term-int`/`term-var`/`term-pair`/
`term-nil` (constructors: `malloc` a `{tag,data1,data2}` struct) and
`term-tag`/`term-int-val`/`term-var-id`/`term-pair-fst`/`term-pair-snd`
(accessors: pointer-cast field reads). Tag 0=INT, 1=VAR, 2=PAIR, 3=NIL. PAIR
stores child *pointers* in data1/data2.

**Group C -- store + engine, lines 271-578:** `lvar-next` (a C `static int64_t`
counter -- the ONLY global mutable state), `subs-empty` (null sentinel),
`logic-walk` (follow VAR bindings through an association list to a ground term),
`logic-unify` (recursive structural unification, no occurs check, extends the
alist by prepending nodes; returns `-1` on failure), `take-n`, `first-state`.

The substitution store is already a **functionally persistent association list**
(`{var_id, term_ptr, next}` nodes, prepend-only) threaded through goals -- not
global. So the only genuine global is `lvar-next`.

---

## Target representation (all interpreter-native today)

- **Terms -> `defdata` + `match`.** A recursive sum type is directly expressible
  and interpreter-supported (construction: `adt_ctor_native`, `eval.c:654`;
  `match`: `EX_MATCH`, `eval.c:5116`):

  ```turmeric
  (defdata Term
    (TInt  :int)
    (TVar  :int)
    (TPair :Term :Term)
    (TNil))
  ```

  Group B constructors become `TInt`/`TVar`/`TPair`/`TNil`; Group B accessors
  become `match` arms. Precedent: `stdlib/either.tur:39` (parametric payload
  ADT), `tests/fixtures/recursive-types/simple-tree/input.tur` (recursive
  `defdata` + `match`), `examples/datalog/minimal.tur`.

- **Substitution store -> pure `defdata` association list** (or cons list of
  `Pair<int,Term>`), a direct translation of the existing `SubsE` alist. **Do
  NOT use `stdlib/map.tur` / `hamt.tur`:** HAMT is inline-C with no interpreter
  native override, so a HAMT store would reintroduce the exact gap being closed.
  The alist is O(n) lookup but matches the original and stays interpreter-clean.

- **Solution stream -> `import backtrack`.** `backtrack.tur`'s `mzero` /
  `mreturn` / `mplus` / `mbind` / `bt-length` are the pure companions, already
  natively shimmed for the interpreter in `wk_register_backtrack_natives`
  (`interpreter_natives.c:3654-3664`) *and* pure enough to run compiled. Delete
  logic.tur's inlined Group A and depend on backtrack.tur instead.

- **Fresh-var counter -> thread it in the state.** Idiomatic miniKanren already
  carries `State = (subst . fresh-counter)`. Replace the `lvar-next` global by
  threading an int counter through goal application. No `set!`/`ref` needed
  (`ref.tur` is itself inline-C and unshimmed -- avoid it); pure state-threading
  is the correct and portable choice.

---

## Phases

### Phase 1 -- terms as a `defdata`

Replace Group B (10 blocks). Define `Term`, rewrite every `term-*` constructor
and accessor as a constructor / `match`. Update `logic-walk`/`logic-unify`/
`reify-walk` call sites to pattern-match instead of `term-tag` dispatch. Terms
lower to `:int` handles, so downstream `:int`-typed code is unaffected; the C
already compared var-ids (not pointers), so ADT value semantics are equivalent.

### Phase 2 -- pure store, walk, unify

Replace Group C's `subs-empty`/`logic-walk`/`logic-unify` (3 blocks) with pure
recursive `defn`s over the `defdata` alist. Return `option<Subst>` (or a
`result`) from `unify` instead of the `-1` sentinel -- a real tagged failure per
the "No Lazy `:int`" rule. `take-n` / `first-state` become trivial pure list ops.

### Phase 3 -- state-threaded fresh vars

Replace `lvar-next` (Group C). Introduce `UState { subst : Subst  next : int }`
(or a `Pair`), thread it through `fresh*` / goal application, and allocate fresh
`TVar`s from `next`. This removes the last global. Verify determinism: fresh-var
ids must stay stable across runs (the reify output in
`tests/fixtures/logic-reify` depends on it).

### Phase 4 -- swap Group A for `backtrack.tur`

`(import backtrack ...)` (or `load`), delete logic.tur's inlined `mzero` /
`mreturn` / `mplus` / `mbind` / `bt-length`. Confirm the HKT instances
(Functor/Applicative/Monad/Alternative for `Goal`, lines 607-653) still resolve
against the backtrack-backed stream -- they are the property
`hkt-stdlib-logic-instances` actually tests.

### Phase 5 -- migrate fixtures + drop the carve

The `tests/fixtures/logic-*` fixtures (`logic-unify-basic`, `logic-fresh`,
`logic-query`, `logic-reify`, `logic-conjoined`, `logic-disjoined`,
`logic-occurs-check`, `logic-unify-fail`) currently **inline the same C
primitives per-fixture** -- migrate them to the new pure API. Then delete
`tests/fixtures/hkt-stdlib-logic-instances/requires.tur-only` (and its
`requires.no-leak-check`, which existed only because the C primitives leaked).

---

## Missing language features

**None.** Confirmed present and interpreter-exercised: tagged unions (`defdata`),
pattern matching (`match`), recursive/parametric payload ADTs, persistent
cons/alist stores, the backtrack stream monad (natively shimmed), and pure
state-threading. The single architectural change is folding the fresh-var
counter into the state -- a refactor, not a feature.

If, during Phase 2, occurs-check or a more efficient store is desired, both are
pure-library additions (a recursive `occurs?` over `Term`; the alist stays). Do
not add them speculatively -- match the current engine's behavior first
(`logic-occurs-check` documents that the current engine has *no* occurs check).

---

## Validation / definition of done

- `bash tests/run.sh` green (logic fixtures pass compiled; regen any codegen
  snapshot in-PR).
- `bash tests/run-turi.sh` green with `hkt-stdlib-logic-instances` now RUN (not
  carved) under `--interpret`, and the `logic-*` fixtures passing under the
  interpreter for the first time.
- `check_turi_parity.py` 0-gap.
- `logic.tur` contains zero `\`\`\`c` blocks.
- The same `logic.tur` loads and runs a query in the WASM REPL (it now shares the
  interpreter path with no inline-C).

---

## Implementation notes (as landed)

The engine came out exactly as the plan predicted -- terms/substitution/state
are `defdata` + `match`, no new language feature was needed -- with two
deliberate deviations forced by how the by-value HKT codegen represents things:

1. **Solution stream: a self-contained pure `Stream` ADT, not `import
   backtrack`.** `backtrack.tur`'s stream is an inline-C `{value,next}` Cell
   list whose cells the interpreter builds via natives that expose *no* head/tail
   accessor -- so `reify-walk` / `first-state` (which peek the first solution)
   cannot be written in pure Turmeric against it. A three-line
   `(defdata Stream (StNil) (StCons :Subst :Stream))` walked with `match` is
   already interpreter-native (needs no shim), is peekable, and keeps the whole
   file inline-C-free. `mzero`/`mreturn`/`mplus`/`mbind`/`bt-length` are thin
   wrappers over it, so the public monad surface and the HKT instances are
   unchanged.

2. **Fresh counter folded into the `Subst` base, not a separate `UState`.** The
   empty substitution is `(SNil next)` carrying the next unused var id; prepending
   `SBind` nodes preserves that base, so a single `Subst` threads both the
   bindings and the counter with no extra product type. A non-recursive `UState`
   struct was tried first but the compiled path represents it by-value, which
   breaks the `:int`-carrier erasure the goal/stream plumbing relies on
   (`(:: state :int)` has no int handle to produce); a recursive ADT like `Subst`
   is heap-boxed and erases cleanly.

Goal application still flows through the `:int` fat-closure carrier
(`apply-goal [g : int ...]`, `apply-fat`): typing the goal handle `:int` and
`(:: g (fn [Subst] Stream))` is what makes codegen emit the fat-closure calling
convention -- a `(Goal int)`-typed or bare-`:ptr<void>` cast emits a *thin* call
that drops the captured env and segfaults. Callbacks handed to `fresh` are typed
`:fn` (uniform thin/fat dispatch); a user lambda must annotate its parameter
`(fn [x : Term] ...)` since `:fn` erases the argument type.

## See Also

- `stdlib/logic.tur`, `stdlib/backtrack.tur` (the pure companion / shim
  reference), `src/turi/interpreter_natives.c:3654` (`wk_register_backtrack_natives`).
- `docs/archive/history/turi-interpret-flip-residual-plan.md` Bucket R3 (where the
  fix-vs-carve split was drawn: backtrack fixed, logic carved).
- `stdlib/either.tur:39`, `tests/fixtures/recursive-types/simple-tree/` (defdata
  precedents).
