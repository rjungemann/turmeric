# Mutable globals -- making `^mut` global state visible to the disciplines that already exist

> **Status:** **G1 LANDED 2026-08-05** (see §10). G2-G5 proposed.
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

- **`^thread-local`** -- each thread gets its own copy, emitted through the
  existing `TUR_THREAD_LOCAL` macro. Initialization is the open question: the
  current `__tur_module_def_init` assigns once, which is wrong for a per-thread
  slot whose initializer is not a constant expression. Resolve during G4;
  restricting the first cut to constant initializers is an acceptable answer.
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
- **G2 -- globals in the frame vocabulary.** §4.2. Upgrades G1's blanket
  UNVERIFIED to "verified against a frame that names the global". Behind
  `--enable=global-state`.
- **G3 -- module encapsulation.** §4.3. The one phase that can reject existing
  code. Behind the same gate. Wants its own soak time before graduating.
- **G4 -- `^thread-local` and `^atomic`.** §4.4. Independent of G2/G3; can land
  in either order relative to them. `^atomic` first -- it is the smaller of the
  two and has no open design question.
- **G5 -- docs and graduation.** The guide section §4.4 promises, a
  `binding-forms-guide.md` cross-reference, the dynvar-vs-global steer from
  §1.2, and the graduation decision for `global-state`.

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

1. **`^thread-local` initialization** (§4.4). Constant-initializer-only is an
   acceptable first cut; per-thread lazy init needs a design.
2. **Does a `#reads` frame naming a global buy anything today?** `#reads` is
   still step 1 -- trusted, refinement-only. A global in a `#reads` frame may be
   documentation until the read side becomes checked. If so, G2 should ship
   `#writes` only and say why.
3. **Should `^atomic` imply `^mut`?** An atomic global that is never written is
   just a global. Leaning yes-implies, so `(def ^atomic n 0)` is writable, but
   it makes `^atomic` the only annotation that grants mutability and that is a
   surprise. Resolve at G4.
4. **Interaction with `--shared` and separate compilation.** §1.1 establishes
   that `tur build <dir>` is single-TU today, so nothing forces the question
   now. A future separately-compiled global needs real linkage (`extern` plus
   one definition), and G3's read-only rule would then have a linker-level
   counterpart worth having.

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
