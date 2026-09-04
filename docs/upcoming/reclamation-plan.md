---
title: Reclamation Plan (RM)
category: Planning
description: Freeing what the emitted program allocates -- the mechanism the sum-representation plan has routed three separate decisions through since 2026-08-25 without a plan existing to route them to. Scope-exit drop glue for the boxed residue, then the recursive spine, then declared regions over an arena that already ships inside every emitted binary. The first phase builds nothing: SR1, SR2a and SR3 slice A have moved the baseline out from under every row of the measurement this plan inherits.
---

# Reclamation (RM)

**Status (2026-09-02): every phase has an outcome.** RM0 ran (census
re-based as allocations; no workload -- see its section). RM1 is built and
has been narrowed four times (erased sweep 8324 -> 5643 B; the residue is
attributed in `docs/artifacts/leak-sweep-decomposition.md`: fixture
scaffolding, recursive spines, dictionary sites). RM2 and RM3 do not
start: no constituency, recorded rather than left pending. RM4 decided the
SR4 default in favour of by value. What remains open on this track is
recorded in the SR plans, not here.

[sum-representation-plan.md](sum-representation-plan.md) has named reclamation
the largest measured win on its board since 2026-08-25 and has routed three
separate decisions through it -- SR4's default flip ("waiting on either a
workload that wants memory over speed, or reclamation landing first"), the
slab's shelving ("reclamation (row B) is the unblocked half"), and its own
ordering rationale ("do the arena first and then re-ask whether SR4 is worth
starting"). No plan existed to route them to. This is that plan.

**The first phase builds nothing, and that is the point.** The 7.64x that
justifies this work was measured on 2026-08-25 against a tree where every
multi-variant ADT boxed. SR1 (2026-08-26), SR2a/SR2b (2026-08-27) and SR3
slice A (2026-08-27) have since removed the allocation outright for most of
that population. The rows still price a real population -- the recursive
sums, which are still carrier-boxed by default -- but "7.64x" stopped being
a claim about the language and became a claim about one corner of it, and
nobody has re-measured. **RM0 exists to re-base the number before anything is
built on it.**

That is the same discipline SR0 was specified under, and the same trap
section 5 of the SR plan records: a phase gate is only as good as the workload
it is measured on. Here the workload did not change -- the compiler did, out
from under it.

## 0. Provenance

- [multi-variant-adts-always-heap-allocate](../archive/multi-variant-adts-always-heap-allocate.md)
  -- "Cause 2 -- nothing ever frees them", and the
  [slab-shelving decision](../archive/multi-variant-adts-always-heap-allocate.md#decision----the-slab-allocator-is-shelved-2026-08-25)
  with its two-part reopen condition.
- [benchmarks/adt-alloc/RESULTS.md](../../benchmarks/adt-alloc/RESULTS.md) --
  the seven representations, and the caveat on rows F and G that is the whole
  design problem of RM3.
- [sum-representation-plan.md](sum-representation-plan.md) sections 2 and 3.
- Three open reports this plan is the fix for:
  [carrier-sum-option-boxes-have-no-owner](../reported/carrier-sum-option-boxes-have-no-owner.md),
  [inline-c-option-carrier-box-leaks](../reported/inline-c-option-carrier-box-leaks.md),
  and the container-element box that
  [container-element-form-plan.md](container-element-form-plan.md) removes for
  niche Options only.

## 1. What is actually true today

### Nothing frees a sum box, and one line says so

```c
/* elab_effects.c:30 */
return def->needs_drop_glue && !def->is_heap && def->n_ctors == 1;
```

`needs_drop_glue` (`elab_structs.c`) is set for `TY_RC` / `TY_REF` / `TY_WEAK`
and boxed-fn fields, and transitively for a nested owning by-value aggregate
field -- so it releases a box's *contents*, never the box itself, and only
for a single-variant def; `elab_forms.c:30` gates the same way. A `:heap` sum yields a typed
pointer instead of an `int64_t` carrier and still never frees. Every
construction that still boxes, still leaks.

### The population that still allocates has shrunk, and the benchmark has not noticed

| construction | allocated 2026-08-25 | allocates today |
|---|---|---|
| non-recursive multi-variant `defdata` | yes | **no** -- SR1, default on |
| concrete `(Option T)` / `(Result T E)` monomorph | yes | **no** -- SR2a, graduated |
| `(none)` | yes | **no** -- SR3 slice A, default on |
| **self-recursive sum spine, per node** | yes | **yes** -- SR4 not defaulted |
| **erased-path `some` / `ok` / `err`** | yes | **yes** -- generic bases |
| **`:heap` ADTs** | yes | **yes** |
| **wide by-value aggregates at a fat/container boundary** | yes | **yes** -- b4box |
| **container elements crossing the erased boundary** | yes | **yes** -- CE's subject |

The top three rows are gone, and they were most of what a corpus constructs.
The bottom five are what reclamation now has for a constituency. `logic.tur`'s
`Term` and `Subst` are self-recursive, so **row A of the ceiling table is still
literally today for them** and the benchmark has not rotted -- it has narrowed.
RM0's job is to say by how much, in the corpus rather than in one benchmark.

### The mechanism to build RM1 out of already exists, three times over

The emitter carries three independent "bind a heap value to a local, prove the
name does not escape, free it at scope exit or at its sole use" families:

| client | escape walk | firing |
|---|---|---|
| closure env | `closure_binding_escapes` (emit_expr.c) | `let_binding_env_freeable` |
| catch box | `catch_box_binding_escapes` | trailing drop |
| `any` widen box | `any_box_binding_escapes` | pending-drop stack + scope-drop stack, fired at `return` and at tail-call back-edges (`emit_any_scope_drops`, emit_expr.c:4061) |

The third landed 2026-08-29 over five passes and is the most complete: it
handles the temporary case (`any_pending_*`, drained per node so an inner call
never drops an outer call's argument), the scope case, and early exits. **A
carrier sum box is the fourth instance of that shape, not a new mechanism.**
That is what makes RM1 the cheap phase and why it goes first.

### And the arena already ships inside every emitted program

`src/runtime/arena.c` is in `TUR_CORE_SOURCES`, so it is in `libturi`, which
every fixture binary already links. Its API is `arena_init` /
`arena_alloc_aligned` / `arena_reset` / `arena_free` / **`arena_owns`**, and it
carries ASan-aware poisoning plus a guard mode that turns a stale pointer into
a hard SIGSEGV at the deref (`arena-debug-poisoning-plan`). The emitted
preamble already declares libturi externs directly (`extern uint64_t
tur_atomic_load_u64(...)`, emit_module.c:8601).

**So RM3's runtime is three extern lines, not an implementation.** And
`arena_owns` is specifically the thing whose absence killed the slab: the slab
died on a `free()` of slab memory reached through `rc/of`, and being safe
needed a whole-program escape pass. A free path that asks `arena_owns(p)`
first is safe *locally*, which is the difference between this and the
representation that was shelved.

## 2. The two mechanisms, and the one honest caveat

| | representation | vs today |
|---|---|---:|
| B | boxed, **freed** per node | 2.49x |
| G | boxed, **arena**-reclaimed | 7.64x |
| F | by value **and** arena-reclaimed | 10.95x |

Reclamation is also nearly *flat* across a 64x heap range where leaking climbs
-- this is a footprint problem before it is a speed problem, which is the
report's own corrected finding.

The caveat on F and G, restated because it is the entire design problem of
RM3 and quoting it is cheaper than rediscovering it:

> An arena needs a region whose end is known, and that is the hard part, not
> the bump pointer. This harness resets at the end of each pass because the
> pass boundary is obvious; real code has no such boundary handed to it, and
> inferring one is region inference.

**RM does not do region inference.** RM3 takes the boundary as *declared*.
That is a smaller feature than the 7.64x implies, and pretending otherwise is
how this plan would end up promising a number it cannot deliver.

## 3. Phases

### RM0 -- re-base the measurement, and find the constituency

Builds nothing. Two deliverables, and the second is the one the slab decision
already asked for and nobody has produced.

**(a) Re-base the ceiling.** `benchmarks/adt-alloc/ceiling.c` models
`logic.tur`'s `Term`/`Subst` shapes by hand in C. Re-run it unchanged (it
still prices the recursive population honestly), then add the measurement it
cannot make: what a corpus program actually still allocates. The instrument
exists -- SR0(a)'s per-constructor counters injected into emitted C
(`benchmarks/sum-census/`), which deliberately are not a codegen flag so the
measurement does not perturb what it measures. Re-run that census against
today's compiler and report allocations, not constructions: SR0(a) counted
constructions when every construction allocated, and those are now different
questions.

**(b) Find a real workload, or record that there is not one.** The slab
decision's reopen condition names two halves and requires both: a real
workload -- not a benchmark -- that constructs enough to care, *and* a reason
it cannot take the cheaper fix. SR0(a) went looking for the first half and did
not find it: 20 `defdata` across 727 spice files, `minikanren` constructs
nothing and does not import the module it is named for, and `datalog` is the
one sum-heavy program. **The honest possible outcome of RM0 is that RM2 and
RM3 have no constituency and only RM1 should be built.** That outcome must be
reportable, or this phase is not a gate.

*Files:* benchmarks only. *Gate:* RM2 and RM3 do not start until (a) reports
the residual allocating population and (b) either names a workload or
concludes there is not one. *Validation:* zero behavior change; the corpus
untouched.

**RM0 ran on 2026-09-02.** Both deliverables are in
[benchmarks/sum-census/RESULTS.md](../../benchmarks/sum-census/RESULTS.md)
(the RM0(a) section) and
[benchmarks/adt-alloc/RESULTS.md](../../benchmarks/adt-alloc/RESULTS.md).

- **(a)** The ceiling re-runs unchanged with the same ordering (F 13.1x,
  G 7.0x over A on this box). The corpus census, reported as ALLOCATIONS:
  2206 attempted, 555 construct anything; 22,176 allocations outside the
  two GC stress fixtures, of which `heap` and `gadt` are by design,
  `sum-flat` is down to **202** (90% of flat-sum constructions are by value
  now -- SR2a/b did their job; the 202 are RM1's erased residue), and the
  **recursive spine is 4,205 across 16 fixtures**: one lazy-stream fixture
  (2,731) and one regex engine (1,336) are 97% of it.
- **(b)** No workload. `datalog` constructs ~110 `:heap` values per run
  (typed pointers by design); `minikanren` constructs nothing; the spice
  profile could not be re-run here (sibling checkout absent) and the SR0
  grep stands (20 `defdata` in 727 files). **RM2 and RM3 have no
  constituency today.** This is the outcome the gate said must be
  reportable, and it is reported: they do not start. The record to reopen
  on is a real program whose recursive-sum construction shows up in a
  profile, not a fixture.

### RM1 -- drop the boxed residue at scope exit

The row B mechanism, local, no whole-program analysis, as the fourth client of
the `any`-box machinery. A carrier sum box bound to a local that provably does
not escape is freed at scope exit and at every early exit, through
`any_scope_drops_push` / `emit_any_scope_drops` and the pending-drop stack for
the temporary case.

Closes [carrier-sum-option-boxes-have-no-owner](../reported/carrier-sum-option-boxes-have-no-owner.md)
for the erased path, which is what it has narrowed to after SR2a. Whether it
closes [inline-c-option-carrier-box-leaks](../reported/inline-c-option-carrier-box-leaks.md)
is an open question RM1 must answer explicitly rather than assume: that box is
built inside a C body the emitter did not write, so the drop has to attach at
the call site from the declared return type, and the report says the obvious
workaround does not transfer.

**Take the `any` family's hard-won rules with it, do not re-derive them.** Two
in particular, both of which cost a pass each: a narrowing cast on a bare name
is not an escape when the cast emits a copy (but that admission is a flag the
`any` rules set, and the closure-env and catch-box callers still refuse a
cast); and `emit_tail` emits a tail-position `let` inline rather than through
`emit_let_value`, so bookkeeping added only to the latter silently never runs
for exactly the loop shape this kind of phase exists to fix.

*Files:* emit_expr.c, emit_stmt.c, emit_fns.c. *Gate:* none -- this is the
phase that runs whatever RM0 says. *Validation:* `requires.leak-check`
fixtures with real teeth (the SR1 pattern: bytes leaked without the change,
zero with it), the full corpus green at its current baseline, and snapshots
regenerated in the same commit.

#### Scope, MEASURED 2026-08-30 (before building anything)

The corpus was swept twice -- once for where the erased base is emitted and
called, once under LeakSanitizer -- because this section was written assuming
a residue nobody had counted. Four findings, and two of them narrow the phase.

**1. Generic wrappers do not box at all.** `(defn wrap [A] [x : A] :
(Option A) (some x))` specializes end to end: `wrap__spec__...` calls
`some__spec__...`, the erased base is not even emitted, and a 200,000-call
loop leaks zero bytes under ASan. The intuition that "a generic body means an
erased box" is wrong; SR2a's specialization reaches through ordinary generic
code.

**2. The erased base is mostly DEAD CODE.** Across 2181 emitted fixtures it is
*defined* in 95 and actually *called* in **27**. The other 68 emit a base the
linker drops.

**3. Of the 27 callers, 24 leak -- and 13 name the sum constructor in the
trace.** Those 13 are RM1's constituency, ~1.7 KB total, 32-528 bytes each.
The other 11 leaks are different allocations (`httpd-req-string-opt` 3756 B,
`re-string` 1281 B lead them) and are NOT this phase's; they want their own
look, and the fact that nobody has taken it is the point of the aside below.

**4. The shape is one shape: typeclass instance bodies / HKT dictionary
dispatch.** Every SUM-BOX caller is `hkt-*`, `result-monad-*`, `zipper-*`,
`option-map-capturing-closure` or `result-basic`. A representative trace:

```
ctor_Some  ->  some  ->  __inst_Applicative_ap_Option  ->  main
```

That is precisely "an instance method's carrier base" from the report, and it
is the one place specialization does not reach -- a dictionary-dispatched
method has no concrete element type to specialize against.

**What this confirms about the mechanism, and what it rules out.** The
consumer keeps the value as a CARRIER (`opt_hyval(__ps_204)`), never bridging
it to a by-value aggregate -- so there is no carrier->concrete crossing to
hang a free on. The owned-carrier table built for
[inline-c-option-carrier-box-leaks](../archive/inline-c-option-carrier-box-leaks.md)
frees at exactly that crossing and therefore does NOT reach this shape. The
scope-exit drop this section proposes is the right mechanism, and the two are
complementary rather than alternatives: one frees a box the caller converts,
the other frees a box the caller merely reads.

**One caveat that shapes the work.** In the representative trace the box is
the instance method's RETURN value -- it escapes the frame that built it, so a
drop inside `ap` would be wrong. The drop belongs to the CALLER's scope, on
the temp holding the returned carrier (`__ps_204` above), which is the
`any`-box pattern applied one frame out. That is the same place the escape
walk has to run, so the machinery fits -- but "free where it was built" would
be a use-after-free, and the phase should be written knowing that.

#### First increment, LANDED 2026-08-30: the null-None mirror

Building the phase started by finding that part of the residue was not an
ownership problem at all. SR3 slice A made the carrier `None` the null pointer
at three producers -- the monomorph ctor (types.c), the base ctor in
`emit_adt_typedef_and_ctors`, and the preamble's `tur_none()`. But
`emit_program` emits base ctors at a FOURTH site of its own (`early_file`,
emit_module.c), whose own comments call it a "mirror" of the second -- and that
copy never got the branch. So an erased `(none)` still malloc'd a `tag = 0`
box, showing up in the sweep as `ctor_None -> none -> ...` traces.

Fixed by adding the missing branch. The read side has accepted NULL as tag 0
since slice A, so this removes an allocation with no ownership analysis at all
-- the "make the box never exist" direction the report prefers, and the
cheapest kind of win this plan can have.

It is the same defect class as the merge-temp bug fixed earlier the same day:
two sibling emitters that are supposed to mirror each other, where a change
landed in one. Worth a standing habit -- when a representation change lands in
a ctor or temp emitter, grep for the other emitter before calling it done.

Measured: 8324 -> 8212 bytes across the 27 callers, with several fixtures
shrinking individually (`option-map-capturing-closure` 72 -> 40 B,
`refined-nonempty` 112 -> 80 B, `zipper-basic` 96 -> 80 B). Modest, and honest:
the bulk is `some`/`ok`/`err` boxes, which carry a payload and cannot be made
null. Snapshot churn was 148 fixtures, regenerated in the same commit.

#### The blocker for the REST of the phase, established 2026-08-30

**The scope-exit drop this section proposes does not fit the shape the sweep
found, and the reason is not fixable by moving the drop.**

In the representative trace the box is built by `some(...)` INSIDE
`__inst_Applicative_ap_Option` and returned, so it escapes the frame that
builds it -- no drop inside `ap` is correct. The drop must go to the caller,
on the temp holding the returned carrier. But at the caller, whether that
carrier is owned depends on the callee's BODY, not its signature, and the
Option instances are split:

```turmeric
(definstance Applicative [Option]
  (ap [ff fa] ...))                          ; every path returns some(..)/none() -- FRESH

(definstance Alternative [Option]
  (alt-or [x y] (match x (Some _) x (None) y)))   ; returns an ARGUMENT
```

Freeing the result of `alt-or` would free a box the caller still holds. That
is the same class of error as keying the inline-C free on a call's resolved
type, where `vec-get` -- also inline C, also Option-typed -- returns a box the
VECTOR owns; that one cost a `double free detected in tcache 2` before the
declared-type key fixed it.

So a safe RM1 needs a **per-callee freshness analysis**: "does every return
path of this function yield a freshly-allocated carrier box or NULL?"
`ap` passes, `alt-or` fails. That is intraprocedural and bounded -- the
tail-walking family (`fn_body_tail_is_carrier_producer`) is the right shape to
extend -- but it is a real analysis the phase text did not anticipate, and
"local, no whole-program analysis, fourth client of the any-box machinery" is
no longer an accurate description of the work.

~~**Recommendation: do not build the drop until that analysis is
specified.**~~ **The analysis was specified and BUILT 2026-08-30.**

`returns_fresh_sum_box` (Binding, expr.h): true iff every value path of the
body ends in a sum-constructor application or a call to a binding already
proven fresh.  Computed at elaboration beside `body_is_inline_c` (walker in
elab_fns.c; instance methods stamped in elab_typeclasses.c), so callee flags
exist before callers elaborate; self-recursion reads its own unset flag and
fails conservatively.  `ap` passes; `alt-or` -- the pass-through that made
the naive drop a use-after-free -- fails, by the same body-not-signature
distinction the `any` family's `returns_fresh_any` already drew.

Two consumers, mirroring the `any` family's two cases:

- **Pending** (the dominant sweep shape, `(ok? (ok 1))`): elab_call.c stamps
  `sum_box_drop_after` on a fresh-producer argument headed into a read-only
  accessor (audited allowlist, `sum_box_reader_name` -- concrete stdlib defns
  only, never a class-method name, since a dictionary-dispatched callee could
  retain).  emit_value's hoist frees the temp after the consuming call
  materializes, gated on the temp's recorded `int64_t` spelling so a
  specialized by-value call is never touched.  Argument positions are
  per-callee: `unwrap-or`'s arg 1 is the DEFAULT the callee can return, so
  only the eq? comparators stamp past position 0.
- **Scope** (`emit_let_value`): a let-bound fresh-producer result with
  accessor-only uses (a widened `catch_box_binding_escapes`) is freed at
  trailing scope exit.  letrec conservatively skipped.

All frees are null-guarded (the None carrier IS NULL) and shallow -- accessors
copy payload words out -- with one exception: a carrier whose static type says
the live arm holds a boxed value-struct payload (`(Option User)` erased, whose
`some__spec__int64_t_...` mallocs a `User` copy into the cell) frees that arm
first through the cell, since freeing the cell alone only turns the payload
box from an indirect leak into a direct one (`emit_carrier_sum_free`).

Follow-up (2026-09-02): the escape walk fell to its conservative `default`
on `EX_REINTERPRET` -- the carrier re-typed for a polymorphic accessor's
tyvar slot -- so every `(unwrap o)` on a carrier `(Option <struct>)` read as
an escape and the drop never fired there.  The walk now models the
reinterpret as its operand, and the accessor / non-retaining-callee argument
check peels it.  **7364 -> 7200 bytes**; `hkt-partial-app-wildcard-byvalue`
left the leak list and carries `requires.leak-check`.

Pass-through hole (2026-09-03): the user-callee half of the pending case
(`nonretain_sum_param_mask`) admits a bare use of the parameter in the body's
result position, on the argument that the callee's RESULT cannot carry the
box out.  That gate was a denylist of result kinds and let `TY_APP` /
`TY_ADT` through, so `(defn pass [n : int v : (Option S)] : (Option S) (if
(= n 0) v (pass (- n 1) v)))` -- which returns `v` as-is, arm box included --
was stamped non-retaining and the caller freed the arm its own result still
pointed at (`tur_type_fuzz_src` seed 1, cases 12 and 17: a freed box's stale
word printed where 3 and -13 were expected).  The gate is now a scalar
allowlist (nil / bool / int family / float family / cstr / sym); an
aggregate-result callee is never stamped, and the box it hands back is a
status-quo leak rather than a use-after-free -- the same trade `alt-or`
already makes on the producer side.  `sum-passthrough-param-not-dropped`
pins the shape.

Class-method sites (2026-09-03): the same pending case reached the one
call shape neither mask could see -- a class-method consumer, built by
`elab_method_call` outside the argument loop that stamps -- and a let
bound through a copying reader (`ok-val` / `err-val` / `unwrap`) of a fresh
producer.  The value-struct report is closed at 9 of 9 and archived; its
"glue route" is assessed there and declined (the tag walk is the glue; the
remainder is a move-only discipline on Option/Result).  A return-dispatched
producer's cell is now drained against its RE-RESOLVED instance's declared
result, which is also what lets the erased `ok__spec__int64_t_<struct>` copy
inside it be freed -- an RM1 residue shape as much as a payload one.

Storing callee (2026-09-03, later): the round-2 sum mask accepted a
parameter handed to any callee whose result was a scalar -- a use-after-free
once the callee stored it (`vec-push!`).  Under the sum walk that hand-off is
an escape now; a tyvar-typed parameter joins the inference; inline-C
Option/Result producers are fresh by declaration.  See the value-struct
report's addendum.

Bind chains (2026-09-02, later): the largest remaining erased entries were
`bind` / `fmap` chains over stdlib `Result` / `Option` -- `result-monad-*`,
`hkt-stdlib-*`.  Each chained `bind` hands back a fresh box (the continuation's
by-value result crosses the poly boundary through the `__tur_fatspill` shim,
the `Err` arm mints `(err e)`) that the typed-boundary bridge copied out and
never freed, and the continuation's closure env was never dropped either.
Four pieces, all keyed on facts about the INSTANCE METHOD at a statically
resolved dispatch site (`fn_binding` is the instance method's binding there):

- Instance methods now get the same inferred masks a defn gets
  (`elab_infer_nonretain_masks`, shared), so `bind`'s continuation slot is
  known non-retaining and the closure-argument hoist applies to dispatch
  calls too (through the poly / fat wrap, which the hoist and the escape walk
  now see through).  The hoisted `__borrowc` var keeps its lambda in
  `hoist_closure_fn_binding`, which the poly-wrap emitter and the per-spec
  passed-closure finder (M6 / G6(c)) now read -- without that the by-value
  aggregate spill shim was skipped and the erased base instance read a struct
  return as an int64 (NULL deref), and `re-cata`'s recursive closure lost its
  per-spec clone.
- Freshness through a continuation: `Binding.fresh_sum_via_param_mask`
  records that a body's every value path is fresh OR a call through
  parameter i (`bind`'s `(k v)`); `call_returns_fresh_sum_box` then asks the
  call site whether every such argument is itself a fresh producer (a closure
  literal with a fresh body, a flagged defn, or a hoisted closure var).
- A fresh Turmeric-level producer read back by value now marks its carrier
  temp owned exactly as the inline-C contract does, so the existing bridge
  consumption frees it after the copy (`explicit-r`'s `return *(T *)box`).
- The scope-exit env free admits a closure whose result is a sum / product /
  cstr when its body has no inline C (a copy cannot point into the env).

**Soundness correction found on the way.** A dictionary dispatch inside a
constrained generic carries a REPRESENTATIVE instance's binding as
`fn_binding`; the first RM1 round read its freshness flag as if it were the
callee's, and freed `(one i)`'s box in `rec [A] [(C A)]` on the strength of
`C [int]` alone.  Every consumer of instance-method facts is now gated on
`call_dispatch_is_static` (concrete receiver / result head), and the accessor
stamp on a dynamic dispatch is tentative: the emitter re-resolves the instance
per monomorph (`emit_reresolve_method_fndef`) and asks THAT binding.  The
`van-laarhoven-lens-*` family (rank-2 dict clones) and
`hkt-cata-fmap-byvalue-carrier` were the fixtures that caught it.

**7200 -> 6856 bytes**; `result-monad-bind-typed-boundary` and
`result-monad-nested-bind-typed-boundary` fully clean and carrying
`requires.leak-check`; `option-map-capturing-closure` 40 -> 16.  Two more
gaps closed on the way to the nested one: the `do-m` macro's
`(list .bind ...)` route into `elab_method_call` bypassed the hoist (both
dot-call routes now apply it, static-gated), and `expr_subtree_has_inline_c`
defaulted to "may hide inline C" on a closure literal and the fat / poly
wraps, so every body holding a hoisted continuation lost the inferences keyed
on it -- it now walks into a nested closure's body (its captures are copies
this body's inline C could equally stash) and through the wraps.  The
audited-reader residue of the erased sweep (rows tagged SUM-BOX) is 704 B:
inline-C `:int` readers in fixture scaffolding (`res-ok`, `unwrap-or-carrier`)
and dictionary-dispatched sites inside constrained generics.

**Measured: 8324 -> 7364 bytes** across the 27 erased-base callers (the
null-None mirror above contributed 112 of that; the drops the rest), with the
`hkt-stdlib-*` fixtures leaving the leak list entirely and
`typed/result-basic` 528 -> 288.  Residue: consumers outside the audited
allowlist (user readers like `opt-val`, monadic `bind` chains -- unstampable
by design, a user instance may retain), plus the non-sum "other" leaks.  The
end state is still monomorphization; this mechanism is the interim owner.
Guarded by `tests/fixtures/erased-sum-box-scope-drop`
(`requires.leak-check`, verified NON-VACUOUS: six erased constructor calls
and six frees in its emitted C -- an ascribed spelling specializes and tests
nothing).  Leak-check 62/0/0; full suite 2749/0; both seams green; 4
snapshots regenerated in the same commit.  Sweep record:
[rm1-leak-sweep-after.txt](../artifacts/rm1-leak-sweep-after.txt).

**An aside worth its own report.** None of these 24 fixtures carries
`requires.leak-check`, so `bash tests/run.sh` has never seen any of it -- which
is CLAUDE.md's documented gap ("a leak in EMITTED code passes run.sh
silently") turning up 8.3 KB of real leaks the moment anyone looks. Widening
the marker set is cheap and is not gated on RM1.

**Taken up 2026-09-04, and the aside was half right.**  Full decomposition in
[rm1-leak-sweep-decomposed-2026-09-04.md](../artifacts/rm1-leak-sweep-decomposed-2026-09-04.md).
Widening the marker set is cheap, but not as simple as adding markers: the
sweep's byte totals attribute nothing, and the largest single entry --
`httpd-req-string-opt` at 1285 B, 2.3x the next -- was **1176 B of the
fixture's own hand-written `calloc(1, sizeof(HttpdConn))`**, three fake conns
it built and never freed.  A marker on a fixture that leaks its own test rig
measures the rig.  Fixed by giving that fixture the `free-conn` it never had
(1285 -> 109 B, output unchanged); the rest need their category resolved before
a marker on them means anything.

Broken down by allocating function rather than by fixture, the 1790 B that
remain are:

| category | bytes | whose |
|---|---:|---|
| recursive sum spine | ~990 | **RM2** |
| `tur_string_from_bytes` payloads | ~250 | String ownership |
| **RM1 sum boxes** | **~240** | this phase |
| poly aggregate-spill shim | ~48 | RM1-adjacent |
| program-owned containers | ~100 | the program's |

So RM1's own residue is ~240 B in 16-48 B units, which matches this section's
account of what is unstampable by design.  **The largest real category is
RM2's** -- see the note added there.

### RM2 -- the recursive spine

The per-node spine box of a self-recursive sum, which is what `logic.tur`
allocates and what SR4 leaves boxed even under `TUR_SR4_RECURSIVE_BYVALUE=1`
(by value halves the mallocs; the spine box per node remains).

**This is where RM1's local rule stops working.** A tree's nodes escape their
constructor by construction -- that is what a spine is -- so scope-exit drop
does not reach them and per-node free needs ownership the emitter does not
have. Row B's 2.49x is measured with the frees written by hand in C, not
inferred.

*Gate:* RM0(b). If there is no workload, this phase does not start, and the
reason is recorded rather than the phase being left implicitly pending.

**The gate's evidence, revisited 2026-09-04.** RM0 closed this phase as "no
constituency".  Two measurements now argue otherwise, and they are recorded
here rather than acted on, because RM2's own blocker is unchanged:

1. **It is the biggest real category left.** Once `httpd-req-string-opt`'s
   test scaffold is out of the RM1 sweep, per-node spine boxes
   (`ctor_Cons_Cons__*`, `re-string`'s regex cells) are ~990 of the remaining
   1790 bytes -- 55%, against RM1's own ~240.
2. **It GROWS, and the corpus sweep could not see that.** A 64-link `Subst`
   chain leaks 64 boxes; 100 rounds of an 8-link chain leak 800, not 8.  A
   program that builds and discards recursive values in a loop grows without
   bound.  RM0 priced the spine as an allocation COUNT across a corpus where
   every fixture builds its structure once -- an axis on which unbounded
   growth and a one-shot allocation are indistinguishable.  A backtracking
   solver is the workload the gate asked for.

This does not reopen the phase on its own: "a tree's nodes escape their
constructor by construction" is still true, per-node free still needs
ownership the emitter does not have, and Row B's 2.49x is still measured with
frees written by hand.  What has changed is that the gate's premise -- that
nobody is paying for this -- no longer holds.

#### How RM2 gets unblocked (assessed 2026-09-04)

**Not by a better analysis.**  The blocker is structural, not a missing pass.
A persistent spine has no unique owner *by construction* -- that is what
persistent means.  `(SBind v t rest)` shares `rest` with every older chain and
backtracking depends on that sharing, so "is this the last reference?" is a
RUNTIME fact.  No static per-node rule can answer it, and looking harder for
one is the wrong move.

So the question changes rather than the analysis.  Three answers, and this
tree already has substrate for all three:

**A. Give each node a runtime owner -- refcount.**  `rc<T>`,
`RcControlBlock`, and a Bacon-Rajan cycle collector (`src/runtime/gc.c`) all
ship today.  Deterministic, and sharing is exactly what it is for.  But a
control block per node is wider than the 48-byte node being reclaimed, and the
inc/dec lands on the walk -- the hot path this plan's sibling report already
measures at 8.8x.  Almost certainly a net loss for `Subst`.  Worth stating so
nobody re-derives it.

**B. Give the spine a LIFETIME instead of an owner -- RM3, and it is the
fit.**  Every node of one query dies together; that is the whole shape.  What
already exists:

- `arena_reset` -- O(slabs) rewind, and in a Debug build it POISONS the
  reclaimed bytes, so a survivor crashes loudly under ASan instead of reading
  stale-but-mapped data.  That is this phase's stated worst risk, already
  instrumented.
- `arena_owns` -- the guard RM3 says must sit on every free path.
- **A precedent with the same shape, already running.**  turi's value-pool
  scratch/permanent split (`eval.c`, `turi-value-pool-scratch-promotion-plan`)
  does reset-with-promotion: walk the escapees out, then rewind.  Its
  conservatism rule is precisely the one RM3 needs --

  > Correctness never depends on catching every shape: a missed shape means
  > "this eval does not shrink", never "use-after-reset".

  A region that cannot prove a value safe to relocate simply does not rewind.
  That turns RM3's "most able to produce a silent wrong answer" into "most
  able to produce no saving", which is a different risk class.  The code is
  the interpreter's and does not transfer directly; the discipline does, and
  it has been load-bearing here before.
- **The boundary already exists in the language.**  `bt-scope`
  (stdlib/trail.tur) brackets exactly the region a solver query occupies.
  This plan already said as much -- "not region inference; it is one
  `arena_reset()` at a call site that already exists".

**C. Make the box never exist.**  The direction the report prefers and the one
RM1 already cashed once (the null-None mirror).  For a spine that means
unboxing the recursive field, which SR4 explored: by value halves the mallocs
and the per-node box remains.  Exhausted, short of a representation change
bigger than this plan.

**Recommendation: RM2 is unblocked by doing RM3, not by unblocking RM2.**

RM3's gate is "RM0(b), and RM1 landed".  RM1 is now landed to its unstampable
residue (~240 B), and the constituency measurement above is RM0(b)'s missing
input.  After RM3, what is left of RM2 is "spines that escape their region" --
a far smaller set, and one where per-node ownership may actually be
inferable because the escaping cases are the ones the promotion walk already
had to identify.

Sequencing this the other way round is what the RM3 gate warns against:
"shipping both orderings at once is how two sites end up disagreeing about who
frees."

*First increment if this is taken up:* an `EXPERIMENTS[]` row (`regions`) with
every descriptor field, `experiment_warn_if_used` at the elaboration entry,
and `plan_path` here -- it is user-visible, so CLAUDE.md requires the gate.
Then wire the EXISTING `bt-scope` to an arena generation rather than inventing
a surface form, and measure on `logic.tur`, which is the workload.  Fixtures
assert the VALUE across the boundary, plus a negative where an escapee is
poisoned -- "asserts the value, not merely that it builds" is the SR4 lesson
and this section already says it applies here with more force.

### RM3 -- declared regions

An explicit scope form over the existing arena. `arena_owns` guards every free
path so a pointer into region memory is never handed to `free()` -- the
slab's exact death, made a local check instead of a whole-program pass.

Applied first to the workload that has a natural boundary: the SR plan already
identifies it, and is careful about what it is claiming --

> a per-query arena there is not region inference; it is one `arena_reset()`
> at a call site that already exists.

**This one is user-visible, so per CLAUDE.md it wants an `EXPERIMENTS[]` row**
with every descriptor field populated, `experiment_warn_if_used` at the
elaboration entry point, and `plan_path` pointing here. It is also the phase
most able to produce a silent wrong answer -- a value outliving its region is
a use-after-reset, and the arena's guard mode exists precisely to turn that
into a SIGSEGV at the deref rather than a plausible-looking read. Any
regression fixture asserts the VALUE, not merely that it builds; that is the
SR4 lesson and it applies here with more force.

*Gate:* RM0(b), and RM1 landed (a region is not a substitute for knowing what
owns a box; it is a different answer to the same question, and shipping both
orderings at once is how two sites end up disagreeing about who frees).

### RM4 -- re-measure, and settle SR4

SR4's default flip is explicitly parked on this: the trade today is 7-13%
time for 2.2x memory, and an arena "makes the carrier's mallocs cheap AND
keeps one-word copies, at which point by-value recursive sums may have no
constituency at all." The seam (`TUR_SR4_RECURSIVE_BYVALUE=1`,
`tests/run-sr4-seam.sh`, ctest `tur_sr4_seam`) is CI-armed, so this
re-measurement is a re-run, not a re-excavation.

Deliverable: the SR plan's SR4 section updated with a decision, either
direction. RM4 is not entitled to leave it open a second time.

**RM4 ran on 2026-09-02, and decided: by value.** RM0 removed the premise for
waiting (no arena is coming), and the re-measurement on the same two
workloads came back at ~1.03x time for 1.8x less memory on `logic.tur`
(370 -> 202 MB peak RSS at 400k passes) and no slower at 1.2x less on
`re.tur`. The default is flipped at the one line the SR plan named;
`TUR_SR4_RECURSIVE_CARRIER=1` restores the carrier and the seam harness now
guards that path. Full suite 2749/0 with no snapshot drift. The decision
record is in the SR plan's SR4 section. The erased sweep moved with it,
6856 -> 6091 B, all of it `re-string`'s payload boxes (1281 -> 516); the
spine boxes are what remain there.

**Addendum 2026-09-03 -- the re-measurement swept the wrong axis.** Both
`logic.tur` rows are ONE chain length (n=8) over varying pass count, and the
by-value / carrier ratio turns out not to be constant in n: parity at n=1,
1.39x at n=8, 2.9x at n=64, and **6.8x at n=512**, still climbing at the top of
the sweep
([benchmarks/logic-subst-results.md](../../benchmarks/logic-subst-results.md)).
The cost is per-link copying in the emitted walk -- 120 bytes per `SBind` link
against the carrier's one word, two thirds of it redundant. Filed as
[../reported/sr4-byvalue-recursive-sum-walk-copies-per-link.md](../reported/sr4-byvalue-recursive-sum-walk-copies-per-link.md).

RM4's construction and memory findings stand and **the flip is not reopened** --
the two redundant copies are removable with no default change. But the next
person to price this should sweep chain length rather than pass count;
`benchmarks/bench-logic-subst.tur` reports that A/B directly. Recorded here as
well as in the SR plan because RM4 is the phase that owns the decision, and
because the plan's own risk list ("do not price RM on `logic.tur` alone") was
aimed at the choice of workload when the trap this time was the choice of axis
within it.

## 4. What this plan does not do

- **Region inference.** Named as the hard part by the benchmark's own caveat.
  RM3 takes the boundary as declared.
- **Turning on the cycle collector.** `src/runtime/gc.c` is a Bacon-Rajan
  trial-deletion collector, `GC_DISABLED` by default for v1. It collects
  `rc<T>` cycles, which is a different problem from a box nobody owns. RM does
  not touch it.
- **The slab.** Shelved 2026-08-25, and re-measurement strengthened the
  decision rather than weakening it (E 1.72x against B 2.49x, and E is the
  only proposed *fix* that still climbs with heap size). The reopen condition
  stands as written and requires both halves.
- **Ownership or affine types.** The dead-arm write is gone with SR2b, so the
  expressiveness argument that would motivate them is already banked.
- **Container element boxes.** That is [CE](container-element-form-plan.md),
  which removes them for niche Options by making storage per-monomorph rather
  than by freeing them.

## 5. Risks, named

- **The escape walk's admissions are the whole soundness argument.** Every
  false negative is a use-after-free, and the `any` family took five passes to
  get its walk right. RM1 inherits the walk rather than writing one; a new
  admission is a new pass, priced as such.
- **Free of arena memory.** The slab's exact failure mode. `arena_owns` is the
  answer and it must be on every free path RM3 can reach, not most of them.
- **ASan sees a bump arena at the wrong granularity.** Already solved --
  `arena.c` carries poisoning and a guard mode with the rationale in
  `arena-debug-poisoning-plan`. Reuse it; do not re-derive it, and do not
  suppress a leak report to make a phase look done.
- **Do not price RM on `logic.tur` alone.** It is the workload the SR plan
  fell into twice, in both directions -- once under-selling SR1 because
  `logic.tur` is structurally blind to it, once over-selling the ceiling
  because `logic.tur` is all of the population it prices. RM0 is specified
  over the corpus for exactly that reason.
- **Fixture churn.** RM1 changes emitted C, so snapshots regenerate in the
  same commit as the codegen change, never in a follow-up.

## 6. If only one phase gets built

**RM1.** It is local, it reuses a mechanism that has been proven three times,
it closes at least one open report outright, and it is the only phase that
does not depend on RM0 finding a constituency. Row B is 2.49x and flat with
heap size, which is the property that matters for a footprint problem.

RM3 is the phase with the big number attached, and it is also the phase whose
number is conditional on a boundary the language does not currently have a way
to say. Build it when RM0(b) names a workload that wants one -- not before,
and not because 7.64x is the largest figure in the table.
