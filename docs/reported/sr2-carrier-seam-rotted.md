---
title: The SR2 carrier seam has rotted -- a compile error, a silent wrong answer, and a layering crash
category: Reported
description: TUR_SR2_APP_SUM_BYVALUE=0, the bisection hatch left behind when parametric-sum-byvalue graduated, no longer produces correct programs. Three distinct defects, found by restoring the harness that was retired at graduation. The layering crash (1) is fixed; the carrier-path compile error (2) and the silent wrong answer (3) remain open.
---

# The SR2 carrier seam has rotted

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

## 2. `sum-passthrough-param-not-dropped`: a hard C compile error

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

## 3. `option-niche-vec-closure-cmp`: a silent wrong answer

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
