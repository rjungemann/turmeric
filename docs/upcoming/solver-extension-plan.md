---
title: Solver Extension Plan (SX)
category: Planning
description: Extending the refinement solver into an incremental, backtracking decision procedure -- a trail with per-cell opt-out, a capture/restore cost curve in the benchmark harness, an offline lazy-SMT stepping stone, an untrusted-search/trusted-checker seam, and a query surface for interrogating the solver from outside the compiler.
---

# Solver Extension (SX)

**Status:** proposal, not started. Nothing here is on the critical path to v1;
the whole of SX is *additive* to a solver that already ships and is already
sound. Read section 5 for what to do first if only one phase gets built.

## 0. Provenance

This plan is the write-up of an outside design review of the refinement
solver. Two excerpts from that review are quoted verbatim in section 1; the
originating conversation
(`https://claude.ai/share/bb3cf410-6151-44f4-a2f3-fed54a946731`) sits behind a
Cloudflare interstitial and could not be fetched from the build container, but
a structured summary of the full discussion was supplied afterwards and is
folded in throughout. Beyond the two headline questions, that summary covers:
why the cube design is what it is and when it stops being tolerable; the real
cost of moving to DPLL(T)/CDCL(T) (the theory seam and the testing burden, not
the SAT core); whether continuations help the theory seam (mostly no, with
specific exceptions); the consequences of `tur` having a live compile-time
runtime; multi-shot versus backtrackable; and a recommended layer split. Each
of those has a home below: sections 2, 3.5-3.7, 4, and the phase structure of
section 5.

The rest of this document is (a) what the codebase actually does today,
checked against the source, (b) the literature each point leans on, and (c) a
phased plan.

One item is *not* from the review: the external interrogation surface (SX8)
was requested in follow-up -- expose the solver so tools, tests, and users
outside the compile pipeline can put queries to it directly. It slots in
cleanly because every later phase makes the same surface strictly more
informative (marks make `push`/`pop` real, certificates make `get-proof`
real), so it is specified here rather than as a separate plan.

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
| S1 | `refine_solver_euf.c` | congruence closure (EUF), naive O(n^2) fixpoint | **no** -- `euf_new` per cube |
| S2 | `refine_solver_arith.c` | linear arithmetic, Fourier-Motzkin over exact int64 rationals | **no** -- `la_new` per cube |
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

### 2.1 Why the cube design is what it is, and when it stops being tolerable

The review walked through this and it is worth pinning down, because it is the
premise behind SX6:

- Theory procedures decide *conjunctions*. Congruence closure takes a set of
  (dis)equalities; Fourier-Motzkin takes a set of linear inequalities. Neither
  handles disjunction. The DNF cube expansion exists to strip boolean structure
  so each cube can be handed to S1/S2/S3 whole.
- DNF is worst-case exponential, which is why no production solver does it this
  way: real solvers run DPLL(T)/CDCL(T), where a SAT engine lazily enumerates
  boolean assignments (each one *is* a cube) with clause learning and theory
  propagation. Turmeric's caps make the naive expansion tolerable, and the
  runtime fallback makes the cap-hit answer sound.
- The disequality wrinkle compounds it: `a != b` over a total order is
  `a < b OR b < a`, so a *negated equality* creates a disjunction in NNF --
  even the most natural postcondition shape, `(= r <expr>)`, needs cube
  expansion. The same integer non-convexity is what S3 declines to case-split
  on.
- The diagnosis for when small-DNF actually fails: **when the propositional
  structure of VCs grows** -- path-sensitive obligations over `match`/`if`, or
  loop unrolling. If RT1 does not generate such VCs yet, a SAT engine buys very
  little. That is a measurable question, and SX0(b) exists to answer it before
  anyone writes one.

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

These are already the right shape. What they are missing is exactly three
operations -- a mark, an undo, and (for SX6b) an *explanation*: the ability to
return the inconsistent subset of asserted literals that caused a conflict, so
the caller can learn a clause from it. The first two are the trail; the third
is the proof machinery of 3.7.

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
produced them. The canonical backtrack is *decrementing a trail index*, and
CDCL does it millions of times per second -- a number that matters in 3.6.

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

### 3.5 Trail meets continuations -- and why multi-shot is the wrong strength

Turmeric already ships `shift`/`reset`, `shift0`/`reset0`, `call/cc`,
`call/cc*` (multi-shot) and `escape`
([delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md)).
A trail is a *stack* discipline; a multi-shot continuation is not.

First, the framing point from the review, which the rest of this section
serves: **backtracking in a solver is asymmetric, and a continuation is
symmetric.** A continuation restores *everything*; a CDCL backjump discards the
trail above the backjump level but **keeps the learned clause** -- the only
thing preventing re-exploration -- and simplex keeps its basis. Threading the
kept state around the restore leaves the continuation carrying nothing. So "use
continuations as the theory-seam backtracking mechanism" dissolves into "use
backtrackable *state*" -- this section's trail -- with control expressed
however is convenient. If "backtrackable continuation" is read as "capture a
resumption point and re-enter", that is one-shot capture by another name and
the same asymmetry objection stands.

Second: **multi-shot is specifically the wrong strength for solver-internal
search.** Reusability is what makes capture expensive -- the copying and the
immutability discipline exist so a second entry does not see the first entry's
mutations. CDCL-style search is *entirely* mutation over a shared trail, watch
lists, and activity scores, and each node of its search tree is entered exactly
once. A multi-shot default pays for reusability, on the most frequent operation,
for an algorithm that never uses it. The runtime already distinguishes the
paths (`call/cc` one-shot vs `call/cc*` multi-shot); the one-shot path is the
relevant one for search, and SX0's `R` axis measures the one-shot/multi-shot
gap instead of assuming it.

With that placed, the interaction between the trail and the existing operators
has one sharp edge and one design decision:

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
affordable and something real wants it.

- **Serialization.** `serial.c` / the serializable-continuation path must
  either serialize the live trail segment alongside the continuation or refuse
  to serialize inside a `bt-scope`. Refusing is the SX1 answer; the diagnostic
  should say which scope.

**One candidate *genuine* multi-shot use: RT6 hint generation.** Today each
candidate hypothesis re-runs the whole chain from scratch (`run_chain`,
`refine_discharge.c:139`), which is why the RT7 memo exists. The multi-shot
shape fits exactly: capture a continuation after the hypotheses are asserted
but before the theory work, then re-enter it once per candidate -- same prefix,
many futures. Whether it beats the memo depends on how much work precedes the
branch point, which is measurable and should be measured (SX0 gives the
capture cost; `TUR_REFINE_STATS` gives the probe count) before anyone builds
it. Filed as open question 3 in section 8, not as a phase.

### 3.6 Honest scoping -- who benefits, and the layer split

Three consumers, and they are not the same. An earlier draft of the review
dismissed compiler-side use on the grounds that the solver is compile-time C
with no runtime available -- that objection **dissolved on inspection**:
full-Turmeric macros run pre-type-resolution, so there *is* a live Turmeric
runtime inside `tur` at compile time. What survives is a cost argument, and it
produces a split rather than a verdict:

| Layer | Frequency | Where it lives | Trail / continuations? |
|---|---|---|---|
| BCP / unit propagation, watch lists, simplex pivoting, union-find find/union | millions of ops/sec | C | **no** -- the canonical backtrack here is "decrement a trail index"; anything heavier is overhead on the hottest loop. WASM stack-switching is also awkward, and the zero-dependency playground runs this code |
| branch-and-bound, splitting-on-demand, strategy selection, RT6 candidate enumeration | thousands of ops/sec | Turmeric (compile-time runtime), eventually | **yes** -- nondeterminism expressed directly with `bt-scope` and one-shot capture |

The seam between the layers is the point: **the Turmeric side proposes, the C
side validates** (see 3.7). The interpreter being ~100-1000x off native for
tight mutation loops is a *category gap* for the top row -- not a deferred
optimization -- and is perfectly adequate for the bottom row.

So, the three consumers:

1. **User-level search in Turmeric** -- the direct beneficiary.
   `stdlib/logic.tur` currently threads a persistent association-list `Subst`
   with a fresh-variable counter riding in its base; `stdlib/backtrack.tur` is
   a list monad over eagerly built alternative lists. Both are the "thread
   state around it" design the reviewer named. With a trail, miniKanren's
   substitution becomes a destructive union-find with undo -- the actual WAM
   design -- and the backtracking monad can become a `bt-scope`-bracketed
   depth-first search that does not materialize alternatives. Elsewhere in the
   compiler's own problem domain, the elaborator's searches (unification with
   backtracking, typeclass instance resolution, RT4 template inference) have
   the same shape and are eventual candidates.
2. **The solver's hot layer** -- the top row of the table. It is C and stays
   C. What SX3/SX4 take from SX1 is the *design* and a small shared C utility
   (`src/compiler/trail_c.h`), so the two implementations agree on stamps,
   opt-out, and undo ordering.
3. **The solver's search layer** -- the bottom row, the genuinely new
   possibility the compile-time runtime opens. Branch-and-bound for the
   integer tail (SX7) can be *prototyped in Turmeric* against
   `tests/corpus/smtlib/` and ported to C only if the stats move. This is
   cheap to try and cheap to discard, which is the right risk profile for the
   most speculative part of the plan.

### 3.7 TCB discipline -- untrusted search, trusted checking

A `RT_VALID` verdict elides a runtime check, so everything that can produce
one is trusted compute. Today that is a few kLOC of in-house C. A
Turmeric-hosted search stage (3.6, consumer 3) would add the macro expander,
the compile-time runtime, and its dependencies to that trusted base --
potentially including Turmeric code that itself carries refinements decided by
the very solver being bootstrapped. That is not a reason to forbid it; it is a
reason to adopt the architecture every serious proof-producing solver adopts:

> **Search is untrusted; checking is trusted.** No verdict proposed by a
> search layer -- Turmeric-hosted or C -- is accepted unless its certificate
> passes an independent checker. An unverified certificate is `RT_UNKNOWN`.

The certificates are small and known:

- **Simplex:** Farkas coefficients from the infeasible row -- a linear
  combination of asserted bounds summing to a contradiction; checking is a dot
  product in exact rationals.
- **EUF:** the path through the proof forest (Nieuwenhuis-Oliveras
  proof-producing congruence closure); checking is replaying a list of merges.
- **Propositional:** the resolution chain behind a learned clause; DRAT is the
  standard format for exactly this.

The checker is a few hundred lines of C that never has to be smart, and it
collapses the TCB back to something auditable regardless of how clever the
search above it becomes. It also converts the soundness invariant from a
review discipline into a mechanical property -- which section 5 leans on hard,
because the classic CDCL(T) bug class (a subtly wrong explanation clause that
prunes the search into a silent, input-dependent wrong answer) is exactly the
kind of thing a 125-case corpus does not catch. Prior art: veriT and the
Alethe proof format; DRAT-trim for the propositional half.

Bootstrap ordering note, so nobody trips on it later: macros run
pre-type-resolution and discharge runs during elaboration, so a solver stage
written in Turmeric must be fully expanded and executable before the first
obligation of the unit being compiled exists. Practically: the stage ships
pre-compiled with `tur` (like the stdlib), it is not compiled by the unit that
uses it.

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
unmeasured number, and it is the number that decides 3.5(b) -- and, per 3.5,
whether a one-shot fast path needs to exist before any search layer uses
capture at all.

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
explicitly *not* the headline number; the per-path slope is. The
interpreter's row matters for one decision only: confirming (or refuting) the
3.6 claim that the tree-walker is adequate for a thousands-per-second search
layer.

**Method discipline**, because this repo has been bitten by it: never run the
sweep concurrently with a build or another suite. `tests/run.sh` stamps the
binary for exactly this reason; the benchmark script should do the same, and
refuse to publish a row if `./build/tur` changed mid-run.

## 5. The plan

Phases are ordered so that each one is independently landable and independently
valuable. SX0 must be first: both of its instruments produce the decision data
the later phases are gated on, and the reviewer asked for one of them "while
you're building it".

### SX0 -- measure first

Two instruments, no language or runtime change:

- **(a) The capture/restore curve** -- section 4.3, with `T = 0`.
  Files: `benchmarks/bench-capture-restore.tur`,
  `benchmarks/run-capture-curve.sh`, `benchmarks/benchmark-results.md`.
  Accept: the three paths of 4.2 each produce a fitted slope and intercept
  over `F` and `E`; the table is checked in; a rerun on an idle box reproduces
  within noise.
- **(b) Cap-hit telemetry** -- extend `TUR_REFINE_STATS=1` to report *which*
  cap bit and how often (cubes, cube literals, LA vars, LA constraints, EUF
  terms, NO shared/rounds), then sweep the corpus, the fuzzer seeds, and the
  largest in-tree refinement users. This answers 2.1's question -- do
  path-sensitive VCs with real propositional structure exist yet? -- and gates
  SX4 and SX6: if `REFINE_MAX_LA_CONSTR` never bites, SX4 waits; if the cube
  caps never bite, SX6 waits.

**Size:** small. **Gate:** none (benchmarks and stats are not compiler
features).

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
  assert/mark/undo over one state. Pick the union-find representation with the
  Nieuwenhuis-Oliveras proof forest in mind -- not built here, but the merge
  history it needs must be recoverable, so SX6b's `euf_explain` is an addition
  rather than a second rewrite.
- **Accept:** identical verdicts on all 125 corpus benchmarks and all
  `refine-*` fixtures -- this phase changes *cost*, not answers, and a verdict
  diff is a bug. `TUR_REFINE_STATS=1` shows the same proven/refuted/unknown
  counts. Measure compile time on the widest fixtures.
- **Gate:** none needed if verdicts are bit-identical; keep the old path behind
  `TUR_REFINE_EUF=rebuild|incremental` as a test seam (env-only, like
  `TUR_REFINE_NO_DISCHARGE`) so the corpus can be replayed against both.

### SX4 -- incremental simplex (S2b)

- **Do:** the Dutertre--de Moura solver behind the existing `la_*` seam, with a
  bound stack and *no* basis restoration (3.4). Add `la_mark` / `la_backtrack`,
  and produce **Farkas coefficients** on infeasibility from day one -- they are
  the explanation SX6b needs and the certificate 3.7's checker verifies, and
  retrofitting them is harder than emitting them.
  Note the naming collision: `la_push` is already taken by the static
  constraint-adder at `refine_solver_arith.c:249` -- do not overload it.
- **Numerics:** the existing exact int64 rational layer is partly reusable, but
  pivoting grows coefficients. Decision to make up front: bignum rationals, or
  keep int64 with the current overflow checks and a disciplined
  overflow -> `RT_UNKNOWN` degrade. The second is in keeping with everything
  else in this solver and is the recommendation; revisit only if telemetry
  shows real obligations degrading.
- **Accept:** every obligation S2 proves today is still proven (superset
  property; a regression here is a real regression), the `REFINE_MAX_LA_CONSTR`
  cap stops biting on the corpus's wide benchmarks, strict inequalities via the
  paper's delta-rational treatment, and every unsat verdict carries a
  Farkas certificate that an independent check accepts.
- **Size:** the design of record already estimates 2--4 weeks for this, and that
  estimate looks right. This is the single largest phase.
- **Gate on starting at all:** SX0(b). The archived plan chose FM over simplex
  deliberately, with simplex reserved for "if the cap ever bites". If the
  telemetry says it does not bite, SX4 is a performance project with no
  user-visible payoff and waits behind SX6a.

### SX5 -- incremental Nelson-Oppen (S3i)

- **Do:** run the equality exchange over the marked/undoable S1i and S2b states
  instead of rebuilding both per cube. The exchange loop itself is unchanged.
- **Accept:** as SX3 -- identical verdicts, lower cost. `NO_MAX_SHARED` and
  `NO_MAX_ROUNDS` can then be raised on evidence rather than caution.

### SX6 -- boolean structure beyond small DNF (S4)

The line the design of record explicitly does not cross ("no DPLL(T) engine").
The review reframed the cost usefully: the SAT core is the cheap part
(Tseitin CNF from the already-existing NNF is days; a MiniSat-grade CDCL core
is ~1.5-2 kLOC of C with paper and source as an effective spec); the
*incremental, backtrackable, explaining theory seam* is the real cost, and the
*testing burden* -- a subtly wrong explanation clause prunes the search into a
silent, input-dependent false verdict -- dominates even that. Ballpark from
the review: 2-4 person-months to make CDCL(T) work, 6+ to trust it with
eliding checks. So this phase is two steps with a stepping stone, and the
stepping stone may be where it permanently stops:

**SX6a -- lazy cubes, then offline lazy SMT.**

- Step zero, available today with no new machinery: *stream* cubes off an
  explicit stack instead of materializing up to 64, so a time budget replaces
  the `overflow` flag. No semantic change; retires the cube-count cap as a
  cliff.
- Then offline lazy SMT: Tseitin-encode the (already-NNF) refutation formula,
  run a plain CDCL SAT core as a **black box**, hand each full boolean model --
  which *is* a cube -- to the existing **non-incremental** S1-S3, and on
  theory conflict add a blocking clause and re-solve. This removes the
  exponential DNF blowup with **none** of the explanation machinery, no
  incremental theories, and no changes to the stages at all. It is the honest
  stepping stone: every piece of it survives into SX6b if SX6b ever happens.
- **Accept:** verdicts never flip against the DNF path where both decide;
  corpus/telemetry `UNKNOWN`s attributable to cube caps shrink; the
  source-level fuzzer stays clean across at least six seeds; the whole thing
  remains dependency-free (the SAT core is in-tree C).
- **Gate:** `EXPERIMENTS[]` row `solver-lazy-smt` while in flux; graduation
  deletes the row per the project rule. Gate on *starting*: SX0(b) shows the
  cube caps biting on real obligations.

**SX6b -- CDCL(T) proper.** Only if SX6a's blocking-clause loop measurably
thrashes (many iterations rediscovering the same theory conflict) on real
obligations.

- **Needs:** the theories incremental (SX3/SX4/SX5), backtrackable (the
  trail), and explaining -- `euf_explain` via the SX3 proof-forest hooks,
  `la_explain` via SX4's Farkas certificates. Learned clauses and activity
  scores are the never-undone rows of 3.4. Splitting-on-demand for S3's
  non-convex integer case becomes "emit the split as a lemma clause", which is
  the modern move precisely because the learning then works across the split.
- **Precondition, not afterthought:** the 3.7 certificate checker, wired so a
  bad explanation is *rejected at solve time* (degrading to `RT_UNKNOWN`)
  rather than trusted. This is what makes the dominant bug class survivable.
- **Accept:** obligations `UNKNOWN` for propositional reasons shrink to none
  on the corpus; no verdict flips; fuzzer clean; every conflict clause checks.
- **Do not:** attempt a competitive SAT solver. The stopping condition is "no
  real obligation is `UNKNOWN` for propositional reasons", not benchmark
  parity with a real SMT solver.

### SX7 -- integer completeness, the long tail (S2c)

- **Do:** branch-and-bound over the SX4 simplex, depth-limited, `UNKNOWN` past
  the limit. Optionally Gomory cuts. Closes the integer non-convexity hole that
  makes S3 incomplete on disequality case-splits today.
- **How:** per 3.6(3), *prototype the search in Turmeric first* -- it is the
  bottom-row, thousands-per-second layer -- and run it against
  `tests/corpus/smtlib/` with verdicts checked through the 3.7 seam. Port to C
  only if the stats move. This is deliberately the first consumer of the
  untrusted-search/trusted-checker architecture, on the smallest search
  problem in the plan.
- **Gate:** depth cap is a constant, not a flag. Deferrable indefinitely.

### SX8 -- external interrogation surface

Expose the solver to callers *outside* the compile pipeline. The review kept
asking "measure it", "replay it", "check it against an external solver by
hand" -- every one of those is easier when the solver answers queries
directly, and the pieces already exist in-tree: `refine_smtlib.c` serializes a
VC *out* to SMT-LIB2, and `tests/unit/refine_corpus.c` already contains a
complete SMT-LIB2 reader *in* (s-expr parser, term builder onto `RefineVC`,
`:status` handling) that today only the ctest harness can invoke. SX8 is
mostly relocation and plumbing, in three tiers that ride the other phases:

**SX8a -- `tur smt` (batch CLI) and the JSON obligation dump.** Independent of
everything else; can land right after SX0.

- `tur smt <file.smt2>`: lift the corpus harness's reader out of `tests/unit/`
  into `src/compiler/refine_smtlib.c` (making that file the reader *and*
  writer, which also lets the corpus harness shrink to a driver), run the
  standard chain, print `sat` / `unsat` / `unknown` plus which stage decided,
  the model when the bounded search found one, and -- once SX4/SX6b exist --
  the certificate. Exit codes mirror the answer so shell harnesses can branch.
  This closes the loop the internals guide already gestures at: today you can
  dump a VC *to* "whatever solver you like"; after SX8a an external harness
  can differentially test any solver against `tur` in both directions without
  `tur` ever linking one.
- `tur emit-c --dump-refine=json <file.tur>` (or a `tur refine-report`
  spelling, settle in-phase): one JSON record per obligation -- location,
  predicate source, hypotheses, the VC as SMT-LIB2 text, verdict, deciding
  stage, counterexample, `help:` hints, memo hit, and which caps (if any) were
  hit. This is deliberately the same stream SX0(b)'s telemetry wants: one
  emitter, two consumers (a human/tool reading a build, and the cap-hit
  sweep). Editors and CI read it; the LSP can surface "show me the VC / why
  UNKNOWN" as a hover or code action from the same records later, without a
  new protocol.
- **Gating:** none. Per the experimental-features rule, diagnostic and dump
  surfaces (`--dump-*`) are explicitly outside the `EXPERIMENTS[]` regime.
  Mark the JSON schema explicitly unstable (a `"schema": 0` field) until SX9.
- **Accept:** the corpus replay can be expressed as `tur smt` invocations and
  agrees with the ctest harness on all 125 labels; the JSON dump round-trips
  through `python3 -m json.tool`; both are exercised by fixtures.

**SX8b -- incremental queries.** After SX3/SX4 land marks, the textual surface
grows the SMT-LIB2 incremental commands, because the trail is exactly what
makes them implementable: `(push)` / `(pop)` map onto `euf_mark` +
`la_mark` / undo, `(get-model)` onto the bounded search, and -- after the 3.7
machinery -- `(get-unsat-core)` onto the conflict explanation and
`(get-proof)` onto the certificate. This tier is the *external witness* that
the trail works: a `push`/`assert`/`check-sat`/`pop` script exercises
mark/undo through a public door, and SX3/SX4's acceptance suites should
include such scripts once the tier exists. `tur smt --interactive` (read
commands from stdin) makes `tur` usable as a backend for anything that speaks
the protocol subset.

**SX8c -- in-language and playground access.**

- A compile-time API for the 3.6 search layer: expose the discharge seam to
  Turmeric compile-time code as `(solver/check hyps goal)` returning
  valid/invalid/unknown plus model. This is not extra work on top of SX7 --
  it *is* the seam SX7's Turmeric-side prototype needs, named and documented.
  Read-only by construction: per 3.7 nothing reachable from this API can
  elide a check; interrogation proposes, the C chain decides.
- A WASM export (`turi_smt_check` in `src/web/wasm_glue.c`, next to
  `turi_doc_lookup`) so the playground can answer queries -- the "static
  checking at zero download cost" story already ships the solver to the
  browser; this makes it visible there.

**Scope honesty, so the surface never overpromises:** `tur smt` accepts the
corpus subset of SMT-LIB2 over `QF_UFLIA`/`QF_UFLRA`, answers `unknown`
freely, and parity with a production SMT solver is a non-goal (SX6b's
stopping condition applies here too). The reader must keep the corpus
harness's discipline of erroring on anything it would otherwise silently
drop: a partially parsed assertion set weakens `unsat` claims, which is the
one dishonest failure mode a query surface can have.

### SX9 -- docs and graduation

- Update
  [refinement-solver-internals-guide.md](../guides/refinement-solver-internals-guide.md)
  (the stage table, the caps table, the new seam operations, the incrementality
  story), write a `docs/guides/backtrackable-state-guide.md` for the SX1
  surface, and cross-link from
  [backtracking-guide.md](../guides/backtracking-guide.md) and
  [logic-programming-guide.md](../guides/logic-programming-guide.md). Document
  the `tur smt` subcommand and the JSON obligation schema (stamping it
  `"schema": 1`) in a `docs/guides/solver-query-guide.md`.
- Graduate `backtrackable-state` (delete the row, behavior unconditional) once
  3.5's re-entry question is settled and SX2 has a benchmark answer. Graduate
  `solver-lazy-smt` on the SX6a corpus criterion. Move this plan to
  `docs/archive/` when the last phase lands, per the archiving rule.

### Recommended order

This mirrors the review's own next-steps list, with the interrogation
surface slotted where it is cheapest:

1. **SX0**, both instruments -- the telemetry and the curve are the decision
   data for everything below, and cost days.
2. **SX8a alongside or immediately after SX0** -- it shares SX0(b)'s
   emitter, costs little (the SMT-LIB reader already exists in
   `tests/unit/refine_corpus.c`), and every phase after it gets a public
   query door for its acceptance tests plus two-way differential testing
   against external solvers.
3. **SX1, then SX2** -- answers the opt-out question with a shipped design
   instead of a promise, and immediately tests whether the primitive pays for
   itself on the stdlib search paths.
4. **If SX0(b) says the cube caps bite: SX6a** -- offline lazy SMT needs no
   incremental theories, so it can leapfrog SX3-SX5 entirely.
5. **SX3-SX5** when SX6b or measured compile-time cost justifies them; SX4
   only if its own cap bites. SX8b rides whichever of SX3/SX4 lands first.
6. **SX7 prototype in Turmeric early**, via the SX8c seam (it needs only
   SX4's seam or even today's S2 for a first cut against the corpus), C port
   last or never. **SX6b** last of all, and only if SX6a thrashes.

## 6. Explicitly not doing

- **Not** linking an external SMT solver, in any build. The Z3 oracle was
  retired for reasons that still hold, and the corpus plus the source-level
  fuzzer replaced it with something that runs everywhere, including WASM. The
  SX6a SAT core, if built, is in-tree C.
- **Not** building a competitive SAT solver (see SX6b's stopping condition).
- **Not** making captured continuations capture *state*. A continuation
  captures control; the trail is a separate mechanism with a separate scope
  discipline. Conflating them is how this design gets slow in a way that cannot
  be undone later.
- **Not** putting the hot layer in Turmeric. BCP, watch lists, pivoting, and
  union-find operations run millions of times a second; per 3.6 they are C on
  every target, WASM included.
- **Not** removing the caps. Every cap stays; incrementality changes what a cap
  costs, not whether it exists. An `UNKNOWN` is always available and always
  sound.
- **Not** trailing by default in the compiler's arena-allocated VC structures.
  Terms are immortal within a query by construction; that is a feature.
- **Not** shipping a general-purpose SMT solver CLI. `tur smt` is an
  interrogation window onto the solver `tur` already contains -- the corpus
  subset of SMT-LIB2, `unknown` as a first-class answer, no parity ambitions.
- **Not** trusting any Turmeric-hosted search stage to decide. Per 3.7 it may
  only propose; the C checker decides, and an unverified certificate is
  `RT_UNKNOWN`.

## 7. Risks

| Risk | Mitigation |
|---|---|
| Incremental EUF/simplex changes a verdict | Corpus + fixtures are verdict-identical by acceptance criterion; keep the rebuild path behind an env test seam for differential replay |
| A subtly wrong explanation clause prunes the search into a silent, input-dependent false verdict (the classic CDCL(T) bug class) | The 3.7 certificate checker validates every conflict at solve time -- a bad explanation degrades to `RT_UNKNOWN` instead of being trusted; differential replay against the DNF and lazy-SMT paths; the corpus is necessary but known-insufficient here and is not the safety argument |
| TCB growth from a Turmeric-hosted search stage | Untrusted-search/trusted-checker split (3.7); a Tur stage proposes, never decides |
| Trail undo mis-handles rc ownership -> leak or double-free | ASan/LSan is on for the compiled fixture path; add a fixture with an rc payload written and undone in a loop |
| Multi-shot resume across a stale mark corrupts state | Generational marks, checked at `bt-undo-to!` (3.2(4)); the alternative is a silent wrong answer |
| SX4 / SX6 are weeks-to-months of work with no user-visible payoff | Both are gated on SX0(b) cap-hit telemetry before starting; SX6a's blocking-clause loop must measurably thrash before SX6b starts |
| Simplex coefficient growth overflows int64 rationals | Disciplined overflow -> `RT_UNKNOWN` (today's policy, kept); bignums only if telemetry shows real degrades |
| Fixture snapshot churn | Regenerate in the same PR as the codegen change, per the project rule; SX1 is the only phase that touches codegen at all |
| The benchmark measures the wrong thing under load | Serialize the sweep, stamp the binary, refuse to publish rows if it changed mid-run |
| The query surface (JSON schema, SMT-LIB subset) hardens into a compatibility contract before the solver settles | Schema versioned and explicitly unstable through SX8a-b; stabilized (`"schema": 1`) only at SX9, alongside the guide |

## 8. Open questions to take back to the reviewer

1. Multi-shot re-entry across a trail scope (3.5): checked error, or
   state-snapshotting `call/cc*`? The review's own framing (multi-shot is the
   wrong strength for search; one-shot is the relevant path) argues for the
   checked error plus a one-shot fast path; the SX0 curve settles the cost
   half, the semantics half is a decision.
2. Is a *write-once* cell flavor (`LVar`) worth its own type, or should
   everything be a value cell with the stamp doing the optimization? The WAM
   says yes; a smaller API says no.
3. RT6 hint generation as the one genuine multi-shot consumer: capture after
   hypotheses are asserted, re-enter once per candidate. Does it beat the RT7
   memo? Depends on the work ahead of the branch point -- measure (SX0 curve
   x `TUR_REFINE_STATS` probe counts) before building.
4. For SX6b: is theory propagation worth it at this scale, or is
   explain-and-learn enough given obligations of a few dozen atoms? Related:
   the review notes the modern treatment of non-convex splits is to emit them
   as SAT lemmas so learning works across them -- adopted in SX6b's design,
   worth confirming it holds at this problem size.

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
- MiniSat's trail / `trail_lim` / backjumping, and what survives a backjump: N. Een and N. Sorensson, "An Extensible SAT-solver", SAT 2003; <https://github.com/niklasso/minisat/blob/master/minisat/core/Solver.cc>, <https://www.cs.cmu.edu/~mheule/publications/JSAT7_11_vanderTak.pdf>.
- G. Nelson and D. Oppen, "Simplification by Cooperating Decision Procedures", ACM TOPLAS 1(2), 1979.
- J. Harrison, *Handbook of Practical Logic and Automated Reasoning*, CUP 2009, ch. 2-5 -- NNF/DNF, congruence closure, Nelson-Oppen, with source.

**External -- proofs and certificates (3.7, SX6b)**

- R. Nieuwenhuis and A. Oliveras, "Proof-Producing Congruence Closure", RTA 2005 -- the proof forest behind `euf_explain`.
- G. Tseitin, "On the Complexity of Derivation in Propositional Calculus", 1968 -- the CNF encoding SX6a uses.
- The veriT solver and the Alethe proof format; N. Wetzler, M. Heule, W. Hunt, "DRAT-trim: Efficient Checking and Trimming Using Expressive Clausal Proofs", SAT 2014 -- prior art for untrusted-search/trusted-checker.

**External -- capture/restore cost**

- R. Hieb, R. K. Dybvig, C. Bruggeman, "Representing Control in the Presence of First-Class Continuations" -- stack segments, constant-time capture: <https://www.cs.tufts.edu/~nr/cs257/archive/kent-dybvig/stack.pdf>.
- C. Bruggeman, O. Waddell, R. K. Dybvig, "Representing Control in the Presence of One-Shot Continuations" -- eliminating the copy for one-shot capture: <https://www.cs.tufts.edu/comp/150VM/modules/archive/kent-dybvig/one-shot-continuations.pdf>.
- W. Clinger, A. Hartheimer, E. Ost, "Implementation Strategies for First-Class Continuations" -- the taxonomy the three paths in 4.2 fall into: <https://link.springer.com/article/10.1023/A:1010016816429>.
- K. Farvardin, "Weighing Continuations for Concurrency" -- modern head-to-head measurement of the strategies: <http://manticore.cs.uchicago.edu/papers/farvardin-masters.pdf>.
- "Continuing WebAssembly with Effect Handlers" (WasmFX) -- segmented-stack switching in a contemporary runtime: <https://arxiv.org/pdf/2308.08347>.
