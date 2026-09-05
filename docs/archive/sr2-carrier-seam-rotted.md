---
title: The SR2 carrier seam has rotted -- a compile error, a silent wrong answer, and a layering crash
category: Archive
description: RESOLVED 2026-09-04. TUR_SR2_APP_SUM_BYVALUE=0, the bisection hatch left behind when parametric-sum-byvalue graduated, no longer produced correct programs. All three defects are now fixed; the last one needed two independent changes, and `match` being absent from the spec-minting walk entirely was the half the root-cause note had not found.
---

# The SR2 carrier seam has rotted

**RESOLVED 2026-09-04.** All three defects fixed. `tests/run-sr2-seam.sh` is
57 passed / 0 failed with nothing excluded, and the four-mode table for
`option-niche-vec-closure-cmp` (default, niche off, SR2 off, both off) is green
in every cell. See **Defect 3: resolution** at the end.

**Severity: medium.** Nothing here affects the default path -- every fixture
below is green with no env vars set. What is broken is the **bisection hatch**
`TUR_SR2_APP_SUM_BYVALUE=0`, which is the documented way to A/B a suspected
representation bug against the old int64 carrier
(`docs/upcoming/sum-representation-plan.md` SR2c, `src/main.c:10280`). It is
the instrument you reach for when something is already wrong, so it failing is
a second problem stacked on whatever sent you to it.

Found 2026-09-04 by restoring `tests/run-sr2-seam.sh`, which was retired at
graduation (`c26e38a8`). Both sibling seams -- `run-sr4-seam.sh` and
`run-option-niche-seam.sh` -- flipped their harness to guard the newly
uncovered path instead of deleting it; SR2's was the one that did not, and
this is what accumulated in the year-equivalent of release lines since.

## 1. Layering: `TUR_SR2_APP_SUM_BYVALUE=0` alone aborts the compiler -- **FIXED 2026-09-04**

**Resolved.** `sr3_option_niche()` (`types.c`) now returns false when
`g_sr2_app_sum_byvalue` is clear, so the niche follows its substrate down and
`TUR_SR2_APP_SUM_BYVALUE=0` is a complete one-variable switch again. All four
compiler aborts and the `httpd-req-string-opt` wrong answer are gone; the
comments in `main.c` and `globals.h` now state the direction of the coupling
(clearing SR2 clears the niche; clearing the niche leaves SR2 alone). Default
path byte-identical -- both globals are true in every shipping build, so the
added test is dead weight there, and the suite is 2781/0 before and after with
zero snapshot drift.

The one fixture that does not recover is `option-niche-vec-closure-cmp`, which
is **defect 3 below**, not this one: with the fix, `SR2=0` *is* the both-off
state, which is the diagonal where defect 3 already lived. The abort simply
stopped hiding it.

The original report follows.

### The defect as found

SR3's Option niche (`TUR_OPTION_NICHE`, default ON since 2026-09-03) is built
**on top of** by-value parametric sums: it narrows an eligible `(Option P)` to
its payload pointer, a representation that exists only because SR2 put the sum
by value. Turning SR2 off underneath a live niche pulls the rug out.

Six fixtures, all green on the default path, with `TUR_SR2_APP_SUM_BYVALUE=0`
alone -- four of them a **compiler abort**, not a diagnostic:

| fixture | SR2=0 | SR2=0 + niche=0 |
|---|---|---|
| `inline-c-option-byval-param` | compiler abort | OK |
| `inline-c-carrier-producer-byval-positions` | compiler abort | OK |
| `option-niche-string` | compiler abort | OK |
| `option-niche-vec-closure-cmp` | compiler abort | (see 3) |
| `option-niche-crossings` | build failed | OK |
| `httpd-req-string-opt` | **silent wrong answer** | OK |

Nothing in the tree says the two hatches must move together. `main.c` reads
them as two independent two-way overrides in the same block, and each one's
comment describes it as a self-contained bisection switch.

**Fix, as applied.** `sr3_option_niche()` returns false when
`g_sr2_app_sum_byvalue` is off, so the niche cannot outlive its substrate, with
the coupling stated in `main.c` and `globals.h`. That makes
`TUR_SR2_APP_SUM_BYVALUE=0` a genuine one-variable switch again, which is what a
bisection hatch has to be. The alternative considered and rejected --
documenting "always set both" -- leaves a compiler abort as the failure mode for
getting it wrong.

Note the fix is deliberately one-directional: `TUR_OPTION_NICHE=1` cannot force
the niche back on underneath a disabled SR2, because that is precisely the
combination that aborted. `tests/run-sr2-seam.sh` sets both hatches anyway, so
it pins the fix rather than depending on it.

## 2. `sum-passthrough-param-not-dropped`: a hard C compile error -- **FIXED 2026-09-04**

**Resolved.** The match binder's `nested-carrier-match` branch read an
Option/Result **ROS pointer-box** slot inline, initializing an aggregate from a
pointer. That branch assumes the typedef emitter lays a non-wide by-value ADT
field out inline ("exactly as the typedef emitter lays it out"), but
`adt_field_is_ros_pointer_box` overrides that width heuristic for an
Option/Result owner and spells the member `tur_adt_T *` however narrow `T` is.

The dedicated pointer-box branch that handles this correctly sits earlier in the
same chain, but is guarded on `adt_byval` -- true only when the OWNER flows by
value. That is why the default path never saw it and the carrier did. Both
binder sites (`emit_expr.c`) now exclude a ROS pointer-box field from the inline
read, falling through to B3, whose deref is byte-identical to what the
pointer-box branch emits.

Default path unaffected: 2781/0 with zero snapshot drift, because on that path
the `adt_byval` branch already caught these fields first. The fixture is now in
`tests/run-sr2-seam.sh` (55 passed, 0 failed).

The defect as found follows.

### The defect as found

Independent of the niche (fails with it off **or** on), so this is the carrier
path's own rot:

```
tests_fixtures_sum-passthrough-param-not-dropped_input_tur.c: In function 'read_hymatch':
7155 |                 tur_adt_Pt s_1448 = __scrut->as.Some._0;
     |                                     ^~~~~~~
error: invalid initializer
```

An aggregate (`tur_adt_Pt`) bound directly from a carrier match slot. This is
recognisably the same family as the flip's original defect list -- "a match on
an erased instance base's param bound the aggregate from an `int64_t` slot",
"an Option/Result pointer-box payload slot bound as a value" (the SR2c table in
`sum-representation-plan.md`) -- all of which were fixed on the by-value side.
The carrier side of the same match-field binder did not get the same
treatment, and nothing compiled it afterwards to notice.

Repro:

```sh
TUR_SR2_APP_SUM_BYVALUE=0 TUR_OPTION_NICHE=0 \
  ./build/tur build tests/fixtures/sum-passthrough-param-not-dropped/input.tur -o /tmp/x
```

## 3. `option-niche-vec-closure-cmp`: a silent wrong answer -- **FIXED 2026-09-04**

The sharper of the two, because it is a **wrong value, not a crash**, and
because of how it hides:

| mode | result |
|---|---|
| default | OK |
| `TUR_OPTION_NICHE=0` alone | OK |
| `TUR_SR2_APP_SUM_BYVALUE=0` alone | compiler abort |
| both off | **wrong output** |

`run-option-niche-seam.sh` carries this fixture green on its own axis, and the
ordinary suite carries it green on the default. It is only wrong on the
diagonal -- which is exactly how a two-axis rot stays invisible to two
one-axis harnesses. Worth keeping in mind for the other seams: SR4's
`TUR_SR4_RECURSIVE_CARRIER` is a third axis nobody has crossed with either of
these.

Since defect 1's fix, `TUR_SR2_APP_SUM_BYVALUE=0` alone reproduces it (that flag
now implies the niche off), so it no longer needs the diagonal to hit.

### Root cause -- established 2026-09-04

Minimal repro, no Vec and no closures:

```turmeric
(load "stdlib/string.tur")
(defn main [] : int
  (let [a (:: (some (string/from-cstr "aa")) (Option String))
        b (:: (some (string/from-cstr "aa")) (Option String))]
    (println (if (eq? a b) "opt-eq" "opt-ne")))    ; default: opt-eq.  SR2=0: opt-ne
  0)
```

`--emit-abi-trace` on the outer `(eq? a b)` says it all:

```
default:  abi-trace __inst_Eq_eq_qu_Option concrete-clone __inst_Eq_eq_qu_Option__spec__bool_void___void__
SR2=0:    abi-trace __inst_Eq_eq_qu_Option dictionary
```

By value a per-instantiation ABI **specialization** is minted, and inside that
spec body the inner dispatch re-resolves correctly to
`__inst_Eq_eq_qu_String`. On the carrier no spec is minted, so the call lands
in the GENERIC instance body, whose inner dispatch was baked against the
representative:

```c
/* SR2=0, generic __inst_Eq_eq_qu_Option */
int64_t vx_939 = (int64_t)__scrut->as.Some._0;
bool __ps_15 = (__inst_Eq_eq_qu_int(vx_939, vy_940));   /* pointer equality on a String */
```

Instrumenting `emit_abi_register_call` for that call gives the decision:

| | `abi_changes` | `instance_changes` | spec |
|---|---|---|---|
| default | **1** | 0 | minted |
| `SR2=0` | 0 | 0 | none |

`abi_changes` is the only trigger that fires, and it fires for a reason that has
nothing to do with dispatch: the class var `a` is bound to `(Option String)`,
whose `type_c_name` is `void *` under the niche and `int64_t` under the carrier.
So the spec exists because the SIGNATURE changed, and correct dispatch is a
side effect of that.

**The consequence worth flagging beyond this hatch:** the comments in
`stdlib/option.tur` and `stdlib/result.tur` say the `(:: vx A)` ascription is
what makes the inner `eq?` "re-dispatch per ABI specialization". The ascription
is necessary but it is not what triggers the specialization -- the C-signature
change is. Where a payload type does NOT change the signature, the ascription
alone does not save the dispatch. That is a latent gap the by-value
representation currently papers over, not a carrier-only defect.

The second trigger, `instance_changes`
(`body_has_dispatch_on_app_tyvar`), is meant for exactly this "ABI unchanged but
the instance differs" case and already documents the opaque-`String` scenario in
its comment. It does not fire here: it looks for a dispatch receiver that is a
tyvar named in `bindings`, and `bindings` holds the CLASS var (`a` ->
`(Option String)`), whereas the inner call dispatches on the INSTANCE's own
constraint var `A`, which is not in that set at all.

**Fix direction.** Extend the `instance_changes` trigger so an instance-method
body dispatching on the instance's own constraint var mints a spec when that var
is recoverable from a `TY_APP` class-var binding with a nominal argument
(`(Option String)` -> `A = String`). Two cautions from a first attempt: the
recursion in `body_has_dispatch_on_app_tyvar` did not appear to reach the inner
call under the concrete binding set, so the traversal wants checking before the
predicate is extended; and because `instance_changes` is consulted only when
`!abi_changes`, a careless widening mints specs that do not exist today on the
DEFAULT path too -- so the change must be validated against the full snapshot
suite, not just the seam.

## What is NOT a defect here

- **~148 `codegen mismatch` failures** under `TUR_SR2_APP_SUM_BYVALUE=0 bash
  tests/run.sh`. The `expected.c` snapshots are committed for the default
  representation; they move when the representation does. Compare stdout, the
  way the seam harnesses do. (The full-suite number under the seam is 2624
  passed / 157 failed, of which 149 are snapshot drift and 8 are real.)
- `option-niche-vec-word`, `option-niche-carrier-some-null-aborts`,
  `option-niche-null-payload-aborts` -- red with the niche off **by design**
  (they assert the niche's own word form and runtime aborts), already
  documented as such in `run-option-niche-seam.sh`.

## Meta: the graduation checklist has a hole

The rule in `CLAUDE.md` covers adding an experiment and graduating it, but not
what happens to the **path the graduation stops exercising**. Three graduations
hit this and two got it right by instinct rather than by rule. Worth one line
in the experimental-flags guide: *if graduation flips a default, the harness
that covered the old default does not retire -- it inverts.*

---

# Defect 3: resolution, 2026-09-04

The root-cause note above was right about the mechanism (`abi_changes` is the
only trigger that fires, and it fires for a signature reason unrelated to
dispatch) and right that the fix belongs in `instance_changes`. It found one of
the two things that had to change, and both were needed -- verified by
subtraction, each half alone still prints `opt-ne`.

## `match` was not in the walk at all

`body_has_dispatch_on_app_tyvar` traverses `EX_PROGRAM`, `EX_FN_DEF`, `EX_DEF`,
`EX_LET`, `EX_DO`, `EX_BUILTIN`, `EX_IF`, `EX_WHILE`, `EX_CALL`,
`EX_MAKE_STRUCT`, `EX_GET_FIELD`, `EX_SET_FIELD`, `EX_RETURN`, `EX_ASCRIBE`,
`EX_CAST`, `EX_REINTERPRET` -- and **not `EX_MATCH`**.

So every `instance_changes` trigger -- the TY_APP tyvar dispatch, the
return-dispatch, the field extraction -- was blind to anything inside a match.
That is not a corner case: matching is how a sum instance is written, so
`(definstance Eq [Option] ... (match x (Some vx) ... (eq? (:: vx A) ...)))` had
its only dispatch permanently out of reach. This is the report's caution (a)
("the recursion did not appear to reach the inner call") with a cause: it was
not a subtle traversal-order problem, the node kind was simply missing.

Found by instrumenting the walk rather than reading it: a probe printing every
call the walk reached under the CONCRETE binding set reached exactly one, with
no `dict_arg`, while the same probe under the abstract binding set reached the
ascribed inner calls. The asymmetry is what pointed at the container, not the
predicate.

## The receiver is an ascription to the instance's own constraint var

With the walk reaching it, the existing predicate still declined. It peels
`EX_ASCRIBE` and asks about the INNER expression -- whose type on the carrier is
`int64_t` -- and `A` is not in `bindings` regardless: that holds the CLASS var,
`a -> (Option String)`.

The added clause asks about the ascription's OWN type: a tyvar that is NOT in
the binding set is the instance's own constraint var, and a spec is minted when
some class-var binding is a `TY_APP` whose type argument is nominal
(`TY_ADT`/`TY_APP`).

**That last condition is the over-minting guard**, and it is why the report's
caution (b) did not materialise: only a nominal payload can have an instance the
`int` representative gets wrong. `(Option int)` / `(Option bool)` /
`(Option float)` have a primitive `args[0]` and mint nothing. Measured: the full
default suite is 2792 passed / 0 failed with **zero snapshot drift**, so the
widening changes no byte of the default path.

## The dispatch was only half of it

Minting the spec fixed WHICH instance is called. It was then handed the wrong
thing: inside a carrier-representation spec the payload locals are `int64_t`,
while the re-resolved concrete instance declares
`__inst_Eq_eq_qu_String(void *, void *)`. The program printed the right answer
and emitted

```
warning: passing argument 1 of '__inst_Eq_eq_qu_String' makes pointer from
integer without a cast [-Wint-conversion]
```

which `tests/run.sh`'s cc ratchet treats as a hard FAIL and GCC >= 14 treats as
an error. A right answer with a `-Wint-conversion` under it is not a fix. The
argument half is a reinterpret at the call site, narrowly gated: a
dict-dispatched call, inside a spec, whose callee's RECORDED param is a pointer
and whose argument's static type is a carrier word.

## What this says beyond the hatch

The report flagged it and it survives the fix: the comments in `option.tur` and
`result.tur` claiming the `(:: vx A)` ascription is what makes the inner `eq?`
re-dispatch describe a coincidence. The ascription is necessary; the
C-signature change is what fired. Both comments now say so.

One thing the report suspected that is NOT a live default-path gap: two distinct
payload types that share an Option C signature do not collide. Probed with two
`defopaque ... :ptr<void> :non-null` types carrying deliberately opposite `Eq`
instances -- the emitter disambiguates the clone names
(`__spec__bool_void___void__` and `..._h1`), and both answered correctly.

## Meta

The graduation-checklist hole the report names is now one section in
[docs/guides/experimental-flags-guide.md](../guides/experimental-flags-guide.md):
*if graduation flips a default, the harness that covered the old default does
not retire -- it inverts.*
