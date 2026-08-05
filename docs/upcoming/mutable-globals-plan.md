# Mutable globals -- making `^mut` global state visible to the disciplines that already exist

> **Status:** **G1, G2, G3, G4a (`^atomic`) LANDED 2026-08-05** (see §10, §15,
> §16, §17). **G4b LANDED** (§18), so G4 is complete. G5a (docs) and G5b
> (graduation) remain.
> All open questions in §9 are answered: §11 (thread-local init), §12 (`#reads`
> strength), §13 (the remainder), §14 (whether the read side gets its own plan).
> Written as the follow-up
> [`def-define-consolidation-plan.md`](def-define-consolidation-plan.md) §8.4
> named ("giving mutable globals a concurrency story is its own plan, not a
> rider on this one").
> **Type:** Language / elaboration / effects
> **Depends on:** `^mut` on a top-level `def`, landed 2026-08-05 (D4).
> **Related:** [`checked-write-frames-plan.md`](checked-write-frames-plan.md)
> (this plan is scoped to the globals half of its step 3),
> [`sealed-opaque-plan.md`](sealed-opaque-plan.md) (the same encapsulation
> argument, applied to handles rather than globals).

## 0. Summary

`(def ^mut x 0)` gives turmeric process-wide mutable state. Every discipline
the language has for reasoning about mutation -- write frames, effect rows,
module boundaries -- was built for *arguments* and *locals*, and none of them
sees a global. The result is that a function can declare it writes nothing,
declare an empty effect row, and mutate global state on every call, with no
diagnostic anywhere.

**This plan does not propose restricting mutable globals.** They are useful and
the feature just landed. It proposes making a global write *visible* to the
machinery that already exists, closing one genuine soundness gap on the way,
and adding two opt-in spellings for the cases where visibility is not enough.

### 0.1 What this is deliberately NOT

- **Not data-race freedom.** Nothing here makes concurrent access to a `^mut`
  global safe by construction. That is a research-scale problem (an ownership
  or region discipline over global state) and pretending otherwise would be
  worse than the current honest gap. §4.4 gives opt-in spellings for the two
  cases that are cheap; everything else stays the programmer's problem, said
  out loud in the guide.
- **Not a ban, a lint-by-default, or a deprecation.** No existing program stops
  compiling because it uses a mutable global.
- **Not a change to `#fx{}`.** The effect system tracks *algebraic* effects.
  `docs/guides/refinement-types-guide.md:438-442` is explicit that it "infers
  nothing from `set!`, from a mutable global, or from inline C", and that is
  correct -- an algebraic-effect row is not a read/write row. Widening `#fx{}`
  to cover global writes would break that meaning. §4.2 explains why the frame
  vocabulary is the right home instead.
- **Not GC, ownership, or representation work.** A global holds whatever a
  local holds, by the same rules.

---

## 1. What exists today

Everything in this section was verified against the tree at
`f30c3a06`/`dc80335b`, by reading the named sites and by running the probes
quoted. Where a claim is inference rather than observation it says so.

### 1.1 How a `^mut` global is emitted

A top-level `def` becomes a file-scope `static` C variable, initialized by
*assignment* rather than by a static initializer:

```c
static int64_t counter_1327;
static double  ratio_1328;

static void __tur_module_def_init(void) {
    counter_1327 = INT64_C(0);
    ratio_1328   = 1.5;
}

/* S1b: explicit static initialization -- see docs/upcoming/jit-engine-plan.md. */
static void __tur_static_init(void) {
    static int __tur_static_init_done = 0;
    if (__tur_static_init_done) return;
    __tur_static_init_done = 1;
    __tur_module_def_init();
}
__attribute__((constructor))
static void __tur_static_init_ctor(void) { __tur_static_init(); }
```

`main()` calls `__tur_static_init()` first thing; the constructor covers the
no-`main` cases (separate compilation, `--shared`). Consequences:

- **Initialization order is source order.** A `def` may read an earlier global
  (`(def derived (+ base 5))` prints `15`); a forward reference is
  `TUR-E0003 unbound symbol`, not a zero-initialized surprise. This is already
  the right behaviour and this plan does not touch it.
- **`__tur_static_init_done` is a plain `int` with a check-then-set.** In
  practice the constructor runs before any thread exists, so this is not a live
  race -- but it is the kind of thing to make atomic if the initialization path
  ever becomes reachable from more than one thread. Noted, not scheduled.
- **`tur build <dir>` is whole-program: one translation unit.** `nm` on a
  two-module build reports `b hits_1327` -- a local BSS symbol -- and a cross-
  module write still works. So `static` linkage is not currently a barrier
  between modules, and H3's encapsulation hole is a *language* hole, not a
  linkage one. Separate compilation and `--shared` are where linkage would
  start to matter; this plan does not assume either.

### 1.2 The concurrency surface that can reach one

Real OS threads exist and are reachable from ordinary code: `thread-spawn-fn`
(`pthread_create`), `thread-join`, `thread-detach`, `cancel-thread`, plus
`stdlib/mutex.tur`, `stdlib/condvar.tur`, `stdlib/atomic.tur` (C11
`__atomic_*` builtins), `stdlib/threadpool.tur`, and `stdlib/future.tur` (which
calls `pthread_create` directly). A `^mut` global is a plain `static` with no
synchronization of any kind, so two threads writing one is a data race today,
diagnosed by nothing.

`TUR_THREAD_LOCAL` already exists in the emitted preamble
(`src/compiler/emit_module.c:7510-7512`), defined to `_Thread_local` or
`__thread` depending on target, because the JIT's c2mir needs the C11 spelling.
That is the plumbing §4.4's `^thread-local` would use; it does not need
inventing.

`stdlib/dynvar.tur` (`defdynamic` / `binding [...]`, behind `-Xdynamic-vars`)
is the existing *scoped* alternative to a mutable global, with `spawn-conveying`
for handing bindings to a task. Where a global is being used as "ambient
configuration", a dynvar is usually the better answer and the guide should say
so (§5, G5).

### 1.3 What already sees a global write -- the refinement side is sound

This is the good news, and it is by design rather than by luck.

`rt_classify_expr`'s `EX_VAR` case (`src/compiler/elab_fns.c:392-395`) reads:

```c
case EX_VAR:
    /* Reading a mutable binding is not congruent -- two reads can differ --
     * but a read is not an EFFECT, so it is UNKNOWN, never impure. */
    if (!x->as.var.binding) return RT_P_UNKNOWN;
    return x->as.var.binding->is_mut ? RT_P_UNKNOWN : RT_P_PURE;
```

It keys on `is_mut`, which is exactly the flag D4 sets for a `^mut` global. So
a measure that reads a mutable global classifies UNKNOWN,
`rt_binding_is_pure` (`:518`) requires PURE, congruence is denied, the
hypothesis is never believed, and a runtime check is emitted instead. Verified
by running it: a probe that establishes `(m x) > 0`, mutates the global `m`
reads, and then crosses into a `Pos` parameter is **not** statically proved --
it falls back to the contract and panics at run time with the real answer.
That is the correct outcome.

The `errors/refine-impure-fx-empty` fixture pins the same hole for an
*inline-C* static counter, with a comment that names "a mutable global" as one
of the three routes in. **At the time that fixture was written a global could
not be mutable**, so the global variant of it is newly expressible as of
yesterday and is not pinned by anything. §7 adds it. This is a test gap, not a
live defect.

### 1.4 What does not see a global write

Three things, all verified by running them.

**Write frames.** `#writes` is a per-argument bitmask on the `Binding`
(`writes_declared` plus the mask, deliberately separate so "no frame" never
collapses into "empty frame"). Globals are not in its vocabulary at all:

```turmeric
(def ^mut hidden 0)

(defn sneaky [x : int] #writes [] : int   ; "writes nothing"
  (set! hidden (+ hidden 1))
  x)
```

`tur --enable=write-frames check` accepts this with no diagnostic beyond the
experiment's own TUR-W0060 lifecycle warning.

**Effect rows.** The same body with `#fx{}` instead of `#writes []` is likewise
accepted:

```turmeric
(defn pure-liar [x : int] #fx{} : int
  (set! hidden (+ hidden 1))
  x)
```

As §0.1 says, this one is *correct by the effect system's own definition* and
is not a bug to fix in `#fx{}`.

**Module boundaries.** An exported global is writable by every importer:

```turmeric
(defmodule ctr
  (export hit peek hits)
  (def ^mut hits 0)
  (defn hit [] : void (set! hits (+ hits 1)))
  (defn peek [] : int hits))

(defmodule main
  (import ctr :refer [hit peek hits])
  (defn main [] : int
    (hit) (hit)
    (println (peek))     ; 2
    (set! hits 99)       ; writes another module's state
    (println (peek))     ; 99
    0))
```

There is no way to export a global readably without also exporting it writably.

---

## 2. The trajectory this plan joins

This is not a new direction. `docs/guides/stateful-refinements-guide.md:296-309`
lays out a three-step path for `#reads`/`#writes`:

1. **trusted now** -- a promise, sound because the safety check is kept;
2. **checkable later** -- verified by the purity walk (landed, behind
   `--enable=write-frames`);
3. **effect-row eventually** -- "`#reads`/`#writes` as a real read/write effect
   row (the one `#fx{}` never was)".

The `write-frames` row in `src/runtime/experiments.c:205-220` repeats it:
"trusted now -> checkable later -> effect-row eventually".

**This plan is the globals-shaped slice of step 3, and nothing more.** It does
not attempt the whole read/write row, does not touch the ECS conflict detection
step 3 mentions subsuming, and does not touch loop-invariant bounds. Doing
globals first is the right order because globals are the one part of step 3
that is (a) newly reachable as of D4, (b) a live soundness gap rather than a
missed optimization, and (c) small enough to land without the rest.

---

## 3. The holes, in priority order

### H1 -- a frame can be VERIFIED while the body writes a global *(soundness)*

This is the only item here that is a bug rather than a gap. The write-frames
plan is explicit that VERIFIED means "a fact an optimization may act on". A
body that writes a global and declares `#writes []` is currently VERIFIED, and
that fact is false. Nothing consumes it dangerously *today* -- WF4, which would
have elided a check on the strength of a frame, is retired, and WF3's
invalidation reads the caller's own body rather than a callee's frame -- so
this is a latent hole, not a live miscompile. It should be closed before
anything else starts trusting frames, not after.

### H2 -- globals are not in the frame vocabulary

Once H1 is closed conservatively, a function that legitimately maintains global
state can no longer have a checked frame at all. That is the right default and
the wrong end state: the vocabulary should be able to *say* "this writes the
cache", the way it can already say "this writes argument `a`".

### H3 -- an exported global is writable by every importer

A module that exports a counter for reading exports it for writing. This is the
same argument `sealed-opaque-plan.md` makes about `::` fabricating access to an
opaque's representation: an abstraction the type checker declines to defend.

### H4 -- no thread-safe spelling

A `^mut` global is a plain `static`. There is no way to ask for a per-thread
copy or an atomic scalar, even though both have existing plumbing
(`TUR_THREAD_LOCAL`; `stdlib/atomic.tur`'s `__atomic_*` codegen shape).

---

## 4. Design

### 4.1 H1: a global write forces UNVERIFIED

The checker already computes a per-body verdict of VERIFIED / EXCEEDED /
UNVERIFIED. Add one input: **if the body writes a global (directly, or through
a callee whose own verdict says it does), the verdict is UNVERIFIED.**

Not EXCEEDED. EXCEEDED (`TUR-E0382`) means "you declared a frame and wrote
outside it", which is a diagnostic the author asked for by writing the frame.
A global write is outside the vocabulary rather than outside the frame, so the
honest verdict is "cannot verify" -- no diagnostic, the declaration still
documents intent, and nothing acts on it. That matches how the plan already
treats every other unmodellable body, and it means **H1 lands with no new
diagnostic and no program stopping compiling.**

"Writes a global" is decided on the elaborated `EX_SET` (`elab_set`,
`src/compiler/elab_forms.c:3039`) by asking `binding->is_global`, not on the
form-level name scan -- see §4.5.

### 4.2 H2: globals join the frame vocabulary

```turmeric
(def ^mut *cache* (hamt/new))

(defn memoize! [k : int v : int] #writes [*cache*] : void
  (set! *cache* (hamt/assoc *cache* k v)))

(defn lookup [k : int] #reads [*cache*] : option<int>
  (hamt/get *cache* k))
```

A frame entry that resolves to a global binding extends the frame rather than
naming a parameter. `writes_declared` stays separate from the mask, so
`#writes []` keeps meaning "writes nothing, including no global" -- which is
what makes H1's conservative verdict upgradeable rather than permanent.

**Why here and not `#fx{}`:** an effect row answers "what algebraic effects can
this perform"; a frame answers "what storage can this write". A global write is
the second question. Putting it in `#fx{}` would make `#fx{}` mean two things,
and would break the property that `#fx{}` is *a veto, not evidence*
(`refinement-types-guide.md:438`) -- code reads an empty row as "no effects
declared", never as "provably writes nothing", and it must stay that way.

**The naming convention is a suggestion, not a rule.** `*earmuffs*` is what
dynvars and `*args*` already use for ambient state, and a global that appears
in frames reads better with them. Nothing should *require* them.

### 4.3 H3: exported globals are read-only outside their module

An exported global is readable by importers and writable only from its defining
module. Writing one from outside becomes a diagnostic that names the module and
suggests exporting a setter.

The escape hatch is an explicit declaration at the definition site -- the
module says "yes, anyone may write this" -- rather than at the use site, so the
decision lives with the code that owns the invariant. Exact spelling is left
open (§5, G3); it should not be a new annotation if an existing one can carry
it.

This is the one phase that **can reject code that compiles today**, which is
what puts the whole plan behind an experiment gate (§6).

### 4.4 H4: two opt-in spellings, and an honest guide section

- **`^thread-local`** -- each thread gets its own copy. **Under `tur --interpret`
  / turi it is observationally a plain global**, and that is the specified
  behaviour rather than a gap: turi has no user-reachable thread spawn (its only
  `pthread_create` is a thread-ring benchmark native), so there is no second
  thread for the annotation to differ on. It is accepted and degenerate there.
  See §13.4. **Superseded by §11** on the compiled side, which found that the
  `TUR_THREAD_LOCAL` route sketched here does not work:
  the JIT has no thread-local storage at all, and its existing workaround
  covers 11 fixed runtime slots that a user variable cannot join. Restricting
  to constant initializers does not rescue it. See §11.4 for the design that
  does work and §11.5 for what it costs.
- **`^atomic`** -- scalar globals only (`:int`, `:bool`, `:float`, pointer-
  width handles). `set!` and reads lower to `__atomic_store_n` /
  `__atomic_load_n` with sequential consistency, which is exactly the shape
  `stdlib/atomic.tur` already ships and proves. A non-scalar `^atomic` is a
  rejection with a reason, pointing at `stdlib/mutex.tur`.

Both compose with `^mut` and neither is a default. Anything beyond them --
compound atomic updates, lock ordering, actual race detection -- is out of
scope and the guide says so plainly rather than implying coverage.

### 4.5 One analysis, computed once

H1, H2, and H3 all need the same fact: **does this function body write a
global, and which one.** Compute it once, on the elaborated tree, keyed on
`Binding.is_global` at each `EX_SET`, with the same fixed-point sweep over
callees the write-frames checker already runs (a caller's verdict depends on
its callees', so one linear pass under-verifies).

**Do not extend the form-level scan for this.** `rt_collect_set_targets`
(`src/compiler/elab_fns.c:954`) walks *forms* and compares symbol *names*; it
never resolves a symbol to a `Binding`, so it cannot tell a local from a global
and cannot see through a rename. Its soundness argument is explicitly about
locals:

> Why a plain symbol target cannot alias: turmeric passes by value, so handing
> a local to a callee -- even `^mut` -- cannot let the callee write the
> caller's slot.

That argument does not extend to globals, which are written by name rather than
passed. It does not need to: as §1.3 established, a hypothesis that depends on
a mutable global is never believed in the first place, because reading one is
not congruent. So WF3 is sound as written. But the reason it is sound is *not*
the reason its comment gives, and the next person to widen either side needs to
know that. G1 adds the comment and the fixture; it changes no behaviour.

---

## 5. Phases

- **G1 -- the soundness fix. LANDED 2026-08-05; see §10.** §4.1 plus the §4.5 analysis it needs. A body
  that writes a global cannot be VERIFIED. No new syntax, no new diagnostic, no
  program stops compiling. Ships under the **existing** `write-frames` gate --
  it is a correction to that feature, not a new one. Also lands the §1.3 test
  gap (the global variant of `refine-impure-fx-empty`) and the §4.5 comment
  correction on `rt_collect_set_targets`. **Independently useful; land first.**
- **G2 -- globals in the frame vocabulary. LANDED 2026-08-05; see §15.** §4.2. Upgrades G1's blanket
  UNVERIFIED to "verified against a frame that names the global". Behind
  `--enable=global-state`.
- **G3 -- module encapsulation. LANDED 2026-08-05; see §16.** §4.3. The one phase that can reject existing
  code. Behind the same gate. Wants its own soak time before graduating.
- **G4 -- `^thread-local` and `^atomic`. BOTH LANDED 2026-08-05; see §17, §18.** §4.4. Independent of G2/G3; can land
  in either order relative to them. **Split them**: `^atomic` first -- it is much
  the smaller, its design question is settled (§13.6), and the JIT already has an
  atomics shim -- then `^thread-local`, which needs the whole of §11.4.
  Deliverables that are easy to drop and must not be: `^atomic` requires an
  explicit `^mut` and diagnoses its absence (§13.6); a `^thread-local`
  initializer may not reference another `^thread-local` (§13.7); and the guide
  states plainly that `^thread-local` is a plain global under turi (§13.4), with
  a fixture asserting it parses and runs there.
- **G5a -- docs.** The guide section §4.4 promises, a `binding-forms-guide.md`
  cross-reference, the dynvar-vs-global steer from §1.2, and the plain statement
  that everything past `^atomic`/`^thread-local` is the programmer's problem
  (§0.1). Independently useful and independently landable: it documents what has
  already shipped, so it does not wait on G4b.
- **G5b -- graduation.** The decision for `global-state`: graduate, shelve, or
  bump `expires_at`. Distinct from G5a because it is a *judgement* that wants
  adoption evidence, not a writing task -- G3 in particular can reject code that
  compiles today, and that is the part worth soaking before it goes always-on.
  Blocked on G5a (a feature should not graduate undocumented) and on whatever
  G4b decides.

G1 is worth doing even if nothing after it is. G2-G5 are each worth doing
alone, in that order of confidence.

---

## 6. Gating

G2, G3, and G4 ship behind **`--enable=global-state`**, per CLAUDE.md's
experimental-features rule: G3 can reject a body that compiles today, and G2/G4
add syntax whose semantics are in flux. One row in `EXPERIMENTS[]`
(`src/runtime/experiments.c`) with every field populated:

| Field | Value |
|---|---|
| `name` | `global-state` |
| `summary` | globals in `#reads`/`#writes` frames, read-only exported globals, `^atomic` / `^thread-local` |
| `plan_path` | `docs/upcoming/mutable-globals-plan.md` |
| `introduced` | the version that lands G2 |
| `expires_at` | +4 minor versions, per the `write-frames` precedent |
| `lifecycle` | `XF_LIFECYCLE_PROTOTYPE` |
| `opt_global` | `&g_opt_global_state` |

Plus `experiment_warn_if_used("global-state")` at the elaboration entry point,
so TUR-W0060/W0061 fire.

`expires_at` is advisory and never blocks a release.

**G1 is not gated by `global-state`** -- it lands under `write-frames`, because
it is that feature telling the truth rather than a new capability.

---

## 7. Compatibility

- **G1**: no program changes meaning. A frame that was VERIFIED-but-lying
  becomes UNVERIFIED, which is a verdict nothing currently acts on.
- **G2**: purely additive syntax; gated.
- **G3**: rejects cross-module global writes that compile today. Gated, and the
  reason it wants its own soak.
- **G4**: purely additive annotations; gated.
- **No codegen change is expected from G1-G3.** G4 changes codegen for
  annotated globals only. **If a fixture snapshot moves in G1-G3, that is a
  signal to investigate, not a snapshot to regenerate reflexively.**

---

## 8. Testing

Fixtures, ASCII only, twelve-minute timeout on every suite run, and every case
under `tests/run-turi.sh` as well as `tests/run.sh` -- `set!` on a global works
in the interpreter too, so both engines owe the same answers.

**G1:**
- `#writes []` plus a global write -> UNVERIFIED, no diagnostic. Assert the
  verdict via whatever `--dump-*` surface the write-frames work already uses,
  not via the absence of output.
- The same through one and two levels of callee, to exercise the fixed point.
- A body that writes only a *local* -> still VERIFIED (the guard is not a
  blanket decline).
- The §1.3 gap: a `^mut` **global** counter version of
  `errors/refine-impure-fx-empty`, asserting the obligation is refuted and the
  runtime check survives. The existing fixture's inline-C counter does not
  cover this, because globals could not be mutable when it was written.

**G2:** a frame naming a global -> VERIFIED; naming a global the body does not
write -> the frame is not a lie but is over-broad (decide and pin which);
writing a global *not* named -> EXCEEDED, matching the parameter case.

**G3:** cross-module write -> rejected with the module named; same-module write
-> fine; read from outside -> fine; the opt-in escape hatch -> fine.

**G4:** `^atomic` int and float globals mutated from two spawned threads, joined,
asserting the total (the float probe uses `7.1`, never `7.0`, per CLAUDE.md);
`^atomic` on a non-scalar -> rejection naming `stdlib/mutex.tur`;
`^thread-local` -> two threads observe independent values.

**Not tested, because it cannot be:** that a `^mut` global is used safely across
threads without `^atomic`. §0.1.

---

## 9. Open questions

Recorded so the reasoning survives; none blocks G1.

1. **`^thread-local` initialization** (§4.4). **ANSWERED -- see §11.** Short
   version: constant-initializer-only is *not* an acceptable first cut (it is
   silently broken under the JIT, which fails on the storage rather than the
   initializer), and per-thread lazy init is unreachable from `__thread` in C.
   The workable design is one `pthread_key_t` holding a per-thread block,
   materialized on first access -- the mechanism dynvars already use.
2. **Does a `#reads` frame naming a global buy anything today?** **ANSWERED --
   see §12.** It would buy something worse than nothing: `#reads` is the one
   annotation that *grants* congruence, so naming a global there would let a
   promise about mutable global state pay out in proofs. G2 ships `#writes`
   only, and `#reads` keeps its hard error for a non-parameter name. A narrow
   read-side check is recommended as a companion (§12.3), not a prerequisite.
3. **Should `^atomic` imply `^mut`?** **DECIDED -- §13.6.** No: keep them
   orthogonal and require `(def ^atomic ^mut n 0)`, matching the `^unique ^mut`
   idiom the codebase already uses. `^atomic` alone is a diagnostic naming
   `^mut`, never a silent grant.
4. **Interaction with `--shared` and separate compilation.** **ANSWERED --
   §13.2.** `--shared` already drops `static`, so user globals are exported
   symbols today and the unstable binding-id suffix is part of that ABI.

---

## 10. G1 execution record (2026-08-05)

**Landed.** `tests/run.sh` -> 2545 passed, 0 failed; `tests/run-turi.sh` ->
1736 passed, 0 failed, 704 skipped. No fixture snapshot moved, and every frame
that was VERIFIED in `wf1-writes-frame-honored` before the change still is.

### 10.1 Shape of the change

The global question is answered by **its own walk**, and the verdict is
combined once at the end of `wf_resolve_write_frames`:

```c
if (v == WF_VERIFIED && wf_fn_writes_global(e, s->fn) != WG_NO)
    v = WF_UNVERIFIED;
```

`wf_walk` is untouched. That is not an accident of implementation -- §4.2's
argument is that a frame and a global are different vocabularies, and keeping
the walks separate is what stops the second question from leaking into the
first. `Binding.writes_global` is a tri-state (`WG_NO` / `WG_YES` /
`WG_UNKNOWN`, plus `WG_IN_PROGRESS` as the cycle guard), memoized per function,
because "I saw no global write" and "I could not see" are different answers and
only the first may back a VERIFIED frame.

**Every `defn` is now registered as a frame site**, not only annotated ones --
the global question has to be answerable for an arbitrary callee, and the body
forms are only reachable through that registry. Both loops in
`wf_resolve_write_frames` gained a `writes_declared` filter so a function with
no frame is never checked against one, and never stamped `writes_checked`.

### 10.2 Two channels ruled out by evidence, not assumption

The calibration that decides whether this feature is usable is what counts as
a possible global write. Two candidates were ruled out by running them:

- **Inline C cannot reach a turmeric global.** A global emits as a C `static`
  whose name carries the binding id (`counter_1327`), and that suffix shifts
  with any unrelated edit earlier in the unit -- verified by adding a `def`
  above and watching it become `counter_1328`. An inline-C body naming the
  source spelling is emitted verbatim and dies at the C compile with
  `undeclared identifier`. So inline C is not a channel, and treating every
  inline-C callee as a possible global writer -- which was the first draft --
  would have poisoned most bodies for nothing.
- **Builtins and special forms are not channels either**, for the same reason.
  This one had teeth: the first working draft answered `WG_UNKNOWN` for any
  call head that did not resolve in the global scope, which is every `if`,
  every `>`, and every field read. It downgraded `calls-clean-cycle` and would
  have downgraded essentially every real frame. An unresolved head is now
  treated as no channel.

### 10.3 The residual gap, stated

A `let`-bound closure invoked by its own name is not followed. Answering
UNKNOWN there would be sound but indistinguishable from the `if`/field-read
case above, so it would cost the whole feature. A fn-typed **parameter** call
IS caught (`wf_param_index` on the head -> `WG_UNKNOWN`), which covers the
common higher-order shape. Following indirect dispatch properly belongs to G2,
where a fn value's own frame becomes a thing that can be asked about.

Shadowing is pessimistic: a `let` local spelled like a global reads as a global
write, because the walk matches names. That direction costs a verification,
never a missed one.

### 10.4 `--dump-write-frames`

New gateless diagnostic flag -- CLAUDE.md exempts `--dump-*` knobs from the
experiment rule, and this one reports what the checker decided and changes
nothing. It exists because without it the only observable difference between
VERIFIED and UNVERIFIED is the *absence* of a diagnostic, which is not
something a fixture can assert on:

```
write-frame sneaky: UNVERIFIED mask=0x0 frame=VERIFIED global=YES
```

`frame=` is the frame walk's own verdict and `global=` is what downgraded it,
kept as separate columns so a fixture can tell "the frame did not hold" from
"the frame held but the body writes a global". Both G1 verdict fixtures assert
the full line.

### 10.5 Fixtures

- `g1-writes-global-unverified` -- the direct case, plus a local-only write
  (float probe `7.1`) and a clean callee that must stay VERIFIED.
- `g1-writes-global-transitive` -- one level, two levels, a mutually-recursive
  cycle that writes a global (the guard must not swallow it), and one that does
  not (the guard must not poison it).
- `errors/g1-writes-global-still-exceeds` -- E0382 still fires. A global write
  downgrades VERIFIED; it does not launder an exceeded frame.
- `errors/refine-impure-global-not-congruent` -- §1.3's test gap. The mutable-
  global route into the congruence hole that `errors/refine-impure-fx-empty`
  names in its comment but could not cover, because a global could not be
  mutable when it was written.

The two verdict fixtures carry `requires.interp`, which selects the `tur run`
path. That is a harness detail, not a claim about the interpreter: the compiled
path runs `tur build` with stdout unredirected and captures only the built
binary's output, so a compile-time dump never reaches `actual.stdout`. The
verdict is computed in the elaborator, which both engines share. Each marker
file says so.

### 10.6 Comment correction shipped alongside

`rt_collect_set_targets`'s comment justified its "a plain symbol target cannot
alias" rule with turmeric's by-value argument. That argument is about locals
and does not extend to a global, which is written by name rather than passed.
The scan is sound anyway -- for a different reason, now written down: a
hypothesis that depends on a mutable global is never believed, because
`rt_classify_expr` answers UNKNOWN for a read of any `is_mut` binding. The
comment now says which reason holds it up, and warns that widening purity to
admit global reads would make that scan load-bearing in a way it cannot
support.

### 10.7 What G2 inherits

Nothing G1 did makes a global *expressible* in a frame -- a function that
legitimately maintains global state simply cannot carry a checked frame now.
That is the right default and the wrong end state, and it is exactly §4.2.

---

## 11. Research: is per-thread lazy init feasible? (open question 1)

Answering §9's first open question. Investigated 2026-08-05. Claims below are
marked **[ran]** where a probe was compiled and executed, **[read]** where they
come from reading the tree. The JIT could not be exercised (this build is not
`-DTUR_JIT=ON`), so every JIT claim is [read].

### 11.0 Verdict

**Feasible, but not the way §4.4 assumed, and the "acceptable first cut" in
§4.4 is not actually acceptable.** §4.4 proposed emitting `^thread-local`
through the existing `TUR_THREAD_LOCAL` macro and restricting the first cut to
constant initializers. Two findings break that:

1. Even a **constant**-initializer `^thread-local` is silently wrong under the
   JIT, so the restriction does not buy a working first cut -- it buys a
   working-on-`cc`, broken-on-`tur jit` one.
2. Per-thread lazy init is not reachable from `__thread` in C at all.

The design that does work is the one the codebase already runs for dynvars: a
`pthread_key_t` with materialize-on-first-access. Recommendation in §11.4.

### 11.1 The constraint stack

**(a) C has no dynamic TLS initialization. [ran]**

```c
static __thread int lazy = compute();
/* error: initializer element is not constant */
```

C++ gives thread-locals dynamic initialization with compiler-generated guards;
C does not, in any dialect we target. So `(def ^thread-local buf (make-buffer))`
cannot lower to a `__thread` variable with an initializer, full stop. The
guard-flag rewrite (`if (!inited) { inited = 1; v = compute(); }`) compiles and
works [ran], but it is a manual lowering, not a language feature -- and it puts
a branch on every read of every `^thread-local`.

**(b) The JIT has no thread-local storage, and the existing workaround does not
generalize. [read]**

c2mir parses `_Thread_local`, warns "Thread local is not implemented", and then
treats the variable as an ordinary global -- so every thread shares one slot.
That is not theoretical: it is how "8 STM workers ended up sharing one
transaction descriptor and losing updates (stm-stress)"
(`src/runtime/tur_tls.c` header).

The fix in place is host residency: `emit_rt_tls` (`emit_module.c`) `#define`s
each name to a deref of an accessor in `src/runtime/tur_tls.c`, which is
compiled by a real `cc` and holds a genuine `__thread` slot. **It works because
there are exactly 11 slots, fixed at compiler build time, each with a
hand-written host counterpart.** A user's `^thread-local` has no host
counterpart, so under the JIT it would fall through to c2mir's "ordinary
global" reading -- silently, and into exactly the bug class the mechanism
exists to prevent. This is why §4.4's constant-initializer first cut does not
work: the initializer was never the hard part.

**(c) The thread trampoline covers 1 of 6 thread-creation sites. [ran: grep]**

"Run the initializer eagerly on thread entry" is attractive -- it needs no
per-access cost and no laziness. `tur_thread_trampoline` (`emit_module.c`) is a
hook we own and already runs per spawned thread. But only `stdlib/thread.tur`
routes through it. Direct `pthread_create` calls, bypassing it entirely:

| Site | Threads |
|---|---|
| `stdlib/future.tur` | 3 (timeout thread, with-timeout t-fn, with-timeout w-fn) |
| `stdlib/httpd.tur` | the worker pool |
| `stdlib/taskgroup.tur` | spawn-timeout thread |

So a trampoline-based design would leave a `^thread-local` **zero-initialized
on an httpd worker** -- reading a plausible-looking wrong value rather than
failing. Routing all six through one entry hook is a worthwhile refactor and a
**prerequisite** for this option, not a detail of it.

**(d) `pthread_key_create` is already in the tree, and works everywhere we
target. [ran + read]**

Dynvars (`defdynamic`) emit one `pthread_key_t` per variable with a cleanup
destructor, registered in the `STATIC_INIT_KEYS` band of the existing phased
static-init registry (`emit_module.c`; `emit_internal.h` defines the bands).
Verified working in this build [ran]: `(binding [*level* 7] ...)` prints
`1 / 7 / 1`, and the emitted read is

```c
TurDynFrame *f = (TurDynFrame *)pthread_getspecific(_dynvar_key_level);
int64_t r = f ? *(int64_t *)f->value : _dynvar_root_level;
```

It needs no compiler TLS support -- `pthread_getspecific` is a libc call -- so
it is the one mechanism here that is not blocked by (b). The dynvar code
carries an explicit note that it works under the JIT. Windows is covered:
`windows-remaining-plan.md` lists `pthread` (winpthreads) among the link deps.

### 11.2 The fork that decides the design

The dynvar layout above is **lazy by construction and needs no per-thread init
at all**: the initializer's value lives in a process-global root, a thread with
no binding reads the root, and only an override allocates a per-thread frame.

It is also the wrong semantics for `^thread-local`. Compare:

```turmeric
(def ^thread-local buf (make-buffer))
```

- **Root-sharing (dynvar semantics):** `make-buffer` runs once; every thread
  reads the *same* buffer until it writes. For a scalar counter that is fine.
  For anything allocating it is precisely the bug `^thread-local` exists to
  prevent -- the threads share the buffer.
- **Per-thread run:** `make-buffer` runs once *per thread*. This is what
  `^thread-local` has to mean, and it is what forces a real initialization
  story.

So the dynvar *mechanism* is reusable; its *layout* is not.

### 11.3 Options evaluated

| # | Design | Init cost | Access cost | JIT | Covers all threads | Verdict |
|---|---|---|---|---|---|---|
| 1 | `__thread` + constant initializers only (§4.4's first cut) | none | 0.34 ns | **broken** | n/a | rejected -- (b) |
| 2 | Eager init in the thread trampoline | per thread, always | 0.34 ns | broken | **no** -- (c) | rejected as-is |
| 3 | `__thread` value + `__thread` guard flag, lazy | per thread, on demand | 0.34 ns + branch | **broken** | yes | rejected -- (b) |
| 4 | One `pthread_key_t` per thread-local, materialize on first access | per thread, on demand | 1.63 ns | **works** | yes | **recommended** |
| 4b | **One** key holding a per-thread block of *all* thread-locals | per thread, on demand | 1.63 ns amortized | works | yes | **recommended at scale** |

Access costs measured on this box, `-O2`, with a compiler barrier so neither
loop hoists [ran]: `__thread` **0.34 ns/access**, `pthread_getspecific`
**1.63 ns/access** -- about 4.8x, or +1.3 ns absolute. Meaningful in a hot
loop, negligible otherwise, and `pthread_getspecific` cannot be hoisted across
a call the way a `__thread` address can.

The key budget is real: `_SC_THREAD_KEYS_MAX` is **1024** on this box and
exactly 1024 keys were creatable before failure [ran]. Option 4 spends one per
`^thread-local`, which is fine for tens and a hard wall at a thousand --
notably a wall shared with dynvars, which already spend one each. **Option 4b
removes the limit entirely**: one key holds a per-thread struct (or arena) of
every thread-local, indexed by slot, so a function touching several pays one
`getspecific` rather than one each. It is strictly better than 4 at any scale
above a handful and is the design to build if this ships.

### 11.4 Recommendation

**Build option 4b, and drop §4.4's constant-initializer restriction** -- it
does not simplify anything and it does not produce a working feature under the
JIT.

Sketch, reusing machinery that exists:

- One `pthread_key_t` for the whole program, created in the `STATIC_INIT_KEYS`
  band (where dynvar keys already go).
- The key holds a per-thread block: a zeroed struct with one slot per
  `^thread-local`, plus one `inited` bit each.
- Each access lowers to `tur_tl_block()` (one `getspecific`, materializing the
  block on first use) then a slot read; a slot whose `inited` bit is clear runs
  the initializer first. Initializer order within a thread is source order,
  matching `__tur_module_def_init`.
- The key's destructor frees the block on thread exit -- which `__thread` in C
  would not give us, and which matters the moment a `^thread-local` holds
  anything allocated.
- **Fast path, optional and last:** under `__GNUC__`, cache the block pointer
  in a `__thread` variable, reducing steady-state cost to a `__thread` read
  plus a null check. This is the same `#if defined(__GNUC__)` split
  `emit_rt_tls` already uses, and it must stay an optimization of an
  already-correct path, never the mechanism -- otherwise the JIT regresses to
  (b).

### 11.5 What this changes in this plan

- **§4.4's `^thread-local` bullet is superseded by this section.** Its premise
  ("the current `__tur_module_def_init` assigns once, which is wrong for a
  per-thread slot whose initializer is not a constant expression") is right
  about the symptom and wrong about the cure; restricting to constant
  initializers does not help, because the JIT breaks on the *storage*, not the
  *initializer*.
- **§9 open question 1 is answered.** The remaining design work is 11.6, not
  "needs a design".
- **G4 gets bigger and reorders.** §5 put `^atomic` first "because it is the
  smaller of the two and has no open design question" -- that still holds, and
  the gap just widened. `^atomic` lowers to `__atomic_*` builtins for which the
  JIT already has a shim (`tur_atomics.c`, same host-residency pattern);
  `^thread-local` needs the whole of 11.4. They should be separate phases, not
  one bullet.

### 11.6 Sub-questions left open

1. **Is `^thread-local` wanted enough to pay for 11.4?** It is the largest item
   in this plan by implementation cost and the one with the least demand
   evidence. A `^mut` global plus explicit `mutex.tur` covers the shared case;
   a dynvar covers the ambient-configuration case. Worth a demand signal before
   building.
2. **What does a `^thread-local` initializer see?** Source-order within a
   thread is the obvious rule, but an initializer that reads a *non*-thread-
   local global is reading state initialized on the main thread, which is fine,
   while one that reads another `^thread-local` needs the ordering to be
   defined. Simplest defensible rule: a `^thread-local` initializer may not
   reference another `^thread-local`.
3. **Interpreter parity.** turi has no thread-spawn of its own [ran: no
   `pthread_create` in `src/turi/eval.c`], so a `^thread-local` there is
   observationally a plain global. That is a defensible answer but it should be
   a written-down one, not an accident.
4. **Does the 1024-key wall already threaten dynvars?** They spend a key each
   today with no cap diagnostic. Independent of this plan, cheap to check, and
   a nastier failure than it looks -- `pthread_key_create` returning `EAGAIN`
   is currently unchecked at the dynvar site.

---

## 12. Research: what would a stronger `#reads` cost, and is it a prerequisite? (open question 2)

Answering §9's second open question, plus the follow-on it prompted: how much
work is a stronger `#reads`, and should it gate this plan? Investigated
2026-08-05. **[ran]** = probe compiled and executed; **[read]** = from the tree.

### 12.0 Verdict

**Not a prerequisite. Recommended as a companion, and it changes what G2 should
ship.**

Nothing in G2 is blocked on `#reads`. But the research turned up something that
does bear on it: a `#reads` frame that omits mutable state the body reads
produces a **false caller-side proof**, and `^mut` globals make that promise far
easier to break -- in plain turmeric, with no inline C. So the answer to §9's
literal question ("does a `#reads` frame naming a global buy anything today?")
is stronger than "no, it would be documentation":

**G2 should ship `#writes` only, and `#reads` should keep REFUSING to name a
global** -- not accept it as documentation. Accepting one would widen the
trusted surface exactly where the lie is cheapest to write. Reasoning in 12.4.

### 12.1 What `#reads` is today [read]

Narrower than `#writes` in every dimension:

| | `#reads` | `#writes` |
|---|---|---|
| Arity | **exactly one parameter** (`elab_fns.c`: `len == 2`) | a bitmask, `#writes [a b]` |
| Non-parameter name | hard error -- "names 'x', which is not a parameter of this function" | same |
| Checked? | **no -- trusted** | yes (WF2), behind `--enable=write-frames` |
| Consumers | one | WF3 invalidation |

The single consumer is `enc_reads_arg_frozen` (`refine_collect.c`). When a
measure declares `#reads w` and `w` is frozen at the call site, an otherwise
**impure** measure is granted congruence:

```c
if (!pure && info.reads_param_plus1 != 0 &&
    enc_reads_arg_frozen(E, f, info.reads_param_plus1))
    pure = true;
```

The soundness argument, in the same comment, is that this "elides the
caller-side crossing check, not the safety check" -- the backstop is the
accessor's own internal check, which lives in user code.

Note for G2: `#reads` cannot name a global **today** even if we wanted it to.
The grammar accepts one bare parameter name and hard-errors on anything else,
so G2 would have to extend the grammar, not just the resolution. `#writes`
already has the bracket form.

### 12.2 What a broken `#reads` promise actually costs [ran]

A `#reads w` measure that also reads a `^mut` global:

```turmeric
(def ^mut fudge 1)
(defn alive? [^borrow w : World e : int] #reads w : bool (> fudge 0))
(defn get! [^borrow w : World e : #refine{ x : int | (alive? w x) }] : int (.n w))

(defn run [] : int
  (let [^mut w (World 7)]
    (let [__b (& w)]                 ;; freezes w
      (if (alive? w 0)
        (do (set! fudge -1)          ;; the state the measure really reads
            (get! w 0))              ;; crossing -- (alive? w 0) is now FALSE
        -1))))
```

Compiles clean and prints **7**. The crossing check was elided on a predicate
that is false at run time. Remove the `(& w)` and the same program is
`TUR-W0372`:

> solver returned unknown for the refinement on argument 2 of 'get!' in 'run';
> **no runtime fallback for an impure `#reads` measure** -- the crossing must be
> proven (guard it inside a `frozen` region)

So at the crossing there is no safety net: the outcome is a proof or a
diagnostic, never a runtime check. That makes the trusted claim load-bearing.

**This is pre-existing, not something `^mut` globals created.** [ran] The
identical program with the global replaced by an inline-C `static int calls`
counter -- writable long before D4 -- behaves exactly the same way: compiles
clean, prints 7, check elided. Nothing regressed when `^mut` landed.

What *did* change is reachability. Breaking the promise used to require
reaching for inline C; now `(> fudge 0)` does it. And the documented trajectory
says the wall is inline-C opacity -- step 2 is "verified by the purity walk
**once a measure's state is Turmeric-visible** (struct fields rather than an
inline-C handle)". A global read is Turmeric-visible. By the trajectory's own
criterion this case is on the checkable side of the wall, and is simply not
being checked.

To be fair to the design: with a self-checking accessor (the ECS case, where
`get!` validates internally and returns a sentinel) a broken promise costs a
wrong-but-safe answer. The toy above has no internal check, which is what makes
the elision visible. The risk is real but its severity depends on user code.

### 12.3 What strengthening would cost

Two very different bills, and conflating them is what makes `#reads` look
expensive.

**The general strengthening is not mainly a compiler cost.** Surveyed every
`#reads` measure in the tree [ran]: they are essentially all inline-C bodies,
and the two that look like turmeric (`sealedlib`'s `w-read`) bottom out in
inline-C helpers. So a checked `#reads` in the general sense would verify
**approximately nothing that exists**, because the state it would verify
against is behind a handle the walk cannot see. Getting value there means
rewriting the measure layer to hold state in turmeric structs -- a change to
the ECS and spices, not to the compiler. That is the real bill, it is large,
and it sits outside this repo's compiler.

**The narrow strengthening is small.** State it as: *refuse the congruence
override when the body demonstrably reads mutable state outside the frame.*

- A classifier over the elaborated body -- "reads an `is_mut` binding that is
  not the `#reads` parameter". Same shape and roughly the same size as
  `rt_classify_expr`, whose walk it can borrow: ~60-120 lines.
- One flag on `RefineFnInfo`, which already carries `reads_param_plus1`,
  `writes_declared`, `writes_checked` -- the precedent slot exists.
- A few lines gating the override at `refine_collect.c`.
- Fixtures, including the 12.2 program as a negative.

Crucially it must be **default-deny-refusal**: refuse only on positive evidence
of an outside mutable read. An inline-C body yields no such evidence, so every
existing measure keeps today's trusted behaviour and **no current fixture
flips** -- checked by construction, since they are all inline-C. It can only
ever turn a currently-proving program into `TUR-W0372`, never the reverse, so it
still wants an experiment gate.

That asymmetry is the useful finding: the expensive part of step 2 and the part
that matters for mutable globals are **different work**, and only the cheap part
is relevant here.

### 12.4 Is it a prerequisite?

**No.**

- **G2 as scoped needs nothing from it.** `#writes` is already checked, and G1
  built the global-write walk it would extend. Globals entering the `#writes`
  vocabulary is independent of anything on the read side.
- **It is a guard, not an unblocker.** It prevents `^mut` globals from widening
  a pre-existing trusted-promise hole into plain turmeric. Worth doing, and
  worth doing near this work because that is when the exposure changes -- but
  sequencing it first would delay G2 for no technical gain.

**What it does change is G2's shape.** §9's question asked whether a global in a
`#reads` frame is merely documentation. It is worse than that: `#reads` is the
one annotation that *grants* congruence, so letting it name a global would let a
user write `#reads [w *cache*]` and buy a proof with a promise about mutable
global state -- the easiest promise in the language to break by accident, since
any function can write the global by name. Documentation that pays out in
proofs is not documentation.

So: **G2 ships `#writes` only**, and `#reads` keeps its current hard error for a
non-parameter name. If a read-side story is wanted later it should arrive
*checked*, via 12.3's narrow rule, not trusted.

### 12.5 Recommended sequencing

1. **G2 (`#writes` naming globals)** -- unchanged, unblocked, as planned.
2. **The narrow `#reads` refusal** (12.3) -- a small companion, independently
   useful, gated. Best landed near G2 while the reasoning is fresh; not before.
3. **General checked `#reads`** -- out of scope for this plan and mostly not a
   compiler change. If it is ever wanted, the prerequisite is a
   Turmeric-visible measure layer, and that decision belongs to the ECS/spice
   side.

### 12.6 Sub-questions surfaced

1. **Should the 12.2 program be a fixture regardless?** **DONE 2026-08-05.**
   `refine-reads-frame-omits-global` pins the elided crossing, and
   `errors/refine-reads-frame-omits-global-no-region` pins the `TUR-W0372` the
   same program gets without the frozen region. Both carry a header saying the
   behaviour is the documented trusted-promise contract rather than a defect,
   that it predates `^mut` globals (the inline-C equivalent is identical), and
   that §12.3's check landing SHOULD flip the positive -- so the next reader
   updates it deliberately instead of "repairing" it back to silence.
2. **Is `#reads`'s one-parameter limit deliberate or incidental?** `#writes`
   grew a bracket form; `#reads` did not. A measure over two frozen resources
   cannot be expressed today. Not needed here -- noting it because the
   asymmetry looks unplanned.
3. **Does `--strict-refine` change the 12.2 outcome?** It did not [ran] -- the
   obligation is *proved*, not unknown, so strictness has nothing to escalate.
   Worth confirming that is intended, since a user reaching for
   `--strict-refine` may reasonably expect it to catch exactly this.

---

## 13. Research: the remaining sub-questions (2026-08-05)

Closing out the sub-questions raised in §9, §11.6, and §12.6. **[ran]** = probe
executed; **[read]** = from the tree.

### 13.1 Should `--strict-refine` reject a trust-based proof? -- NO

**Recommendation: leave `--strict-refine` alone. Escalating there would delete
the feature's only real consumer.**

`--strict-refine` means "hard-fail refinement obligations the solver **cannot
prove**" [read: `main.c` help text]. Mechanically it escalates `TUR-W0372`
(undecided) from warning to error (`refine_discharge.c`). In §12.2's program the
obligation **is proved** -- so strictness has nothing to escalate, and that is
it working as designed. The gap is not in strictness; it is that the promise
backing the proof is unchecked.

The tempting change -- "strict should also refuse a proof that rests on a
trusted `#reads` override" -- is **wrong**, and measurably so. Nine fixtures
combine `#reads` with `--strict-refine` [ran], including the flagship
`refine-macrogen-foreach` (`for-each-alive!`):

```
refine-macrogen-crossings   refine-stateful-guard-discharges   refine-template-emitters
refine-macrogen-foreach     refine-stateful-nonfinal-statement refine-wf3-disjoint-set
refine-stateful-frozen-macro refine-stateful-resizable-bounds  wf3-borrow-write-free
```

They exist *because* the override grants the proof. The two features are
designed to work together: strict says "every crossing must be **decided**", and
the override is what decides them. Making strict refuse trusted proofs would
turn all nine into errors and leave `#reads` with no working consumer at all.

**The cheap interim that does work:** ship §12.3's classifier as a **warning
first** -- "this `#reads` frame omits mutable state the body reads" -- gateless,
before any refusal. It closes the "you cannot even tell" problem immediately and
is provably zero-risk for the nine: every one of their measures is inline-C, so
the default-deny classifier finds no positive evidence and stays silent. §12.2's
program would warn. Escalating warning -> refusal later is then a gated,
evidence-backed step rather than a guess.

### 13.2 `--shared` exports user globals, and the mangled suffix is the ABI [ran]

§9's fourth open question asked how separate compilation interacts. Sharper than
expected:

| Mode | Emitted | `nm` |
|---|---|---|
| executable | `static int64_t hits_1327;` | `b` (local) |
| `--shared` | `int64_t hits_1327;` | `B` (**global**) |

In `--shared` the emitter drops `static`, so every user global becomes an
exported symbol of the `.so`. Two consequences, neither urgent but both worth
recording:

- **The binding-id suffix is part of the exported ABI.** `_1327` shifts with any
  unrelated edit earlier in the unit -- proven in §11.1 by adding a `def` above
  and watching `counter_1327` become `counter_1328`. Nothing consumes these by
  name today, so nothing is broken; it does mean the mangling is not a stable
  interface and should not become one.
- **Two shared libraries can collide.** Two `.so`s each defining a global that
  mangles to the same name are a load-time clash under `RTLD_GLOBAL`.
  Mechanism identified, not reproduced -- flagged rather than claimed.

This does not change any phase of this plan. It does mean H3's read-only-export
rule (§4.3) has a linker-level counterpart worth having if separate compilation
grows, exactly as §9.4 suspected.

### 13.3 The 1024-key wall is latent, but `pthread_key_create` is unchecked [ran]

`_SC_THREAD_KEYS_MAX` is 1024 and exactly 1024 keys were creatable (§11.3). The
tree declares **20** `defdynamic`s total, 4 of them in stdlib, so the wall is
nowhere near -- this is robustness, not a live risk.

The defect is that the return value is discarded:

```c
static void _dynvar_init_X(void) {
    pthread_key_create(&_dynvar_key_X, _dynvar_cleanup_X);   /* rc ignored */
}
```

On `EAGAIN` the key is left uninitialized and every later `pthread_getspecific`
on it is undefined behaviour -- a silent wrong-value failure rather than a
diagnosable one. A two-line fix (check and `abort()` with a message naming the
limit). **Independent of this plan**; recorded here because §11.4's design would
spend one more key, and because a plan that recommends the mechanism should say
that the mechanism's one failure mode is currently unhandled.

### 13.4 `^thread-local` in the interpreter: a plain global [ran]

turi has no user-reachable thread spawn. The only `pthread_create` in the
interpreter is `ring_worker_nat` (`interpreter_natives.c`), a thread-ring
*benchmark* native -- not a general spawn primitive, and not something user
turmeric reaches. So under `--interpret` a `^thread-local` is observationally a
plain global, and there is no second thread for it to differ on.

That is a defensible answer, and §11.6.3's point stands: it should be a
written-down rule, not an accident. **Recorded 2026-08-05** -- §4.4 now states
it as the specified behaviour, and §5's G4 carries it as a named deliverable
(guide sentence plus a fixture asserting the annotation parses and runs under
turi) so it cannot be dropped when the phase is built.

### 13.5 `#reads`'s one-parameter limit: a deliberate minimal slice [read]

Deliberate, but as a *slice* rather than an argued permanent limit. The design
commit is titled "blocker 2 design -- trusted `#reads w`, sound via kept entry
check", and the guide frames the whole annotation as "the **minimal**, trusted,
refinement-only slice, shaped so a stronger version can grow from it without a
rename or a semantics break", explicitly calling it "a coarse (per-argument, not
per-heap-location) `reads` clause".

So single-parameter is consistent with the stated design, and the asymmetry with
`#writes`'s bracket form is a consequence of `#writes` having grown later, not a
considered decision that `#reads` should stay narrower. A measure over two
frozen resources cannot be expressed today. Nothing here needs it; if the
read-side plan (§14) is ever written, the bracket form is a natural early item
and costs nothing semantically.

### 13.6 Should `^atomic` imply `^mut`? -- NO, require both [read]

§9's third open question. **DECIDED 2026-08-05 (owner): keep them orthogonal;
`^atomic` requires an explicit `^mut`.**

`^mut` is the single gate for `set!` -- `elab_forms.c` tests `b->is_mut` at both
the bare-symbol and the field-write site, and nothing else grants mutability.
Making `^atomic` a second route would make it the only annotation in the
language that confers write permission as a side effect, which is exactly the
kind of surprise the D4 audit existed to remove.

The codebase already composes rather than implies: `^unique ^mut` appears
together throughout (`tests/fixtures/unique-vec-ops`, sealedlib's
`^unique ^mut w : W`). `(def ^atomic ^mut n 0)` is one token longer and reads
honestly; `^atomic` alone should be a diagnostic naming `^mut`, not a silent
grant.

### 13.7 What a `^thread-local` initializer may see [read]

§11.6.2, resolved as a recommendation rather than left open. Under §11.4's
design each thread runs the initializers in source order, so:

- Reading a **non**-thread-local global is fine: it was initialized on the main
  thread by `__tur_module_def_init` before any thread could exist.
- Reading **another `^thread-local`** is the ordering hazard, and the simplest
  defensible rule is to reject it outright. Per-thread initialization order is
  already source order; allowing cross-references would make it observable and
  therefore load-bearing, for no demonstrated need.

Recommend: **a `^thread-local` initializer may not reference another
`^thread-local`**, diagnosed by name. Cheap to check with the walk §11.4 needs
anyway, and looseneable later if anyone asks.

---

## 14. Should the read side get its own plan?

**Yes -- but written as part of G2, not before it, and only if G2 actually
lands.**

§12 established that the read-side work splits into two pieces with almost
nothing in common: a small refusal rule that belongs near this work, and a
general checked `#reads` whose cost is mostly a measure-layer rewrite outside
the compiler. That is exactly the shape that wants its own document -- this plan
is about globals, and a `#reads` plan is about the trusted-promise tier as a
whole (the ECS measures, the inline-C wall, the bracket form of §13.5, the
CSE/parallelization consumers the guide lists as blocked on checking).

Concretely:

- **In G2's scope:** §12.3's narrow rule, shipped as §13.1's warning first. It
  is small, it is motivated by this plan's own findings, and splitting it out
  would strand it.
- **A new plan, drafted as G2's last step:** everything else on the read side.
  Name it for the tier, not the annotation -- the subject is "making the trusted
  refinement claims checkable", of which `#reads` is one instance and the
  measure layer is the actual blocker. Its §1 is largely written already: §12.1
  (what `#reads` is), §12.2 (what a broken promise costs, with a running repro),
  §12.3 (the two bills), §13.5 (the arity slice).
- **Not before G2.** The findings that justify the plan came out of doing this
  work; writing it first would be guessing at them. And it must not become a
  precondition -- §12.4 already established the read side blocks nothing.

The one thing worth doing *immediately*, independent of any plan: §12.6.1's
fixture. There is currently nothing in the suite pinning what a broken `#reads`
promise costs, which makes §12.2's behaviour easy to rediscover as a bug. A
fixture turns the trust boundary from prose into something the suite asserts.

---

## 15. G2 execution record (2026-08-05)

**Landed**, behind `--enable=global-state`. `tests/run.sh` -> 2551 passed, 0
failed; `tests/run-turi.sh` -> 1742 passed, 0 failed, 704 skipped. No fixture
snapshot moved, and every frame verdict in `wf1-writes-frame-honored` is
unchanged.

### 15.1 What landed

`#writes` frame entries that resolve to a **mutable global** extend the frame
instead of being rejected. G1's blanket "any global write blocks VERIFIED"
becomes a coverage question, in a helper (`wf_global_verdict`) kept separate
from `wf_walk` for the reason §4.2 gives -- the two speak different
vocabularies:

| Body | Gate off (G1) | Gate on (G2) |
|---|---|---|
| writes no global | VERIFIED | VERIFIED |
| writes only declared globals | UNVERIFIED | **VERIFIED** |
| writes an undeclared global | UNVERIFIED | **TUR-E0382**, naming it |
| walk cannot tell | UNVERIFIED | UNVERIFIED |

G1's walk grew a set alongside its tri-state (`WgSet`, capped at
`WF_MAX_FRAME_GLOBALS` = 16, memoized on the `Binding` beside the verdict so a
second caller replays rather than re-walks). Overflow degrades coverage to
UNVERIFIED rather than letting a truncated set read as "all covered".

Transitivity comes free from G1: a callee's set unions into the caller's, so an
undeclared write inside a callee taking **no parameters** is caught -- the gap
the parameter walk leaves open by design.

### 15.2 Three rejections with their own reasons

- **An undeclared global write** is `TUR-E0382` -- the same code a write
  outside the parameter frame gets, because it is the same kind of mistake --
  but the message names the global, since "widen the frame" is not actionable
  if you cannot see what to widen it with. The note says "add the global to it,
  or stop writing it".
- **An immutable global in a frame** is `TUR-E0381` with its own text. Naming
  one is a claim that cannot be true, and saying so beats a message about
  parameters.
- **A global without the experiment** stays the pre-G2 "not a parameter" error.
  The gate covers the **grammar**, not just the checking: accepting the name and
  ignoring it would be a silently-dropped frame member, which is what
  `TUR-E0381` exists to prevent.

### 15.3 Deliberately `#writes` only

Per §12.4 and the owner decision. `#reads` keeps its hard error for a
non-parameter name, and the experiment row says why in full: `#reads` is the
annotation that *grants* congruence, so a global there would let a promise about
mutable global state pay out in proofs. The `refine-reads-frame-omits-global`
pair pins what that costs.

### 15.4 Fixtures

`g2-writes-global-frame` (declared-and-written, two globals, declared-but-
unwritten, a mixed parameter+global frame; asserts the dump's new
`declared=[...]` column), `errors/g2-writes-global-undeclared` (direct **and**
transitive-through-a-parameterless-callee), `errors/g2-writes-global-immutable`,
`errors/g2-writes-global-needs-gate`.

The positive carries `requires.interp` for the same harness reason G1's do --
the compiled path does not capture compile-phase stdout. Its marker file says
so.

### 15.5 What G3 inherits

Unchanged: exported globals are still writable by every importer (H3). G2 gave
frames a vocabulary for globals; it said nothing about who may write one.

---

## 16. G3 execution record (2026-08-05)

**Landed**, behind `--enable=global-state`. `tests/run.sh` -> 2555 passed, 0
failed; `tests/run-turi.sh` -> 1746 passed, 0 failed, 704 skipped. No fixture
snapshot moved.

### 16.1 The rule

An exported global is readable by importers and writable only from its defining
module. `set!` on another module's global is an error naming the owning module
and both ways out:

```
error: set!: 'hits' is owned by module 'ctr' and is exported read-only;
call a setter that module exports, or have it export the global as
`(export (mut hits))`
```

It bites only across a **real module boundary**. A global with no defining
module -- every single-file program -- and a write from inside the owning module
are both untouched, which is what keeps this from being a broad break.

### 16.2 The escape hatch: `(export (mut g))`

§4.3 asked for a permission declared at the definition site, and said it
"should not be a new annotation if an existing one can carry it". It does not
need one: the export list **already** has a structured form,
`(export (effect Name))`, and `(mut g)` slots into the same parse arm.

```turmeric
(defmodule ctr
  (export bump peek hits (mut shared))   ; hits read-only outside; shared writable
  (def ^mut hits   0)
  (def ^mut shared 0)
  ...)
```

A `(mut g)` entry exports the name normally *as well*, so a reader needs no
second entry. This beats an annotation on the `def` for the reason §4.3 gives:
the permission is a statement about the module's interface, and it belongs
where the rest of that interface is written.

### 16.3 Two rejections, by name

`(mut ...)` on something that cannot be written is refused rather than left
inert -- the silently-meaningless-annotation shape the D4 audit and G2 both went
out of their way to remove.

- **On a function.** A top-level `defn` is `is_global` too, so staticness alone
  does not separate a data global from a function; the fn type (or a backing
  `FnDef`) is what does. Caught during implementation: the first cut reported a
  function as "an immutable global", which is true and useless.
- **On an immutable global.** Same reasoning as G2's rejection of one in a
  `#writes` frame: permitting outside writes to something nothing can write is
  a claim that cannot be true.

### 16.4 Fixtures

`g3-export-mut-permits-write` (reading a plain export, calling an exported
setter, and writing a `(mut ...)` export -- all fine), plus
`errors/g3-cross-module-global-write`, `errors/g3-export-mut-not-a-global`,
`errors/g3-export-mut-immutable`.

Each is a two-module fixture in the shape
`errors/sealed-opaque-cross-module-fabricate` established: `input.tur` holds the
importing module and a sibling directory holds the imported one.

**Harness bug found on the way, filed not fixed:** a fixture directory whose
input is not `input.tur` or `<dirname>.tur` is reported as **PASS** while
running nothing. Four directories are in that state -- `sandbox/` (17 files),
`stm/` (7, including atomicity and deadlock-freedom), `module-transitive-imports/`
(4), `typeclass/` (2) -- and no other harness picks them up. 30 `.tur` files
that the summary line counts as passing. Filed as
[docs/reported/fixture-dirs-with-loose-tur-files-pass-without-running.md](../reported/fixture-dirs-with-loose-tur-files-pass-without-running.md).
Found because `module-transitive-imports` looked like the multi-module fixture
precedent and turned out not to run; the shape G3's fixtures actually use is
`errors/sealed-opaque-cross-module-fabricate`'s.

### 16.5 What G4/G5 inherit

Unchanged. G3 said who may write a global; it said nothing about thread safety
(G4, §11/§13.6-13.7) or the guide/graduation work (G5).

---

## 17. G4a execution record -- `^atomic` (2026-08-05)

**Landed**, behind `--enable=global-state`. `tests/run.sh` -> 2559 passed, 0
failed; `tests/run-turi.sh` -> 1750 passed, 0 failed, 704 skipped.
**G4b (`^thread-local`) is NOT in this; it remains as §11.4 describes.**

### 17.1 What landed

`(def ^atomic ^mut g v)` makes every read a `TUR_ATOMIC_LOAD_*` and every `set!`
a `TUR_ATOMIC_STORE_*`, sequentially consistent, through two chokepoints:
`atom_var` (value position) and `emit_set_stmt`. `name_for_binding` stays bare
-- it also spells the definition and every assignment target.

Wrapping the **read** matters as much as the write, and is the easier half to
skip: a bare global read in a loop may be hoisted into a register, so a spinning
reader would never observe another thread's store however atomically that store
was made.

§11.5's claim that "the JIT already has an atomics shim" holds, for a reason
worth stating exactly: the shim takes a **pointer**
(`tur_atomic_store_u64(volatile uint64_t *p, ...)`), so it operates on storage
the JIT already owns. That is the asymmetry with `^thread-local`, which needs
storage the *host* must own -- `tur_tls.c`'s own header draws the same line
("applied to state instead of operations"). Atomics generalize to user
variables; thread-local storage does not.

### 17.2 The limit that matters most

**`^atomic` does not make `(set! c (+ c 1))` safe.** That is a load then a
store, not an atomic read-modify-write; two threads still lose updates.
`^atomic` makes each half indivisible, it does not fuse them. A concurrent
counter wants a CAS or fetch-add (`stdlib/atomic.tur`) or a lock.

This is stated in the changelog, the fixture header, and here, because it is the
single most likely way for the feature to be misread -- "atomic counter" is what
the annotation *sounds* like it delivers.

### 17.3 A miscompile caught by the first fixture

The initial scalar set admitted `:bool`. A `bool` global is `static bool g;` --
**one byte** -- while the atomics layer's host shim is `uint64_t`-typed, so the
emitted access is eight bytes regardless. `(set! flag true)` compiled to an
8-byte `__atomic_store_n` into a 1-byte object: a genuine overflow of the
adjacent statics.

It **printed the right answer**. GCC's `-Wstringop-overflow` is what surfaced
it, and only because the fixture happened to exercise a bool. Had the first
fixture used only ints and floats it would have shipped.

`type_is_atomic_scalar` is now eight-byte kinds only -- `:int`, `:float`,
`:cstr`, `:ptr` -- and a narrower kind is rejected with a reason ("for a bool
use an int flag") rather than silently widened. Restoring `:bool` means a
1-byte shim on the non-GNU branch, not a change to the predicate.

### 17.4 Three rejections

`^atomic` without `^mut` (§13.6's decision, enforced rather than assumed); a
non-8-byte type, pointing at `stdlib/mutex.tur`; and `^atomic` without the
experiment, refused by name rather than accepted and ignored -- a silently-inert
`^atomic` would be a global the program believes is synchronised and is not.

### 17.5 Snapshot regen

The float path crosses the integer-typed atomics layer through its bit pattern
(`__tur_bits_to_f64` / `__tur_f64_to_bits`, `memcpy`-based so it is not
strict-aliasing UB), which adds two `static inline` helpers to the preamble.
That is a new preamble, so **141 `expected.c` snapshots were regenerated in this
same change**, per CLAUDE.md. The diff is uniform and purely additive -- +6 lines
per file, 846 insertions, **0 deletions** -- which is the shape that confirms the
helper block is the only codegen change.

### 17.6 What G4b and G5 inherit

- **G4b (`^thread-local`)**: unchanged, and still the larger piece -- all of
  §11.4 (one `pthread_key_t`, a per-thread block, materialize-on-first-access),
  plus §13.7's rule that its initializer may not reference another
  `^thread-local`, plus §13.4's turi note and fixture.
- **G5a**: the guide section §4.4 promises and the dynvar-vs-global steer.
- **G5b**: the graduation decision, which wants adoption evidence rather than
  writing time.

### 17.7 Not covered, stated

No fixture asserts that two threads sharing an `^atomic` global do not tear.
That property is *observed*, not asserted -- a racing fixture would be flaky
rather than rigorous, and a passing race proves nothing. What the fixture does
assert is that the lowering is correct and round-trips every admitted scalar
kind. The honest claim for `^atomic` is "the emitted accesses are atomic", and
that is what is tested.

---

## 18. G4b execution record -- `^thread-local` (2026-08-05)

**Landed**, behind `--enable=global-state`. `tests/run.sh` -> 2563 passed, 0
failed; `tests/run-turi.sh` -> 1753 passed, 0 failed, 705 skipped. No snapshot
regen needed -- the per-thread machinery is emitted only into units that declare
a `^thread-local`, so no fixture's `expected.c` moved.

**G4 is complete.** Remaining: G5a (docs) and G5b (graduation).

### 18.1 What landed, and that it is §11.4's design

One `pthread_key_t` for the whole program holding **one per-thread block**,
exactly as §11.4 recommended over a key each -- the budget is 1024 process-wide
and shared with dynvars, and a function touching several thread-locals pays one
`getspecific` rather than one each.

Each global gets a value field and its own `inited` byte in the block, named by
mangled binding name, so an accessor names both directly and there is no
slot-index bookkeeping. `calloc` zeroes the block, so `inited` starts false on
every new thread -- which is precisely "not yet initialized on this thread".

Per global the emitter produces an `__tur_tl_initfn_<n>` (the declared
initializer, as a function so it can run once per thread), a `__tur_tl_get_<n>`
(materialize block, run initializer if this thread has not, return), and a
`__tur_tl_set_<n>` (a write counts as initialization -- it replaces what the
initializer would have produced, so running it afterwards would clobber the
write). Reads and writes route through the same two chokepoints `^atomic` uses.

A `^thread-local` has **no** process-wide storage and no entry in
`__tur_module_def_init`; that is the difference the whole design turns on.

**§11.4's optional `__thread` fast path is NOT implemented**, deliberately. It
was described as an optimization of an already-correct path and last in
sequence; the correct path is what landed.

### 18.2 The one failure mode, checked

`pthread_key_create`'s return value is checked and aborts with a message naming
the feature. §13.3 found the dynvar path ignores it -- leaving the key
uninitialized and every later `getspecific` undefined -- and a plan that
recommends this mechanism should not reproduce its one unhandled failure. The
dynvar site is still unfixed and still filed.

### 18.3 The fixture asserts both properties without a race

`g4b-thread-local-global` spawns two workers plus main, each incrementing its
own copy from 100 a different number of times: **103 / 105 / 101**. If the
copies were shared the totals would be 109-ish and order-dependent, so a shared
slot cannot produce that output by luck. It proves per-thread *isolation* and
per-thread *initialization* in one deterministic assertion, and nothing in it
races -- the only cross-thread writes go to distinct malloc'd result slots.

This is a stronger test than G4a got, and the difference is inherent rather
than effort: "each thread has its own copy" is a functional property a
deterministic program can pin, where "these accesses do not tear" is not.

### 18.4 Three rejections

An initializer referencing another `^thread-local` (§13.7's rule, enforced);
`^thread-local` with `^atomic` (a per-thread copy is unshared, so atomicity
would suggest a synchronisation that is not happening -- worse than nothing,
because it reads as a safety claim); and `^thread-local` without the experiment,
refused by name, since a silently-inert one would be a global every thread
shares while the program believes each has its own.

### 18.5 turi

`^thread-local` is accepted and degenerate under `--interpret`, as §13.4
specified. Recorded in the guide (G5a's first item, landed early here since it
is one paragraph and belongs beside the annotation it describes).
