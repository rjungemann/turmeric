---
title: Solver Extension Plan (SX)
category: Planning
description: Extending the refinement solver into an incremental, backtracking decision procedure -- and the two things an outside reviewer asked for first -- a trail with per-cell opt-out, and a capture/restore cost curve in the benchmark harness.
---

# Solver Extension (SX)

**Status:** proposal, not started. Nothing here is on the critical path to v1;
the whole of SX is *additive* to a solver that already ships and is already
sound. Read section 5 for what to do first if only one phase gets built.

## 0. Provenance

This plan is the write-up of an outside design review of the refinement
solver. Two excerpts from that review are the load-bearing part and are quoted
verbatim in section 1; the rest of this document is (a) what the codebase
actually does today, checked against the source, (b) the literature each of the
reviewer's two points points at, and (c) a phased plan that answers both.

> **Transcript note.** The originating conversation
> (`https://claude.ai/share/bb3cf410-6151-44f4-a2f3-fed54a946731`) is behind a
> Cloudflare interstitial and could not be fetched from the build container --
> `curl` and the fetch tool both get the SPA shell, not the messages. The two
> excerpts below were supplied directly and are treated as the review of
> record. If other parts of that transcript should shape this plan, paste them
> and this document gets a revision, not a rewrite: sections 3 and 4 are
> organized around the two questions, and more questions become more sections.

## 1. The two review questions

### 1.1 Backtrackable state as a runtime primitive

> "If it means something closer to **backtrackable state** -- a trail that
> records mutations and undoes them on backtrack, a la Prolog's WAM or a CLP
> engine -- that's a much better match. That's precisely what EUF's undo trail
> and simplex's bound stack are hand-rolling in C. Having it as a runtime
> primitive would be real leverage, provided you can mark specific cells as
> *not* backtrackable (learned clauses, activity scores, the simplex basis).
> Every constraint system that does this well has that escape hatch; Prolog
> spells it `assert`, ASP solvers spell it 'global.'
>
> So the question I'd put to your own runtime: **can I opt a cell out of the
> trail?** If yes, the theory-seam story gets substantially better than what I
> said earlier. If the trail is all-or-nothing, you're back to threading state
> around it."

Short answer, and the reason section 3 exists: **there is no trail in the
runtime today, so the honest answer is "not yet -- and when it lands, opt-out
is the default, not an escape hatch."** See 3.3 for the spelling.

One correction to the premise, recorded because it changes the shape of the
work: EUF's undo trail and simplex's bound stack are **not** hand-rolled in
`tur` today -- they do not exist. Today's solver backtracks by *discarding*
theory state and rebuilding it from scratch for the next cube
(`refine_solver_euf.c:199`, `refine_solver_no.c:44` -- a fresh `euf_new` /
`la_new` per cube out of an arena). The reviewer is describing the shape the
extension has to take, and is right that it is the shape; SX3/SX4 are where
those two structures get built, and section 3.4 is the table of what they trail
and what they deliberately do not.

### 1.2 The number to get out of the benchmarks

> "On the benchmarks -- the number I'd want out of them isn't 'how far off
> native,' it's **cost per capture/restore as a function of live state under
> the prompt**. That single curve decides more of this design than the
> interpreter-vs-JIT ratio does. Worth making sure it's in the harness while
> you're building it."

Taken as written, and it sets the phase order: the harness change (SX0) lands
**before** the primitive it is meant to measure (SX1). Section 4 designs the
curve.

## 2. Where the solver is today

Facts, checked against the source. The user-facing feature is documented in
[refinement-types-guide.md](../guides/refinement-types-guide.md); the internals
in
[refinement-solver-internals-guide.md](../guides/refinement-solver-internals-guide.md);
the design of record is
[../archive/refinement-types-plan.md](../archive/refinement-types-plan.md).

| Stage | File | What it decides | Incremental? |
|---|---|---|---|
| S0 | `refine_solver_s0.c` | trivial / syntactic | n/a (stateless) |
| S1 | `refine_solver_euf.c` | congruence closure (EUF) | **no** -- `euf_new` per cube |
| S2 | `refine_solver_arith.c` | linear arithmetic, Fourier-Motzkin over exact rationals | **no** -- `la_new` per cube |
| S3 | `refine_solver_no.c` | Nelson-Oppen combination of S1+S2 | **no** -- both rebuilt per cube |
| (S4) | -- | never built; boolean structure is handled by small-DNF cube expansion | -- |

The invariants that constrain every line of this plan:

- **One-directional soundness.** A stage may never answer `RT_VALID` for
  something not entailed. `RT_UNKNOWN` is always safe and always available,
  because every obligation still has its runtime contract check to fall back
  on. Adding a stage moves obligations left in the chain; it never changes an
  answer.
- **Everything is capped** (`refine_solver.h:29-41`): 64 cubes, 64 literals per
  cube, 32 LA variables, 512 LA constraints, 512 EUF terms. A cap hit is
  `RT_UNKNOWN`, i.e. a kept runtime check. Today, *the caps are the reason
  there is no incrementality*: a rebuild per cube is affordable precisely
  because the cube count is bounded at 64.
- **No solver dependency.** No SMT library in any default or release build; the
  Z3 dev oracle was retired in 0.32.5. The replacement is data in the repo --
  `tests/corpus/smtlib/` (125 labelled `.smt2` benchmarks) replayed by the
  `tur_refine_corpus` ctest target -- plus the source-level differential fuzzer
  `tests/refine-fuzz-src.py`. Both of those are the acceptance instruments for
  every phase below.

The existing seams the extension slots behind (`refine_solver.h:70-105`):

```c
EufState *euf_new(RefineVC *, Arena *);
bool      euf_assert_cube(EufState *, const VCCube *);
bool      euf_assert_eq(EufState *, VCTerm *, VCTerm *);
bool      euf_equal(EufState *, VCTerm *, VCTerm *);

LaState  *la_new(RefineVC *, Arena *);
bool      la_assert_cube(LaState *, const VCCube *);
bool      la_assert_le(LaState *, VCTerm *lhs, VCTerm *rhs, bool strict);
bool      la_unsat(LaState *);
bool      la_entails_eq(LaState *, VCTerm *, VCTerm *);
```

These are already the right shape. What they are missing is exactly two
operations -- a mark and an undo -- plus, for SX6, an explanation.

## 3. Answer to Q1 -- the trail

### 3.1 What every system that does this well actually does

The reviewer's claim that the escape hatch is universal holds up. The research,
system by system, with what each one keeps *out* of the undo trail:

**Prolog / the WAM.** On backtracking to a choice point, every binding made
since that choice point is undone; the trail is the record of which. The WAM
does not trail every binding -- it uses **conditional trailing**: compare the
address of the variable being bound against the heap/stack pointers saved in
the topmost choice point, and trail only if the variable is *older* than the
choice point. A variable created after the choice point will be discarded
wholesale on backtrack, so recording it is pure overhead. This is the single
most important optimization in the design, and it generalizes (see 3.2, "the
stamp"). The escape hatch is `assert`/`asserta`: a clause added to the database
is not undone on backtracking, which is exactly how Prolog programs accumulate
knowledge across a search.

**CLP engines.** Constraint engines extend the WAM's binding trail into a
**value trail**: to allow destructive update of a location (a domain bitmap, a
bound), the trail entry records the location *and its previous value*, so undo
restores an arbitrary earlier state rather than just "unbound". The literature
flags the cost this introduces -- the same location written repeatedly inside
one choice-point level gets trailed repeatedly unless the engine timestamps
cells. Handle it the same way conditional trailing handles the WAM case.

**CDCL SAT (MiniSat and descendants).** The assignment trail plus `trail_lim`
(the index in the trail where each decision level began) *is* the mechanism:
backjumping to level `d` pops the trail down to `trail_lim[d]`. What is
explicitly **not** popped: learned clauses (a clause derived by resolution is
implied by the original formula, so it is valid at every level, not just the
one that produced it), VSIDS activity scores and their decay, phase saving, and
the restart/luby state. Those are the reviewer's "learned clauses, activity
scores" -- and their whole value comes from surviving the backtrack that
produced them.

**ASP solvers** spell the same idea "global" -- nogoods/lemmas recorded at
level 0 are permanent.

**Simplex in DPLL(T)** (Dutertre and de Moura, CAV 2006 -- the exact reference
S2b targets). Backtracking saves the value of each **bound** on a stack before
updating it and restores from that stack; the **tableau and the basis are not
restored at all**. Any basis is a valid starting point for the next check, so
restoring one costs pivots and buys nothing. This is the cleanest example in
the literature of "the escape hatch is not a hack, it is where the performance
comes from."

The pattern across all five: the trail covers *semantic* state whose value is
level-dependent, and skips *derived, monotone, or heuristic* state. A design
that cannot express that distinction is not a trail, it is a checkpoint.

### 3.2 The proposed runtime primitive

A per-thread trail in the runtime, with an explicit, allocation-time choice per
cell. Sketch of the C side (`src/runtime/trail.c`, new):

```c
typedef struct TurTrailEntry {
    int64_t     *slot;       /* the cell's payload word */
    int64_t      old;        /* value to restore */
    uint32_t     stamp;      /* level at which this cell was last trailed */
    TurDropFn    drop;       /* rc/aggregate teardown for the discarded value */
    TurCloneFn   clone;      /* rc/aggregate re-acquire for the restored value */
} TurTrailEntry;

uint32_t tur_trail_mark(void);              /* push a level, return its id     */
void     tur_trail_undo_to(uint32_t mark);  /* pop to that level, running undo */
void     tur_trail_commit_to(uint32_t mark);/* drop the level, keep the writes */
void     tur_trail_record(int64_t *slot, int64_t old, TurDropFn, TurCloneFn);
```

Four decisions that are not obvious and should be settled in SX1:

1. **Two cell flavors, not one.** A *value cell* (`BtCell`) trails the previous
   payload on every write -- the CLP shape, needed for union-find parents,
   simplex bounds, domain bitmaps. A *write-once cell* (`LVar`) is the WAM
   shape: it can go from unbound to bound once per level, so the trail entry
   only has to say "reset to unbound", and conditional trailing applies
   verbatim. `stdlib/logic.tur`'s `TVar`/`Subst` pair is exactly the second
   one, today encoded as a persistent association list.
2. **The stamp is what makes it cheap.** Every cell carries the level at which
   it was last trailed. `bt-set!` records to the trail only when
   `cell->stamp < current_level`, then bumps the stamp. A cell written a
   thousand times inside one level costs one trail entry. This subsumes the
   WAM's address comparison (a cell allocated inside the current level starts
   with the current stamp, so its first write is not trailed either) and fixes
   the redundant-trailing problem the CLP literature reports.
3. **Undo must be ownership-correct.** Turmeric values can be rc handles.
   Restoring an old value means re-acquiring it and dropping the value being
   discarded, which is why the entry carries a clone/drop pair. This is the
   same mechanism the continuation machine already uses for owning frame
   environments (`DKEnvClone`/`DKEnvDrop`, `src/runtime/cps_prompt.h`), and
   SX1 should reuse those function types rather than mint parallel ones. A
   `:copy` cell gets both fields NULL and the fast path.
4. **Marks are generational.** A mark is a level id plus a generation counter,
   so `bt-undo-to!` on a mark that has already been undone or committed is a
   detected runtime error rather than memory corruption. This matters because
   multi-shot continuations make stale marks reachable (see 3.5).

Turmeric surface (`stdlib/trail.tur`, new). Types are spelled out per the
project's no-`:int` rule -- a mark is an opaque newtype, not a bare integer:

```turmeric
(defopaque BtCell [A] :ptr<void>)   ; kind (* -> *), like (Goal A) in logic.tur
(defopaque GCell  [A] :ptr<void>)   ; never trailed -- the opt-out flavor
(defopaque Mark   :int)             ; opaque level handle

(defn bt-cell-new  [A] [init : A] : (BtCell A) ...)
(defn g-cell-new   [A] [init : A] : (GCell A)  ...)   ; opts out at allocation
(defn bt-get  [A] [c : (BtCell A)] : A ...)
(defn bt-set!  [A] [c : (BtCell A) v : A] #fx{Bt} : void ...)
(defn bt-mark [] #fx{Bt} : Mark ...)
(defn bt-undo-to!  [m : Mark] #fx{Bt} : void ...)
(defn bt-commit-to! [m : Mark] #fx{Bt} : void ...)
(defn bt-scope [A] [body : (fn [] #fx{Bt} A)] #fx{Bt} : A ...)  ; mark/undo bracket
(defn with-untrailed [A] [body : (fn [] #fx{Bt} A)] #fx{Bt} : A ...)
```

`#fx{Bt}` is a new leaf effect row (name to settle in SX1 against
[effects-system-guide.md](../guides/effects-system-guide.md)). It has to be a
real row for one specific reason: the refinement solver's own purity whitelist
decides whether two occurrences of a call may be treated as the same value
(`refine_collect.h:53`). A function that mutates a trailed cell is not pure and
must not be congruence-collapsed. Adding the primitive without adding the row
would put a hole in the thing this plan is extending.

### 3.3 So: can you opt a cell out?

**Yes, three ways, in ascending scope.** The design deliberately makes the
answer "yes" at every granularity the reviewer's examples need:

| Granularity | Spelling | Motivating case |
|---|---|---|
| Per cell, forever | `g-cell-new` instead of `bt-cell-new` | activity scores, learned-clause database, the simplex basis and tableau |
| Per write | `with-untrailed` around the mutation | a counter bumped inside a search that must survive it (node count, statistics) |
| Per level | `bt-commit-to!` instead of `bt-undo-to!` | a level's writes are kept -- Prolog's `assert`, ASP's "global", promoting a lemma to level 0 |

`GCell` and `BtCell` are *distinct types*, so opting out is visible in the
signature of anything that touches the cell rather than a flag someone forgets.
A `GCell` write is not an undo-able effect, which is precisely the property the
learned-clause database wants.

The trail is therefore not all-or-nothing, and the "threading state around it"
fallback the reviewer worried about does not apply.

### 3.4 What the extended solver trails, and what it does not

This table is the actual answer to "does the theory-seam story get better", and
it is worth writing down before any code because it is the specification for
SX3 and SX4:

| Solver state | Trailed | Why |
|---|---|---|
| EUF union-find parent / rank arrays | yes (value cells) | class membership is level-dependent |
| EUF congruence lookup table entries | yes | derived from the partition |
| EUF disequality list | yes | asserted per level |
| Simplex bounds `lo[x]`, `hi[x]` | yes -- *this is the bound stack* | Dutertre--de Moura; the only thing that must be restored |
| Simplex tableau rows and basis | **no** | any basis is a valid start; restoring costs pivots, buys nothing |
| Simplex assignment `beta(x)` | **no** | repaired lazily on the next `check()` |
| Learned lemmas / blocking clauses (SX6) | **no** | implied by the original VC, hence valid at every level |
| Branching heuristic scores (SX6) | **no** | heuristic; the whole point is that it accumulates |
| Hash-consed `VCTerm` table, the arena | **no** | monotone within a query; terms are immortal by construction |
| RT7 discharge memo (fingerprint -> verdict) | **no** | process-lifetime, already keyed on VC identity |
| RT6 hint-generation probes | **no** (each probe is its own query) | speculative, reports nothing |

Five of the eleven rows are opt-outs. That ratio is the reason a trail without
an escape hatch would not have been usable here.

### 3.5 The part that needs care -- trail meets multi-shot continuations

Turmeric already ships `shift`/`reset`, `shift0`/`reset0`, `call/cc`,
`call/cc*` (multi-shot) and `escape`
([delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md)).
A trail is a *stack* discipline; a multi-shot continuation is not. The
interaction has one sharp edge and one design decision:

- **Unwinding is fine.** `escape`, an abortive `shift0`, a panic: the runtime
  unwinds past a `bt-scope`, which runs the undo. Same as any scope-exit
  action.
- **Re-entering is the sharp edge.** Invoke a captured `k` twice and the second
  invocation resumes a computation whose trail levels were already undone. The
  writes are not replayed by the capture -- the continuation captured *control*,
  not *state*. Options: (a) make it a checked runtime error via the generation
  counter of 3.2(4) -- cheap, honest, and the recommendation for SX1; (b) have
  `call/cc*` snapshot the live trail segment, making state genuinely multi-shot
  at a cost proportional to the trail depth -- which is the second axis of the
  benchmark in section 4 and should not be built before that curve exists.

Recommendation: ship (a); measure; only consider (b) if the curve says it is
affordable and something real wants it. Note that this is precisely why the
reviewer's benchmark request comes first in the phase order.

- **Serialization.** `serial.c` / the serializable-continuation path must
  either serialize the live trail segment alongside the continuation or refuse
  to serialize inside a `bt-scope`. Refusing is the SX1 answer; the diagnostic
  should say which scope.

### 3.6 Honest scoping -- who actually benefits

Two consumers, and they are not the same:

1. **User-level search in Turmeric** -- the direct beneficiary.
   `stdlib/logic.tur` currently threads a persistent association-list `Subst`
   with a fresh-variable counter riding in its base; `stdlib/backtrack.tur` is
   a list monad over eagerly built alternative lists. Both are the "thread state
   around it" design the reviewer named. With a trail, miniKanren's
   substitution becomes a destructive union-find with undo -- the actual WAM
   design -- and the backtracking monad can become a `bt-scope`-bracketed
   depth-first search that does not materialize alternatives.
2. **The compiler's own solver** -- an *indirect* beneficiary, and this should
   not be oversold. `tur`'s solver is C and stays C; a Turmeric runtime
   primitive does not simplify it. What SX3/SX4 take from SX1 is the *design*
   and a small shared C utility (`src/compiler/trail_c.h`), so the two
   implementations agree on stamps, opt-out, and undo ordering, and so a later
   self-hosting step is a port rather than a redesign.

Calling this out matters because the review's phrase "having it as a runtime
primitive would be real leverage" is true for (1) and only aspirationally true
for (2).

## 4. Answer to Q2 -- the capture/restore curve

### 4.1 What the literature measures

The reviewer's metric is the standard one for this design space, and the
implementation strategies are distinguished precisely by their shape on that
curve:

- **Stack copying** -- capture copies the live stack segment. Cost is linear in
  bytes of live state under the prompt; restore likewise. Simple, and the
  baseline every other strategy is measured against.
- **Segmented / chunked stacks** (Hieb, Dybvig and Bruggeman; then Bruggeman,
  Waddell and Dybvig for one-shot continuations). Capture splits the stack at
  the top frame and hands the bottom segment to the continuation -- **constant
  time**, with the copying deferred to (and paid only by) an actual re-entry.
  One-shot continuations then eliminate the copy entirely, because there is no
  second entry to protect against.
- **Heap-allocated frames / CPS** -- capture is O(1) pointer copy because
  frames are already heap objects and immutable; the cost moves to every call
  (allocation) rather than to capture. Farvardin's "Weighing Continuations for
  Concurrency" is the modern head-to-head of these three.
- Contemporary systems land in the same taxonomy: OCaml 5's effect handlers use
  segmented fibers; WasmFX's stack switching is the same idea at the Wasm
  level.

The single most useful thing the curve reveals is **the intercept versus the
slope**: a strategy with a large constant and zero slope beats a strategy with
a small constant and a real slope at some crossover point, and where that
crossover sits relative to real workloads is the whole decision.

### 4.2 Turmeric's three capture paths and their predicted shapes

This is what the harness has to separate, because they are genuinely different
curves and today nothing distinguishes them:

| Path | Where | Predicted capture cost | Predicted restore cost |
|---|---|---|---|
| DK chain slice (compiled `shift`/`reset`, effect handlers) | `cps_prompt.c`, mirrored in `tur_rt_split.c:1134+` | O(frames under the prompt) -- one `dk_copy_node` per frame, plus one `env_clone` per *owning* frame | O(frames) again on each `dk_invoke`, because invoke replays a fresh copy |
| Fiber / ucontext | `FiberBlock`, `tur_rt_split.c:1857+` | O(1) to suspend, but 1 MiB default stack per fiber allocated up front | O(1) switch |
| Cloneable cont snapshot | `tur_cloneable_cont_clone` = `tur_continuation_snapshot`, `tur_rt_split.c:1113` | O(env), one `clone_env` call | per *resume*, not per capture -- `emit_effects.c:1113` snapshots before every `(k v)` |

The DK chain is documented as "Capture is O(depth-of-slice) and unbounded -- no
16-frame ceiling" (`tur_rt_split.c:1137`). That is a slope, and the multi-shot
path pays it on *every resume*. Whether the constant is small enough that the
slope does not matter until 100 frames or 10 000 frames is exactly the
unmeasured number, and it is the number that decides 3.5(b).

### 4.3 The benchmark, specified

**Independent variables** (sweep each, hold the others fixed):

1. `F` -- frames under the prompt: 1, 2, 4, ... 4096 (log sweep).
2. `E` -- owning-env bytes per frame: 0 (all `:copy`, `env_clone == NULL`),
   1 rc handle, 8, 64. This separates the per-node cost from the per-owner
   clone cost, which is the difference between a memcpy slope and a refcount
   slope.
3. `R` -- resumes per capture: 1 (one-shot) and 8 (multi-shot). The gap between
   these two *is* the one-shot optimization the literature reports, measured
   rather than assumed.
4. `T` -- trailed writes live under the prompt: 0, 10, 10^3, 10^5 (added by
   SX1; the axis exists in the harness from SX0 with `T = 0` so the file does
   not need reshaping later).

**Dependent variables:** ns per capture, ns per restore, bytes allocated per
capture, peak RSS. Report the fitted `a + b*F + c*E` per path, not just the
totals -- the intercept and the slope are separately actionable.

**Baselines to include in the same table**, so the curve has meaning rather
than just a shape:
- a plain function call and a plain closure invocation (the floor);
- `hamt-bench-snapshot.tur`, the existing persistent-structure snapshot cost
  (this is the "thread state around it" alternative the trail competes with,
  and the crossover between them is a real design output);
- once SX1 lands, `bt-mark`/`bt-undo-to!` over `T` writes.

**Deliverables:** `benchmarks/bench-capture-restore.tur`,
`benchmarks/bench-trail-undo.tur` (SX1), `benchmarks/run-capture-curve.sh`
emitting CSV plus a markdown table appended to
`benchmarks/benchmark-results.md`. Run compiled and `--interpret`, and under
the JIT where available -- but note that the interpreter/JIT ratio is
explicitly *not* the headline number; the per-path slope is.

**Method discipline**, because this repo has been bitten by it: never run the
sweep concurrently with a build or another suite. `tests/run.sh` stamps the
binary for exactly this reason; the benchmark script should do the same, and
refuse to publish a row if `./build/tur` changed mid-run.

## 5. The plan

Phases are ordered so that each one is independently landable and independently
valuable. SX0 is the only one with a hard ordering claim attached (the reviewer
asked for the harness "while you're building it", and 3.5(b) cannot be decided
without it).

### SX0 -- the capture/restore curve in the harness

- **Do:** section 4.3, with `T = 0`. No language or runtime change.
- **Files:** `benchmarks/bench-capture-restore.tur`,
  `benchmarks/run-capture-curve.sh`, `benchmarks/benchmark-results.md`.
- **Accept:** the three paths of 4.2 each produce a fitted slope and intercept
  over `F` and `E`; the table is checked in; a rerun on an idle box reproduces
  within noise.
- **Size:** small. **Gate:** none (benchmarks are not a compiler feature).

### SX1 -- the trail primitive

- **Do:** `src/runtime/trail.{c,h}` per 3.2, `stdlib/trail.tur` per the surface
  in 3.2, the `#fx{Bt}` effect row, the generational stale-mark error, the
  serialization refusal of 3.5.
- **Accept:** fixtures for value cells, write-once cells, stamp suppression of
  redundant trailing, `GCell` never appearing on the trail, `with-untrailed`,
  `bt-commit-to!`, ownership-correct undo of an rc payload under ASan/LSan, and
  a stale-mark error that is a diagnostic rather than a crash. `bench-trail-undo`
  joins the SX0 table.
- **Gate:** `EXPERIMENTS[]` row `backtrackable-state`, `plan_path` pointing
  here, `introduced` 0.36.0, `expires_at` 0.39.0, lifecycle prototype,
  `opt_global` `g_opt_backtrackable_state`, and
  `experiment_warn_if_used("backtrackable-state")` at the elaboration entry
  point -- per the project's experimental-features rule. This is exactly the
  "semantics in flux" case the gate exists for: 3.5's re-entry question is open
  on purpose.
- **Size:** medium. The runtime is a few hundred lines; the effect row and the
  ownership glue are where the work is.

### SX2 -- make the stdlib search paths use it

- **Do:** a trailed union-find substitution behind `stdlib/logic.tur`'s existing
  `Goal`/`Subst` API (the persistent path stays as the default until the
  benchmark says otherwise); a `bt-scope`-bracketed depth-first driver in
  `stdlib/backtrack.tur` alongside the list monad.
- **Accept:** `benchmarks/bench-logic-query.tur` and
  `bench-backtrack-n-queens.tur` run both paths and the crossover point is
  recorded. Existing fixtures keep passing on the persistent path.
- **Why it is in this plan at all:** it is the honest test of 3.6(1). If a
  trailed union-find does not beat a persistent assoc list on
  `bench-logic-query`, the primitive is not paying for itself and SX3/SX4
  should proceed as plain C with no shared utility.

### SX3 -- incremental EUF (S1i)

- **Do:** extend the `euf_*` seam with `euf_mark` / `euf_undo_to`, backed by
  `src/compiler/trail_c.h` (the C sibling of SX1, same stamp discipline).
  Replace the per-cube `euf_new` in `refine_s1_decide` and `no_cube_unsat` with
  assert/mark/undo over one state.
- **Accept:** identical verdicts on all 125 corpus benchmarks and all
  `refine-*` fixtures -- this phase changes *cost*, not answers, and a verdict
  diff is a bug. `TUR_REFINE_STATS=1` shows the same proven/refuted/unknown
  counts. Measure compile time on the widest fixtures.
- **Gate:** none needed if verdicts are bit-identical; keep the old path behind
  `TUR_REFINE_EUF=rebuild|incremental` as a test seam (env-only, like
  `TUR_REFINE_NO_DISCHARGE`) so the corpus can be replayed against both.

### SX4 -- incremental simplex (S2b)

- **Do:** the Dutertre--de Moura solver behind the existing `la_*` seam, with a
  bound stack and *no* basis restoration (3.4). Add `la_mark` / `la_backtrack`.
  Note the naming collision: `la_push` is already taken by the static
  constraint-adder at `refine_solver_arith.c:249` -- do not overload it.
- **Accept:** every obligation S2 proves today is still proven (superset
  property; a regression here is a real regression), the `REFINE_MAX_LA_CONSTR`
  cap stops biting on the corpus's wide benchmarks, and the strict-inequality
  handling matches the paper's delta-rational treatment. Exact rationals with
  overflow checks throughout, as today.
- **Size:** the design of record already estimates 2--4 weeks for this, and that
  estimate looks right. This is the single largest phase.
- **Note:** the archived plan says FM was chosen over simplex deliberately, and
  that simplex slots in "if the cap ever bites". Before building SX4, *check
  whether it bites*: run the corpus and the fuzzer with cap-hit telemetry. If
  nothing real hits `REFINE_MAX_LA_CONSTR`, SX4 is a performance project with no
  user-visible payoff and should wait behind SX6.

### SX5 -- incremental Nelson-Oppen (S3i)

- **Do:** run the equality exchange over the marked/undoable S1i and S2b states
  instead of rebuilding both per cube. The exchange loop itself is unchanged.
- **Accept:** as SX3 -- identical verdicts, lower cost. `NO_MAX_SHARED` and
  `NO_MAX_ROUNDS` can then be raised on evidence rather than caution.

### SX6 -- a small CDCL(T) core (S4)

The line the design of record explicitly does not cross ("no DPLL(T) engine").
This phase proposes crossing it *narrowly*, and the framing matters:

- **Do:** replace the DNF cube expansion **only where it overflows** with a
  small CDCL loop over theory atoms: decide, propagate, on conflict ask the
  theory for an explanation (`euf_explain` / `la_explain` returning a conflict
  literal set), learn the clause, backjump via the trail. Learned clauses and
  activity scores are `GCell`-equivalent -- never undone (3.4).
- **Why narrowly:** today a cube-cap overflow is `RT_UNKNOWN`, i.e. a kept
  runtime check. That is *already correct*. So S4 is a pure recall improvement
  with a hard floor: if the CDCL core answers `UNKNOWN` for everything, nothing
  regresses. That makes it the safest possible way to build a SAT engine.
- **Accept:** the corpus benchmarks currently answered `UNKNOWN` for cap
  reasons shrink; no verdict flips from `VALID`/`INVALID` to anything else; the
  source-level fuzzer stays clean across at least six seeds.
- **Gate:** `EXPERIMENTS[]` row `solver-cdcl` while in flux. Graduating means
  deleting the row and making it unconditional, per the project rule.
- **Do not:** attempt a competitive SAT solver. The stopping condition is "no
  real obligation is `UNKNOWN` for propositional reasons", not benchmark
  parity with a real SMT solver.

### SX7 -- integer completeness, the long tail (S2c)

- **Do:** branch-and-bound over the SX4 simplex, depth-limited, `UNKNOWN` past
  the limit. Optionally Gomory cuts. Closes the integer non-convexity hole that
  makes S3 incomplete on disequality case-splits today.
- **Gate:** depth cap is a constant, not a flag. Deferrable indefinitely; it is
  the last phase for a reason.

### SX8 -- docs and graduation

- Update
  [refinement-solver-internals-guide.md](../guides/refinement-solver-internals-guide.md)
  (the stage table, the caps table, the new seam operations, the incrementality
  story), write a `docs/guides/backtrackable-state-guide.md` for the SX1
  surface, and cross-link from
  [backtracking-guide.md](../guides/backtracking-guide.md) and
  [logic-programming-guide.md](../guides/logic-programming-guide.md).
- Graduate `backtrackable-state` (delete the row, behavior unconditional) once
  3.5's re-entry question is settled and SX2 has a benchmark answer. Graduate
  `solver-cdcl` on the corpus criterion in SX6. Move this plan to
  `docs/archive/` when the last phase lands, per the archiving rule.

### Recommended order if only some of it gets built

**SX0, then SX1, then SX2.** Those three answer both review questions with
evidence and leave a primitive the language did not have. SX3 is a cheap
follow-on. SX4 waits on cap-hit telemetry. SX6 is the one with real recall
payoff for users and is worth pulling forward past SX4 if the telemetry says
the LA cap is not biting. SX7 last, or never.

## 6. Explicitly not doing

- **Not** linking an external SMT solver, in any build. The Z3 oracle was
  retired for reasons that still hold, and the corpus plus the source-level
  fuzzer replaced it with something that runs everywhere, including WASM.
- **Not** building a competitive SAT solver (see SX6's stopping condition).
- **Not** making captured continuations capture *state*. A continuation
  captures control; the trail is a separate mechanism with a separate scope
  discipline. Conflating them is how this design gets slow in a way that cannot
  be undone later.
- **Not** removing the caps. Every cap stays; incrementality changes what a cap
  costs, not whether it exists. An `UNKNOWN` is always available and always
  sound.
- **Not** trailing by default in the compiler's arena-allocated VC structures.
  Terms are immortal within a query by construction; that is a feature.

## 7. Risks

| Risk | Mitigation |
|---|---|
| Incremental EUF/simplex changes a verdict | Corpus + fixtures are verdict-identical by acceptance criterion; keep the rebuild path behind an env test seam for differential replay |
| Trail undo mis-handles rc ownership -> leak or double-free | ASan/LSan is on for the compiled fixture path; add a fixture with an rc payload written and undone in a loop |
| Multi-shot resume across a stale mark corrupts state | Generational marks, checked at `bt-undo-to!` (3.2(4)); the alternative is a silent wrong answer |
| SX4 is weeks of work with no user-visible payoff | Gate SX4 behind cap-hit telemetry from the corpus before starting |
| Fixture snapshot churn | Regenerate in the same PR as the codegen change, per the project rule; SX1 is the only phase that touches codegen at all |
| The benchmark measures the wrong thing under load | Serialize the sweep, stamp the binary, refuse to publish rows if it changed mid-run |

## 8. Open questions to take back to the reviewer

1. Multi-shot re-entry (3.5): checked error, or state-snapshotting `call/cc*`?
   The SX0 curve informs it but does not settle the semantics question.
2. Is a *write-once* cell flavor (`LVar`) worth its own type, or should
   everything be a value cell with the stamp doing the optimization? The WAM
   says yes; a smaller API says no.
3. For SX6: is theory propagation (not just conflict explanation) worth it at
   this scale, or is explain-and-learn enough given obligations of a few dozen
   atoms?

## 9. References

**In-tree**

- [refinement-solver-internals-guide.md](../guides/refinement-solver-internals-guide.md) -- the current pipeline, caps, seams, diagnostics.
- [refinement-types-guide.md](../guides/refinement-types-guide.md) -- the user-facing feature.
- [../archive/refinement-types-plan.md](../archive/refinement-types-plan.md) -- design of record; S0--S4 and RT1--RT7 phase names, the S2b/S2c/S4 tail, the Z3 retirement criteria.
- [delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md), [effects-system-guide.md](../guides/effects-system-guide.md), [backtracking-guide.md](../guides/backtracking-guide.md), [logic-programming-guide.md](../guides/logic-programming-guide.md), [experimental-flags-guide.md](../guides/experimental-flags-guide.md).
- Source: `src/compiler/refine_*.{c,h}`, `src/runtime/cps_prompt.{c,h}`, `src/runtime/generated/tur_rt_split.c`, `stdlib/{logic,backtrack,stm,ref}.tur`.

**External -- backtrackable state**

- Warren Abstract Machine trailing and conditional trailing; see the ECLiPSe and BinProlog architecture papers for the CLP value-trail extension and its redundant-trailing problem: <https://ar5iv.labs.arxiv.org/html/1012.4240>, <https://arxiv.org/pdf/1102.1178>, <http://eclipseclp.org/reports/ECRC-95-11.pdf>.
- B. Dutertre and L. de Moura, "A Fast Linear-Arithmetic Solver for DPLL(T)", CAV 2006 -- the bound stack, and the decision not to restore the basis: <https://yices.csl.sri.com/papers/cav06.pdf>.
- MiniSat's trail / `trail_lim` / backjumping, and what survives a backjump: <https://github.com/niklasso/minisat/blob/master/minisat/core/Solver.cc>, <https://www.cs.cmu.edu/~mheule/publications/JSAT7_11_vanderTak.pdf>.
- G. Nelson and D. Oppen, "Simplification by Cooperating Decision Procedures", ACM TOPLAS 1(2), 1979.

**External -- capture/restore cost**

- R. Hieb, R. K. Dybvig, C. Bruggeman, "Representing Control in the Presence of First-Class Continuations" -- stack segments, constant-time capture: <https://www.cs.tufts.edu/~nr/cs257/archive/kent-dybvig/stack.pdf>.
- C. Bruggeman, O. Waddell, R. K. Dybvig, "Representing Control in the Presence of One-Shot Continuations" -- eliminating the copy for one-shot capture: <https://www.cs.tufts.edu/comp/150VM/modules/archive/kent-dybvig/one-shot-continuations.pdf>.
- W. Clinger, A. Hartheimer, E. Ost, "Implementation Strategies for First-Class Continuations" -- the taxonomy the three paths in 4.2 fall into: <https://link.springer.com/article/10.1023/A:1010016816429>.
- K. Farvardin, "Weighing Continuations for Concurrency" -- modern head-to-head measurement of the strategies: <http://manticore.cs.uchicago.edu/papers/farvardin-masters.pdf>.
- "Continuing WebAssembly with Effect Handlers" (WasmFX) -- segmented-stack switching in a contemporary runtime: <https://arxiv.org/pdf/2308.08347>.
